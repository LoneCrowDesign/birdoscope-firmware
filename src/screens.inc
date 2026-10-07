// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Screen carousel drawing, which the firmware (src/main_oled.cpp) and the
// host-side renderer (tools/screen_render) both include verbatim. It draws
// through whatever `u8g2` object and state globals the includer has in scope,
// real on the device and stubs on the host.
//
// The includer must provide a `u8g2` object, the disp* display state, core's
// screen and menu state, and the board macros HAS_GPS and CHANNEL_DWELL_MS.

// ============================================================
// SCREEN CAROUSEL
// One draw function per ScreenId. Core owns the state and this file owns the
// pixels. Content rows sit below a shared title and index header. The menu
// screens drill in via SELECT and BACK. See docs/menu_ux.md.
// ============================================================

#define ROW1 24
#define ROW2 36
#define ROW3 48
#define ROW4 60

// Brief confirmation overlay for a logged manual mark. While markOverlayUntil
// is in the future, displayTick() holds this frame in place of the current
// screen, then clears the timer once it expires.
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

// Display names for the panel. The order must match the Vendor enum in core.h.
// vendorName() holds the lowercase strings, the stable form for serial and
// JSON.
static const char* dispVendorName(int8_t vendor) {
  static const char* const names[VENDOR_COUNT] = {
    "Flock", "Axon", "Axis", "Utility",
    "Motorola", "Verkada", "Genetec", "DigAlly", "Infra",
  };
  if (vendor < 0 || vendor >= (int)(sizeof(names) / sizeof(names[0]))) return "--";
  return names[vendor];
}

