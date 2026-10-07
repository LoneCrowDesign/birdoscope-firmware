// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Main file for the sprite-graphics TFT boards. Detection logic and the
// notification and input peripherals live in lib/birdoscope_core, so this file
// covers display drawing plus setup() and loop() orchestration only.
// Another TFT board reuses this file and differs only in its board_config.h.
#include <Arduino.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include <math.h>
#include <SPIFFS.h>
#include <SD.h>
#include <TFT_eSPI.h>

// ============================================================
// CONFIG
// Pins, feature flags and tuning constants for this exact board. The -I build
// flag in platformio.ini selects the board_config.h for each env.
// ============================================================

#include "board_config.h"
#include "core.h"
#include "roost_session.h"
#include "web_portal.h"

// ============================================================
// DISPLAY STATE
// ============================================================

static TFT_eSPI    tft;
static TFT_eSprite  spr = TFT_eSprite(&tft);   // full-screen 8bpp sprite, fits easily in SRAM (57.6KB)

// Board-local two-screen toggle (scan view / detection-count view). The TFT_
// prefix avoids clashing with core's shared ScreenId carousel, where
// SCREEN_COUNT is the enum cardinality. See docs/board_parity.md.
typedef enum { TFT_SCREEN_SCAN = 0, TFT_SCREEN_COUNT = 1 } TftScreen;
static TftScreen currentScreen = TFT_SCREEN_SCAN;

static char    dispMac[18]  = "--:--:--:--:--:--";
static char    dispOui[9]   = "-------";
static int8_t  dispRssi     = 0;
static uint8_t dispCh       = 0;
static float   dispDistM    = -1.0f;   // -1 = not estimable (e.g. addr1 hit)
static bool    dispDirty    = false;
static unsigned long dispLastRefresh = 0;
// The scan screen's red/black state expires as HB_DEVICE_ACTIVE_MS elapses,
// with no event to trigger the redraw, so the display also refreshes on a
// short interval.
#define DISPLAY_REFRESH_MS 500

// ============================================================
// DRAIN QUEUE
// Pops core's alert queue, hands each entry to coreHandleAlert() for the table,
// SD, JSON and notification work, then updates the display state from the
// result.
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
// DISPLAY
// GC9A01 240x240 round, full-screen sprite (double-buffered).
// ============================================================
//
// 240x240 @ 8bpp = 57.6KB, which fits classic-ESP32 SRAM alongside the WiFi
// promiscuous driver. The sprite keeps redraws free of tearing and flicker.

#define DISP_CX 120
#define DISP_CY 120

// "Active" window for the scan screen's red/ring state. A target counts as
// active for HB_DEVICE_ACTIVE_MS after coreHandleAlert() or
// triggerManualAlert() last stamped fyLastTargetSeen.
static inline bool targetActive() {
  return fyLastTargetSeen != 0 &&
         (millis() - fyLastTargetSeen) <= HB_DEVICE_ACTIVE_MS;
}

// RSSI to angle on a 270° gauge swept from GAUGE_START. The hardware has a
// single omni antenna and no AoA, so the ring position indicates proximity
// and carries no bearing.
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

// Single chevron "bird", a wide-lined V. The scan screen's flock and the boot
// splash both draw it. bg must match the fillSprite() color behind it, because
// drawWideLine blends its anti-aliased edge against that color.
static void drawBird(int bx, int by, int wingSpan, uint16_t bg = TFT_BLACK) {
  spr.drawWideLine(bx - wingSpan, by, bx, by - wingSpan / 2, 3, TFT_WHITE, bg);
  spr.drawWideLine(bx, by - wingSpan / 2, bx + wingSpan, by, 3, TFT_WHITE, bg);
}

// Small procedural flock, a handful of birds scattered around center.
// drawScanScreen() redraws it every frame, idle or during an active detection
// (red background), and adds only the ring pointer on top.
static void drawBirdFlock(uint16_t bg = TFT_BLACK) {
  static const int8_t offs[][3] = {   // {dx, dy, wingSpan}
    {  0, -14, 20 }, { -38,  8, 14 }, {  32,  18, 15 },
    { -14,  34, 12 }, {  20, -30, 11 },
  };
  for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
    drawBird(DISP_CX + offs[i][0], DISP_CY + offs[i][1], offs[i][2], bg);
  }
}

// Pointer marker on the ring at the edge of the screen, positioned by
// rssiToAngle().
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

  // Three small birds above the title. "Birdoscope Mini" is too wide for the
  // round bezel on one line, so it takes two. Small top-left, medium
  // top-right, large in the middle (lower and in front of the other two).
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

  drawBirdFlock(bg);   // centered, idle (black) or detected (red)

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

// Admin (AP) screen, drawn once when the web portal comes up. The portal
// pauses scanning, so nothing refreshes it until the portal stops.
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

// Manual "area of interest" marker. Shows the red background and ring pointer
// a real hit would, and writes one operator_mark row to the SD log. It leaves
// the detection table and SPIFFS alone, so the detection count stays a count
// of cameras. The row holds only the timestamp and current channel, and leaves
// mac/ssid/ap_mac/dist_m blank.
//
// This logs the mark before it opens the survey window, per spec O1.
static void triggerManualAlert() {
  fyLastTargetSeen = millis();   // drives the scan screen's red/ring window
  dispRssi  = RSSI_MAX;          // pointer parks at the gauge's near end
  dispDirty = true;

#if USE_SD
  roostLogOperatorMark();
#endif

  dualPrintln("[bscope] MANUAL ALERT logged (area of interest)");
  coreSurveyStart();
}

