// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Untethered "Admin" web portal. See web_portal.h for the interface contract.
//
// Backed by WebConsole, an async, schema-driven console pulled in as a
// lib_deps git dependency. This file declares what Birdoscope exposes and
// keeps the stable webPortalStart/Stop/Tick interface so the board main files
// are unchanged:
//   • session    → the SPIFFS session JSON (and the promoted previous session),
//                  downloadable on every board.
//   • logs       → on SD boards, an escape-hatch page listing every CSV on the
//                  card, each with a download link. The live capture logs are
//                  GPS-named, and /log.csv is only the pre-anchor buffer, so a
//                  single addFile() cannot reach them but a directory listing
//                  can.
//   • status     → a console command that prints firmware version + scan state
//                  into the log stream.
//
// SoftAP mode competes for the same radio as the promiscuous sniffer, so the
// caller pauses detection while the portal is up (see web_portal.h). This file
// owns only the server. The promiscuous and SoftAP radio handoff lives in the
// core and board code that calls webPortalStart() and webPortalStop().
#include "web_portal.h"
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

// mDNS / device name (SSID + password live in web_portal.h so the boards can
// pass them to webPortalStart()). board_config.h may override.
#ifndef WEB_PORTAL_DEVICE_NAME
#define WEB_PORTAL_DEVICE_NAME  "birdoscope"   // also mDNS: birdoscope.local
#endif

// Idle auto-resume: Admin releases back to Detect on its own so an accidental
// BOOT double-press in the field can't silently pause scanning. Two windows:
// nobody ever connected within _NO_CLIENT_MS of entry, or the last client left
// more than _IDLE_MS ago. board_config.h may override.
#ifndef WEB_PORTAL_NO_CLIENT_MS
#define WEB_PORTAL_NO_CLIENT_MS 180000UL  // 3min to join the AP + connect before we give up
#endif
#ifndef WEB_PORTAL_IDLE_MS
#define WEB_PORTAL_IDLE_MS      60000UL   // 60s after the last client disconnects
#endif

static WebConsole console;
static bool          portalActive = false;
static bool          registered   = false;
static char          status[96]   = "browse to configure";
// Runtime release state (reset each webPortalStart):
static volatile bool exitRequested = false;   // set by the web "return to scan" action
static unsigned long exitReqMs     = 0;        // when it was requested (grace for the response to flush)
static unsigned long portalStartMs = 0;        // for the never-connected timeout
static unsigned long lastClientMs  = 0;        // last time a client was seen
static bool          everHadClient = false;

// "status" command. Prints firmware version and current scan state into the
// console log, the "web serial" stream, so the running state is readable
// without opening a page. Composed from core externs, since printStatus() is
// static to each board main and unreachable here. Mirrors the serial `status`
// verb.
static String reportStatus() {
  unsigned long s = millis() / 1000;
  console.logf("Birdoscope v%s", BIRDOSCOPE_VERSION);
  console.logf("uptime=%lus ch=%u mode=%s det=%d spiffs=%d sniffing=%d",
               s, (unsigned)currentChannel, channelModeName(), fyDetCount,
               fySpiffsReady ? 1 : 0, sniffingStopped ? 0 : 1);
  console.logf("heap=%u min_free=%u largest_block=%u",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
               (unsigned)ESP.getMaxAllocHeap());
  return String("Birdoscope v") + BIRDOSCOPE_VERSION + " – status printed to log";
}

// Batches log lines into ~900-byte console.log() calls. Streaming a whole log
// at one WebSocket frame per line overran the socket and dropped the client,
// which surfaced as disconnect and reconnect churn after a bulk `log`.
// Batching keeps it to a handful of frames.
struct LogBatcher {
  String buf;
  void add(const String& s) {
    if (buf.length() + s.length() + 1 > 900) flush();
    if (buf.length()) buf += '\n';
    buf += s;
  }
  void flush() { if (buf.length()) { console.log(buf); buf = String(); } }
};

