// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for the Birdoscope Analyze r0.2 bare (ESP32-S3). It carries an
// ESP32-S3-WROOM-1U N16R8 with a USB-C receptacle, data to native USB pins and
// a 4-pin UART0 header. 
//
// env:analyze_r02_n16r8_bare. Same peripherals as env:analyze_r02_n16r8_dev
//
// Every peripheral keeps its r0.2 dev GPIO except the button row, which are wired
// in the opposite order. See docs/hardware/hardware_analyze_r02_bare_esp32s3.md.
//
// Selected via the -I include/boards/analyze_r02_bare_esp32s3 build flag in
// platformio.ini.
#pragma once

#define BUZZER_PIN 4
#define USE_BUZZER 1

// External WS2812B RGB LED on GPIO2. The r0.1 and r0.2 PCBs both drive an
// external pixel rather than the module's onboard GPIO48 one, on the same GPIO.
#define LED_PIN          2
#define USE_LED          1
#define LED_FLASH_MS     120
// repeat detection (default)
#define LED_COLOR_R      0
#define LED_COLOR_G      180
#define LED_COLOR_B      0
// new detection / rediscovery
#define LED_COLOR_NEW_R  180
#define LED_COLOR_NEW_G  0
#define LED_COLOR_NEW_B  0
// heartbeat pulse
#define LED_COLOR_HB_R   80
#define LED_COLOR_HB_G   0
#define LED_COLOR_HB_B   80
// startup confirmation
#define LED_COLOR_BOOT_R 255
#define LED_COLOR_BOOT_G 255
#define LED_COLOR_BOOT_B 255

// GPS: ATGM336, a bare module carrying VCC/GND/TXD/RXD only and running
// always-on 9600 baud NMEA.
//
// Both pin names are for the ESP32 side, not the GPS module side, and the
// UART crosses over:
//   GPS_RX_PIN  the ESP32 receives here
//   GPS_TX_PIN  the ESP32 transmits here

#define HAS_GPS          1
#define GPS_SERIAL       Serial1
#define GPS_RX_PIN       17   // ESP RX  ← ATGM336 TXD
#define GPS_TX_PIN       16   // ESP TX  → ATGM336 RXD
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

// I2C OLED with an SH1106 controller (u8g2-compatible). OLED_SH1106 selects a
// driver that applies the 2px RAM column offset this controller needs. 
// No 180 flip required.

#define OLED_SDA     8
#define OLED_SCL     9
#define OLED_RST     10
#define OLED_HW_I2C  1
#define OLED_SH1106  1

// The serial log lists every device answering on the bus at boot. Set 
// this to what it finds.

#define OLED_I2C_ADDR 0x3C
#define OLED_ROTATION U8G2_R0

// Retires the 3BTN nav scheme for 4BTN with dedicated back button.
// K1 - Up K2 - Down K3 - Confirm K4 - Back
//
// The bare layout runs the module's button pins to GPIO42/41/40/39, the reverse
// of the r0.2 dev board on the same connector. The pins below are ordered by
// button, not by GPIO.

#define HAS_BUTTONS     1
#define NAV_SCHEME_4BTN 1
#define BTN_PIN_1       42
#define BTN_PIN_2       41
#define BTN_PIN_3       40
#define BTN_PIN_4       39
#define BTN_DEBOUNCE_MS 50

// No button-bound Admin trigger (inaccessible). Admin is entered from the 
// Web Config screen
#define BOOT_ADMIN_TRIGGER 0

// micro SD card on its own SPI bus.
#define USE_SD        1
#define SD_SELFTEST   0
// VFS open-file limit, passed to SD.begin(). Covers wifi_obs and gps_track held
// open, one transient, and spares. Each open file costs internal heap, and
// SD.open() aborts the firmware with std::bad_alloc when that heap runs out.
#define SD_MAX_OPEN_FILES 6
#define SD_CS_PIN     5
#define SD_MOSI_PIN   11
#define SD_MISO_PIN   13
#define SD_SCK_PIN    12

