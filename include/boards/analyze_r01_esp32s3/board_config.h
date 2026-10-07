// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for the Birdoscope Analyze r0.1 (ESP32-S3), a carrier board for
// an ESP32-S3 DevKitC-1 module with an SH1106 OLED and three buttons.
// The analyze_r01_n8r2 and analyze_r01_n16r8 envs select it with
// -I include/boards/analyze_r01_esp32s3, and differ only in the module's flash
// and PSRAM.
//
// docs/hardware/hardware_analyze_r01_esp32s3.md holds the full PCB pinout from
// the schematic as manufactured.
#pragma once

// Board revision the roost session manifest reports.
#define HW_REVISION "HWr0.1"

#define BUZZER_PIN 4
#define USE_BUZZER 1

// External WS2812B pixel. The firmware leaves the module's GPIO48 pixel unused.
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

// ATGM336 GPS, a bare VCC/GND/TXD/RXD module running always-on NMEA. Pin names
// follow the ESP32 side.

#define HAS_GPS          1
#define GPS_SERIAL       Serial1
#define GPS_RX_PIN       17   // ESP RX  ← ATGM336 TXD
#define GPS_TX_PIN       16   // ESP TX  → ATGM336 RXD
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

// I2C OLED with an SH1106 controller, mounted upside down, so OLED_ROTATION
// flips it 180°. OLED_SH1106 selects its driver, see main_oled.cpp.

#define OLED_SDA     8
#define OLED_SCL     9
#define OLED_RST     10
#define OLED_HW_I2C  1
#define OLED_SH1106  1
#define OLED_ROTATION U8G2_R2

// Three-button nav, with K1 to K3 on BTN_PIN_1 to BTN_PIN_3. core.h holds the
// button map.

#define HAS_BUTTONS     1
#define NAV_SCHEME_3BTN 1
#define BTN_PIN_1       40
#define BTN_PIN_2       41
#define BTN_PIN_3       42
#define BTN_DEBOUNCE_MS 50

// BOOT (GPIO0) enters Admin mode.
#define BOOT_BTN_PIN    0

// microSD card on its own SPI bus.
#define USE_SD        1
// VFS open-file limit, passed to SD.begin(). The roost writer holds every
// declared record file open, six with BLE, and the manifest snapshot and the
// radio bench each need one more. roost_session.cpp fails the build if this
// leaves the writer short. Each open file costs internal heap, and SD.open()
// aborts with std::bad_alloc when that heap runs out.
#define SD_MAX_OPEN_FILES 8
#define SD_CS_PIN     5
#define SD_MOSI_PIN   11
#define SD_MISO_PIN   13
#define SD_SCK_PIN    12

// Debug mirror on its own UART.
#define MIRROR_SERIAL    1
#define MIRROR_TX_PIN    15
#define MIRROR_BAUD      115200

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
// Two full 125ms frame-burst windows per visit. A Custom rotation takes 0.75s
// and a Full Hop rotation 2.75s.
#define CHANNEL_DWELL_MS 250
#define SINGLE_CHANNEL 1

// Descending, so the sweep crosses a station's usual ascending 1 to 11 scan.
// Only the order differs from 1, 6, 11.
static const uint8_t customChannels[]  = {11, 6, 1};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

static const uint8_t fullHopChannels[] = {1,2,3,4,5,6,7,8,9,10,11};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

#define HEARTBEAT_MS    30000
// Deliberately below the radio's usable floor, to keep the marginal tail of
// real detections at the cost of noise frames. See docs/detection_methods.md,
// Receiver Sensitivity Floor.
#define RSSI_MIN        -100
#define ALERT_COOLDOWN_MS 5000

#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
#define REDISCOVER_MS          30000
// Tones for this board's 12mm piezo, the 9mm-tuned originals scaled by
// (9/12)² ≈ 0.5625. Scaling keeps the intervals and moves the tones off the
// 12mm's resonant peak. Tune the factor between 0.55 and 0.62 by ear, lower is
// mellower.
#define NEW_CHIRP_LO_HZ        1125   // 2000 × (9/12)²
#define NEW_CHIRP_HI_HZ        1575   // 2800 × (9/12)²
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             844    // 1500 × (9/12)²
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

// Off until field captures supply target SSIDs to match. A directed probe from
// a target OUI already logs its SSID, see wifiSniffer.
#define ENABLE_SSID_MATCH 0
#define CHECK_ADDR1 1   // dst/rx, catches Flock STAs receiving probe responses
#define CHECK_ADDR3 0   // bssid fallback for randomised addr2

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

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
#define ROOST_CAP_BLE              1
#define ROOST_CAP_BLE_PROMISCUOUS  1
#define ROOST_CAP_TARGET_MATCH     1
#define ROOST_CAP_OPERATOR_MARK    HAS_BUTTONS

// One entry per physical capture component. Every row's cap_component names one
// of these, and the manifest renders from the list. The band field states the
// hardware's reach, independent of the channel plan in effect, so a manifest
// without 5 GHz reads as a hardware limit.
#define ROOST_COMPONENTS(X)                                                    \
  X(WIFI0, "wifi0", WIFI, "ESP32-S3", ROOST_BAND_REACH_2_4)                    \
  X(BLE0,  "ble0",  BLE,  "ESP32-S3", ROOST_BAND_REACH_2_4)                    \
  X(GNSS0, "gnss0", GNSS, "ATGM336", 0)                                        \
  X(SYS,   "sys",   SYSTEM, NULL, 0)

// Fields ie_parse could reach that this build does not produce. Excluding them
// makes the manifest state they go unrecorded, per roost_logging design_spec
// 7.1. auth_mode has no RSN element parser. The four IE fingerprint fields have
// a walker in roost_ie.h, but passing its output to the queue drain widens
// every alert entry and needs the alert queue resized first. Remove each field
// from this list once a producer fills it.
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
// Prefix of every SD session directory name. coreSessionDirName() and
// roost_session.cpp build the rest.
#define LOG_PREFIX            "bscope-"
