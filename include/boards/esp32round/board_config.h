// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for the ESP32-D0WD round-LCD board (env:esp32round).
// Selected via the -I include/boards/esp32round build flag in platformio.ini.
#pragma once

// GC9A01 240x240 round SPI TFT. TFT_MOSI/TFT_SCLK/TFT_CS/TFT_DC/TFT_RST/
// TFT_BL come from build_flags (User_Setup via TFT_eSPI's USER_SETUP_LOADED)
// in platformio.ini. Shares the SPI bus with the microSD slot.

// micro SD on the same SPI bus as the TFT (SCK=TFT_SCLK, MOSI=TFT_MOSI, plus
// MISO via TFT_MISO on IO2, wired in platformio.ini build_flags so the shared
// bus has a MISO line for the card, since the GC9A01 never reads over SPI).
// Only CS differs. IO2 is a strapping pin, so an inserted card can interfere
// with flashing. Pull the card before uploading if uploads fail.
#define USE_SD     1
#define SD_CS_PIN  13

// ATGM336 GNSS module on the FPC breakout's UART1. Module TXD reaches U1_RX
// (IO25). Module RXD is left unconnected, which plain NMEA output does not
// need. Neither the board nor the module exposes power, reset, or wakeup
// control pins, so the module runs always-on.
#define HAS_GPS          1
#define GPS_RX_PIN       25   // U1_RX
#define GPS_TX_PIN       26   // U1_TX, unused unless sending config cmds
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
// 250ms gives two full 125ms frame-burst windows per visit while keeping the
// rotation short: 3 channels in 0.75s on Custom, 11 in 2.75s on Full Hop.
#define CHANNEL_DWELL_MS 250
#define SINGLE_CHANNEL 1

static const uint8_t customChannels[]  = {1, 6, 11};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

static const uint8_t fullHopChannels[] = {1,2,3,4,5,6,7,8,9,10,11};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

#define HEARTBEAT_MS    30000
#define RSSI_MIN        -95
#define RSSI_MAX        -30   // gauge ceiling only, does not gate capture
#define ALERT_COOLDOWN_MS 5000

// Two GPIO buttons, top (IO19) and bottom (IO4). The middle physical button is
// the hardware power switch and is not GPIO-controlled. The names stay abstract
// (BTN_PIN_1/BTN_PIN_2 rather than TOP_/BOTTOM_) because physical placement
// varies by board. Behaviour lives in core's input module.
#define HAS_BUTTONS     1
#define BTN_PIN_1       19   // toggle screen
#define BTN_PIN_2       4    // manual area-of-interest marker
#define BTN_DEBOUNCE_MS 50

// BOOT (GPIO0) drives the Admin-mode trigger. No dedicated pin, since BOOT is
// idle once startup finishes.
#define BOOT_BTN_PIN    0

// "Still in range" window: a target counts as actively present if it was seen
// within this many ms. This board carries no buzzer, so the value only affects
// what the display shows.
#define HB_DEVICE_ACTIVE_MS    3000

#define CHECK_ADDR1 1   // dst/rx, catches Flock STAs receiving probe responses
#define CHECK_ADDR3 0   // bssid fallback for randomised addr2
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define AUTOSAVE_INTERVAL_MS 15000
// SD card append-only event log, one row per raw detection. The timestamp
// column carries real UTC once the ATGM336 gets a fix, and millis() since boot
// before that.
#define SD_LOG_FILE          "/log.csv"
// Canonical post-anchor SD log filename: /bscope-M-D-YY-N.csv
#define LOG_PREFIX            "bscope-"

// Boot-time NTP-bridge join timeout. ntpSync() scans before joining, so an
// out-of-range saved network costs only the scan and never reaches this
// timeout. The full 10s therefore covers a slow join or DHCP lease on a
// network that is present.
#define NTP_JOIN_TIMEOUT_MS  10000

// RSSI to distance estimate (log-distance path loss model). Only meaningful
// for addr2/wildcard-probe hits. addr1 RSSI reflects AP-to-scanner path loss
// rather than target-to-scanner, so it never feeds the estimate. You must test
// this any time you change the antenna.
#define RSSI_AT_1M   -59
#define PATH_LOSS_N  2.5f

// ── Roost logging contract ─────────────────────────────────────────────────
//
// core.h includes the generated registry header, which hard-errors on any
// capability or component left undeclared here. A capability answers what this
// build can produce, never what the silicon could.
// Rationale: vendor/jellybeans/roost_logging/registry/capabilities.toml.
#define ROOST_CAP_GNSS             HAS_GPS
#define ROOST_CAP_STORAGE          USE_SD
#define ROOST_CAP_WIFI             1
#define ROOST_CAP_WIFI_PROMISCUOUS 1
#define ROOST_CAP_WIFI_SCAN        0
#define ROOST_CAP_IE_PARSE         1
#define ROOST_CAP_BLE              0
#define ROOST_CAP_BLE_PROMISCUOUS  0
#define ROOST_CAP_TARGET_MATCH     1
#define ROOST_CAP_OPERATOR_MARK    HAS_BUTTONS

#define ROOST_COMPONENTS(X)                                                    \
  X(WIFI0, "wifi0", WIFI, "ESP32-D0WD", ROOST_BAND_REACH_2_4)                       \
  X(GNSS0, "gnss0", GNSS, "ATGM336", 0)                                        \
  X(SYS,   "sys",   SYSTEM, NULL, 0)

// auth_mode has no producer in this build. Excluded rather than emitted empty,
// so the manifest records that it is not captured. Migration S2.
#define ROOST_WIFI_OBS_EXCLUDE (ROOST_F(ROOST_WIFI_OBS_AUTH_MODE))
