// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// The Admin web portal. See web_portal.h for the interface.
//
// WebConsole, an async schema-driven console from lib_deps, serves the portal.
// This file declares what Birdoscope exposes through it. The main pieces are:
//   - session: the SPIFFS session JSON and the previous session, for download
//     on every board.
//   - logs: on SD boards, a page listing each session directory's files, and
//     any .csv in the card's root, with download links.
//   - commands: the serial console's verbs, with status, wifi, calibrate and
//     help pinned as buttons.
//
// SoftAP and the promiscuous sniffer share one radio, so webPortalStart() stops
// the selected radio and webPortalStop() restarts it.
#include "web_portal.h"
#include "roost_session.h"
#include "board_config.h"   // must precede core.h so USE_SD/HAS_GPS gate its externs
#include "core.h"

#include <WebConsole.h>
#include <ESPAsyncWebServer.h>   // AsyncWebServerRequest for the escape-hatch routes
#include <SPIFFS.h>
#include <WiFi.h>                // WiFi.mode(WIFI_OFF) to fully release Arduino WiFi on stop

#if USE_SD
#include <SD.h>
#endif

using jelly::webconsole::WebConsole;

// mDNS and device name. The AP SSID and password live in web_portal.h, where
// the boards read them for webPortalStart(). board_config.h may override.
#ifndef WEB_PORTAL_DEVICE_NAME
#define WEB_PORTAL_DEVICE_NAME  "birdoscope"   // also the mDNS name, birdoscope.local
#endif

// Idle timeouts that return Admin to Detect, so an accidental BOOT double press
// in the field does not pause scanning for good. Admin ends when no client
// connects within WEB_PORTAL_NO_CLIENT_MS of entry, or WEB_PORTAL_IDLE_MS after
// the last client leaves. board_config.h may override.
#ifndef WEB_PORTAL_NO_CLIENT_MS
#define WEB_PORTAL_NO_CLIENT_MS 180000UL  // 3 min to join the AP and connect
#endif
#ifndef WEB_PORTAL_IDLE_MS
#define WEB_PORTAL_IDLE_MS      60000UL   // 60s after the last client disconnects
#endif

static WebConsole console;
static bool          portalActive = false;
static bool          registered   = false;
static char          status[96]   = "browse to configure";
// Release state, reset on each webPortalStart().
static volatile bool exitRequested = false;   // set by the web "return to scan" action
static unsigned long exitReqMs     = 0;        // request time, for the flush grace period
static unsigned long portalStartMs = 0;        // for the never-connected timeout
static unsigned long lastClientMs  = 0;        // last time any client held a connection
static bool          everHadClient = false;

// The "status" command. Prints firmware version and scan state into the console
// log, mirroring the serial `status` verb. It reads core externs, since each
// board main keeps its own static printStatus().
static String reportStatus() {
  unsigned long s = millis() / 1000;
  console.logf("Birdoscope %s", coreBuildIdentity());
  console.logf("uptime=%lus ch=%u mode=%s det=%d spiffs=%d sniffing=%d",
               s, (unsigned)currentChannel, channelModeName(), fyDetCount,
               fySpiffsReady ? 1 : 0, sniffingStopped ? 0 : 1);
  // Frame counts show whether the path is alive, and device counts show what is
  // out there. Device counts overlap and can sum past det, spec C2.
  console.logf("frames direct=%u indirect=%u",
               (unsigned)coreDirectFrames, (unsigned)coreIndirectFrames);
  console.logf("devices direct=%u indirect=%u (of %d total)",
               (unsigned)coreDirectDeviceCount(),
               (unsigned)coreIndirectDeviceCount(), fyDetCount);
  // These hold their values from when the portal stopped the sniffer. Serial
  // shows the live figures.
  console.logf("sniffer seen=%u cand=%u qdrop=%u",
               (unsigned)coreSeenFrames, (unsigned)coreCandidateFrames,
               (unsigned)coreQueueDrops);
  // Reads the roost writer, the same source as the serial `status` command.
  uint32_t rw = 0, rd = 0, wf = 0, fx = 0;
  roostSessionStats(&rw, &rd, &wf, &fx);
  console.logf("load qmax=%u/%u bqmax=%u/%u bqdrop=%u rows=%u fixes=%u dropped=%u"
               " worst_flush=%ums session=%s",
               (unsigned)coreQueueDepthMax, (unsigned)coreAlertQueueSize(),
               (unsigned)coreBleQueueDepthMax, (unsigned)coreBleQueueSize(),
               (unsigned)coreBleQueueDrops,
               (unsigned)rw, (unsigned)fx, (unsigned)rd, (unsigned)wf,
               roostSessionOpen() ? roostSessionDir() : "none");
  console.logf("heap=%u min_free=%u largest_block=%u",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
               (unsigned)ESP.getMaxAllocHeap());
  return String("Birdoscope v") + BIRDOSCOPE_VERSION + " – status printed to log";
}

