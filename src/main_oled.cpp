// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Shared main file for the u8g2 status-line OLED boards. Detection logic lives
// in lib/birdoscope_core, so this file covers OLED drawing plus setup() and
// loop() orchestration only. The one per-board difference here is the u8g2
// constructor, selected by OLED_HW_I2C in board_config.h.
#include <Arduino.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include <SPIFFS.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>

// ============================================================
// CONFIG: pins, feature flags, tuning constants for specific hardware.
// Selected per-env via the -I build flag in platformio.ini.
// ============================================================

#include "board_config.h"
#include "core.h"
#include "web_portal.h"

static void stopSniffing(const char* reason) {
  if (sniffingStopped) return;
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  dualPrintf("[bscope] sniffing stopped: %s\n", reason);
}

// ============================================================
// DISPLAY STATE
// ============================================================

// OLED_ROTATION is the u8g2 rotation constant passed to the constructor. It
// defaults to U8G2_R0 (native). A board whose panel is mounted upside-down
// sets U8G2_R2 (180°) in its board_config.h.
#ifndef OLED_ROTATION
#define OLED_ROTATION U8G2_R0
#endif

// OLED_HW_I2C selects between two u8g2 constructors:
//   0  software-bitbanged I2C with explicit SDA/SCL/RST pins, for a board
//      with no hardware I2C bus free
//   1  hardware I2C, with pins set via Wire.begin() in displayInit() rather
//      than the constructor
// OLED_SH1106 (HW-I2C path only) picks the SH1106 controller instead of the
// SSD1306. The SH1106 has 132 columns of RAM behind a 128px panel, so its
// u8g2 constructor applies a 2px column offset the SSD1306 one does not.
// Using the wrong one shows a 2px horizontal shift and edge-column garbage.
#if OLED_HW_I2C
#include <Wire.h>
#if defined(OLED_SH1106)
static U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#else
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(OLED_ROTATION, U8X8_PIN_NONE);
#endif
#else
static U8G2_SSD1315_128X64_NONAME_F_SW_I2C u8g2(OLED_ROTATION, OLED_SCL, OLED_SDA, OLED_RST);
#endif

static char   dispMac[18]  = "--:--:--:--:--:--";
static char   dispOui[9]   = "-------";
static int8_t dispRssi     = 0;
static uint8_t dispCh      = 0;
static bool   dispDirty    = false;
static unsigned long dispLastRefresh = 0;
#define DISPLAY_REFRESH_MS 2000

// ============================================================
// DRAIN QUEUE: pops core's alert queue, calls coreHandleAlert() for the
// shareable table/SD/JSON/notification middle, then updates display state
// and handles the (rare) stop-on-hit option.
// ============================================================

static void drainAlertQueue() {
  AlertEntry e;
  while (coreDequeueAlert(e)) {
    CoreAlertResult r = coreHandleAlert(e);
    if (r.suppressed) continue;   // rate-limited, no display update

    strlcpy(dispMac, r.macStr, sizeof(dispMac));
    strlcpy(dispOui, r.oui,    sizeof(dispOui));
    dispRssi  = r.rssi;
    dispCh    = r.channel;
    dispDirty = true;

#if STOP_ON_OUI_HIT
    if (r.type != ALERT_SSID) stopSniffing("OUI hit");
#endif
#if STOP_ON_SSID_HIT
    if (r.type == ALERT_SSID) stopSniffing("SSID hit");
#endif
  }
}

// ============================================================
// DISPLAY
// ============================================================

static void displayInit() {
#if !OLED_HW_I2C
  // Power the OLED rail. Vext is active LOW.
  pinMode(VEXT_CTRL, OUTPUT);
  digitalWrite(VEXT_CTRL, LOW);
  delay(50);

  // Hard-reset the SSD1315 controller before init
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(50);
  digitalWrite(OLED_RST, HIGH);
  delay(50);
#else
  Wire.begin(OLED_SDA, OLED_SCL);
#endif

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "birdoscope");
  u8g2.drawStr(0, 28, "starting...");
  u8g2.sendBuffer();
}

// Two-line centred-ish message (boot-time status, e.g. SD-not-found notice).
static void displayMessage(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 26, line1);
  u8g2.drawStr(0, 42, line2);
  u8g2.sendBuffer();
}