// Switching screens changes only the drawing. Scanning, logging, and
// persistence keep running on either screen.
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
// SERIAL COMMANDS
// 'status' is board-specific, since it reports ntp_time=. 'log' exists on
// every USE_SD build. Core handles the shared verbs.
// ============================================================

static void printStatus() {
  unsigned long ms = millis();
  unsigned long s  = ms / 1000;
  dualPrintf("[bscope] status: uptime=%lus ch=%u mode=%s det=%d spiffs=%d"
             " heap=%u sniffing=%d ntp_time=%d survey=%us\n",
             s, currentChannel, channelModeName(), fyDetCount,
             fySpiffsReady ? 1 : 0,
             (unsigned)ESP.getFreeHeap(),
             sniffingStopped ? 0 : 1,
             coreTimeAnchored() ? 1 : 0,
             (unsigned)(coreSurveyRemainingMs() / 1000));   // 0 when no window is open
}

// Injects a synthetic addr2 detection through the same alert queue the real
// promiscuous callback uses, so the display, SD, and SPIFFS path can run
// without a live camera nearby. Successive calls walk the ring pointer around
// the gauge from RSSI_MAX down to RSSI_MIN.
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
  if (!roostSessionOpen()) { dualPrintln("[bscope] dump: no session open"); return; }
  char name[48], path[96];
  roostFileName(name, sizeof(name), ROOST_REC_WIFI_OBS);
  snprintf(path, sizeof(path), "%s/%s", roostSessionDir(), name);
  File f = SD.open(path, FILE_READ);
  if (!f) { dualPrintf("[bscope] dump: cannot open %s\n", path); return; }
  dualPrintf("[bscope] dump: %s (%u bytes)\n", path, (unsigned)f.size());
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
  corePrintSerialHelp();   // core's shared verbs
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

  // SPIFFS first, since coreTimeSync()'s NTP join reads the saved WiFi
  // credentials off it. SPIFFS formats on first boot if missing, and a failure
  // is non-fatal.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[bscope] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[bscope] SPIFFS init FAILED - running without persistence");
  }

  // One-shot time anchor, before any promiscuous setup. coreTimeSync() probes
  // for a GPS module, then bridges the time until GPS locks. It joins the saved
  // WiFi network, syncs over NTP, then disconnects and leaves the WiFi driver
  // initialized but stopped, which the raw esp_wifi_* promiscuous setup later
  // in this function accepts. With no saved network it falls through to
  // millis() without touching WiFi.
  coreTimeSync();

  precompileOuis();

#if USE_SD
  // micro SD shares the TFT's SPI bus and differs only in CS. It must reuse
  // TFT_eSPI's private SPIClass (HSPI/VSPI) via getSPIinstance() and never call
  // SPI.begin() on the global `SPI` object. A second begin() on another
  // SPIClass with the same pins makes the GPIO matrix route them to that
  // peripheral, which disconnects TFT_eSPI from the bus. platformio.ini
  // build_flags define TFT_MISO so this shared instance has MISO for SD reads.
  if (SD.begin(SD_CS_PIN, tft.getSPIinstance(), 4000000, "/sd", SD_MAX_OPEN_FILES)) {
    fySDReady = true;
    dualPrintln("[bscope] SD card ready");
    roostSessionBegin();
    roostSessionAnchor();
  } else {
    dualPrintln("[bscope] SD card not found - skipping");
  }
#endif

  // Raw-IDF promiscuous capture bring-up (Detect mode). webPortalStop() reruns
  // it through coreRadioStart() on resume.
  coreWifiSnifferStart();

  dualPrintln("[bscope] esp32round WiFi detector started");
  dualPrintf("[bscope] mode=%s dwell_ms=%u start_channel=%u rssi_min=%d spiffs=%d\n",
                channelModeName(), CHANNEL_DWELL_MS, currentChannel,
                RSSI_MIN, fySpiffsReady ? 1 : 0);
}

void loop() {
  static bool wasAdmin = false;
  // Admin (AP) mode services only the portal. A web "return to scan" command
  // or an idle timeout in webPortalTick() releases it back to Detect, so Detect
  // and Admin can alternate freely.
  if (webPortalActive()) {
    webPortalTick();                                             // may release (web / idle timeout)
    if (webPortalActive() && coreAdminTriggerCheck()) webPortalStop();   // BOOT double-press also exits
    wasAdmin = true;
    delay(2);
    return;
  }
  if (wasAdmin) { wasAdmin = false; dispDirty = true; }   // resumed, so force a redraw

  // A BOOT double press enters Admin from any screen. This board has no other
  // Admin gesture.
  if (coreAdminTriggerCheck()) {
    webPortalStart(WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_PASSWORD);
    displayAdmin();
    return;
  }

  coreTick();
  updateChannelMode();
  checkSerialCommands();
  checkInput();
  coreSurveyTick();     // closes an operator survey window once it has elapsed
  drainAlertQueue();
  displayTick();
  autosaveTick();
  printHeartbeat();
  delay(1);
}