// The active distance model with a worked example, so an operator can check
// the numbers against a tape measure.
static String describeDistanceModel() {
  char buf[192];
  int8_t last = coreLastDetectionRssi();
  if (last != 0) {
    snprintf(buf, sizeof(buf),   // prefers a real reading to the -80 dBm example
             "density=%s (n=%.1f) rssi_1m=%ddBm; last detection %ddBm reads ~%.1fm",
             envDensityName(coreEnvDensity), corePathLossExponent(),
             (int)coreRssiAt1mDbm, (int)last, coreRssiToDistanceM(last));
  } else {
    snprintf(buf, sizeof(buf),
             "density=%s (n=%.1f) rssi_1m=%ddBm; no detection yet, a %ddBm hit would read ~%.1fm",
             envDensityName(coreEnvDensity), corePathLossExponent(),
             (int)coreRssiAt1mDbm, -80, coreRssiToDistanceM(-80));
  }
  return String(buf);
}

// Batches log lines into console.log() calls of about 900 bytes. One WebSocket
// frame per line overruns the socket on a bulk dump and drops the client.
struct LogBatcher {
  String buf;
  void add(const String& s) {
    if (buf.length() + s.length() + 1 > 900) flush();
    if (buf.length()) buf += '\n';
    buf += s;
  }
  void flush() { if (buf.length()) { console.log(buf); buf = String(); } }
};

// Streams a file into the console log in batches, for the web dump and prev
// commands. Past the cap it stops and points at the download button.
static const size_t WEB_LOG_STREAM_CAP = 8192;

static String streamFileToConsole(fs::FS& fs, const char* path, const char* label) {
  if (!fs.exists(path)) { console.logf("%s: not found (%s)", label, path); return String(label) + ": not found"; }
  File f = fs.open(path, "r");
  if (!f) { console.logf("%s: open failed", label); return String(label) + ": open failed"; }
  console.logf("--- %s (%s, %u bytes) ---", label, path, (unsigned)f.size());
  LogBatcher b;
  size_t emitted = 0;
  bool   truncated = false;
  String line;
  while (f.available()) {
    if (emitted >= WEB_LOG_STREAM_CAP) { truncated = true; break; }
    char c = (char)f.read();
    emitted++;
    if (c == '\n')      { b.add(line); line = String(); }
    else if (c != '\r') { line += c; }
  }
  if (line.length()) b.add(line);
  b.flush();
  f.close();
  if (truncated)
    console.logf("… truncated at %u bytes – use the download button for the full file",
                 (unsigned)WEB_LOG_STREAM_CAP);
  return String(label) + ": printed to log";
}

#if USE_SD
// Prints the open session's wifi_obs file, which roostSessionDir() locates
// before and after the anchor rename. By default it prints the header and the
// last 10 rows, through a rolling window, so the file never loads into RAM
// whole. full=true streams the whole file in batches, and a very large file may
// drop the client.
static const int WEB_LOG_TAIL = 10;