#if NAV_SCHEME_3BTN
// ============================================================
// SCREEN CAROUSEL: one draw function per ScreenId (core owns the state, this
// board owns the pixels). Content rows sit below a shared title/index header.
// Menu screens (SCAN MODE / ALERTS / WEB CONFIG) drill in via SELECT/BACK. See
// docs/menu_ux.md.
// ============================================================

#define ROW1 24
#define ROW2 36
#define ROW3 48
#define ROW4 60

// Brief confirmation overlay shown when a manual mark is logged. While
// markOverlayUntil is in the future, displayTick() holds this frame instead of
// redrawing the current screen. It clears itself when the timer expires.
static unsigned long markOverlayUntil = 0;
#define MARK_OVERLAY_MS 1500

static void drawMarkOverlay() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawFrame(2, 18, 124, 30);
  u8g2.drawStr(12, 32, "Saved Manual");
  u8g2.drawStr(12, 44, "Record!");
  u8g2.sendBuffer();
}

static void drawScreenHeader(const char* title) {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 8, title);
  char idx[8];
  snprintf(idx, sizeof(idx), "%d/%d", (int)coreCurrentScreen + 1, (int)SCREEN_COUNT);
  u8g2.drawStr(128 - u8g2.getStrWidth(idx), 8, idx);
  u8g2.drawHLine(0, 11, 128);
}

static void drawOverview() {
  char line[22];
  drawScreenHeader("OVERVIEW");
#if HAS_GPS
  // Compact fix flag (Y/N). Full GPS detail lives on the GPS screen, and Y/N keeps
  // the line within 128px even at max det/channel. `ch` is left-justified to
  // width 2 (channels are 1–13) so a single→double digit hop pads with a space
  // instead of shoving `gps:` right as the channel scans.
  snprintf(line, sizeof(line), "det:%-3d ch:%-2u gps:%c",
           fyDetCount, currentChannel, gpsHasFix ? 'Y' : 'N');
#else
  snprintf(line, sizeof(line), "det:%-3d ch:%u", fyDetCount, currentChannel);
#endif
  u8g2.drawStr(0, ROW1, line);
  if (fyDetCount > 0) {
    snprintf(line, sizeof(line), "last oui:%s", dispOui);
    u8g2.drawStr(0, ROW2, line);
    snprintf(line, sizeof(line), "rssi:%d ch:%u", (int)dispRssi, dispCh);
    u8g2.drawStr(0, ROW3, line);
  } else {
    u8g2.drawStr(0, ROW2, "scanning...");
  }
  u8g2.drawStr(0, ROW4, "hold Up = mark");   // dedicated always-on gesture (long BTN_1)
}

static void drawGpsDetail() {
  drawScreenHeader("GPS");
#if HAS_GPS
  char line[22];
  unsigned long good, bad, fixSent;
  int sats;
  coreGpsStats(good, bad, fixSent, sats);

  if (sats >= 0) snprintf(line, sizeof(line), "fix:%s sats:%d", gpsHasFix ? "YES" : "no", sats);
  else           snprintf(line, sizeof(line), "fix:%s sats:-",  gpsHasFix ? "YES" : "no");
  u8g2.drawStr(0, ROW1, line);
  snprintf(line, sizeof(line), "lat:%.5f", gpsLat);
  u8g2.drawStr(0, ROW2, line);
  snprintf(line, sizeof(line), "lng:%.5f", gpsLng);
  u8g2.drawStr(0, ROW3, line);
  snprintf(line, sizeof(line), "ok:%lu bad:%lu fx:%lu", good, bad, fixSent);
  u8g2.drawStr(0, ROW4, line);
#else
  u8g2.drawStr(0, ROW1, "no GPS module");
#endif
}

static void drawDetections() {
  char line[22];
  drawScreenHeader("DETECTIONS");
  snprintf(line, sizeof(line), "count: %d", fyDetCount);
  u8g2.drawStr(0, ROW1, line);
  if (fyDetCount > 0) {
    u8g2.drawStr(0, ROW2, "last MAC:");
    u8g2.drawStr(0, ROW3, dispMac);
  } else {
    u8g2.drawStr(0, ROW2, "no detections yet");
  }
}

static void drawScanDetail() {
  char line[22];
  drawScreenHeader("SCAN");
  snprintf(line, sizeof(line), "mode: %s", channelModeName());
  u8g2.drawStr(0, ROW1, line);
  snprintf(line, sizeof(line), "channel: %u", currentChannel);
  u8g2.drawStr(0, ROW2, line);
  snprintf(line, sizeof(line), "dwell: %ums", (unsigned)CHANNEL_DWELL_MS);
  u8g2.drawStr(0, ROW3, line);
}