// Streams a whole file into the console log, capped and batched. This is the
// web equivalent of the serial dump and prev commands. Past the cap it stops
// and points at the download button.
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
// Prints the SD detection log. Default (full=false) prints the CSV header + only
// the last 10 rows, held in a RAM-safe rolling window so a log far larger than
// RAM never loads at once (and only ~11 batched lines hit the WebSocket).
// full=true streams the whole file, batched and opt-in, and may reconnect on a
// very large log. Always draws from the currently active log (`sdLog`), which
// is /log.csv until GPS anchors a timestamped name and then follows the
// rename.
static const int WEB_LOG_TAIL = 10;

static String dumpSdLog(bool full) {
  if (!fySDReady) { console.log("log: SD not ready"); return String("log: SD not ready"); }
  sdLog.flush();
  String path = sdLog.path();
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
  // Tail: header + a rolling window of the last WEB_LOG_TAIL rows.
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
// A filename is safe to serve from SD root only if it is a bare name, with no
// path separators, no "..", and no leading slash. This is an open AP, so the
// guard matters.
static bool sdNameSafe(const String& f) {
  if (f.length() == 0 || f.length() > 40) return false;
  if (f.indexOf('/') >= 0 || f.indexOf('\\') >= 0) return false;
  if (f.indexOf("..") >= 0) return false;
  return true;
}

// Escape-hatch page: list every .csv on the SD card with a download link. The
// live capture logs are GPS-named (see core.cpp sdTryNameLog), so the schema's
// single-fs addFile() cannot reach them. This raw route walks the card
// instead.
static String buildLogsBody() {
  String p;
  p.reserve(1024);
  p += "<section class=\"card\"><h2>SD card logs</h2>";
  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    p += "<p>SD card not available.</p></section>";
    return p;
  }
  p += "<ul>";
  bool any = false;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.endsWith(".csv")) {
      any = true;
      p += "<li><a href=\"/dl?f=";
      p += name;
      p += "\">";
      p += name;
      p += "</a> (";
      p += String((unsigned long)f.size());
      p += " bytes)</li>";
    }
    f.close();
  }
  root.close();
  if (!any) p += "<li><i>no CSV logs on card yet</i></li>";
  p += "</ul></section>";
  return p;
}
#endif  // USE_SD

