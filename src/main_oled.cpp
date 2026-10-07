// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Main file for the u8g2 OLED boards. Detection logic lives in
// lib/birdoscope_core, so this file covers OLED drawing plus setup() and loop()
// orchestration only. Boards differ here only through board_config.h flags.
#include <Arduino.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include <SPIFFS.h>
#include <SPI.h>
#include <SD.h>
#include <U8g2lib.h>

// ============================================================
// CONFIG
// Pins, feature flags and tuning constants for specific hardware. The -I build
// flag in platformio.ini selects the board_config.h for each env.
// ============================================================

#include "board_config.h"
#include "core.h"
#include "web_portal.h"
#include "roost_session.h"

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
// defaults to U8G2_R0 (native). A board with its panel upside-down sets
// U8G2_R2 (180°) in its board_config.h.
#ifndef OLED_ROTATION
#define OLED_ROTATION U8G2_R0
#endif

// OLED_HW_I2C picks one of two u8g2 constructors.
//   0  software-bitbanged I2C with explicit SDA/SCL/RST pins, for a board
//      with no hardware I2C bus free
//   1  hardware I2C, with displayInit() setting the pins via Wire.begin()
// OLED_SH1106 (HW-I2C path only) picks the SH1106 controller over the SSD1306.
// The SH1106 has 132 columns of RAM behind a 128px panel, so its u8g2
// constructor applies a 2px column offset. The constructor must match the
// panel's controller.
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
static int8_t dispVendor   = -1;   // Vendor enum value, or -1 for no match
// Metres, straight from CoreAlertResult::distM so the panel and logs agree. -1
// renders "via AP", spec S3. The panel keeps the cached value, so a
// recalibration shows on the next detection.
static float  dispDistM    = -1.0f;
static bool   dispDirty    = false;
static unsigned long dispLastRefresh = 0;
#define DISPLAY_REFRESH_MS 2000

// ============================================================
// DEMO MODE (holds a still frame for photos, off unless -DDEMO_MODE=1)
//
// displayTick() overwrites the display state with fixed values on each pass.
// Scanning still runs underneath, and only the screen's readout holds still.
// Reflash a standard build afterwards, or the detector looks broken.
// ============================================================
#ifndef DEMO_MODE
#define DEMO_MODE 0
#endif

#if DEMO_MODE
#define DEMO_DET_COUNT 5
#define DEMO_CHANNEL   6
#define DEMO_OUI       "e4:aa:ea"       // drawn from core.cpp's oui_table[]
#define DEMO_MAC       DEMO_OUI ":7b:04:19"
#define DEMO_RSSI      (-80)   // mid-range, so the frame shows a plausible distance
#define DEMO_VENDOR    VENDOR_FLOCK      // DEMO_OUI is a Flock-tagged prefix
// A literal, so a demo frame is a fixed picture. Keep it consistent by hand
// with DEMO_RSSI under the defaults, and equal to DEMO_DIST_M in
// tools/screen_render/render.cpp.
#define DEMO_DIST_M    (25.1f)

static void demoSeedDisplayState() {
  strlcpy(dispMac, DEMO_MAC, sizeof(dispMac));
  strlcpy(dispOui, DEMO_OUI, sizeof(dispOui));
  dispRssi       = DEMO_RSSI;
  dispCh         = DEMO_CHANNEL;
  dispVendor     = DEMO_VENDOR;
  dispDistM      = DEMO_DIST_M;
  fyDetCount     = DEMO_DET_COUNT;
  currentChannel = DEMO_CHANNEL;
#if HAS_GPS
  gpsHasFix      = true;
#endif
}
#endif  // DEMO_MODE

// ============================================================
// DRAIN QUEUE
// Pops core's alert queue, hands each entry to coreHandleAlert() for the table,
// SD, JSON and notification work, then updates the display state and applies
// the stop-on-hit options.
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
    dispVendor = r.vendor;     // -1 when the OUI is not a target
    dispDistM  = r.distM;      // already -1 for addr1 hits
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

  // OLED_I2C_ADDR is the panel's 7-bit address, set by boards whose module
  // does not answer on u8g2's default. When nothing acks at that address, a
  // boot-time scan names every device on the bus, so a blank screen tells a
  // wrong address from a missing panel.
