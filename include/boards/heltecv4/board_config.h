// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Board config for the Heltec WiFi LoRa 32 V4 (env:heltec_v4).
// Selected via the -I include/boards/heltecv4 build flag in platformio.ini.
#pragma once

#define BUZZER_PIN 3
#define USE_BUZZER 1

// External NeoPixel on a free GPIO since the V4 carries no onboard RGB LED.
#define LED_PIN          4
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

// GPS: Quectel L76 over UART (Serial2), receiving NMEA only. TX is wired but
// stays unused until config commands are needed. HAS_GPS=1 is safe with no
// module fitted, because the time chain falls back when no fix arrives. Set to
// 0 if your board does not have a GPS module
#define HAS_GPS          1
#define GPS_RX_PIN       39   // ESP RX  ← L76 TXD
#define GPS_TX_PIN       38   // ESP TX  → L76 RXD
#define GPS_RST_PIN      42   // GNSS_RST, active LOW, must be driven HIGH
#define GPS_WAKEUP_PIN   40   // GNSS_Wakeup
#define GPS_VGNSS_CTRL   34   // GNSS power enable, active LOW (same as VEXT_CTRL)
#define GPS_BAUD         9600
#define GPS_FIX_MAX_AGE_MS 5000

// Built-in SSD1315 OLED on GPIO17/18 I2C with GPIO21 reset. This board leaves
// no hardware I2C bus free, so OLED_HW_I2C=0 selects a software I2C driver.
#define OLED_SDA  17
#define OLED_SCL  18
#define OLED_RST  21
#define OLED_HW_I2C 0
// Vext (GPIO36) is active-LOW and drives the OLED power rail
#define VEXT_CTRL 36

// No buttons on this board.
#define HAS_BUTTONS 0

// BOOT (GPIO0) drives the Admin-mode trigger. See core.h.
#define BOOT_BTN_PIN 0

// micro SD card on SPI2 (HSPI). The V4 has no onboard slot, so these are the
// pins this build drives. Wire the card to match, or change them here. CS is
// the only required define, and MOSI/MISO/SCK can be remapped via SPI.begin().
#define USE_SD        1
#define SD_SELFTEST   0   // set 1 to run wiring verification at boot (independent of USE_SD)
#define SD_CS_PIN     5
#define SD_MOSI_PIN   6
#define SD_MISO_PIN   2
#define SD_SCK_PIN    46   // GPIO46 is a strapping pin (ROM log enable), but SCK is
                           // ESP32-driven so the line floats low at boot, which is
                           // benign. GPIO45 (VDD_SPI) would be worse.

#define MIRROR_SERIAL    1
#define MIRROR_TX_PIN    43
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

// Audio cadence: two fast ascending beeps on a new MAC, then two monotone
// heartbeat beeps every HB_BEEP_INTERVAL_MS while any target is still in range
// (seen within HB_DEVICE_ACTIVE_MS).
#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
// A MAC unheard for REDISCOVER_MS counts as a fresh discovery next time it
// appears, firing the ascending chirp again. Shorter than a Flock's burst-sleep
// gap would mean false chirps, longer would miss a drive-away and return.
// 30s sits between the two.
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
static const char* target_ssid_keywords[] = { "flock" };
static const size_t SSID_KEYWORD_COUNT = sizeof(target_ssid_keywords) / sizeof(target_ssid_keywords[0]);

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
// SD card append-only event log, one CSV row per detection event. SD_LOG_FILE
// is the pre-GPS-fix buffer. Once GPS time is anchored the log moves to a
// canonically-named file for the rest of the session.
#define SD_LOG_FILE          "/log.csv"
// Canonical post-anchor SD log filename: /bscope-M-D-YY-N.csv
#define LOG_PREFIX            "bscope-"