// One-time declaration of everything the console exposes. Registration must
// happen before begin() and persists across start/stop cycles, so it runs
// once. begin() and stop() then toggle the server.
static void ensureRegistered() {
  if (registered) return;

  // The SPIFFS session (upstream flock-you persistence), download only.
  console.addFile("session", FY_SESSION_FILE, "application/json", false);
  console.addFile("prev_session", FY_PREV_FILE, "application/json", false);

#if USE_SD
  console.addPage("Logs", "/logs");   // top-bar button → the SD CSV listing
#endif
  console.addPage("Return to scan", "/scan");   // top-bar button → leave Admin

  // Pinned so it stays a one-click button in the Controls card. Unpinned
  // commands are typed verbs only, and printing status to the log is the most
  // reached-for diagnostic. No args and non-destructive, so no confirm.
  jelly::webconsole::CommandOpts statusOpts;
  statusOpts.pinned = true;
  console.onCommand("status", "print firmware version + scan state to the log",
                    [](JsonVariantConst) -> String { return reportStatus(); },
                    statusOpts);

  // Release the AP and resume Detect. This only flags the request. The actual
  // teardown runs in webPortalTick(), in loop context, after this response has
  // flushed, so the server is never torn down inside its own handler. Left as a
  // typed verb rather than pinned, because the "Return to scan" top-bar page
  // below already surfaces the action as a button.
  console.onCommand("scan", "leave Admin and resume detection",
                    [](JsonVariantConst) -> String {
                      exitRequested = true; exitReqMs = millis();
                      return String("returning to detection…");
                    });

  // WiFi station credentials for the boot-time NTP fallback (used only when no
  // GPS module is detected). Pinned so it's a one-click form in the Controls
  // card, since the point is to set these without a serial cable. Persisted to
  // SPIFFS by core and consumed at the next boot. WebConsole has no password
  // field type, so `pass` renders as plain text, which is acceptable on this
  // local AP. The stored password is never echoed back, only the SSID.
  // Submitting an empty SSID reports the currently-saved network.
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
                      if (ssid.length() == 0) {          // no SSID entered → report current
                        String cur, cpass;
                        if (coreWifiCredsLoad(cur, cpass))
                          return String("saved network: ") + cur + " (enter an SSID to change)";
                        return String("no network saved – enter an SSID + password to set one");
                      }
                      if (coreWifiCredsSave(ssid.c_str(), pass.c_str()))
                        return String("saved \"") + ssid + "\" – used for NTP at next boot when GPS is absent";
                      return String("save failed – SPIFFS not ready");
                    }, wifiOpts);

  jelly::webconsole::CommandOpts wifiForgetOpts;
  wifiForgetOpts.confirm = true;   // destructive-ish: wipes the stored network
  console.onCommand("wifi-forget", "erase the saved WiFi network",
                    [](JsonVariantConst) -> String {
                      coreWifiCredsClear();
                      return String("saved WiFi network erased");
                    }, wifiForgetOpts);

  // --- Serial-console parity ---------------------------------------------
  // The same verbs the UART serial console exposes, so the web console is a
  // full stand-in and `help` lists everything. dump, prev, and log stream a
  // file into the log, capped like the serial dumps. inject and nav are
  // Detect-loop actions that no-op in Admin, where scanning is paused, and say
  // so. Typed verbs rather than pinned, keeping the Controls card to the
  // one-click diagnostics.
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
  // Default: header + last 10 rows (RAM-safe, batched). `log full` (or --full)
  // streams the whole file. Text arg (not Enum) so the bare `--full` a user
  // reflexively types isn't rejected by server-side enum validation.
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
  // Replay the buzzer sounds on demand, without waiting for a real detection.
  // The players block via delay(), but WebConsole runs command handlers in
  // loop() context, so that is safe. Left unpinned, since these are occasional
  // and do not earn a button.
  console.onCommand("chirp", "play the new-detection chirp",
                    [](JsonVariantConst) -> String {
                      corePlayDetectChirp();
                      return String("played detection chirp");
                    });
  console.onCommand("jingle", "play the boot jingle",
                    [](JsonVariantConst) -> String {
                      corePlayStartupJingle();
                      return String("played boot jingle");
                    });
#endif

  // Pinned "help" button in the Controls card. Typing `help` is a client-side
  // built-in that lists every command from the manifest. The pinned button
  // routes to the server, so this handler prints the same reference into the
  // log.
  jelly::webconsole::CommandOpts helpOpts;
  helpOpts.pinned = true;
  console.onCommand("help", "list the available console commands",
                    [](JsonVariantConst) -> String {
                      // One console.log() rather than a line per command, so the
                      // button does not reintroduce the WebSocket flooding that
                      // batching exists to prevent.
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
                      h += "  chirp / jingle – buzzer tone tests\n";
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

  // Radio handoff via full stack separation, not coexistence. Detect drives the
  // WiFi driver in raw promiscuous mode, using esp_wifi_* with no esp_netif or
  // IP layer. Admin drives it through Arduino WiFi, via WiFi.softAP inside
  // console.begin, which owns esp_netif and DHCP. These are different modes, so
  // the switch is hard: tear the raw driver fully down here, then let Arduino
  // bring it back up from a clean, uninitialized state, which is what
  // WiFiGeneric's lazy init expects. If Arduino instead inits on top of a live
  // raw driver it does not own, the AP radio comes up with no netif or DHCP
  // attached, leaving the SSID visible but 10.99.7.1 unreachable. The two
  // stacks never run at once. esp_wifi_* comes from core.h.
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  esp_wifi_stop();
  esp_wifi_deinit();

  WebConsole::Config cfg;
  cfg.apSsid     = apSsid;
  cfg.apPassword = apPassword;
  cfg.apIp       = IPAddress(10, 99, 7, 1);   // distinctive subnet, avoids LAN clashes
  cfg.deviceName = WEB_PORTAL_DEVICE_NAME;    // also mDNS: birdoscope.local
  cfg.fs         = &SPIFFS;
  console.begin(cfg);

  // "Return to scan" top-bar button. Flags the release, with teardown deferred
  // to webPortalTick(), as in the "scan" command. Re-added each start, because
  // begin() rebuilds the server.
  console.server().on("/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
    exitRequested = true; exitReqMs = millis();
    req->send(200, "text/html",
              console.pageShell("Return to scan",
                                "<section class=\"card\"><h2>Returning to detection…</h2>"
                                "<p>The access point is closing and scanning is resuming. "
                                "You can close this tab.</p></section>"));
  });

#if USE_SD
  // Escape hatch: SD-card CSV listing + per-file download. Re-added each start
  // because begin() rebuilds the server.
  console.server().on("/logs", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->send(200, "text/html", console.pageShell("Logs", buildLogsBody()));
  });
  console.server().on("/dl", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!req->hasParam("f")) { req->send(400, "text/plain", "missing f"); return; }
    String f = req->getParam("f")->value();
    if (!sdNameSafe(f)) { req->send(400, "text/plain", "bad name"); return; }
    String path = "/" + f;
    if (!SD.exists(path)) { req->send(404, "text/plain", "not found"); return; }
    req->send(SD, path, "text/csv", true);   // download (Content-Disposition: attachment)
  });