#ifdef OLED_I2C_ADDR
  u8g2.setI2CAddress(OLED_I2C_ADDR << 1);
  Wire.beginTransmission(OLED_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    dualPrintf("[bscope] oled: no ack at 0x%02X on SDA=%d SCL=%d\n",
               OLED_I2C_ADDR, OLED_SDA, OLED_SCL);
    for (uint8_t a = 0x08; a < 0x78; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) dualPrintf("[bscope] oled: i2c device at 0x%02X\n", a);
    }
  }
#endif
#endif

  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.clearBuffer();
  u8g2.drawStr(0, 12, "birdoscope");
  u8g2.drawStr(0, 28, "starting...");
  // The build identity shows on the splash, for confirming a flash. The serial
  // banner and `status` also print it.
  {
    char line[22];
    snprintf(line, sizeof(line), "v%s %s", BIRDOSCOPE_VERSION, coreBuildRev());
    u8g2.drawStr(0, 44, line);
    u8g2.drawStr(0, 58, BIRDOSCOPE_BUILD_DATE);
  }
  u8g2.sendBuffer();
}

// Two-line message for boot-time status, such as the SD-not-found notice.
static void displayMessage(const char* line1, const char* line2) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 26, line1);
  u8g2.drawStr(0, 42, line2);
  u8g2.sendBuffer();
}

#ifdef BENCH_RADIO_TOGGLE
#include "z_bench_radio.inc"   // local and gitignored, built by z_bench.ini
#endif

#if NAV_BTN_COUNT
#include "screens.inc"
#endif  // NAV_BTN_COUNT

static void displayTick() {
#if DEMO_MODE
  // Seeds before the hop check below, so the pinned channel never reads as a
  // change or forces an extra repaint.
  demoSeedDisplayState();
#if NAV_BTN_COUNT
  coreCurrentScreen = SCREEN_OVERVIEW;
#endif
#endif
#if NAV_BTN_COUNT
  if (markOverlayUntil) {
    if (millis() < markOverlayUntil) return;   // hold the mark overlay
    markOverlayUntil = 0;
    dispDirty = true;                          // force a clean redraw of the screen
  }
  // The channel hops every CHANNEL_DWELL_MS (~250ms) but the periodic refresh
  // is 2s. The two screens that show the live channel repaint on each hop, and
  // the others stay on the 2s cadence.
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

#if NAV_BTN_COUNT
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
#endif  // NAV_BTN_COUNT
}

// Manual "area of interest" marker. On the nav-button schemes it's the long
// press of BTN_1 (NAV_MARK, available on every screen). On the 2-button scheme
// it is BTN_PIN_2 (INPUT_MANUAL_MARK). This logs the mark before it opens the
// survey window, per spec O1.
static void triggerManualAlert() {
  fyLastTargetSeen = millis();
#if USE_SD
  roostLogOperatorMark();
#endif
  dualPrintln("[bscope] MANUAL ALERT logged (area of interest)");
  coreSurveyStart();
#if NAV_BTN_COUNT
  drawMarkOverlay();                            // brief "Saved Manual Record!" flash
  markOverlayUntil = millis() + MARK_OVERLAY_MS;
#endif
}

static void displayAdmin();   // checkInput() draws it on Admin entry

static void displayCenteredMessage(const char* l1, const char* l2, const char* l3) {
  const int w   = u8g2.getDisplayWidth();
  const int n   = l3 ? 3 : 2;
  const int gap = 13;
  int y = (u8g2.getDisplayHeight() - ((n - 1) * gap + 9)) / 2 + 7;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  const char* lines[3] = { l1, l2, l3 };
  for (int i = 0; i < n; i++, y += gap)
    u8g2.drawStr((w - u8g2.getStrWidth(lines[i])) / 2, y, lines[i]);
  u8g2.sendBuffer();
}

