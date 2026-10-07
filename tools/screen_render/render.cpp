// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host-side renderer for the OLED screen carousel. It includes src/screens.inc
// verbatim, links against the u8g2 plain-C sources render.sh compiles natively,
// and dumps each frame's 1KB display buffer for to_png.py to turn into an
// image.
//
// This file supplies stubs for everything screens.inc expects the firmware to
// have in scope, namely a `u8g2` object, the disp* state, core's screen and
// menu state and helpers, and the board macros. Nothing here runs on the
// device.
//
// u8x8_byte_empty and u8x8_dummy_cb stand in for the I2C and GPIO callbacks.
// dumpBuffer reads the buffer directly, and main never initialises a display.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "u8g2.h"
}

// ============================================================
// BOARD CONFIG
// This file sets these itself and includes no board_config.h, so the images
// show no single PCB's quirks.
// ============================================================

#define NAV_SCHEME_3BTN  1
#define HAS_GPS          1
#define CHANNEL_DWELL_MS 250

// ============================================================
// CORE STATE MIRROR
// These enums mirror core.h, which pulls in Arduino.h and esp_wifi.h and
// cannot compile on the host. Update them when core.h changes.
// ============================================================

typedef enum {
  SCREEN_OVERVIEW,
  SCREEN_GPS,
  SCREEN_DETECTIONS,
  SCREEN_SCAN_DETAIL,
  SCREEN_SCAN_MODES,
  SCREEN_TARGETS,
  SCREEN_RADIO,
  SCREEN_ALERTS,
  SCREEN_CONFIG,
  SCREEN_WIPE,
  SCREEN_COUNT,
} ScreenId;

typedef enum {
  MENU_NONE, MENU_LIST, MENU_PICK_CHANNEL, MENU_PICK_PROX, MENU_CONFIRM_WIPE
} MenuState;

typedef enum { WIPE_NONE = 0, WIPE_DEVICE = 1, WIPE_DEVICE_AND_CARD = 2 } WipeScope;

typedef enum { RADIO_MODE_WIFI = 0, RADIO_MODE_BLE = 1, RADIO_MODE_COUNT } RadioMode;
static RadioMode coreRadioMode = RADIO_MODE_WIFI;   // set per frame

#define VENDOR_COUNT     9
#define TARGET_ROW_COUNT 4
#define WIPE_CONFIRM_PRESSES 3

static ScreenId  coreCurrentScreen = SCREEN_OVERVIEW;
static MenuState coreMenuState     = MENU_NONE;
static int       coreMenuSel       = 0;
// The wipe confirmation reads a scope and a press count in place of
// coreMenuSel, so the renderer carries both and main sets them per frame.
static int       coreWipeConfirmCount = 0;
static WipeScope dispWipeScope        = WIPE_DEVICE;
static WipeScope coreWipeSelectedScope() { return dispWipeScope; }
static bool      coreBuzzerEnabled = true;
static bool      coreLedEnabled    = true;

// Per-device direction counts, mirrored from core.h. They sum to more than
// DEMO_DET_COUNT (5) because a camera seen both ways counts in both.
static uint16_t coreDirectDeviceCount()   { return 4; }
static uint16_t coreIndirectDeviceCount() { return 3; }

// Raw sniffer counters, mirrored from core.h. The values exercise the k/M
// abbreviation on the Detections row.
static uint32_t coreSeenFrames      = 128400;
static uint16_t coreSeenRate() { return 214; }

// The uptime the Detections row prints, 2h05.
static uint32_t millis() { return 125u * 60000u; }

// BLE counters and detection table, mirrored from core.h, for the BLE frames.
static uint32_t coreBleReports  = 18400;
static uint32_t coreBleMatched  = 212;
static uint32_t coreBlePhy1M    = 18350;
static uint32_t coreBlePhyCoded = 50;
static uint16_t coreBleRate() { return 31; }

typedef struct {
  char     key[18];
  char     mac[18];
  int8_t   vendor;
  bool     accessory;
  int8_t   rssi;
  uint16_t count;
  uint32_t firstSeen;
  uint32_t lastSeen;
} BleDetection;

// A made-up Axon device keyed by its advertised serial. Keep these values
// synthetic, since the repo publishes the renders. Vendor 1 is Axon in core.h's
// Vendor enum.
static BleDetection coreBleDet[1] = {
  { "X0000TEST", "00:25:df:00:00:01", 1, false, -71, 37, 0, 0 },
};
static uint16_t coreBleDetCount  = 1;
static uint16_t coreBleDetMissed = 0;
static int16_t  coreBleDetLast   = 0;