// No debug mirror. This layout does not bring GPIO15 out, and UART0 has its own
// header.
#define MIRROR_SERIAL    0
#define MIRROR_BAUD      115200

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
// Two full 125ms frame-burst windows per visit. Custom covers 3 channels in
// 0.75s, Full Hop 11 in 2.75s.
#define CHANNEL_DWELL_MS 250
#define SINGLE_CHANNEL 1

// Descending, counter-rotating against a station's usual ascending 1->11 scan so
// the two sweeps cross. Order only, same coverage and dwell.
static const uint8_t customChannels[]  = {11, 6, 1};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

static const uint8_t fullHopChannels[] = {1,2,3,4,5,6,7,8,9,10,11};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

#define HEARTBEAT_MS    30000
// Deliberately below the radio's usable floor, trading noise frames for the
// marginal tail of real detections. See docs/detection_methods.md, receiver
// sensitivity floor.
#define RSSI_MIN        -100
#define ALERT_COOLDOWN_MS 5000

#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
#define REDISCOVER_MS          30000
// Tones for this board's 12mm piezo, transposed down from the 9mm-tuned
// originals by (9/12)² ≈ 0.5625. Holds the intervals and moves off the 12mm's
// resonant peak. Nudge 0.55 to 0.62 by ear, lower is mellower.
#define NEW_CHIRP_LO_HZ        1125   // 2000 × (9/12)²
#define NEW_CHIRP_HI_HZ        1575   // 2800 × (9/12)²
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             844    // 1500 × (9/12)²
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

// Off. Keyword matching needs observed names and there are none. A directed
// probe from a target OUI already logs its SSID, see wifiSniffer.
#define ENABLE_SSID_MATCH 0
#define CHECK_ADDR1 1   // dst/rx, catches Flock STAs receiving probe responses
#define CHECK_ADDR3 0   // bssid fallback for randomised addr2

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// ── Roost logging contract ─────────────────────────────────────────────────
//
// core.h includes the generated registry header, which hard-errors on any
// capability or component left undeclared below. A capability answers what this
// build can produce, never what the silicon could. See
// vendor/jellybeans/roost_logging/registry/capabilities.toml.
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

// One entry per physical capture component. Every row's cap_component names one
// of these, and the manifest renders from the list. `bands` is immutable reach,
// not the channel plan in effect, so "no 5 GHz rows" reads as a hardware limit.
#define ROOST_COMPONENTS(X)                                                    \
  X(WIFI0, "wifi0", WIFI, "ESP32-S3", ROOST_BAND_REACH_2_4)                    \
  X(GNSS0, "gnss0", GNSS, "ATGM336", 0)                                        \
  X(SYS,   "sys",   SYSTEM, NULL, 0)

// Reachable through ie_parse, no producer in this build. Declaring them makes
// the manifest say this build does not record them (spec 7.1). Migration S2.
//
// auth_mode has no RSN element parser here. The IE fingerprint four have a
// walker in roost_ie.h, but carrying its output to the queue drain widens every
// alert entry and needs a queue-sizing decision first, Migration P5. Remove each
// from this list when its producer is wired.
#define ROOST_WIFI_OBS_EXCLUDE          \
  (ROOST_F(ROOST_WIFI_OBS_AUTH_MODE)    \
   | ROOST_F(ROOST_WIFI_OBS_CAP_INFO)          \
   | ROOST_F(ROOST_WIFI_OBS_BEACON_INTERVAL)   \
   | ROOST_F(ROOST_WIFI_OBS_IE_IDS)            \
   | ROOST_F(ROOST_WIFI_OBS_VENDOR_IES))

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define AUTOSAVE_INTERVAL_MS 15000
#define SD_LOG_FILE          "/log.csv"
// Canonical post-anchor SD log filename, /bscope-M-D-YY-N.csv
#define LOG_PREFIX            "bscope-"
