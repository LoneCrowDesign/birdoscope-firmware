// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for the Birdoscope Analyze r0.1 (ESP32-S3), a custom carrier
// board that takes an ESP32-S3 DevKitC-1 module. Built by env:analyze_r01_n8r2
// or env:analyze_r01_n16r8, which differ only in the populated module's flash
// and PSRAM. Notable features:
//   * SH1106 OLED, mounted upside-down, requiring OLED_SH1106 lib
//     plus a flip in code: OLED_ROTATION = U8G2_R2 (180°)
//   * Three onboard buttons, driving the semantic nav scheme.
//
// docs/hardware/hardware_analyze_r01_esp32s3.md carries the full PCB pinout from
// the schematic as manufactured.
//
// Selected via the -I include/boards/analyze_r01_esp32s3 build flag in
// platformio.ini.
#pragma once

#define BUZZER_PIN 4
#define USE_BUZZER 1

// External WS2812B RGB LED on GPIO2. The r0.1 PCB drives an external pixel
// rather than the module's onboard GPIO48 one.
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
// always-on. It exposes no reset, wakeup, or power-enable lines. 9600 baud
// NMEA.
//
// Both pin names describe the ESP32 side, not the GPS module side, and the
// UART crosses over:
//   GPS_RX_PIN  the ESP32 receives here, so wire it to the module's TXD
//   GPS_TX_PIN  the ESP32 transmits here, so wire it to the module's RXD
// Wiring RX to RX yields no NMEA at all. This order matches the argument order
// of HardwareSerial::begin(baud, config, rx, tx).
#define HAS_GPS          1
#define GPS_SERIAL       Serial1
#define GPS_RX_PIN       17   // ESP RX  ← ATGM336 TXD
#define GPS_TX_PIN       16   // ESP TX  → ATGM336 RXD
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

// I2C OLED with an SH1106 controller (u8g2-compatible), mounted upside-down on
// this PCB. OLED_SH1106 selects a driver that applies the 2px RAM column offset
// this controller needs and then clears it. Driving an SH1106 through an
// SSD1306 driver instead leaves that 2px shift visible. OLED_ROTATION turns the
// framebuffer 180° for the upside-down mount. u8g2 applies the column offset
// independently of rotation, so R2 does not reintroduce the shift.
#define OLED_SDA     8
#define OLED_SCL     9
#define OLED_RST     10
#define OLED_HW_I2C  1
#define OLED_SH1106  1
#define OLED_ROTATION U8G2_R2

// Three buttons on GPIO40/41/42, running the semantic nav scheme
// (NAV_SCHEME_3BTN) rather than the legacy toggle/mark inputs. The gesture
// grammar lives in docs/menu_ux.md. No button can sit on GPIO2, which drives
// this board's external LED (LED_PIN=2).
#define HAS_BUTTONS     1
#define NAV_SCHEME_3BTN 1
#define BTN_PIN_1       40
#define BTN_PIN_2       41
#define BTN_PIN_3       42
#define BTN_DEBOUNCE_MS 50

// BOOT (GPIO0) drives the Admin-mode trigger, retained until BTN_PIN_3 takes
// over the gesture on this controls-equipped board.
#define BOOT_BTN_PIN    0

// micro SD card on its own SPI bus.
#define USE_SD        1
#define SD_SELFTEST   0
#define SD_CS_PIN     5
#define SD_MOSI_PIN   11
#define SD_MISO_PIN   13
#define SD_SCK_PIN    12

// Debug mirror on a separate UART from GPS.
#define MIRROR_SERIAL    1
#define MIRROR_TX_PIN    15
#define MIRROR_BAUD      115200

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
#define ALERT_COOLDOWN_MS 5000

#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
#define REDISCOVER_MS          30000
// Buzzer tones scaled for this board's 12mm piezo. The originals were tuned for
// a 9mm element. A 12mm diaphragm resonates lower (bending-disc resonance ∝
// 1/diameter²) and sits closer to these frequencies, which makes the 9mm-tuned
// tones louder and harsher. Transposing every tone down by the same factor,
// (9/12)² ≈ 0.5625, preserves the original intervals while mellowing the pitch
// and moving off the 12mm's resonant peak. Nudge 0.55–0.62 by ear if needed,
// where lower is mellower.
#define NEW_CHIRP_LO_HZ        1125   // 2000 × (9/12)²
#define NEW_CHIRP_HI_HZ        1575   // 2800 × (9/12)²
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             844    // 1500 × (9/12)²
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

#define ENABLE_SSID_MATCH 0
#define CHECK_ADDR1 1   // dst/rx, catches Flock STAs receiving probe responses
#define CHECK_ADDR3 0   // bssid fallback for randomised addr2

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define AUTOSAVE_INTERVAL_MS 15000
#define SD_LOG_FILE          "/log.csv"
// Canonical post-anchor SD log filename: /bscope-M-D-YY-N.csv
#define LOG_PREFIX            "bscope-"