static String dumpSdLog(bool full) {
  if (!fySDReady) { console.log("log: SD not ready"); return String("log: SD not ready"); }
  if (!roostSessionOpen()) { console.log("log: no session open"); return String("log: no session"); }
  char name[48];
  roostFileName(name, sizeof(name), ROOST_REC_WIFI_OBS);
  String path = String(roostSessionDir()) + "/" + name;
  File f = SD.open(path.c_str(), "r");
  if (!f) { console.logf("log: open failed (%s)", path.c_str()); return String("log: open failed"); }
  console.logf("--- SD log (%s, %u bytes)%s ---", path.c_str(), (unsigned)f.size(),
               full ? "" : " – last 10 (add `full` for all)");
  LogBatcher b;
  if (full) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.replace("\r", "");
      if (line.length()) b.add(line);
    }
    b.flush();
    f.close();
    return String("SD log: printed to log (full)");
  }
  // Header, then a rolling window of the last WEB_LOG_TAIL rows.
  String header, win[WEB_LOG_TAIL];
  int cnt = 0, pos = 0;
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    if (line.length() == 0) continue;
    if (first) { header = line; first = false; continue; }
    win[pos] = line; pos = (pos + 1) % WEB_LOG_TAIL; if (cnt < WEB_LOG_TAIL) cnt++;
  }
  f.close();
  if (header.length()) b.add(header);
  int start = (pos - cnt + WEB_LOG_TAIL) % WEB_LOG_TAIL;
  for (int i = 0; i < cnt; i++) b.add(win[(start + i) % WEB_LOG_TAIL]);
  b.flush();
  return String("SD log: last ") + cnt + " rows printed";
}
#endif

#if USE_SD
// Accepts a bare file name, or one directory level as `dir/file`, with no
// empty part, backslash or "..", since /dl serves the card to anyone on the AP.
static bool sdNameSafe(const String& f) {
  if (f.length() == 0 || f.length() > 80) return false;
  if (f.indexOf('\\') >= 0 || f.indexOf("..") >= 0) return false;
  int slash = f.indexOf('/');
  if (slash < 0) return true;
  if (slash == 0 || slash == (int)f.length() - 1) return false;
  return f.indexOf('/', slash + 1) < 0;
}

static String sdBaseName(File& f) {
  String name = f.name();
  int slash = name.lastIndexOf('/');
  return slash >= 0 ? name.substring(slash + 1) : name;
}

// Appends one download link per file in `dir`. `prefix` is empty for the root
// and `name/` for a session directory.
static bool sdListFiles(String& p, File& dir, const String& prefix, bool csvOnly) {
  bool any = false;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) {
      String name = sdBaseName(f);
      if (!csvOnly || name.endsWith(".csv")) {
        any = true;
        p += "<li><a href=\"/dl?f=";
        p += prefix + name;
        p += "\">";
        p += name;
        p += "</a> (";
        p += String((unsigned long)f.size());
        p += " bytes)</li>";
      }
    }
    f.close();
  }
  return any;
}

// The /logs page body. Lists each roost session directory and its files, then
// any loose .csv in the card's root.
static String buildLogsBody() {
  String p;
  p.reserve(2048);
  p += "<section class=\"card\"><h2>SD card logs</h2>";
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    p += "<p>SD card not available.</p></section>";
    return p;
  }
  bool any = false;
  for (File d = root.openNextFile(); d; d = root.openNextFile()) {
    String name = sdBaseName(d);
    if (d.isDirectory() && name.startsWith(LOG_PREFIX)) {
      File dir = SD.open("/" + name);
      p += "<h3>" + name + "</h3><ul>";
      if (dir && sdListFiles(p, dir, name + "/", false)) any = true;
      else p += "<li><i>empty</i></li>";
      p += "</ul>";
      if (dir) dir.close();
    }
    d.close();
  }
  root.rewindDirectory();
  String loose = "<ul>";
  if (sdListFiles(loose, root, "", true)) { p += "<h3>Card root</h3>" + loose + "</ul>"; any = true; }
  root.close();
  if (!any) p += "<p><i>no logs on card yet</i></p>";
  p += "</section>";
  return p;
}
#endif  // USE_SD