// Scan Mode menu. Browsing marks the active mode with '*'. Drilled in
// (MENU_LIST), the cursor '>' tracks coreMenuSel, and Single opens a channel
// picker.
static void drawScanModes() {
  drawScreenHeader("SCAN MODE");
  if (coreMenuState == MENU_PICK_CHANNEL) {
    char line[22];
    u8g2.drawStr(0, ROW1, "Single channel:");
    snprintf(line, sizeof(line), "  ch %d", coreMenuSel);
    u8g2.drawStr(0, ROW2, line);
    u8g2.drawStr(0, ROW3, "Up/Dn: change");
    u8g2.drawStr(0, ROW4, "Sel: set  hold: back");
    return;
  }
  static const char* const opts[3] = { "Custom Scan", "Full Channel", "Single" };
  int active = coreScanModeIndex();
  for (int i = 0; i < 3; i++) {
    char line[22];
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    char act    = (i == active) ? '*' : ' ';
    snprintf(line, sizeof(line), "%c%c%s", cursor, act, opts[i]);
    u8g2.drawStr(0, ROW1 + i * 12, line);
  }
  if (coreMenuState == MENU_NONE) u8g2.drawStr(0, ROW4, "Select to change");
}

// Alerts menu. Two toggles (Buzzer mute/unmute, LED on/off). Select flips the
// highlighted row in place (core toggles coreBuzzerEnabled / coreLedEnabled).
// Rows show the CURRENT state so the toggle is unambiguous.
static void drawAlerts() {
  drawScreenHeader("ALERTS");
  char line[22];
  const char* rows[2] = { "Buzzer", "LED" };
  const char* vals[2] = { coreBuzzerEnabled ? "Unmuted" : "Muted",
                          coreLedEnabled    ? "On"      : "Off" };
  for (int i = 0; i < 2; i++) {
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    snprintf(line, sizeof(line), "%c%s: %s", cursor, rows[i], vals[i]);
    u8g2.drawStr(0, ROW1 + i * 12, line);
  }
  if (coreMenuState == MENU_NONE) u8g2.drawStr(0, ROW4, "Select to change");
}

// Config menu. Web Console On enters Admin (SoftAP portal). Off closes it.
static void drawConfig() {
  drawScreenHeader("WEB CONFIG");
  u8g2.drawStr(0, ROW1, "Web Console");
  static const char* const opts[2] = { "On (Admin)", "Off" };
  for (int i = 0; i < 2; i++) {
    char line[22];
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    snprintf(line, sizeof(line), "%c%s", cursor, opts[i]);
    u8g2.drawStr(0, ROW2 + i * 12, line);
  }
  if (coreMenuState == MENU_NONE) u8g2.drawStr(0, ROW4, "Select to enter");
}

static void displayScreen() {
  u8g2.clearBuffer();
  switch (coreCurrentScreen) {
    case SCREEN_OVERVIEW:    drawOverview();    break;
    case SCREEN_GPS:         drawGpsDetail();   break;
    case SCREEN_DETECTIONS:  drawDetections();  break;
    case SCREEN_SCAN_DETAIL: drawScanDetail();  break;
    case SCREEN_SCAN_MODES:  drawScanModes();   break;
    case SCREEN_ALERTS:      drawAlerts();      break;
    case SCREEN_CONFIG:      drawConfig();      break;
    default: break;
  }
  u8g2.sendBuffer();
}
#endif  // NAV_SCHEME_3BTN

