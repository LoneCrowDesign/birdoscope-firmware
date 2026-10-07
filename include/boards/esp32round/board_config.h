// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for the ESP32-D0WD round-LCD board. The esp32round env selects
// it with -I include/boards/esp32round.
#pragma once

// GC9A01 240x240 round SPI TFT. Its pins come from the TFT_eSPI build_flags in
// platformio.ini.

// microSD on the TFT's SPI bus, with its own CS. The GC9A01 never reads, so
// platformio.ini sets TFT_MISO to IO2 to give the card a MISO line. IO2 is a
// strapping pin, so pull the card if uploads fail.
#define USE_SD     1
#define SD_CS_PIN  13

// ATGM336 GNSS on the FPC breakout's UART1, running always-on with no power,
// reset or wakeup pin. The board leaves its RXD unconnected, since plain NMEA
// needs only TXD to U1_RX.
#define HAS_GPS          1
#define GPS_RX_PIN       25   // U1_RX
#define GPS_TX_PIN       26   // U1_TX, unused unless sending config cmds
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
// Two full 125ms frame-burst windows per visit. A Custom rotation takes 0.75s
// and a Full Hop rotation 2.75s.
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

// Two GPIO buttons, top and bottom. The middle button is the hardware power
// switch. coreInputTick() in core.cpp handles both.
#define HAS_BUTTONS     1
#define BTN_PIN_1       19   // toggle screen
#define BTN_PIN_2       4    // manual area-of-interest marker
#define BTN_DEBOUNCE_MS 50

// BOOT (GPIO0) enters Admin mode.
#define BOOT_BTN_PIN    0

// Window in which a target counts as still in range. This board has no buzzer,
// so the value only affects the display.
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
// Prefix of every SD session directory name. coreSessionDirName() and
// roost_session.cpp build the rest.
#define LOG_PREFIX            "bscope-"

// Boot-time NTP join timeout. ntpSync() scans first, so the timeout only runs
// against a saved network in range, covering a slow join or DHCP lease.
#define NTP_JOIN_TIMEOUT_MS  10000

// Log-distance path loss model for the RSSI distance estimate. addr1 hits never
// feed it, since their RSSI measures the AP. Recalibrate after any antenna
// change. See docs/distance_estimation.md.
#define RSSI_AT_1M   -59
#define PATH_LOSS_N  2.5f

// ── Roost logging contract ─────────────────────────────────────────────────
//
// core.h includes the generated registry header, which fails the build on any
// capability or component left undeclared below. A capability states what this
// build produces, never what the silicon could. See
// vendor/jellybeans/roost_logging/registry/capabilities.toml.
#define ROOST_CAP_GNSS             HAS_GPS
#define ROOST_CAP_STORAGE          USE_SD
#define ROOST_CAP_WIFI             1
#define ROOST_CAP_WIFI_PROMISCUOUS 1
#define ROOST_CAP_WIFI_SCAN        0
#define ROOST_CAP_IE_PARSE         1
// Classic ESP32 with no Bluetooth 5 controller, so no BLE capture.
#define HAS_BLE_SCAN 0
#define ROOST_CAP_BLE              0
#define ROOST_CAP_BLE_PROMISCUOUS  0
#define ROOST_CAP_TARGET_MATCH     1
#define ROOST_CAP_OPERATOR_MARK    HAS_BUTTONS

#define ROOST_COMPONENTS(X)                                                    \
  X(WIFI0, "wifi0", WIFI, "ESP32-D0WD", ROOST_BAND_REACH_2_4)                       \
  X(GNSS0, "gnss0", GNSS, "ATGM336", 0)                                        \
  X(SYS,   "sys",   SYSTEM, NULL, 0)

// auth_mode has no producer in this build. Excluding it makes the manifest
// state it goes unrecorded, per roost_logging design_spec 7.1.
#define ROOST_WIFI_OBS_EXCLUDE (ROOST_F(ROOST_WIFI_OBS_AUTH_MODE))