// Declares everything the console exposes. Registration must come before
// begin() and persists across start and stop, so it runs once.
static void ensureRegistered() {
  if (registered) return;

  // The SPIFFS session files from the upstream flock-you format, download only.
  console.addFile("session", FY_SESSION_FILE, "application/json", false);
  console.addFile("prev_session", FY_PREV_FILE, "application/json", false);

#if USE_SD
  console.addPage("Logs", "/logs");   // top-bar button to the SD CSV listing
#endif
  console.addPage("Return to scan", "/scan");   // top-bar button to leave Admin

  // Pinned as a button in the Controls card, since status is the most used
  // diagnostic. An operator types the unpinned commands.
  jelly::webconsole::CommandOpts statusOpts;
  statusOpts.pinned = true;
  console.onCommand("status", "print firmware version + scan state to the log",
                    [](JsonVariantConst) -> String { return reportStatus(); },
                    statusOpts);

  // Releases the AP and resumes Detect. The handler only sets a flag, and
  // webPortalTick() tears down after the response flushes, outside the server's
  // own handler. Typed only, since the "Return to scan" page is the button.
  console.onCommand("scan", "leave Admin and resume detection",
                    [](JsonVariantConst) -> String {
                      exitRequested = true; exitReqMs = millis();
                      return String("returning to detection…");
                    });

  // Station credentials for the boot-time NTP fallback, which runs only when no
  // GPS module answers. Pinned, so an operator can set them without a serial
  // cable. Core saves them to SPIFFS and reads them at the next boot.
  // WebConsole has no password field type, so `pass` shows as plain text on
  // this local AP. Replies echo the SSID, never the password. An empty SSID
  // reports the saved network.
  static const jelly::webconsole::Field wifiArgs[] = {
    { "ssid", "network SSID", jelly::webconsole::FieldType::Text, nullptr, false },
    { "pass", "password",     jelly::webconsole::FieldType::Text, nullptr, false },
  };
  jelly::webconsole::CommandOpts wifiOpts;
  wifiOpts.args     = wifiArgs;
  wifiOpts.argCount = 2;
  wifiOpts.pinned   = true;
  console.onCommand("wifi", "save the WiFi network used for NTP time sync when GPS is absent",
                    [](JsonVariantConst a) -> String {
                      String ssid = a["ssid"] | "";
                      String pass = a["pass"] | "";
                      ssid.trim();
                      if (ssid.length() == 0) {          // no SSID entered, report the saved one
                        String cur, cpass;
                        if (coreWifiCredsLoad(cur, cpass))
                          return String("saved network: ") + cur + " (enter an SSID to change)";
                        return String("no network saved – enter an SSID + password to set one");
                      }
                      if (coreWifiCredsSave(ssid.c_str(), pass.c_str()))
                        return String("saved \"") + ssid + "\" – used for NTP at next boot when GPS is absent";
                      return String("save failed – SPIFFS not ready");
                    }, wifiOpts);

  // Distance-estimate calibration, one field per term of the model. `rssi_trim`
  // steps the reference by a delta, and an empty submit reports the current
  // model. See docs/distance_estimation.md.
  static const jelly::webconsole::Field calibrateArgs[] = {
    { "density",   "environment density",              jelly::webconsole::FieldType::Enum,   "low,medium,high", false },
    { "rssi_1m",   "expected RSSI at 1m (dBm)",        jelly::webconsole::FieldType::Number, nullptr, false },
    { "rssi_trim", "or: step it by (dB, + reads farther)", jelly::webconsole::FieldType::Number, nullptr, false },
  };
  jelly::webconsole::CommandOpts calibrateOpts;
  calibrateOpts.args     = calibrateArgs;
  calibrateOpts.argCount = 3;
  calibrateOpts.pinned   = true;
  console.onCommand("calibrate", "tune the distance estimate: environment density and expected RSSI at 1m",
                    [](JsonVariantConst a) -> String {
                      String density = a["density"] | "";
                      density.trim();
                      // WebConsole omits blank inputs, so an absent key means unset.
                      bool haveRef  = !(a["rssi_1m"].isNull());
                      bool haveStep = !(a["rssi_trim"].isNull());

                      if (density.length() == 0 && !haveRef && !haveStep)
                        return describeDistanceModel()
                             + " \u2013 set a density, an rssi_1m, or an rssi_trim step to change it";

                      if (density.length() > 0) {
                        if      (density == "low")    coreSetEnvDensity(DENSITY_LOW);
                        else if (density == "medium") coreSetEnvDensity(DENSITY_MEDIUM);
                        else if (density == "high")   coreSetEnvDensity(DENSITY_HIGH);
                        else return String("unknown density \"") + density + "\" \u2013 expected low, medium, or high";
                      }

                      // An absolute rssi_1m wins over a step. `requested` holds
                      // the asked-for value for the clamp check below.
                      int before    = (int)coreRssiAt1mDbm;
                      int requested = before;
                      if (haveRef) {
                        requested = (int)(a["rssi_1m"] | before);
                        // Clamps before the int8_t cast, which would wrap.
                        int v = requested;
                        if (v > RSSI_AT_1M_MAX) v = RSSI_AT_1M_MAX;
                        if (v < RSSI_AT_1M_MIN) v = RSSI_AT_1M_MIN;
                        coreSetRssiAt1mDbm((int8_t)v);
                      } else if (haveStep) {
                        int step = (int)(a["rssi_trim"] | 0);
                        requested = before + step;
                        if (step >  127) step =  127;   // int8_t parameter range
                        if (step < -127) step = -127;
                        coreNudgeRssiAt1mDbm((int8_t)step);
                      }

                      // Notes a clamp only when the value left the accepted range.
                      // A resent value or a zero step leaves after == requested.
                      int after = (int)coreRssiAt1mDbm;
                      String note;
                      if (after != requested)
                        note = String(" \u2013 ") + requested + " dBm is outside the accepted "
                             + RSSI_AT_1M_MIN + " to " + RSSI_AT_1M_MAX + " dBm, clamped to " + after;

                      String applied = describeDistanceModel();
                      if (!coreSettingsSave())
                        return applied + note + " \u2013 applied for this session, but saving failed, SPIFFS not ready";
                      return applied + note + " \u2013 saved; applies to the next detection";
                    }, calibrateOpts);

  jelly::webconsole::CommandOpts wifiForgetOpts;
  wifiForgetOpts.confirm = true;   // erases the stored network
  console.onCommand("wifi-forget", "erase the saved WiFi network",
                    [](JsonVariantConst) -> String {
                      coreWifiCredsClear();
                      return String("saved WiFi network erased");
                    }, wifiForgetOpts);

  // --- Serial-console parity ---------------------------------------------
  // The serial console's verbs, so the web console stands in for a cable. dump,
  // prev and log stream a file into the log with a cap. inject and nav act on
  // the Detect loop, so in Admin they only say they did nothing. None
  // gets a button.
  console.onCommand("dump", "print the current session JSON to the log",
                    [](JsonVariantConst) -> String {
                      return streamFileToConsole(SPIFFS, FY_SESSION_FILE, "current session");
                    });
  console.onCommand("prev", "print the previous session JSON to the log",
                    [](JsonVariantConst) -> String {
                      return streamFileToConsole(SPIFFS, FY_PREV_FILE, "previous session");
                    });
#if HAS_GPS
  console.onCommand("gps", "print GPS fix, satellites, position, and parser counters",
                    [](JsonVariantConst) -> String {
                      unsigned long good, bad, fixSent;
                      int sats;
                      coreGpsStats(good, bad, fixSent, sats);
                      console.logf("fix=%s sats=%d", gpsHasFix ? "YES" : "no", sats);
                      console.logf("lat=%.6f lng=%.6f", gpsLat, gpsLng);
                      console.logf("ok=%lu bad=%lu fixsent=%lu", good, bad, fixSent);
                      return String("gps: printed to log");
                    });
#else
  console.onCommand("gps", "GPS status (no GPS module on this board)",
                    [](JsonVariantConst) -> String {
                      console.log("gps: no GPS module on this board");
                      return String("gps: no module");
                    });
#endif
#if USE_SD
  // Prints the header and last 10 rows, or the whole file with `log full` or
  // `--full`. The argument is Text so server-side enum validation accepts
  // `--full`.
  static const jelly::webconsole::Field logArgs[] = {
    { "mode", "mode", jelly::webconsole::FieldType::Text, nullptr, false },
  };
  jelly::webconsole::CommandOpts logOpts;
  logOpts.args     = logArgs;
  logOpts.argCount = 1;
  console.onCommand("log", "print the SD log – last 10 rows (`log full` for all)",
                    [](JsonVariantConst a) -> String {
                      String m = a["mode"] | "";
                      m.replace("-", "");
                      m.toLowerCase();
                      return dumpSdLog(m == "full" || m == "all");
                    }, logOpts);
#endif
  console.onCommand("inject", "(Detect only) inject a synthetic detection – no-op in Admin",
                    [](JsonVariantConst) -> String {
                      console.log("inject: no-op in Admin – scanning is paused. "
                                  "Return to scan, then use serial `inject`.");
                      return String("inject: no-op in Admin mode");
                    });
  console.onCommand("nav", "(Detect only) inject a screen-nav event – no-op in Admin",
                    [](JsonVariantConst) -> String {
                      console.log("nav: drives the on-device screen menu, which is paused in "
                                  "Admin. Use the buttons or serial `nav <dir>` in Detect.");
                      return String("nav: no-op in Admin mode");
                    });
  // -----------------------------------------------------------------------

#if USE_BUZZER
  // Replays the buzzer sounds on demand. The players block in delay(), which is
  // safe because WebConsole runs command handlers in loop() context.
  console.onCommand("chirp", "play the new-detection chirp",
                    [](JsonVariantConst) -> String {
                      corePlayDetectChirp();
                      return String("played detection chirp");
                    });
  console.onCommand("prox", "play the proximity chirp",
                    [](JsonVariantConst) -> String {
                      corePlayProximityChirp();
                      return String("played proximity chirp");
                    });
  console.onCommand("jingle", "play the boot sound",
                    [](JsonVariantConst) -> String {
                      corePlayStartupJingle();
                      return String("played boot sound");
                    });
  console.onCommand("crow", "play the crow call",
                    [](JsonVariantConst) -> String {
                      corePlayCrowCall();
                      return String("played crow call");
                    });
  console.onCommand("hawk", "play the hawk call",
                    [](JsonVariantConst) -> String {
                      corePlayHawkCall();
                      return String("played hawk call");
                    });
#endif

  // Pinned "help" button. Typed `help` is a client-side built-in that lists
  // every command, but the button goes to the server, so this handler prints
  // the reference into the log. Keep it in step with the commands above.
  jelly::webconsole::CommandOpts helpOpts;
  helpOpts.pinned = true;
  console.onCommand("help", "list the available console commands",
                    [](JsonVariantConst) -> String {
                      // One console.log() for the whole list, to stay under the
                      // WebSocket frame limit LogBatcher guards.
                      String h = "commands:\n";
                      h += "  status   – firmware version + scan state\n";
                      h += "  gps      – GPS fix, sats, position, counters\n";
                      h += "  wifi     – save the NTP-fallback WiFi network (SSID/pass)\n";
                      h += "  wifi-forget – erase the saved WiFi network\n";
                      h += "  scan     – leave Admin and resume detection\n";
                      h += "  dump     – current session JSON\n";
                      h += "  prev     – previous session JSON\n";
#if USE_SD
                      h += "  log      – SD log, last 10 rows (`log full` for all)\n";
#endif
                      h += "  inject   – (Detect only) synthetic detection\n";
                      h += "  nav      – (Detect only) screen-nav event\n";
#if USE_BUZZER
                      h += "  chirp / prox / jingle – buzzer tone tests\n";
                      h += "  crow / hawk – play either boot call\n";
#endif
                      h += "  clear    – wipe the log";
                      console.log(h);
                      return String("help: printed to log");
                    }, helpOpts);

  registered = true;
}