static void displayTick() {
#if NAV_SCHEME_3BTN
  if (markOverlayUntil) {
    if (millis() < markOverlayUntil) return;   // hold the mark overlay
    markOverlayUntil = 0;
    dispDirty = true;                          // force a clean redraw of the screen
  }
  // The channel hops every CHANNEL_DWELL_MS (~350ms) but the periodic refresh is
  // 2s, so a channel readout would look stuck between refreshes. On the two
  // screens that show the live channel, repaint when it changes so it
  // tracks the hop. Other screens don't show it, so they stay on the 2s cadence.
  static uint8_t lastShownChannel = 0;
  if (currentChannel != lastShownChannel) {
    lastShownChannel = currentChannel;
    if (coreCurrentScreen == SCREEN_OVERVIEW || coreCurrentScreen == SCREEN_SCAN_DETAIL)
      dispDirty = true;
  }
#endif
  unsigned long now = millis();
  if (!dispDirty && (now - dispLastRefresh < DISPLAY_REFRESH_MS)) return;
  dispDirty = false;
  dispLastRefresh = now;

#if NAV_SCHEME_3BTN
  displayScreen();
#else
  char line[22];
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  snprintf(line, sizeof(line), "det:%-3d ch:%u", fyDetCount, currentChannel);
  u8g2.drawStr(0, 10, line);
  u8g2.drawStr(0, 22, dispMac);
  snprintf(line, sizeof(line), "oui:%s", dispOui);
  u8g2.drawStr(0, 34, line);
  if (fyDetCount > 0) {
    snprintf(line, sizeof(line), "rssi:%d", (int)dispRssi);
  } else {
    snprintf(line, sizeof(line), "scanning...");
  }
  u8g2.drawStr(0, 46, line);
#if HAS_GPS
  if (gpsHasFix) {
    snprintf(line, sizeof(line), "%.4f,%.4f", gpsLat, gpsLng);
  } else {
    snprintf(line, sizeof(line), "gps:no fix");
  }
  u8g2.drawStr(0, 58, line);
#endif
  u8g2.sendBuffer();
#endif  // NAV_SCHEME_3BTN
}

// Manual "area of interest" marker. On the 3-button scheme it's the dedicated
// long-press of BTN_1 (NAV_MARK, available regardless of screen). On the
// 2-button scheme it is BTN_PIN_2 (INPUT_MANUAL_MARK).
static void triggerManualAlert() {
  fyLastTargetSeen = millis();
#if USE_SD
  sdAppendRow("", "MANUALALERT", "manual", 0, currentChannel, "", "", -1.0f);
#endif
  dualPrintln("[bscope] MANUAL ALERT logged (area of interest)");
#if NAV_SCHEME_3BTN
  drawMarkOverlay();                            // brief "Saved Manual Record!" flash
  markOverlayUntil = millis() + MARK_OVERLAY_MS;
#endif
}

static void displayAdmin();   // defined below. checkInput() shows it on Admin entry

// Returns true if a nav action entered Admin mode this tick, so loop() can bail
// out before displayTick() repaints over the Admin screen (mirrors the BOOT
// double-press path). Only ever true on the 3-button scheme.
static bool checkInput() {
#if NAV_SCHEME_3BTN
  // Drain every pending nav event this tick (physical buttons + serial injector).
  NavEvent ev;
  while ((ev = coreNavTick()) != NAV_NONE) {
    NavAction act = coreNavApply(ev);
    if (act == NAV_ACT_MARK) {
      triggerManualAlert();
    } else if (act == NAV_ACT_ADMIN) {   // Config → Web Console → On
      webPortalStart(WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_PASSWORD);
      displayAdmin();
      return true;
    } else if (act == NAV_ACT_REDRAW) {
      dispDirty = true;                  // repaint on next displayTick
    }
  }
#else
  InputEvent ev = coreInputTick();
  if (ev == INPUT_MANUAL_MARK) triggerManualAlert();
#endif
  return false;
}

// Admin (AP) screen, shown once when the web portal comes up. Static: the
// portal pauses scanning, so there's nothing to refresh until power-cycle.
static void displayAdmin() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "ADMIN / AP mode");
  u8g2.drawStr(0, 26, WEB_PORTAL_AP_SSID);
  u8g2.drawStr(0, 40, "http://10.99.7.1");
  u8g2.drawStr(0, 54, "2x BOOT or web=exit");
  u8g2.sendBuffer();
}

// ============================================================
// SERIAL COMMANDS: 'status' is board-specific, since it reports psram=.
// Core handles the shared verbs.
// ============================================================

static void printStatus() {
  unsigned long ms = millis();
  unsigned long s  = ms / 1000;
  dualPrintf("[bscope] status: uptime=%lus ch=%u mode=%s det=%d spiffs=%d"
             " heap=%u psram=%u sniffing=%d\n",
             s, currentChannel, channelModeName(), fyDetCount,
             fySpiffsReady ? 1 : 0,
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getFreePsram(),
             sniffingStopped ? 0 : 1);
}

// Injects a synthetic addr2 detection through the same alert queue the real
// promiscuous callback feeds, so the display, SD, SPIFFS, and JSON path can
// run without a live camera nearby. Sweeps RSSI across successive calls.
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

