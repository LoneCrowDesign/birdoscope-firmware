// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Shared main file for the sprite-graphics TFT boards. Detection logic and the
// LoRa, notification, and input peripherals live in lib/birdoscope_core, so
// this file covers display drawing plus setup() and loop() orchestration only.
// Another TFT board would reuse this file, differing only via board_config.h.
#include <Arduino.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include <math.h>
#include <SPIFFS.h>
#include <SD.h>
#include <TFT_eSPI.h>

// ============================================================
// CONFIG: pins, feature flags, tuning constants for this exact board.
// Selected per-env via the -I build flag in platformio.ini.
// ============================================================

#include "board_config.h"
#include "core.h"
#include "web_portal.h"

// ============================================================
// DISPLAY STATE
// ============================================================

static TFT_eSPI    tft;
static TFT_eSprite  spr = TFT_eSprite(&tft);   // full-screen 8bpp sprite, fits easily in SRAM (57.6KB)

// Board-local two-screen toggle (scan view / detection-count view). Prefixed
// to avoid clashing with core's shared ScreenId carousel (SCREEN_COUNT there is
// the enum cardinality). This TFT board keeps its two-screen toggle until it
// adopts the shared carousel. See docs/board_parity.md.
typedef enum { TFT_SCREEN_SCAN = 0, TFT_SCREEN_COUNT = 1 } TftScreen;
static TftScreen currentScreen = TFT_SCREEN_SCAN;

static char    dispMac[18]  = "--:--:--:--:--:--";
static char    dispOui[9]   = "-------";
static int8_t  dispRssi     = 0;
static uint8_t dispCh       = 0;
static float   dispDistM    = -1.0f;   // -1 = not estimable (e.g. addr1 hit)
static bool    dispDirty    = false;
static unsigned long dispLastRefresh = 0;
// Short interval, rather than event-driven alone, since the scan screen's red/black
// state has to expire on its own as HB_DEVICE_ACTIVE_MS elapses, not only
// on a new detection event.
#define DISPLAY_REFRESH_MS 500

// ============================================================
// DRAIN QUEUE: pops core's alert queue, calls coreHandleAlert() for the
// shareable table/SD/JSON/notification middle, then updates display state
// from the result.
// ============================================================

static void drainAlertQueue() {
  AlertEntry e;
  while (coreDequeueAlert(e)) {
    CoreAlertResult r = coreHandleAlert(e);
    if (r.suppressed) continue;   // rate-limited, display already reflects the active state

    strlcpy(dispMac, r.macStr, sizeof(dispMac));
    strlcpy(dispOui, r.oui,    sizeof(dispOui));
    dispRssi  = r.rssi;
    dispCh    = r.channel;
    dispDistM = r.distM;
    dispDirty = true;
  }
}

// ============================================================
// DISPLAY: GC9A01 240x240 round, full-screen sprite (double-buffered)
// ============================================================
//
// 240x240 @ 8bpp = 57.6KB, comfortably fits classic-ESP32 SRAM alongside
// the WiFi promiscuous driver. Sprite avoids visible tearing/flicker on
// every redraw vs. drawing straight to the panel.

#define DISP_CX 120
#define DISP_CY 120

// "Active" window for the scan screen's red/ring state. Reuses the same
// definition of "still in range" as core's fyLastTargetSeen update, so the
// visual signal always agrees with what coreHandleAlert() last processed.
static inline bool targetActive() {
  return fyLastTargetSeen != 0 &&
         (millis() - fyLastTargetSeen) <= HB_DEVICE_ACTIVE_MS;
}

// RSSI to angle on a 270° gauge swept from GAUGE_START. No bearing information
// is available (single omni antenna, no AoA hardware), so this maps signal
// strength to a position on the ring as a proximity indicator, not a true
// compass direction.
#define GAUGE_START 135.0f
#define GAUGE_SWEEP 270.0f

static float rssiToAngle(int8_t rssi) {
  int r = constrain((int)rssi, RSSI_MIN, RSSI_MAX);
  float t = (float)(r - RSSI_MIN) / (float)(RSSI_MAX - RSSI_MIN);
  return GAUGE_START + t * GAUGE_SWEEP;
}

static void xyFromAngle(float deg, int len, int& x, int& y) {
  float rad = deg * DEG_TO_RAD;
  x = DISP_CX + (int)(len * sinf(rad));
  y = DISP_CY - (int)(len * cosf(rad));
}