void webPortalStart(const char* apSsid, const char* apPassword) {
  if (portalActive) return;
  ensureRegistered();

  // Detect drives the WiFi driver raw through esp_wifi_*, with no esp_netif or
  // IP layer. Admin drives it through Arduino WiFi, whose WiFi.softAP inside
  // console.begin() owns esp_netif and DHCP. The two stacks never run at once.
  // coreRadioStop() tears the selected radio fully down first, because
  // WiFiGeneric's lazy init needs an uninitialized driver. On a live raw driver
  // the AP comes up with no netif or DHCP, and 10.99.7.1 is unreachable.
  coreRadioStop();

  WebConsole::Config cfg;
  cfg.apSsid     = apSsid;
  cfg.apPassword = apPassword;
  cfg.apIp       = IPAddress(10, 99, 7, 1);   // distinctive subnet, avoids LAN clashes
  cfg.deviceName = WEB_PORTAL_DEVICE_NAME;    // also the mDNS name
  cfg.fs         = &SPIFFS;
  console.begin(cfg);

  // The "Return to scan" page sets the same flag as the "scan" command. Routes
  // go back on at each start, because begin() rebuilds the server.
  console.server().on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    exitRequested = true; exitReqMs = millis();
    req->send(200, "text/html",
              console.pageShell("Return to scan",
                                "<section class=\"card\"><h2>Returning to detection…</h2>"
                                "<p>The access point is closing and scanning is resuming. "
                                "You can close this tab.</p></section>"));
  });