// The '~' marks the number as a coarse estimate. Distances of 999m or more
// show as ">999m" to hold the row width. A negative distance means an addr1
// hit, which is different from a missing reading. Spec S3.
static void dispFormatDist(float m, char* buf, size_t len) {
  if (m < 0.0f)        snprintf(buf, len, "via AP");
  else if (m >= 999.0f) snprintf(buf, len, ">999m");
  else if (m < 10.0f)   snprintf(buf, len, "~%.1fm", m);
  else                  snprintf(buf, len, "~%.0fm", m);
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
  if (coreRadioMode == RADIO_MODE_BLE) {
    // The BLE table's device count, with the most recent device below. An
    // accessory shows "acc", since it proves equipment and not a camera [M3].
    // BLE has no channel or distance to show.
#if HAS_GPS
    snprintf(line, sizeof(line), "det:%-3u%c gps:%c", (unsigned)coreBleDetCount,
             coreBleDetMissed ? '!' : ' ', gpsHasFix ? 'Y' : 'N');
#else
    snprintf(line, sizeof(line), "det:%-3u%c", (unsigned)coreBleDetCount,
             coreBleDetMissed ? '!' : ' ');
#endif
    u8g2.drawStr(0, ROW1, line);
    if (coreBleDetLast >= 0) {
      const BleDetection& d = coreBleDet[coreBleDetLast];
      snprintf(line, sizeof(line), "typ:%s%s", dispVendorName(d.vendor),
               d.accessory ? " acc" : "");
      u8g2.drawStr(0, ROW2, line);
      snprintf(line, sizeof(line), "rssi:%d n:%u", (int)d.rssi, (unsigned)d.count);
      u8g2.drawStr(0, ROW3, line);
      u8g2.drawStr(0, ROW4, d.key);
    } else {
      u8g2.drawStr(0, ROW2, "scanning...");
    }
    return;
  }
#if HAS_GPS
  // The GPS screen shows full GPS detail, and the Y/N flag keeps this line
  // within 128px at the widest det and channel. `ch` pads to width 2, so `gps:`
  // holds still as the channel moves between one and two digits. '!' after the
  // count marks a full detection table, and the DETECTIONS screen shows how
  // many it missed.
  snprintf(line, sizeof(line), "det:%-3d%c ch:%-2u gps:%c",
           fyDetCount, fyDroppedNew ? '!' : ' ', currentChannel,
           gpsHasFix ? 'Y' : 'N');
#else
  snprintf(line, sizeof(line), "det:%-3d%c ch:%u",
           fyDetCount, fyDroppedNew ? '!' : ' ', currentChannel);
#endif
  u8g2.drawStr(0, ROW1, line);
  if (fyDetCount > 0) {
    snprintf(line, sizeof(line), "typ:%s", dispVendorName(dispVendor));
    u8g2.drawStr(0, ROW2, line);
    snprintf(line, sizeof(line), "rssi:%d ch:%u", (int)dispRssi, dispCh);
    u8g2.drawStr(0, ROW3, line);
    char dist[10];
    dispFormatDist(dispDistM, dist, sizeof(dist));
    snprintf(line, sizeof(line), "dst:%s", dist);
    u8g2.drawStr(0, ROW4, line);
  } else {
    u8g2.drawStr(0, ROW2, "scanning...");
  }
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

// Formats uptime in at most 4 characters, the space ROW4 has left after the
// count and rate at their widest. Above 10h it drops the minutes.
static const char* shortUptime(uint32_t ms, char* buf, size_t cap) {
  const uint32_t mins = ms / 60000u;
  if      (mins < 60)   snprintf(buf, cap, "%um", (unsigned)mins);
  else if (mins < 600)  snprintf(buf, cap, "%uh%02u",
                                 (unsigned)(mins / 60), (unsigned)(mins % 60));
  else if (mins < 6000) snprintf(buf, cap, "%uh", (unsigned)(mins / 60));
  else                  snprintf(buf, cap, "%ud", (unsigned)(mins / 1440));
  return buf;
}

// Abbreviates a count to fit ROW4, keeping only its magnitude.
static const char* shortCount(uint32_t v, char* buf, size_t cap) {
  if      (v < 10000)   snprintf(buf, cap, "%u",  (unsigned)v);
  else if (v < 1000000) snprintf(buf, cap, "%uk", (unsigned)(v / 1000));
  else                  snprintf(buf, cap, "%uM", (unsigned)(v / 1000000));
  return buf;
}

static void drawDetections() {
  char line[22];
  drawScreenHeader("DETECTIONS");
  const bool bleMode = coreRadioMode == RADIO_MODE_BLE;
  if (bleMode) {
    snprintf(line, sizeof(line), "devices: %u", (unsigned)coreBleDetCount);
    u8g2.drawStr(0, ROW1, line);
    // Every BLE detection is direct, so matched advertisements take the row the
    // dir/ind split uses on 802.11.
    snprintf(line, sizeof(line), "hits:%u", (unsigned)coreBleMatched);
    u8g2.drawStr(0, ROW2, line);
    u8g2.drawStr(0, ROW3, coreBleDetLast >= 0 ? coreBleDet[coreBleDetLast].mac
                                              : "no detections yet");
  } else {
    snprintf(line, sizeof(line), "devices: %d", fyDetCount);
    u8g2.drawStr(0, ROW1, line);
    // Overlapping camera counts. A camera seen both ways counts in each, so
    // dir+ind can exceed the total above. Spec C1-C2.
    snprintf(line, sizeof(line), "dir:%u ind:%u",
             (unsigned)coreDirectDeviceCount(), (unsigned)coreIndirectDeviceCount());
    u8g2.drawStr(0, ROW2, line);
    if (fyDetCount > 0) {
      u8g2.drawStr(0, ROW3, dispMac);
    } else {
      u8g2.drawStr(0, ROW3, "no detections yet");
    }
  }
  // The overflow warning takes ROW4 when present, spec S4. Without it, the
  // `devices:` count pins at the table size and reads as a quiet area.
  const uint16_t missed = bleMode ? coreBleDetMissed : fyDroppedNew;
  if (missed) {
    snprintf(line, sizeof(line), "FULL! missed:%u", (unsigned)missed);
    u8g2.drawStr(0, ROW4, line);
  } else {
    // The one row not in device units, spec S2. It counts everything the
    // active radio hears, matched or not, with its rate and the uptime. The
    // label names the unit, since advertisements and 802.11 frames arrive at
    // very different rates.
    char sn[10], up[8];
    snprintf(line, sizeof(line), "%s:%s %u/s %s",
             bleMode ? "adv" : "seen",
             shortCount(bleMode ? coreBleReports : coreSeenFrames, sn, sizeof(sn)),
             (unsigned)(bleMode ? coreBleRate() : coreSeenRate()),
             shortUptime(millis(), up, sizeof(up)));
    u8g2.drawStr(0, ROW4, line);
  }
}

static void drawScanDetail() {
  char line[22];
  drawScreenHeader("SCAN");
  if (coreRadioMode == RADIO_MODE_BLE) {
    // Channel and dwell have no BLE reading, since the controller cycles the
    // three advertising channels itself and the scan runs at full duty. The
    // PHY split takes their place and shows whether the scanner hears coded PHY
    // at all.
    u8g2.drawStr(0, ROW1, "mode: BLE passive");
    snprintf(line, sizeof(line), "adv: %u", (unsigned)coreBleReports);
    u8g2.drawStr(0, ROW2, line);
    snprintf(line, sizeof(line), "1M:%u cod:%u",
             (unsigned)coreBlePhy1M, (unsigned)coreBlePhyCoded);
    u8g2.drawStr(0, ROW3, line);
    return;
  }
  snprintf(line, sizeof(line), "mode: %s", channelModeName());
  u8g2.drawStr(0, ROW1, line);
  snprintf(line, sizeof(line), "channel: %u", currentChannel);
  u8g2.drawStr(0, ROW2, line);
  snprintf(line, sizeof(line), "dwell: %ums", (unsigned)CHANNEL_DWELL_MS);
  u8g2.drawStr(0, ROW3, line);
}

// Scan Mode menu. '*' marks the active mode. In MENU_LIST the cursor '>'
// tracks coreMenuSel, and Single opens a channel picker.
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

// Targets menu. Picks which vendors the matchers accept, independent of Scan
// Mode, which picks where to listen. Selecting a row applies it and closes the
// menu, as on Scan Mode. Row order must match TARGET_MASKS in core.cpp.
static void drawTargets() {
  drawScreenHeader("TARGETS");
  static const char* const opts[TARGET_ROW_COUNT] = {
    "Flock", "Axon", "Motorola", "All",
  };
  int active = coreTargetIndex();          // -1 if the mask matches no row
  for (int i = 0; i < TARGET_ROW_COUNT; i++) {
    char line[22];
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    char act    = (i == active) ? '*' : ' ';
    snprintf(line, sizeof(line), "%c%c%s", cursor, act, opts[i]);
    u8g2.drawStr(0, ROW1 + i * 12, line);
  }
  // Four option rows fill the panel and leave no room for the footer.
  if (coreMenuState == MENU_NONE && TARGET_ROW_COUNT < 4)
    u8g2.drawStr(0, ROW4, "Select to change");
}

// Radio menu. Picks which radio captures, independent of Targets and Scan Mode.
// The modes are exclusive, so the row index is the RadioMode value and one row
// always shows the active mark.
static void drawRadio() {
  drawScreenHeader("RADIO");
  static const char* const opts[RADIO_MODE_COUNT] = { "2.4GHz", "BLE" };
  for (int i = 0; i < RADIO_MODE_COUNT; i++) {
    char line[22];
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    char act    = (i == (int)coreRadioMode) ? '*' : ' ';
    snprintf(line, sizeof(line), "%c%c%s", cursor, act, opts[i]);
    u8g2.drawStr(0, ROW1 + i * 12, line);
  }
  if (coreMenuState == MENU_NONE) u8g2.drawStr(0, ROW4, "Select to change");
}

// Alerts menu. Two toggles (Buzzer mute/unmute, LED on/off) plus the proximity
// ring, which drills into a range picker the way Single opens the channel
// picker. Rows show the current state, so a toggle reads unambiguously.
static void drawAlerts() {
  drawScreenHeader("ALERTS");
  char line[22];
  if (coreMenuState == MENU_PICK_PROX) {
    u8g2.drawStr(0, ROW1, "Chirp at range:");
    uint8_t m = (coreMenuSel >= 0 && coreMenuSel < PROX_RING_OPTION_COUNT)
                  ? PROX_RING_OPTIONS[coreMenuSel] : 0;
    if (m == 0) snprintf(line, sizeof(line), "  Off");
    else        snprintf(line, sizeof(line), "  %um", (unsigned)m);
    u8g2.drawStr(0, ROW2, line);
    u8g2.drawStr(0, ROW3, "Up/Dn: change");
    u8g2.drawStr(0, ROW4, "Sel: set  hold: back");
    return;
  }
  char prox[10];
  if (coreProxRingM == 0) snprintf(prox, sizeof(prox), "Off");
  else                    snprintf(prox, sizeof(prox), "%um", (unsigned)coreProxRingM);
  const char* rows[3] = { "Buzzer", "LED", "Prox" };
  const char* vals[3] = { coreBuzzerEnabled ? "Unmuted" : "Muted",
                          coreLedEnabled    ? "On"      : "Off",
                          prox };
  for (int i = 0; i < 3; i++) {
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    snprintf(line, sizeof(line), "%c%s: %s", cursor, rows[i], vals[i]);
    u8g2.drawStr(0, ROW1 + i * 12, line);
  }
  if (coreMenuState == MENU_NONE) u8g2.drawStr(0, ROW4, "Select to change");
}

// Config menu. Web Console On enters Admin (SoftAP portal), and Off closes the
// menu.
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

// Device-wipe menu. Both rows arm the three-press confirmation, so the
// operator picks the scope before anything is irreversible. See core.h.

#define WIPE_ROW1    33
#define WIPE_ROW_GAP 13
static void drawWipe() {
  drawScreenHeader("DEVICE WIPE");
  static const char* const opts[2] = { "Wipe Device", "Wipe Device + Card" };
  for (int i = 0; i < 2; i++) {
    char line[22];
    char cursor = (coreMenuState == MENU_LIST && coreMenuSel == i) ? '>' : ' ';
    snprintf(line, sizeof(line), "%c%s", cursor, opts[i]);
    u8g2.drawStr(0, WIPE_ROW1 + i * WIPE_ROW_GAP, line);
  }
}

// Wipe confirmation, drawn in place of the carousel while coreMenuState is
// MENU_CONFIRM_WIPE. It names the scope as well as the count, so an operator
// never erases the card with a gesture meant for the device alone.
static void drawWipeConfirm() {
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawFrame(2, 8, 124, 52);

  const char* scope = (coreWipeSelectedScope() == WIPE_DEVICE_AND_CARD)
                        ? "Wipe Device + Card" : "Wipe Device";
  u8g2.drawStr((128 - u8g2.getStrWidth(scope)) / 2, 22, scope);

  char prompt[22];
  snprintf(prompt, sizeof(prompt), "Press Confirm %dx", WIPE_CONFIRM_PRESSES);
  u8g2.drawStr((128 - u8g2.getStrWidth(prompt)) / 2, 36, prompt);

  // One star per confirmed press, in a larger face so the count reads at a
  // glance.
  char stars[WIPE_CONFIRM_PRESSES * 2] = "";
  int n = 0;
  for (int i = 0; i < coreWipeConfirmCount && i < WIPE_CONFIRM_PRESSES; i++) {
    if (i) stars[n++] = ' ';
    stars[n++] = '*';
  }
  stars[n] = '\0';
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.drawStr((128 - u8g2.getStrWidth(stars)) / 2, 56, stars);
  u8g2.setFont(u8g2_font_6x10_tf);
}

static void displayScreen() {
  u8g2.clearBuffer();
  // The confirmation replaces the whole carousel, so the operator cannot
  // mistake another reading for the action the presses arm.
  if (coreMenuState == MENU_CONFIRM_WIPE) {
    drawWipeConfirm();
    u8g2.sendBuffer();
    return;
  }
  switch (coreCurrentScreen) {
    case SCREEN_OVERVIEW:    drawOverview();    break;
    case SCREEN_GPS:         drawGpsDetail();   break;
    case SCREEN_DETECTIONS:  drawDetections();  break;
    case SCREEN_SCAN_DETAIL: drawScanDetail();  break;
    case SCREEN_SCAN_MODES:  drawScanModes();   break;
    case SCREEN_TARGETS:     drawTargets();     break;
    case SCREEN_RADIO:       drawRadio();       break;
    case SCREEN_ALERTS:      drawAlerts();      break;
    case SCREEN_CONFIG:      drawConfig();      break;
    case SCREEN_WIPE:        drawWipe();        break;
    default: break;
  }
  u8g2.sendBuffer();
}