static void printSerialHelp() {
  dualPrintln("[bscope] serial commands (word-based, newline-terminated):");
  dualPrintln("  status            print status");
  dualPrintln("  inject            inject test detection");
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
    else if (!strcmp(verb, "help") || !strcmp(verb, "?")) printSerialHelp();
    else if (verb[0]) dualPrintf("[bscope] unknown command: %s (try 'help')\n", verb);
    // blank line silently ignored
  }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Crucial for USB-optional operation: without this, Serial.write() blocks
  // indefinitely on a native-USB-CDC port when no host is attached. A board
  // using a hardware UART-to-USB bridge does not set ARDUINO_USB_CDC_ON_BOOT
  // and does not need this, since plain HardwareSerial has no such method and
  // never blocks.
  Serial.setTxTimeoutMs(0);
#endif
  delay(300);

#if MIRROR_SERIAL
  Serial2.begin(MIRROR_BAUD, SERIAL_8N1, -1, MIRROR_TX_PIN);  // TX-only
#endif

#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif

  displayInit();
  coreNotifyBoot();    // startup tune + RGB sanity cycle (no-op unless USE_LED/USE_BUZZER)

  // SPIFFS: format on first boot if missing. Non-fatal if it fails. Mounted
  // BEFORE coreTimeSync() because the NTP fallback (when no GPS module is found)
  // reads the saved WiFi credentials off SPIFFS.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[bscope] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[bscope] SPIFFS init FAILED – running without persistence");
  }

  // coreTimeSync() powers on the GPS rail and starts Serial2 at GPS_BAUD on
  // GPS_RX_PIN/GPS_TX_PIN, reconfiguring the same UART2 the mirror above
  // started at MIRROR_BAUD. It probes for a GPS module and, if none is
  // found, falls back to a WiFi/NTP join (saved creds from SPIFFS), else millis.
  coreTimeSync();

  precompileOuis();

#if USE_SD
  // micro SD on SPI2 via SD_SCK/MOSI/MISO/CS_PIN. Non-fatal if absent.
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (SD.begin(SD_CS_PIN)) {
    fySDReady = true;
    dualPrintln("[bscope] SD card ready");
    sdSetup();
    sdTryNameLog();   // name the log now if time is already anchored (NTP path).
                      // On the GPS path this no-ops and gpsTick() names it later
  } else {
    // No card, which is non-fatal: fall back to onboard SPIFFS. Signal it visibly
    // (5 blue LED flashes + on-screen notice) since boot otherwise looks
    // silent. Runs before loop()/coreInputTick(), so the LED pin is still ours.
    dualPrintln("[bscope] SD card not found – saving to SPIFFS");
    coreLedBlink(0, 0, 255, 5, 150, 150);
#if NAV_SCHEME_3BTN
    // A board with a Confirm button blocks here until the operator
    // acknowledges, so a missing card cannot pass unnoticed at boot.
    // coreNavTick() self-inits the button pins on first call. A board without
    // buttons holds the notice below instead and carries on.
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 20, "SD Card Not Found");
    u8g2.drawStr(0, 34, "Saving to SPIFFS");
    u8g2.drawStr(0, 52, "Press Confirm");
    u8g2.sendBuffer();
    while (coreNavTick() != NAV_SELECT) delay(10);   // wait for the Confirm (BTN_3 short)
#else
    displayMessage("SD Card Not Found", "Saving to SPIFFS");
    delay(1500);   // no Confirm button here, so hold the notice and carry on
#endif
  }
#endif

  // Raw-IDF promiscuous capture bring-up (Detect mode). Shared with
  // webPortalStop()'s resume path. See coreWifiSnifferStart() in core.
  coreWifiSnifferStart();

  dualPrintln("[bscope] OLED-family WiFi detector started");
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

  // Anytime BOOT double-press enters Admin.
  if (coreAdminTriggerCheck()) {
    webPortalStart(WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_PASSWORD);
    displayAdmin();
    return;
  }

  coreTick();           // drain GPS UART bytes into parser, update fix state
  updateChannelMode();
  checkSerialCommands();
  if (checkInput()) return;   // Config menu → Admin: portal serviced next loop
  drainAlertQueue();
  displayTick();        // refresh OLED after any queue drain
  autosaveTick();       // periodic SPIFFS write if dirty
  coreNotifyTick();     // audible heartbeat while a target is in range + LED off-timer
  printHeartbeat();
  delay(1);
}