#if USE_SD
  // The SD listing and per-file download, also re-added at each start.
  console.server().on("/logs", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", console.pageShell("Logs", buildLogsBody()));
  });
  console.server().on("/dl", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("f")) { req->send(400, "text/plain", "missing f"); return; }
    String f = req->getParam("f")->value();
    if (!sdNameSafe(f)) { req->send(400, "text/plain", "bad name"); return; }
    String path = "/" + f;
    if (!SD.exists(path)) { req->send(404, "text/plain", "not found"); return; }
    const char* type = path.endsWith(".csv")  ? "text/csv"
                     : path.endsWith(".json") ? "application/json"
                     : "application/octet-stream";
    req->send(SD, path, type, true);   // sends Content-Disposition attachment
  });
#endif

  snprintf(status, sizeof(status), "browse to http://10.99.7.1/");
  exitRequested = false;
  portalStartMs = millis();
  lastClientMs  = portalStartMs;
  everHadClient = false;
  portalActive  = true;
  dualPrintf("[web_portal] started (WebConsole) - heap=%u largest_block=%u\n",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void webPortalStop() {
  if (!portalActive) return;
  console.stop();          // softAPdisconnect, WiFi.mode(WIFI_STA), server down

  // The reverse of the webPortalStart() handoff. console.stop() leaves Arduino
  // WiFi initialized in STA. WiFi.mode(WIFI_OFF) makes Arduino destroy its
  // netifs, call esp_wifi_deinit and reset its init flags, which leaves the
  // driver clean for coreRadioStart() and for the next webPortalStart().
  WiFi.mode(WIFI_OFF);
  coreRadioStart();

  portalActive  = false;
  exitRequested = false;
  dualPrintln("[web_portal] stopped - resumed Detect");
}

void webPortalTick() {
  console.tick();
  if (!portalActive) return;

  unsigned long now = millis();

  // Logs heap fragmentation and WebSocket client count to serial every 5 s
  // while the portal is up.
  static unsigned long lastHeapLogMs = 0;
  if (now - lastHeapLogMs > 5000) {
    lastHeapLogMs = now;
    dualPrintf("[web_portal] heap=%u largest_block=%u ws_clients=%u\n",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
               (unsigned)console.clientCount());
  }

  // Deferred release for "return to scan". The 300 ms wait lets the
  // confirmation response flush before teardown.
  if (exitRequested && now - exitReqMs > 300) { webPortalStop(); return; }

  // Idle timeouts, see WEB_PORTAL_NO_CLIENT_MS.
  if (console.clientCount() > 0) { everHadClient = true; lastClientMs = now; }
  bool neverConnected = !everHadClient && (now - portalStartMs > WEB_PORTAL_NO_CLIENT_MS);
  bool wentIdle       =  everHadClient && console.clientCount() == 0
                                       && (now - lastClientMs > WEB_PORTAL_IDLE_MS);
  if (neverConnected || wentIdle) {
    dualPrintln("[web_portal] idle timeout - resuming Detect");
    webPortalStop();
  }
}

bool webPortalActive() { return portalActive; }

IPAddress webPortalIp() { return console.ip(); }

const char* webPortalStatus() { return status; }