#endif

  snprintf(status, sizeof(status), "browse to http://10.99.7.1/");
  exitRequested = false;
  portalStartMs = millis();
  lastClientMs  = portalStartMs;
  everHadClient = false;
  portalActive  = true;
  dualPrintf("[web_portal] started (WebConsole) – heap=%u largest_block=%u\n",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void webPortalStop() {
  if (!portalActive) return;
  console.stop();          // Arduino: softAPdisconnect + WiFi.mode(WIFI_STA), server torn down

  // Full stack handoff back to Detect, mirroring webPortalStart().
  // console.stop() leaves Arduino WiFi initialized in STA. WiFi.mode(WIFI_OFF)
  // makes Arduino fully deinit itself, destroying its netifs, calling
  // esp_wifi_deinit, and resetting its internal init flags, so the driver is
  // clean. coreWifiSnifferStart() then re-inits raw promiscuous from that clean
  // state. Because Arduino reset its own flags, the next webPortalStart re-inits
  // cleanly too, which is what makes repeated Detect and Admin hops safe.
  WiFi.mode(WIFI_OFF);
  coreWifiSnifferStart();   // clears sniffingStopped, Detect resumes

  portalActive  = false;
  exitRequested = false;
  dualPrintln("[web_portal] stopped – resumed Detect");
}

void webPortalTick() {
  console.tick();
  if (!portalActive) return;

  unsigned long now = millis();

  // Diagnostic: watch heap fragmentation and WebSocket client count while the
  // portal is up. Serial output only, since serial input is paused in Admin.
  // Every 5s.
  static unsigned long lastHeapLogMs = 0;
  if (now - lastHeapLogMs > 5000) {
    lastHeapLogMs = now;
    dualPrintf("[web_portal] heap=%u largest_block=%u ws_clients=%u\n",
               (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
               (unsigned)console.clientCount());
  }

  // Deferred release for the web "return to scan" action. Waits a beat after
  // the request so the confirmation response flushes before teardown.
  if (exitRequested && now - exitReqMs > 300) { webPortalStop(); return; }

  // Idle auto-resume so an accidental entry self-heals.
  if (console.clientCount() > 0) { everHadClient = true; lastClientMs = now; }
  bool neverConnected = !everHadClient && (now - portalStartMs > WEB_PORTAL_NO_CLIENT_MS);
  bool wentIdle       =  everHadClient && console.clientCount() == 0
                                       && (now - lastClientMs > WEB_PORTAL_IDLE_MS);
  if (neverConnected || wentIdle) {
    dualPrintln("[web_portal] idle timeout – resuming Detect");
    webPortalStop();
  }
}

bool webPortalActive() { return portalActive; }

IPAddress webPortalIp() { return console.ip(); }

const char* webPortalStatus() { return status; }