// Single chevron "bird", a wide-lined V. Shared by the idle scan screen's
// flock and the boot splash so they look like the same graphic. bg must match
// whatever fillSprite() color is currently behind it, because drawWideLine
// blends its anti-aliased edge against that color.
static void drawBird(int bx, int by, int wingSpan, uint16_t bg = TFT_BLACK) {
  spr.drawWideLine(bx - wingSpan, by, bx, by - wingSpan / 2, 3, TFT_WHITE, bg);
  spr.drawWideLine(bx, by - wingSpan / 2, bx + wingSpan, by, 3, TFT_WHITE, bg);
}

// Small procedural flock, a handful of birds scattered around center. Drawn
// fresh each frame, cheap enough not to need caching. Stays on screen during an
// active detection too (red background) so the flock does not disappear, with
// only the ring pointer added on top of it.
static void drawBirdFlock(uint16_t bg = TFT_BLACK) {
  static const int8_t offs[][3] = {   // {dx, dy, wingSpan}
    {  0, -14, 20 }, { -38,  8, 14 }, {  32,  18, 15 },
    { -14,  34, 12 }, {  20, -30, 11 },
  };
  for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
    drawBird(DISP_CX + offs[i][0], DISP_CY + offs[i][1], offs[i][2], bg);
  }
}

// Pointer marker on the ring at the edge of the screen, position driven by
// rssiToAngle(). As above, this is a proximity gauge, not a directional
// bearing.
static void drawRingPointer(int8_t rssi) {
  const int ringR = 104;
  float angle = rssiToAngle(rssi);

  spr.drawCircle(DISP_CX, DISP_CY, ringR, TFT_WHITE);

  int tipX, tipY, baseLX, baseLY, baseRX, baseRY;
  xyFromAngle(angle,        ringR + 14, tipX,  tipY);
  xyFromAngle(angle - 7.0f, ringR - 6,  baseLX, baseLY);
  xyFromAngle(angle + 7.0f, ringR - 6,  baseRX, baseRY);
  spr.fillTriangle(tipX, tipY, baseLX, baseLY, baseRX, baseRY, TFT_WHITE);
}

static void displayInit() {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);   // keep dark until init completes

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(8);
  spr.createSprite(240, 240);
  spr.setTextDatum(MC_DATUM);

  spr.fillSprite(TFT_BLACK);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);

  // Three small birds above the title. "Birdoscope Mini" is too wide for
  // the round bezel at one line, so it's split across two. Small top-left,
  // medium top-right, large in the middle (lower and front of the other two).
  drawBird(DISP_CX - 26, DISP_CY - 74, 7);
  drawBird(DISP_CX + 32, DISP_CY - 68, 10);
  drawBird(DISP_CX + 2,  DISP_CY - 56, 13);

  spr.setTextFont(4);
  spr.drawString("Birdoscope", DISP_CX, DISP_CY - 14);
  spr.drawString("Mini",       DISP_CX, DISP_CY + 12);
  spr.setTextFont(2);
  spr.drawString("starting...", DISP_CX, DISP_CY + 38);
  spr.pushSprite(0, 0);

  digitalWrite(TFT_BL, HIGH);
}

static void drawScanScreen() {
  bool active = targetActive();
  uint16_t bg = active ? TFT_RED : TFT_BLACK;
  spr.fillSprite(bg);

  drawBirdFlock(bg);   // stays centered whether idle (black) or detected (red)

  if (active) {
    drawRingPointer(dispRssi);
  }
}

static void drawCountScreen() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);

  spr.setTextSize(1);
  spr.setTextFont(2);
  spr.drawString("Flocks:", DISP_CX, DISP_CY - 80);

  spr.setTextFont(6);   // largest numeral-only font enabled in build_flags
  spr.setTextSize(2);   // doubled on top of font 6 for a bigger count
  char line[8];
  snprintf(line, sizeof(line), "%d", fyDetCount);
  spr.drawString(line, DISP_CX, DISP_CY + 20);
  spr.setTextSize(1);   // reset so other screens aren't affected next frame
}

// Admin (AP) screen, shown once when the web portal comes up. Static: the
// portal pauses scanning, so there's nothing to refresh until power-cycle.
static void displayAdmin() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  spr.setTextFont(4);
  spr.drawString("Admin", DISP_CX, DISP_CY - 50);
  spr.setTextFont(2);
  spr.drawString("AP: " WEB_PORTAL_AP_SSID, DISP_CX, DISP_CY - 12);
  spr.drawString("http://10.99.7.1",        DISP_CX, DISP_CY + 14);
  spr.drawString("2x BOOT or web to exit",  DISP_CX, DISP_CY + 46);
  spr.pushSprite(0, 0);
}

