// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host-side renderer for the OLED screen carousel. Compiles u8g2's plain-C
// sources natively, includes src/screens.inc verbatim, and dumps each frame's
// 1KB display buffer for to_png.py to turn into an image. Because the draw code
// is the firmware's own, a rendered frame cannot drift from the panel.
//
// What this file supplies is everything screens.inc expects the firmware to
// have in scope: a `u8g2` object, the disp* state, core's screen/menu state and
// helpers, and the board macros. All of it is a stub; nothing here runs on the
// device.
//
// u8g2 has no I/O here. u8x8_byte_empty / u8x8_dummy_cb stand in for the I2C
// and GPIO callbacks, and the buffer is read directly rather than sent, so no
// display init ever happens.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "u8g2.h"
}

// ============================================================
// BOARD CONFIG: set here rather than inherited from any one board's
// board_config.h, so the images show the screens rather than one PCB's quirks.
// ============================================================

#define NAV_SCHEME_3BTN  1
#define HAS_GPS          1
#define CHANNEL_DWELL_MS 250

// ============================================================
// CORE STATE MIRROR: these enums mirror core.h, which cannot be included on the
// host (it pulls in Arduino.h and esp_wifi.h). Needs to be kept in sync with
// current menu state or it will render older versions.
// ============================================================

typedef enum {
  SCREEN_OVERVIEW,
  SCREEN_GPS,
  SCREEN_DETECTIONS,
  SCREEN_SCAN_DETAIL,
  SCREEN_SCAN_MODES,
  SCREEN_TARGETS,
  SCREEN_ALERTS,
  SCREEN_CONFIG,
  SCREEN_COUNT,
} ScreenId;

typedef enum { MENU_NONE, MENU_LIST, MENU_PICK_CHANNEL, MENU_PICK_PROX } MenuState;

static ScreenId  coreCurrentScreen = SCREEN_OVERVIEW;
static MenuState coreMenuState     = MENU_NONE;
static int       coreMenuSel       = 0;
static bool      coreBuzzerEnabled = true;
static bool      coreLedEnabled    = true;

// Per-device direction counts, mirrored from core.h. Deliberately sum to more
// than DEMO_DET_COUNT (5), since a camera seen both ways counts in both and the
// rendered frame should show that rather than hide it.
static uint16_t coreDirectDeviceCount()   { return 4; }
static uint16_t coreIndirectDeviceCount() { return 3; }

// Raw sniffer counters, mirrored from core.h. Sized to exercise the k/M
// abbreviation on the Detections row rather than the bare-integer case.
static uint32_t coreSeenFrames      = 128400;
static uint32_t coreCandidateFrames = 96200;

// Proximity ring, mirrored from core.h. No board_config.h here, so the active
// value is spelled out rather than taken from PROX_RING_M.
#define PROX_RING_OPTION_COUNT 5
static const uint8_t PROX_RING_OPTIONS[PROX_RING_OPTION_COUNT] = { 0, 10, 25, 50, 100 };
static uint8_t       coreProxRingM = 25;

// ============================================================
// SCENARIO: the sample scan the images depict. Matches the DEMO_MODE values in
// main_oled.cpp so renders and device photos tell the same story, but kept
// separate: DEMO_MODE pins one frame, the renderer varies state per frame.
// ============================================================

#define DEMO_DET_COUNT 5
#define DEMO_CHANNEL   6
#define DEMO_OUI       "e4:aa:ea"        // drawn from core.cpp's oui_table[]
#define DEMO_MAC       DEMO_OUI ":7b:04:19"
#define DEMO_RSSI      (-80)
// 0 = Flock in core.h's Vendor enum, matching DEMO_OUI's tag in oui_table[].
// Spelled as a literal because the enum is not mirrored here; screens.inc
// indexes its own name table and never names the enumerators.
#define DEMO_VENDOR    0
// Metres. Not derived from DEMO_RSSI: the renderer deliberately carries no
// board_config.h, so there are no path-loss constants here to derive it from.
// Kept equal to DEMO_DIST_M in src/main_oled.cpp, and consistent with DEMO_RSSI
// under the default constants, so the frame does not contradict itself.
#define DEMO_DIST_M    (25.1f)