// Proximity ring, mirrored from core.h. With no board_config.h here,
// coreProxRingM spells out the active value in place of PROX_RING_M.
#define PROX_RING_OPTION_COUNT 5
static const uint8_t PROX_RING_OPTIONS[PROX_RING_OPTION_COUNT] = { 0, 10, 25, 50, 100 };
static uint8_t       coreProxRingM = 25;

// ============================================================
// SCENARIO
// The sample scan the images depict. These values match DEMO_MODE in
// main_oled.cpp so renders and device photos agree. The two stay separate
// because DEMO_MODE pins one frame and the renderer varies state per frame.
// ============================================================

#define DEMO_DET_COUNT 5
#define DEMO_CHANNEL   6
#define DEMO_OUI       "e4:aa:ea"        // drawn from core.cpp's oui_table[]
#define DEMO_MAC       DEMO_OUI ":7b:04:19"
#define DEMO_RSSI      (-80)
// 0 = VENDOR_FLOCK in core.h, matching DEMO_OUI's tag in oui_table[]. A
// literal, since this file does not mirror the Vendor enum.
#define DEMO_VENDOR    0
// Metres. With no board_config.h, this file has no path-loss constants to
// derive it from DEMO_RSSI. Keep it equal to DEMO_DIST_M in src/main_oled.cpp,
// which matches DEMO_RSSI under the default constants.
#define DEMO_DIST_M    (25.1f)

// Placeholder fix at Point Nemo, the oceanic pole of inaccessibility.
#define DEMO_LAT       (-48.87664)
#define DEMO_LNG       (-123.39335)

static char    dispMac[18] = DEMO_MAC;
static int8_t  dispRssi    = DEMO_RSSI;
static uint8_t dispCh      = DEMO_CHANNEL;
static int8_t  dispVendor  = DEMO_VENDOR;
static float   dispDistM   = DEMO_DIST_M;

static int      fyDetCount     = DEMO_DET_COUNT;
// Zero, so the OVERVIEW and DETECTIONS frames show the normal case. screens.inc
// draws the full-table indicators only when this is nonzero.
static uint16_t fyDroppedNew   = 0;
static uint8_t  currentChannel = DEMO_CHANNEL;
static bool     gpsHasFix      = true;
static double   gpsLat         = DEMO_LAT;
static double   gpsLng         = DEMO_LNG;

static void coreGpsStats(unsigned long& good, unsigned long& bad,
                         unsigned long& fixSent, int& sats) {
  good = 1284; bad = 3; fixSent = 5; sats = 9;
}
// Spelling matches core.cpp's channelModeName(), which the SCAN screen prints
// verbatim.
static const char* channelModeName() { return "CUSTOM"; }
static int         coreScanModeIndex() { return 0; }
// The last row is All, the boot default (VENDOR_MASK_ALL), so the TARGETS frame
// shows what a freshly flashed board does.
static int         coreTargetIndex()   { return TARGET_ROW_COUNT - 1; }

// ============================================================
// U8G2 SHIM
// The firmware draws through U8g2lib's C++ object, which is Arduino-only.
// U8g2Shim forwards the handful of calls screens.inc makes to the plain-C API
// underneath, so the draw code compiles unchanged.
// ============================================================

static u8g2_t u8g2_dev;

struct U8g2Shim {
  void clearBuffer()                       { u8g2_ClearBuffer(&u8g2_dev); }
  void sendBuffer()                        { /* dumpBuffer reads the buffer */ }
  void setFont(const uint8_t* f)           { u8g2_SetFont(&u8g2_dev, f); }
  void drawStr(int x, int y, const char* s){ u8g2_DrawStr(&u8g2_dev, x, y, s); }
  int  getStrWidth(const char* s)          { return u8g2_GetStrWidth(&u8g2_dev, s); }
  void drawHLine(int x, int y, int w)      { u8g2_DrawHLine(&u8g2_dev, x, y, w); }
  void drawFrame(int x, int y, int w, int h) { u8g2_DrawFrame(&u8g2_dev, x, y, w, h); }
  void drawBox(int x, int y, int w, int h) { u8g2_DrawBox(&u8g2_dev, x, y, w, h); }
};
static U8g2Shim u8g2;

// The firmware's screen drawing, verbatim.
#include "screens.inc"

// ============================================================
// FRAME TABLE + OUTPUT
// ============================================================

struct Frame {
  const char* name;
  ScreenId    screen;
  MenuState   menu;
  int         sel;
  // Wipe confirmation only, where `sel` names no row. The presses so far and
  // the scope they apply to.
  int         wipePresses;
  WipeScope   wipeScope;
  RadioMode   radio;   // RADIO_MODE_WIFI unless a frame says otherwise
};

