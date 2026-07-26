// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for an ESP-32-S3-N16R8 or N8R2 (low-cost alternative). Pins
// here match the schematics found in the birdoscope-hardware repo for board
// revision 0.1 of the analyzer model.
//
// Shared by two environments, esp32-s3-devkitc1-n16r8 and
// esp32-s3-devkitc1-n8r2, which differ only in on-module flash and PSRAM. Both
// select this file with the -I include/boards/esp32s3_devkitc1 build flag in
// platformio.ini, so an edit here changes both boards.
#pragma once

#define BUZZER_PIN 4
#define USE_BUZZER 1

// Onboard WS2812B RGB LED (GPIO48), so no external NeoPixel is needed.
#define LED_PIN          48
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
// always-on. It exposes no power, reset, or wakeup control lines.
//
// Both pin names describe the ESP32 side, not the GPS module side, and the
// UART crosses over:
//   GPS_RX_PIN  the ESP32 receives here, so wire it to the module's TXD
//   GPS_TX_PIN  the ESP32 transmits here, so wire it to the module's RXD
// Wiring RX to RX yields no NMEA at all. This order matches the argument order
// of HardwareSerial::begin(baud, config, rx, tx).
#define HAS_GPS          1
#define GPS_SERIAL       Serial1
#define GPS_RX_PIN       16   // ESP RX  ← ATGM336 TXD
#define GPS_TX_PIN       17   // ESP TX  → ATGM336 RXD
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

// I2C OLED (SSD1306/SSD1315-class, u8g2-compatible) on the hardware I2C bus.
// Generic modules ship with either controller and the two are drop-in
// compatible under u8g2. Set OLED_RST to U8X8_PIN_NONE for a module that
// exposes no reset line.
#define OLED_SDA     8
#define OLED_SCL     9
#define OLED_RST     10
#define OLED_HW_I2C  1

// Plain debounced buttons on two spare GPIOs. The names stay abstract
// (BTN_PIN_1/BTN_PIN_2) because physical placement varies by board.
#define HAS_BUTTONS     1
#define BTN_PIN_1       1
#define BTN_PIN_2       2
#define BTN_DEBOUNCE_MS 50

// BOOT (GPIO0) drives the Admin-mode trigger. No dedicated pin, since BOOT is
// idle once startup finishes.
#define BOOT_BTN_PIN    0

// micro SD card on its own SPI bus, shared with nothing else on this board, so
// card access never contends with another peripheral.
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
#define CHANNEL_DWELL_MS 350
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
#define NEW_CHIRP_LO_HZ        2000
#define NEW_CHIRP_HI_HZ        2800
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             1500
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