// Runs a confirmed or resumed device wipe and powers the board down. Never
// returns. coreDeviceWipe() does the erase, and this draws the two frames
// around it and the warning not to cut power.
static void runDeviceWipe(WipeScope scope) {
  // A card scope with no card reports the card it could not reach.
#if USE_SD
  const bool cardMissed = (scope == WIPE_DEVICE_AND_CARD) && !fySDReady;
#else
  const bool cardMissed = (scope == WIPE_DEVICE_AND_CARD);
#endif
  displayCenteredMessage("DEVICE WIPE", "Erasing, do not", "power off");
  coreDeviceWipe(scope);
  displayCenteredMessage(cardMissed ? "WIPED, NO CARD" : "WIPE COMPLETE",
                         "Powering off", nullptr);
  delay(2000);
  u8g2.setPowerSave(1);        // blank the panel, so the board reads as off
  corePowerOff();
}

// Returns true if a nav action entered Admin mode this tick, so loop() can bail
// out before displayTick() repaints over the Admin screen (mirrors the BOOT
// double-press path). Only ever true on the nav-button schemes.
static bool checkInput() {
#if NAV_BTN_COUNT
  // Drain every pending nav event this tick, from the buttons and the serial
  // injector.
  NavEvent ev;
  while ((ev = coreNavTick()) != NAV_NONE) {
    NavAction act = coreNavApply(ev);
    if (act == NAV_ACT_MARK) {
      triggerManualAlert();
    } else if (act == NAV_ACT_ADMIN) {   // Config → Web Console → On
      webPortalStart(WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_PASSWORD);
      displayAdmin();
      return true;
    } else if (act == NAV_ACT_WIPE) {   // Device Wipe, after the third Confirm
      runDeviceWipe(coreWipeSelectedScope());
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

// Admin (AP) screen, drawn once when the web portal comes up. The portal
// pauses scanning, so nothing refreshes it until the portal stops.
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
// SERIAL COMMANDS
// 'status' is board-specific, since it reports psram=. Core handles the shared
// verbs.
// ============================================================

static void printStatus() {
  unsigned long ms = millis();
  unsigned long s  = ms / 1000;
  dualPrintf("[bscope] status: uptime=%lus radio=%s ch=%u mode=%s det=%d spiffs=%d"
             " heap=%u psram=%u sniffing=%d direct=%u indirect=%u"
             " seen=%u cand=%u qdrop=%u survey=%us"
             " ble=%u match=%u 1m=%u cod=%u\n",
             s, radioModeName(coreRadioMode),
             currentChannel, channelModeName(), fyDetCount,
             fySpiffsReady ? 1 : 0,
             (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getFreePsram(),
             sniffingStopped ? 0 : 1,
             (unsigned)coreDirectFrames, (unsigned)coreIndirectFrames,
             (unsigned)coreSeenFrames, (unsigned)coreCandidateFrames,
             (unsigned)coreQueueDrops,
             (unsigned)(coreSurveyRemainingMs() / 1000),   // 0 when no window is open
             (unsigned)coreBleReports, (unsigned)coreBleMatched,
             (unsigned)coreBlePhy1M, (unsigned)coreBlePhyCoded);
  // From the roost writer, the same figures as the web console's load line.
  uint32_t rw = 0, rd = 0, wf = 0, fx = 0;
  roostSessionStats(&rw, &rd, &wf, &fx);
  dualPrintf("[bscope] load: qmax=%u/%u qdrop=%u bqmax=%u/%u bqdrop=%u rows=%u"
             " fixes=%u dropped=%u worst_flush=%ums session=%s\n",
             (unsigned)coreQueueDepthMax, (unsigned)coreAlertQueueSize(),
             (unsigned)coreQueueDrops,
             (unsigned)coreBleQueueDepthMax, (unsigned)coreBleQueueSize(),
             (unsigned)coreBleQueueDrops,
             (unsigned)rw, (unsigned)fx, (unsigned)rd, (unsigned)wf,
             roostSessionOpen() ? roostSessionDir() : "none");
  dualPrintf("[bscope] build: %s\n", coreBuildIdentity());
}

// Injects a synthetic detection through the same alert queue the real
// promiscuous callback feeds, so the display, SD, SPIFFS, and JSON path can
// run without a live camera nearby. Sweeps RSSI across successive calls.
//
// `indirect` builds an addr1 hit, with a synthetic AP as the transmitter. It
// reaches the addr1 branches without a camera in range. Those hits carry no
// scanner-to-camera RSSI, and spec A2, C1 and S3 govern them.
static void injectTestDetection(bool indirect) {
  static const int8_t sweep[] = { -30, -45, -60, -75, -95 };
  static size_t sweepIdx = 0;
  int8_t rssi = sweep[sweepIdx];
  sweepIdx = (sweepIdx + 1) % (sizeof(sweep) / sizeof(sweep[0]));

  uint8_t targetOui[3];
  coreGetFirstTargetOui(targetOui);
  // Each direction gets its own last byte, so direct and indirect injects land
  // on separate rows.
  uint8_t fakeMac[6] = { targetOui[0], targetOui[1], targetOui[2],
                         0xAA, 0xBB, (uint8_t)(indirect ? 0xCD : 0xCC) };
  // A locally administered MAC outside every target prefix, so the AP cannot
  // match, as on a real addr1 hit.
  uint8_t fakeAp[6] = { 0x02, 0x00, 0x5E, 0x11, 0x22, 0x33 };
  // Addresses by position, as in a real frame. On an indirect hit the target
  // is the receiver and the AP the transmitter.
  FrameMeta fm = {};
  if (indirect) { memcpy(fm.addr1, fakeMac, 6); memcpy(fm.addr2, fakeAp, 6); }
  else          { memcpy(fm.addr2, fakeMac, 6); }
  memcpy(fm.addr3, fakeAp, 6);
  fm.frameLen = 128;
  strcpy(fm.bbFormat, "11g");
  enqueueAlert(indirect ? ALERT_OUI_ADDR1 : ALERT_OUI_ADDR2,
               fakeMac, &fm, rssi, currentChannel,
               nullptr, indirect ? "addr1" : "test", "probe_req");
  dualPrintf("[bscope] injected %s test detection rssi=%d\n",
             indirect ? "indirect" : "direct", (int)rssi);
}

static void printSerialHelp() {
  dualPrintln("[bscope] serial commands (word-based, newline-terminated):");
  dualPrintln("  status            print status");
  dualPrintln("  inject [addr1]    inject a test detection, direct or indirect");
  corePrintSerialHelp();   // core's shared verbs
  dualPrintln("  help              this help (also '?')");
}

static void checkSerialCommands() {
  const char* verb;
  const char* arg;
  while (coreReadSerialCommand(&verb, &arg)) {
    if (coreHandleSerialCommand(verb, arg)) continue;
    if      (!strcmp(verb, "status")) printStatus();
    else if (!strcmp(verb, "inject")) injectTestDetection(arg && !strcmp(arg, "addr1"));
    else if (!strcmp(verb, "help") || !strcmp(verb, "?")) printSerialHelp();
    else if (verb[0]) dualPrintf("[bscope] unknown command: %s (try 'help')\n", verb);
    // a blank line falls through silently
  }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Required for USB-optional operation. Without it, Serial.write() blocks
  // indefinitely on a native-USB-CDC port with no host attached. A board with
  // a hardware UART-to-USB bridge leaves ARDUINO_USB_CDC_ON_BOOT unset, since
  // plain HardwareSerial has no such method and never blocks.
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

  // First line of the session's serial log, so a capture carries the build
  // that made it.
  dualPrintf("[bscope] %s\n", coreBuildIdentity());

  displayInit();
  coreNotifyBoot();    // startup tune + RGB sanity cycle (no-op unless USE_LED/USE_BUZZER)

  // SPIFFS formats on first boot if missing, and a failure is non-fatal. Mount
  // it before coreTimeSync(), whose NTP join reads the saved WiFi credentials
  // off SPIFFS.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[bscope] SPIFFS ready");
    fyPromotePrevSession();
    // Before the first detection. Nothing reloads it lazily.
    coreSettingsLoad();
  } else {
    dualPrintln("[bscope] SPIFFS init FAILED - running without persistence");
  }

  // coreTimeSync() powers the GPS module and starts GPS_SERIAL at GPS_BAUD on
  // GPS_RX_PIN/GPS_TX_PIN. Where GPS_SERIAL is the default Serial2, this
  // reconfigures the UART the mirror above started. It probes for the module,
  // then bridges the time until GPS locks with an NTP join on the saved WiFi
  // credentials, else millis().
  coreTimeSync();

  precompileOuis();

#if USE_SD
  // micro SD on SPI2 via SD_SCK/MOSI/MISO/CS_PIN. Non-fatal if absent.
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  // The frequency and mountpoint repeat the Arduino defaults, because
  // SD_MAX_OPEN_FILES is positional.
  if (SD.begin(SD_CS_PIN, SPI, 4000000, "/sd", SD_MAX_OPEN_FILES)) {
    fySDReady = true;
    dualPrintln("[bscope] SD card ready");
    roostSessionBegin();
    roostSessionAnchor();   // names the session now if NTP anchored the clock,
                            // else gpsTick() names it once GPS locks
  } else {
    // No card is non-fatal, and detections save to onboard SPIFFS. Five blue
    // LED flashes and an on-screen notice signal it, since boot otherwise looks
    // silent. This runs before loop() and coreInputTick(), so the LED pin is
    // still free.
    dualPrintln("[bscope] SD card not found - saving to SPIFFS");
    coreLedBlink(0, 0, 255, 5, 150, 150);
#if NAV_BTN_COUNT
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

  // A wipe cut short by a power loss finishes here, with both filesystems
  // mounted and before the sniffer can write anything new. Never returns.
  {
    const WipeScope pending = coreWipePending();
    if (pending != WIPE_NONE) {
      dualPrintln("[bscope] interrupted device wipe found, resuming");
      runDeviceWipe(pending);
    }
  }

  // Starts the radio coreRadioMode selects (Detect mode). webPortalStop()'s
  // resume path calls it too.
  coreRadioStart();

  dualPrintln("[bscope] OLED-family WiFi detector started");
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
#if NAV_BTN_COUNT
    const NavEvent adminEv = coreNavTick();   // drained every loop so the queue cannot go stale
#else
    const NavEvent adminEv = NAV_NONE;
#endif
    // Buttons are otherwise inert in Admin. Only NAV_BACK_HOLD (a Back hold, or
    // a double press on 4-button boards) acts, so a stray press cannot drop the
    // portal mid-session.
    if (webPortalActive() && (adminEv == NAV_BACK_HOLD || coreAdminTriggerCheck())) webPortalStop();
    wasAdmin = true;
    delay(2);
    return;
  }
  if (wasAdmin) { wasAdmin = false; dispDirty = true; }   // resumed, so force a redraw

  // A BOOT double press enters Admin from any screen.
  if (coreAdminTriggerCheck()) {
    webPortalStart(WEB_PORTAL_AP_SSID, WEB_PORTAL_AP_PASSWORD);
    displayAdmin();
    return;
  }

  coreTick();           // drain GPS UART bytes into parser, update fix state
  roostSessionTick();   // manifest snapshot, so a power cut still leaves counters
  updateChannelMode();
  checkSerialCommands();
  if (checkInput()) return;   // entered Admin, and the next loop services the portal
  coreSurveyTick();     // closes an operator survey window once it has elapsed
  drainAlertQueue();
  if (coreBleDrain()) dispDirty = true;
#ifdef BENCH_RADIO_TOGGLE
  benchRadioTick();
#endif
  displayTick();        // refresh OLED after any queue drain
  autosaveTick();       // periodic SPIFFS write if dirty
  coreNotifyTick();     // LED off-timer, spec A1
  printHeartbeat();
  delay(1);
}