static void displayTick() {
  unsigned long now = millis();
  if (!dispDirty && (now - dispLastRefresh < DISPLAY_REFRESH_MS)) return;
  dispDirty = false;
  dispLastRefresh = now;

  if (currentScreen == TFT_SCREEN_COUNT) {
    drawCountScreen();
  } else {
    drawScanScreen();
  }

  spr.pushSprite(0, 0);
}

// Manual "area of interest" marker. Simulates a detection on the scan screen,
// using the same red background and ring pointer a real hit would trigger, and
// writes one MANUALALERT row to the SD log. It never touches the real detection
// table or SPIFFS, since no camera was seen and the count must not be inflated.
// The row carries only real data, the timestamp and the current channel.
// mac/ssid/ap_mac/dist_m stay blank rather than fabricated, and
// method="MANUALALERT" flags the row as operator-generated to anything parsing
// the log later.
static void triggerManualAlert() {
  fyLastTargetSeen = millis();   // drives the scan screen's red/ring window
  dispRssi  = RSSI_MAX;          // pointer parks at the gauge's near end
  dispDirty = true;

#if USE_SD
  sdAppendRow("", "MANUALALERT", "manual", 0, currentChannel, "", "", -1.0f);
#endif

  dualPrintln("[bscope] MANUAL ALERT logged (area of interest)");
}

// Switching screens only changes what is drawn. Scanning, logging, and
// persistence all keep running regardless of currentScreen.
static void checkInput() {
  InputEvent ev = coreInputTick();
  if (ev == INPUT_TOGGLE_SCREEN) {
    currentScreen = (currentScreen == TFT_SCREEN_SCAN) ? TFT_SCREEN_COUNT : TFT_SCREEN_SCAN;
    dispDirty = true;
  } else if (ev == INPUT_MANUAL_MARK) {
    triggerManualAlert();
  }
}

// ============================================================
// SERIAL COMMANDS: 'status' is board-specific, since it reports ntp_time=.
// 'log' is available wherever USE_SD is set. Core handles the shared verbs.
// ============================================================

static void printStatus() {
  unsigned long ms = millis();
  unsigned long s  = ms / 1000;
  dualPrintf("[bscope] status: uptime=%lus ch=%u mode=%s det=%d spiffs=%d"
             " heap=%u sniffing=%d ntp_time=%d\n",
             s, currentChannel, channelModeName(), fyDetCount,
             fySpiffsReady ? 1 : 0,
             (unsigned)ESP.getFreeHeap(),
             sniffingStopped ? 0 : 1,
             coreTimeAnchored() ? 1 : 0);
}

// Injects a synthetic addr2 detection through the same alert queue the real
// promiscuous callback uses, so the display, SD, and SPIFFS path can run
// without a live camera nearby. Cycles through a fixed RSSI sweep on each call,
// so successive calls walk the ring pointer around the gauge from RSSI_MIN to
// RSSI_MAX.
static void injectTestDetection() {
  static const int8_t sweep[] = { -30, -45, -60, -75, -95 };
  static size_t sweepIdx = 0;
  int8_t rssi = sweep[sweepIdx];
  sweepIdx = (sweepIdx + 1) % (sizeof(sweep) / sizeof(sweep[0]));

  uint8_t targetOui[3];
  coreGetFirstTargetOui(targetOui);
  uint8_t fakeMac[6] = { targetOui[0], targetOui[1], targetOui[2], 0xAA, 0xBB, 0xCC };
  enqueueAlert(ALERT_OUI_ADDR2, fakeMac, nullptr, rssi, currentChannel,
               nullptr, "test", "test_inject");
  dualPrintf("[bscope] injected test detection rssi=%d\n", (int)rssi);
}

#if USE_SD
static void dumpSdLog() {
  if (!fySDReady) { dualPrintln("[bscope] dump: SD not ready"); return; }
  if (sdLog) sdLog.flush();
  File f = SD.open(SD_LOG_FILE, FILE_READ);
  if (!f) { dualPrintf("[bscope] dump: cannot open %s\n", SD_LOG_FILE); return; }
  dualPrintf("[bscope] dump: %s (%u bytes)\n", SD_LOG_FILE, (unsigned)f.size());
  uint8_t buf[256];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, (size_t)n);
  Serial.write('\n');
  f.close();
}
#endif

