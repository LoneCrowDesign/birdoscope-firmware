// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Screen carousel drawing, shared verbatim between the firmware
// (src/main_oled.cpp) and the host-side renderer (tools/screen_render). Included
// as a .inc rather than compiled on its own. It draws through whatever `u8g2`
// object and state globals the includer has in scope, which on the device are
// the real ones and on the host are stubs. Keeping it a single source means a
// rendered PNG cannot drift from what the panel shows.
//
// The includer must provide: a `u8g2` object, the disp* display state, core's
// screen/menu state, and the board macros HAS_GPS / CHANNEL_DWELL_MS.

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
  // '!' after the count marks a full detection table (see DETECTIONS screen for
  // how many were missed). One char, so the line still fits 128px.
  snprintf(line, sizeof(line), "det:%-3d%c ch:%-2u gps:%c",
           fyDetCount, fyDroppedNew ? '!' : ' ', currentChannel,
           gpsHasFix ? 'Y' : 'N');
#else
  snprintf(line, sizeof(line), "det:%-3d%c ch:%u",
           fyDetCount, fyDroppedNew ? '!' : ' ', currentChannel);
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
  // Only drawn once the table has actually overflowed, so the row costs nothing
  // in the normal case. Without it, count: pins at MAX_DETECTIONS and reads as
  // a quiet area rather than a full table.
  if (fyDroppedNew) {
    snprintf(line, sizeof(line), "FULL! missed:%u", (unsigned)fyDroppedNew);
    u8g2.drawStr(0, ROW4, line);
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

// Targets menu. Which vendors the OUI matcher accepts, orthogonal to Scan Mode:
// this picks WHAT to look for, Scan Mode picks WHERE to listen. Act-and-close,
// same idiom as Scan Mode. Row order must match TARGET_MASKS in core.cpp.
static void drawTargets() {
  drawScreenHeader("TARGETS");
  static const char* const opts[3] = { "Flock", "Axon", "All" };
  int active = coreTargetIndex();          // -1 if the mask matches no row
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
    case SCREEN_TARGETS:     drawTargets();     break;
    case SCREEN_ALERTS:      drawAlerts();      break;
    case SCREEN_CONFIG:      drawConfig();      break;
    default: break;
  }
  u8g2.sendBuffer();
}