// Placeholder fix: Point Nemo, the oceanic pole of inaccessibility and the
// furthest point on earth from any land. Exact rather than approximate, and a
// scan reporting zero traffic from there is at least honest.
#define DEMO_LAT       (-48.87664)
#define DEMO_LNG       (-123.39335)

static char    dispMac[18] = DEMO_MAC;
static int8_t  dispRssi    = DEMO_RSSI;
static uint8_t dispCh      = DEMO_CHANNEL;
static int8_t  dispVendor  = DEMO_VENDOR;
static float   dispDistM   = DEMO_DIST_M;

static int      fyDetCount     = DEMO_DET_COUNT;
// Zero so the OVERVIEW and DETECTIONS frames show the normal case. The full-table
// indicators only draw when this is nonzero.
static uint16_t fyDroppedNew   = 0;
static uint8_t  currentChannel = DEMO_CHANNEL;
static bool     gpsHasFix      = true;
static double   gpsLat         = DEMO_LAT;
static double   gpsLng         = DEMO_LNG;

static void coreGpsStats(unsigned long& good, unsigned long& bad,
                         unsigned long& fixSent, int& sats) {
  good = 1284; bad = 3; fixSent = 5; sats = 9;
}
// Spelling matches core.cpp's channelModeName() exactly; the SCAN screen prints
// it verbatim, so a prettified stub would show a string the device never does.
static const char* channelModeName() { return "CUSTOM"; }
static int         coreScanModeIndex() { return 0; }
// 2 = All, the boot default (VENDOR_MASK_ALL), so the TARGETS frame shows what a
// freshly flashed board does.
static int         coreTargetIndex()   { return 2; }

// ============================================================
// U8G2 SHIM: the firmware draws through U8g2lib's C++ object, which is
// Arduino-only. This forwards the handful of calls screens.inc makes to the
// plain-C API underneath, so the draw code compiles unchanged.
// ============================================================

static u8g2_t u8g2_dev;

struct U8g2Shim {
  void clearBuffer()                       { u8g2_ClearBuffer(&u8g2_dev); }
  void sendBuffer()                        { /* buffer is read directly */ }
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
};

// More frames than there are ScreenIds: the menu screens each look different
// browsing (MENU_NONE) versus drilled in (MENU_LIST), and Single opens a
// channel picker. The mark overlay is handled separately below.
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
  // Appended rather than inserted at carousel position 6: renumbering would
  // rename the committed PNGs and break every image link in docs/menu_ux.md.
  // The prose list in that doc renumbers on its own.
  { "13_targets",           SCREEN_TARGETS,     MENU_NONE,         0 },
  { "14_targets_open",      SCREEN_TARGETS,     MENU_LIST,         1 },
  { "15_alerts_prox",       SCREEN_ALERTS,      MENU_PICK_PROX,    2 },
};
static const int FRAME_COUNT = sizeof(FRAMES) / sizeof(FRAMES[0]);

static bool dumpBuffer(const char* outDir, const char* name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.bin", outDir, name);
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return false; }
  // Full-buffer mode: 128 columns x 8 tile rows, one byte per 8 vertical px.
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

  // Orientation probe: a 4x4 block in the top-left, so the buffer-to-pixel
  // mapping can be confirmed on a known shape instead of guessed at from text.
  u8g2.clearBuffer();
  u8g2.drawBox(0, 0, 4, 4);
  if (!dumpBuffer(outDir, "00_probe")) return 1;

  for (int i = 0; i < FRAME_COUNT; i++) {
    coreCurrentScreen = FRAMES[i].screen;
    coreMenuState     = FRAMES[i].menu;
    coreMenuSel       = FRAMES[i].sel;
    displayScreen();
    if (!dumpBuffer(outDir, FRAMES[i].name)) return 1;
    printf("%s\n", FRAMES[i].name);
  }

  // The mark overlay is not a carousel position; it is drawn over whatever
  // screen is up when a manual mark is logged.
  drawMarkOverlay();
  if (!dumpBuffer(outDir, "12_mark_overlay")) return 1;
  printf("12_mark_overlay\n");

  return 0;
}