static void printSerialHelp() {
  dualPrintln("[bscope] serial commands (word-based, newline-terminated):");
  dualPrintln("  status            print status");
  dualPrintln("  inject            inject test detection");
#if USE_SD
  dualPrintln("  log               dump SD log");
#endif
  corePrintSerialHelp();   // core-owned: dump/prev/nav (+ chirp/jingle on buzzer boards)
  dualPrintln("  help              this help (also '?')");
}

static void checkSerialCommands() {
  const char* verb;
  const char* arg;
  while (coreReadSerialCommand(&verb, &arg)) {
    if (coreHandleSerialCommand(verb, arg)) continue;
    if      (!strcmp(verb, "status")) printStatus();
    else if (!strcmp(verb, "inject")) injectTestDetection();
#if USE_SD
    else if (!strcmp(verb, "log")) dumpSdLog();
#endif
    else if (!strcmp(verb, "help") || !strcmp(verb, "?")) printSerialHelp();
    else if (verb[0]) dualPrintf("[bscope] unknown command: %s (try 'help')\n", verb);
  }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  displayInit();

  // SPIFFS first: coreTimeSync()'s NTP fallback reads the saved WiFi creds off
  // it when no GPS module is present. Format on first boot if missing.
  // Non-fatal.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[bscope] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[bscope] SPIFFS init FAILED – running without persistence");
  }

  // One-shot time anchor, before any promiscuous setup. coreTimeSync() probes
  // for a GPS module. Finding none, it joins the saved WiFi network, syncs over
  // NTP, then disconnects and leaves the WiFi driver initialized but stopped,
  // which the raw esp_wifi_* promiscuous setup later in this function accepts.
  // With no saved network it falls through to millis() without touching WiFi.
  coreTimeSync();

  precompileOuis();

#if USE_SD
  // micro SD shares the TFT's SPI bus and differs only in CS. It must reuse
  // TFT_eSPI's own SPIClass instance via getSPIinstance(), and must not call
  // SPI.begin() on the global `SPI` object. TFT_eSPI owns its own
  // private SPIClass (HSPI/VSPI), and a second begin() on a different
  // SPIClass with the same physical pins re-routes them via the GPIO matrix
  // to that second peripheral, silently disconnecting TFT_eSPI from the bus
  // (display freezes on whatever was last pushed). TFT_MISO is wired in
  // platformio.ini build_flags specifically so this shared instance already
  // has MISO when SD needs to read the card.
  if (SD.begin(SD_CS_PIN, tft.getSPIinstance())) {
    fySDReady = true;
    dualPrintln("[bscope] SD card ready");
    sdSetup();
    sdTryNameLog();
  } else {
    dualPrintln("[bscope] SD card not found – skipping");
  }
#endif

  // Raw-IDF promiscuous capture bring-up (Detect mode). Shared with
  // webPortalStop()'s resume path. See coreWifiSnifferStart() in core.
  coreWifiSnifferStart();

  dualPrintln("[bscope] esp32round WiFi detector started");
  dualPrintf("[bscope] mode=%s dwell_ms=%u start_channel=%u rssi_min=%d spiffs=%d\n",
                channelModeName(), CHANNEL_DWELL_MS, currentChannel,
                RSSI_MIN, fySpiffsReady ? 1 : 0);
}

void loop() {
  static bool wasAdmin = false;
  // Admin (AP) mode: service only the portal. It releases back to Detect on a
  // web "return to scan" command or an idle timeout (webPortalTick may end it),
  // so this is not a latch. Detect and Admin can alternate freely.
  if (webPortalActive()) {
    webPortalTick();                                             // may release (web / idle timeout)
    if (webPortalActive() && coreAdminTriggerCheck()) webPortalStop();   // BOOT double-press also exits
    wasAdmin = true;
    delay(2);
    return;
  }
  if (wasAdmin) { wasAdmin = false; dispDirty = true; }   // resumed, so force a redraw

  // Anytime BOOT double-press enters Admin. This board has no dedicated Admin
  // gesture, so BOOT serves that role.
  if (coreAdminTriggerCheck()) {
    webPortalStart(WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_PASSWORD);
    displayAdmin();
    return;
  }

  coreTick();
  updateChannelMode();
  checkSerialCommands();
  checkInput();
  drainAlertQueue();
  displayTick();
  autosaveTick();
  printHeartbeat();
  delay(1);
}