// More frames than there are ScreenIds, because each menu screen looks
// different browsing (MENU_NONE) and drilled in (MENU_LIST), the pickers add
// their own, and BLE mode changes three screens. main draws the mark overlay
// separately.
static const Frame FRAMES[] = {
  { "01_overview",          SCREEN_OVERVIEW,    MENU_NONE,         0 },
  { "02_gps",               SCREEN_GPS,         MENU_NONE,         0 },
  { "03_detections",        SCREEN_DETECTIONS,  MENU_NONE,         0 },
  { "04_scan",              SCREEN_SCAN_DETAIL, MENU_NONE,         0 },
  { "05_scan_mode",         SCREEN_SCAN_MODES,  MENU_NONE,         0 },
  { "06_scan_mode_open",    SCREEN_SCAN_MODES,  MENU_LIST,         1 },
  { "07_scan_mode_channel", SCREEN_SCAN_MODES,  MENU_PICK_CHANNEL, 6 },
  { "08_alerts",            SCREEN_ALERTS,      MENU_NONE,         0 },
  { "09_alerts_open",       SCREEN_ALERTS,      MENU_LIST,         0 },
  { "10_web_config",        SCREEN_CONFIG,      MENU_NONE,         0 },
  { "11_web_config_open",   SCREEN_CONFIG,      MENU_LIST,         0 },
  // Frames from 13 on sit out of carousel order, so the committed PNG names and
  // the image links in docs/menu_ux.md stay stable.
  { "13_targets",           SCREEN_TARGETS,     MENU_NONE,         0 },
  { "14_targets_open",      SCREEN_TARGETS,     MENU_LIST,         1 },
  { "15_alerts_prox",       SCREEN_ALERTS,      MENU_PICK_PROX,    2 },
  { "16_wipe",              SCREEN_WIPE,        MENU_NONE,         0 },
  { "17_wipe_open",         SCREEN_WIPE,        MENU_LIST,         1 },
  { "18_wipe_confirm",      SCREEN_WIPE,        MENU_CONFIRM_WIPE, 0, 2,
                                                WIPE_DEVICE_AND_CARD },
  { "19_radio",             SCREEN_RADIO,       MENU_NONE,         0 },
  { "20_radio_open",        SCREEN_RADIO,       MENU_LIST,         1 },
  { "21_overview_ble",      SCREEN_OVERVIEW,    MENU_NONE,         0, 0,
                                                WIPE_NONE, RADIO_MODE_BLE },
  { "22_detections_ble",    SCREEN_DETECTIONS,  MENU_NONE,         0, 0,
                                                WIPE_NONE, RADIO_MODE_BLE },
  { "23_scan_ble",          SCREEN_SCAN_DETAIL, MENU_NONE,         0, 0,
                                                WIPE_NONE, RADIO_MODE_BLE },
};
static const int FRAME_COUNT = sizeof(FRAMES) / sizeof(FRAMES[0]);

static bool dumpBuffer(const char* outDir, const char* name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.bin", outDir, name);
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return false; }
  // The full buffer holds 128 columns x 8 tile rows, one byte per 8 vertical
  // px.
  size_t bytes = (size_t)u8g2_GetBufferTileWidth(&u8g2_dev) * 8
               * (size_t)u8g2_GetBufferTileHeight(&u8g2_dev);
  fwrite(u8g2_GetBufferPtr(&u8g2_dev), 1, bytes, f);
  fclose(f);
  return true;
}

int main(int argc, char** argv) {
  const char* outDir = (argc > 1) ? argv[1] : ".";

  u8g2_Setup_ssd1306_128x64_noname_f(&u8g2_dev, U8G2_R0,
                                     u8x8_byte_empty, u8x8_dummy_cb);
  u8g2_SetFontMode(&u8g2_dev, 1);
  u8g2_SetFontDirection(&u8g2_dev, 0);

  // Orientation probe, a 4x4 block in the top-left that confirms the
  // buffer-to-pixel mapping on a known pattern.
  u8g2.clearBuffer();
  u8g2.drawBox(0, 0, 4, 4);
  if (!dumpBuffer(outDir, "00_probe")) return 1;

  for (int i = 0; i < FRAME_COUNT; i++) {
    coreCurrentScreen = FRAMES[i].screen;
    coreMenuState     = FRAMES[i].menu;
    coreMenuSel       = FRAMES[i].sel;
    coreWipeConfirmCount = FRAMES[i].wipePresses;
    dispWipeScope        = FRAMES[i].wipeScope;
    coreRadioMode        = FRAMES[i].radio;
    displayScreen();
    if (!dumpBuffer(outDir, FRAMES[i].name)) return 1;
    printf("%s\n", FRAMES[i].name);
  }

  // The firmware draws the mark overlay over whichever screen is up when the
  // operator logs a manual mark. It has no carousel position.
  drawMarkOverlay();
  if (!dumpBuffer(outDir, "12_mark_overlay")) return 1;
  printf("12_mark_overlay\n");

  return 0;
}
