// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Arduino.h comes first, since board_config.h uses uint8_t and size_t.
// board_config.h comes before core.h, or core.h's #ifndef fallbacks for USE_SD
// and HAS_GPS lock in 0 on a board that sets them.
#include <Arduino.h>
#include "board_config.h"
#include "core.h"
#include "ble_ad.h"
#include "mac_limit.h"
#include "roost_session.h"
#include "esp_event.h"   // esp_event_loop_create_default() for coreWifiSnifferStart()
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <SD.h>
#if HAS_BLE_SCAN
#include <NimBLEDevice.h>
#endif
#include <ArduinoJson.h>   // wifi-creds file parse/serialize (SSID is user bytes)
#include <Preferences.h>   // boot_count, which must survive a power cut
#include "esp_mac.h"       // esp_read_mac() for own_macs and device_serial
#include "esp_event.h"
#include "nvs_flash.h"     // whole-partition erase, in coreDeviceWipe()
#include "esp_sleep.h"     // deep sleep, in corePowerOff()
#include "esp_heap_caps.h" // per-radio buffers, allocated while that radio runs

#ifndef MIRROR_SERIAL
#define MIRROR_SERIAL 0
#endif
#ifndef ENABLE_SSID_MATCH
#define ENABLE_SSID_MATCH 0
#endif
#ifndef REDISCOVER_MS
#define REDISCOVER_MS 30000
#endif
#ifndef HB_BEEP_INTERVAL_MS
#define HB_BEEP_INTERVAL_MS 10000
#endif
#ifndef HAS_GPS
#define HAS_GPS 0
#endif

// The boot-time NTP fallback needs no board opt-in. It joins WiFi only when GPS
// is absent and the web console has stored credentials.
#ifndef WIFI_CREDS_FILE
#define WIFI_CREDS_FILE "/wifi.json"
#endif
// The web console writes persisted tuning here.
#ifndef SETTINGS_FILE
#define SETTINGS_FILE "/settings.json"
#endif

// Distance-model tuning. RSSI_AT_1M seeds the runtime reference `calibrate`
// tunes. See docs/distance_estimation.md.
#ifndef RSSI_AT_1M
#define RSSI_AT_1M  -45
#endif
#ifndef PATH_LOSS_N
#define PATH_LOSS_N 2.5f
#endif
// Environment Density presets, the path-loss exponent for each setting. Medium
// defers to PATH_LOSS_N so a board that tuned that value keeps it.
#ifndef PATH_LOSS_N_LOW
#define PATH_LOSS_N_LOW  2.0f    // open ground, near line of sight
#endif
#ifndef PATH_LOSS_N_MED
#define PATH_LOSS_N_MED  PATH_LOSS_N
#endif
#ifndef PATH_LOSS_N_HIGH
#define PATH_LOSS_N_HIGH 3.5f    // dense urban, heavy obstruction
#endif
// Proximity-alert defaults. See the PROXIMITY ALERT section and docs/alerts.md.
#ifndef PROX_RING_M
#define PROX_RING_M 25        // default ring in metres, 0 disables
#endif
#ifndef PROX_HYST_PCT
#define PROX_HYST_PCT 130     // clear the latch beyond this percent of the ring
#endif
#ifndef PROX_EMA_SHIFT
#define PROX_EMA_SHIFT 2      // alpha = 1/4, tracks a moving vehicle and ignores a null
#endif

// Time limit on joining the saved network. Past it, timestamps count from
// boot.
#ifndef NTP_JOIN_TIMEOUT_MS
#define NTP_JOIN_TIMEOUT_MS 10000
#endif
// Time coreTimeSync() waits for a checksum-valid NMEA sentence before it treats
// the GPS module as absent and falls through to NTP.
#ifndef GPS_PRESENCE_PROBE_MS
#define GPS_PRESENCE_PROBE_MS 3000
#endif

// ============================================================
// BUILD IDENTITY
// ============================================================

const char* coreBuildRev() { return BIRDOSCOPE_GIT_REV; }

const char* coreBuildIdentity() {
  static char buf[96];
  if (buf[0] == '\0') {
    snprintf(buf, sizeof(buf), "v%s %s committed %s, built %s",
             BIRDOSCOPE_VERSION, BIRDOSCOPE_GIT_REV, BIRDOSCOPE_GIT_DATE,
             BIRDOSCOPE_BUILD_TS);
  }
  return buf;
}

// ============================================================
// TARGET OUI TABLE. Shared target data, not board config.
//
// Flock entries contributed by @NitekryDPaul and Michael/DeFlockJoplin field
// research, https://github.com/DeflockJoplin/flock-you
// Axon entries come from the IEEE registry (https://standards-oui.ieee.org),
// covering the Axon Enterprise, VieVu and Fusus assignments.
//
// matchOuiRaw() runs from the WiFi promiscuous callback and must read the table
// while SPIFFS writes have flash unmapped. DRAM_ATTR places it in .dram0.data.
// Keep DRAM_ATTR and leave off const, since GCC moves a never-written static
// array into flash .rodata without it.
// ============================================================

// `nibbles` counts the significant hex digits of the prefix.
//   6 = MA-L, a 24-bit OUI, b[3] unused
//   7 = MA-M, a 28-bit prefix, high nibble of b[3] significant
typedef struct {
  uint8_t b[4];
  uint8_t nibbles;
  uint8_t vendor;
  uint8_t cls;      // OuiClass, how far a match on the entry counts
} OuiEntry;

static DRAM_ATTR OuiEntry oui_table[] = {
  // --- Flock Safety ---
  // The only block IEEE assigns to Flock Safety. Every other Flock prefix
  // belongs to a module vendor and can match third-party hardware. See
  // docs/detection_methods.md, "Target OUI table provenance".
  {{0xB4,0x1E,0x52,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},

  // Original flock-you findings from @NitekryDPaul
  {{0x70,0xC9,0x4E,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x3C,0x91,0x80,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0xD8,0xF3,0xBC,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x80,0x30,0x49,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0xB8,0x35,0x32,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x14,0x5A,0xFC,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x74,0x4C,0xA1,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x08,0x3A,0x88,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x9C,0x2F,0x9D,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0xC0,0x35,0x32,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x94,0x08,0x53,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0xE4,0xAA,0xEA,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0xF4,0x6A,0xDD,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0xE0,0x0A,0xF6,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x24,0xB2,0xB9,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x00,0xF4,0x8D,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0xD0,0x39,0x57,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0xE8,0xD0,0xFC,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0xE0,0x4F,0x43,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0xB8,0x1E,0xA4,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x70,0x08,0x94,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x58,0x8E,0x81,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0xEC,0x1B,0xBD,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x3C,0x71,0xBF,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x58,0x00,0xE3,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x90,0x35,0xEA,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x5C,0x93,0xA2,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0x64,0x6E,0x69,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  {{0x14,0xB5,0xCD,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  // Espressif variants the source marks low confidence. A match on either is
  // weaker evidence than the rest of this block.
  {{0x48,0x27,0xEA,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY}, {{0xA4,0xCF,0x12,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
  // Qualcomm Atheros `00:03:7f` appears in Flock radio firmware blobs and is
  // deliberately absent. It is Atheros' own MA-L and would match every Atheros
  // radio in range, so it can only ever serve as a secondary attribute.

  // Locally administered and unregistered with IEEE, consistent with a derived
  // virtual-interface MAC. Michael/DeFlockJoplin field-tested it, and live data
  // has not shown it since. See g_haveLaaTargets.
  {{0x82,0x6B,0xF2,0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},

  // --- Axon Enterprise and acquired brands ---
  {{0x00,0x25,0xDF,0}, 6, VENDOR_AXON, OUI_CLASS_PRIMARY},   // Axon Enterprise (was TASER Intl)
  {{0xFC,0x01,0x9E,0}, 6, VENDOR_AXON, OUI_CLASS_PRIMARY},   // VieVu, acquired 2018
  {{0x7C,0x83,0x34,0x40}, 7, VENDOR_AXON, OUI_CLASS_PRIMARY},// Fusus MA-M /28, acquired 2024
  {{0x84,0xB3,0x86,0x50}, 7, VENDOR_AXON, OUI_CLASS_PRIMARY},// Fusus MA-M /28

  // --- Axis Communications ---
  // Surveillance cameras. Expect false positives from commercial installs until
  // a larger dataset verifies the tag.
  {{0x00,0x40,0x8C,0}, 6, VENDOR_AXIS, OUI_CLASS_PRIMARY}, {{0xAC,0xCC,0x8E,0}, 6, VENDOR_AXIS, OUI_CLASS_PRIMARY},
  {{0xB8,0xA4,0x4F,0}, 6, VENDOR_AXIS, OUI_CLASS_PRIMARY}, {{0xE8,0x27,0x25,0}, 6, VENDOR_AXIS, OUI_CLASS_PRIMARY},

  // --- Utility, Inc ---
  // BodyWorn and in-car law-enforcement video, under the vendor's own
  // registrations.
  {{0x00,0x09,0xBC,0}, 6, VENDOR_UTILITY, OUI_CLASS_PRIMARY}, {{0x00,0x16,0xED,0}, 6, VENDOR_UTILITY, OUI_CLASS_PRIMARY},

  // --- Motorola Solutions family ---
  // Verified against the 2026-08-07 IEEE registry and the analytics registry
  // mirror. The parent's own blocks are infra, since their install
  // base is mostly radio. Analytics uses them, spec M2.
  {{0x00,0x04,0x7D,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  {{0x00,0x18,0x85,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  {{0x00,0x1F,0x92,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  {{0x4C,0xCC,0x34,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  {{0x10,0x74,0x6F,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  {{0x9C,0x86,0x2B,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  {{0xB8,0xE2,0x8C,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_INFRA},
  // Acquired surveillance product lines under the same parent.
  {{0x70,0x1A,0xD5,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_PRIMARY},  // Avigilon Alta
  {{0x00,0x1D,0x96,0}, 6, VENDOR_MOTOROLA, OUI_CLASS_PRIMARY},  // WatchGuard Video

  // --- Other surveillance vendors, own registrations ---
  {{0xE0,0xA7,0x00,0}, 6, VENDOR_VERKADA, OUI_CLASS_PRIMARY},
  {{0x00,0x0A,0xB1,0}, 6, VENDOR_GENETEC, OUI_CLASS_PRIMARY},
  {{0x00,0xBF,0x15,0}, 6, VENDOR_GENETEC, OUI_CLASS_PRIMARY},
  {{0x0C,0xBF,0x15,0}, 6, VENDOR_GENETEC, OUI_CLASS_PRIMARY},
  {{0x00,0x23,0xBD,0}, 6, VENDOR_DALLY,   OUI_CLASS_PRIMARY},

  // --- Backhaul ---
  // Cradlepoint cellular routers, seen beside pole-mounted surveillance and
  // municipal infrastructure, and also fleet vehicles, retail and construction
  // sites. Never a detection on its own.
  {{0x00,0x30,0x44,0}, 6, VENDOR_INFRA, OUI_CLASS_INFRA},
  {{0x00,0xE0,0x1C,0}, 6, VENDOR_INFRA, OUI_CLASS_INFRA},

#ifdef BENCH_BAIT_OUI
  // Bench load generator. Matches a local OUI so a stationary bench exercises
  // the queue and the write path. Sessions captured with this are load
  // measurements, not detections.
  {{BENCH_BAIT_OUI, 0}, 6, VENDOR_FLOCK, OUI_CLASS_PRIMARY},
#endif
};
static const size_t OUI_COUNT = sizeof(oui_table) / sizeof(oui_table[0]);

// Which vendors the matcher accepts, one bit per Vendor. An aligned 16-bit
// store is atomic on Xtensa, so the Targets menu switches this live without
// stopping the sniffer.
volatile uint16_t coreVendorMask = VENDOR_MASK_ALL;

// True when any active target prefix is locally administered, which disables
// the LAA fast path in matchOuiRaw(). precompileOuis() and coreSetVendorMask()
// recompute it. Defaults true so the matcher scans everything until the first
// recompute.
static DRAM_ATTR bool g_haveLaaTargets = true;

static void recomputeLaaTargets() {
  bool any = false;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if ((oui_table[i].b[0] & 0x02) &&
        (coreVendorMask & (1u << oui_table[i].vendor))) { any = true; break; }
  }
  g_haveLaaTargets = any;
}

// Logs the config_change row here so every caller records it. Re-applying the
// same mask writes no row.
void coreSetVendorMask(uint16_t mask) {
  coreVendorMask = (uint16_t)(mask & VENDOR_MASK_ALL);
  recomputeLaaTargets();
  roostLogConfigVendorMask();
}

// Menu row order for SCREEN_TARGETS. screens.inc renders labels in the same
// order. Parent categories only, so a row covers every subsidiary tagged to it.
// Vendors with no row of their own are reachable through All.
static const uint16_t TARGET_MASKS[TARGET_ROW_COUNT] = {
  (uint16_t)(1u << VENDOR_FLOCK),
  (uint16_t)(1u << VENDOR_AXON),
  (uint16_t)(1u << VENDOR_MOTOROLA),
  VENDOR_MASK_ALL,
};

int coreTargetIndex() {
  for (int i = 0; i < TARGET_ROW_COUNT; i++) {
    if (coreVendorMask == TARGET_MASKS[i]) return i;
  }
  return -1;   // a mask with no row, e.g. everything cleared
}

// ============================================================
// OUTPUT
// ============================================================

static char _dualBuf[384];

void dualPrintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(_dualBuf, sizeof(_dualBuf), fmt, args);
  va_end(args);
  if (n > 0) {
    Serial.write(_dualBuf, n);
#if MIRROR_SERIAL
    Serial2.write(_dualBuf, n);
#endif
  }
}

void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL
  Serial2.println(str);
#endif
}

// ============================================================
// MAC / OUI HELPERS
// ============================================================

void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

// Lowercase slugs for serial, JSON and the roost vendor_mask list. screens.inc
// holds a title-case table for the panel.
static const char* const kVendorSlugs[VENDOR_COUNT] = {
  "flock", "axon", "axis", "utility",
  "motorola", "verkada", "genetec", "digital_ally", "infra",
};

// Menu row labels, lowercase for serial, in TARGET_MASKS order. screens.inc
// holds the title-case copy for the panel.
const char* coreTargetRowName(int row) {
  static const char* const kRows[TARGET_ROW_COUNT] = {
    "flock", "axon", "motorola", "all",
  };
  return (row >= 0 && row < TARGET_ROW_COUNT) ? kRows[row] : "unknown";
}

const char* vendorName(uint8_t vendor) {
  return vendor < VENDOR_COUNT ? kVendorSlugs[vendor] : "unknown";
}

// Seeds g_haveLaaTargets and prints the active target set at boot. Both board
// mains call it in setup().
void precompileOuis() {
  uint16_t per[VENDOR_COUNT] = {0};
  uint16_t laa = 0;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (oui_table[i].vendor < VENDOR_COUNT) per[oui_table[i].vendor]++;
    if (oui_table[i].b[0] & 0x02) laa++;
  }
  recomputeLaaTargets();
  dualPrintf("[bscope] targets: %u flock, %u axon, %u axis, %u utility"
             " (%u total, %u locally-administered)\n",
             (unsigned)per[VENDOR_FLOCK], (unsigned)per[VENDOR_AXON],
             (unsigned)per[VENDOR_AXIS], (unsigned)per[VENDOR_UTILITY],
             (unsigned)OUI_COUNT, (unsigned)laa);
#ifdef BENCH_BAIT_OUI
  // Loud, because a bait session that reaches the corpus reads as a detection
  // run.
  dualPrintln("[bscope] *** BENCH BAIT OUI COMPILED IN - THIS IS NOT A CAPTURE ***");
#endif
}

void coreGetFirstTargetOui(uint8_t out[3]) {
  // Honours the active mask, because the `inject` command builds a synthetic
  // target MAC from this and the frame has to match.
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (coreVendorMask & (1u << oui_table[i].vendor)) {
      out[0] = oui_table[i].b[0];
      out[1] = oui_table[i].b[1];
      out[2] = oui_table[i].b[2];
      return;
    }
  }
  out[0] = oui_table[0].b[0];   // mask cleared entirely, fall back to entry 0
  out[1] = oui_table[0].b[1];
  out[2] = oui_table[0].b[2];
}

bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

// Returns the matching oui_table index, or -1. Skips the LAA gate, for BLE,
// where a random address sets the LAA bit.
static int IRAM_ATTR ouiLookup(const uint8_t* mac) {
  const uint16_t mask = coreVendorMask;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const OuiEntry& e = oui_table[i];
    // Prefix compare first, so the mask test runs only on a prefix hit.
    if (mac[0] != e.b[0] || mac[1] != e.b[1] || mac[2] != e.b[2]) continue;
    if (!(mask & (1u << e.vendor)))                               continue;
    if (e.nibbles == 7 && ((mac[3] ^ e.b[3]) & 0xF0))             continue;
    return (int)i;
  }
  return -1;
}

static int IRAM_ATTR matchOuiIndex(const uint8_t* mac) {
  // Filter out locally administered MACs except the LAA hits
  if ((mac[0] & 0x02) && !g_haveLaaTargets) return -1;
  return ouiLookup(mac);
}

int IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  const int i = matchOuiIndex(mac);
  return i < 0 ? -1 : (int)oui_table[i].vendor;
}

// Manufacturer-data rules, the axis that survives address randomisation. A
// company id alone is weak, so a rule may require a type byte and a printable
// ASCII identifier. `cls` states what a match proves, as on the OUI table, spec
// M3 and M8. A company id is a Bluetooth SIG assignment, unrelated to IEEE
// OUIs.
typedef struct {
  uint16_t companyId;
  int16_t  typeByte;    // -1 when the rule has no discriminator byte
  uint8_t  idOffset;    // from the start of the manufacturer data
  uint8_t  idLen;       // 0 when the rule extracts no identifier
  uint8_t  vendor;
  uint8_t  cls;         // OuiClass
} BleMfrRule;

static const BleMfrRule ble_mfr_rules[] = {
  { BLE_COMPANY_AXON,     BLE_AXON_TYPE_BYTE, 3, 9, VENDOR_AXON,     OUI_CLASS_PRIMARY },
  // Penguin battery pack. Nobody has characterised its serial offset, so the
  // rule matches on the company id alone.
  { BLE_COMPANY_XUNTONG,  -1,                 0, 0, VENDOR_FLOCK,    OUI_CLASS_ACCESSORY },
  // The SIG assigned these ids to the vendors themselves, per its company
  // identifier list as of 2026-09-22. Neither has a characterised payload. A
  // vendor-owned company id is narrow enough to stand alone under M1, except
  // Motorola's, which radios may share, so it is infra until a payload
  // separates its camera lines.
  { BLE_COMPANY_AXIS,     -1,                 0, 0, VENDOR_AXIS,     OUI_CLASS_PRIMARY },
  { BLE_COMPANY_MOTOROLA, -1,                 0, 0, VENDOR_MOTOROLA, OUI_CLASS_INFRA },
};
static const size_t BLE_MFR_RULE_COUNT =
    sizeof(ble_mfr_rules) / sizeof(ble_mfr_rules[0]);

volatile uint32_t coreBleMatched = 0;

bool coreBleMatch(const uint8_t* mac, uint8_t addrType,
                  const uint8_t* payload, size_t payloadLen, BleMatch* out) {
  out->vendor    = -1;
  out->accessory = false;
  out->infra     = false;
  out->method    = "unmatched";
  out->id[0]     = '\0';

  // 1. Manufacturer data, the strongest axis and the cheapest rejection.
  uint8_t mfrLen = 0;
  const uint8_t* mfr =
      bleFindAdType(payload, payloadLen, BLE_AD_MANUFACTURER, &mfrLen);
  if (mfr) {
    const int company = bleMfrCompanyId(mfr, mfrLen);
    for (size_t i = 0; i < BLE_MFR_RULE_COUNT; i++) {
      const BleMfrRule& r = ble_mfr_rules[i];
      if (company != (int)r.companyId)                    continue;
      if (!(coreVendorMask & (1u << r.vendor)))           continue;
      if (r.typeByte >= 0
          && (mfrLen < 3 || mfr[2] != (uint8_t)r.typeByte)) continue;
      // A company id plus a type byte matches too much without the printable
      // id check.
      if (r.idLen
          && !bleMfrAsciiId(mfr, mfrLen, r.idOffset, r.idLen,
                            out->id, sizeof(out->id)))      continue;
      out->vendor    = (int8_t)r.vendor;
      out->accessory = r.cls == OUI_CLASS_ACCESSORY;
      out->infra     = r.cls == OUI_CLASS_INFRA;
      out->method    = "ble_mfr";
      return true;
    }
  }

  // 2. The OUI table, public addresses only, since a random address may carry
  //    any prefix.
  if (addrType == BLE_ADDR_PUBLIC || addrType == BLE_ADDR_PUBLIC_ID) {
    const int i = ouiLookup(mac);
    if (i >= 0) {
      out->vendor    = (int8_t)oui_table[i].vendor;
      out->accessory = oui_table[i].cls == OUI_CLASS_ACCESSORY;
      out->infra     = oui_table[i].cls == OUI_CLASS_INFRA;
      out->method    = "ble_oui";
      return true;
    }
  }
  return false;
}

#if ENABLE_SSID_MATCH
static char* strcasestr_local(const char* haystack, const char* needle) {
  if (!*needle) return (char*)haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack; const char* n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
    if (!*n) return (char*)haystack;
  }
  return nullptr;
}
static bool matchSsidKeyword(const char* ssid) {
  for (size_t i = 0; i < SSID_KEYWORD_COUNT; i++)
    if (strcasestr_local(ssid, target_ssid_keywords[i])) return true;
  return false;
}
#endif

// ============================================================
// CHANNEL HOPPING
// ============================================================

uint8_t currentChannel = 1;
static size_t   customChannelIndex = 0;
static size_t   fullHopIndex = 0;
static unsigned long lastHop = 0;

// Runtime scan mode and Single-mode channel, seeded from the board's
// CHANNEL_MODE and SINGLE_CHANNEL. The Scan Mode menu changes them in RAM, and a
// reboot restores the defaults.
static uint8_t g_scanMode        = CHANNEL_MODE;
uint8_t        coreSingleChannel = SINGLE_CHANNEL;

// Valid range for the Single-mode channel picker on 2.4 GHz. Channels 12 and 13
// are listen-only but legal to receive on.
#define CHANNEL_PICK_MIN 1
#define CHANNEL_PICK_MAX 13

const char* channelModeName() {
  switch (g_scanMode) {
    case CHANNEL_MODE_FULL_HOP: return "FULL_HOP";
    case CHANNEL_MODE_CUSTOM:   return "CUSTOM";
    case CHANNEL_MODE_SINGLE:   return "SINGLE";
    default:                    return "UNKNOWN";
  }
}

// Active mode as the Scan-Mode menu's list order (0=Custom, 1=Full, 2=Single),
// which differs from the CHANNEL_MODE_* numeric constants.
int coreScanModeIndex() {
  switch (g_scanMode) {
    case CHANNEL_MODE_CUSTOM:   return 0;
    case CHANNEL_MODE_FULL_HOP: return 1;
    case CHANNEL_MODE_SINGLE:   return 2;
    default:                    return 0;
  }
}

// Display and JSON only. wifi_obs logs channel and band, which determine the
// frequency. 802.11 puts channel 14 at 2484 MHz, off the linear formula.
uint16_t channelFreqMhz(uint8_t ch) {
  if (ch == 14) return 2484;
  return (ch >= 1 && ch <= 13) ? (uint16_t)(2407 + 5 * ch) : 0;
}

void applyInitialChannel() {
  switch (g_scanMode) {
    case CHANNEL_MODE_SINGLE: currentChannel = coreSingleChannel; break;
    case CHANNEL_MODE_CUSTOM: currentChannel = customChannels[0]; break;
    default:                  currentChannel = fullHopChannels[0]; break;
  }
  // Callers can change the channel plan while BLE holds the radio and the
  // 802.11 driver is down. currentChannel still tracks the plan.
  if (coreRadioMode == RADIO_MODE_WIFI)
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
}

void updateChannelMode() {
  if (sniffingStopped) return;
  if (g_scanMode == CHANNEL_MODE_SINGLE) {
    if (currentChannel != coreSingleChannel) {
      currentChannel = coreSingleChannel;
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    }
    return;
  }
  if (millis() - lastHop < CHANNEL_DWELL_MS) return;
  if (g_scanMode == CHANNEL_MODE_CUSTOM) {
    customChannelIndex = (customChannelIndex + 1) % customChannelCount;
    currentChannel = customChannels[customChannelIndex];
  } else {
    fullHopIndex = (fullHopIndex + 1) % fullHopChannelCount;
    currentChannel = fullHopChannels[fullHopIndex];
  }
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
}

// Switch scan mode live (from the Scan Mode menu). `singleChannel` applies only
// to SINGLE. Resets the hop indices and re-applies the new mode's start channel.
static void coreSetScanMode(uint8_t mode, uint8_t singleChannel) {
  g_scanMode = mode;
  if (mode == CHANNEL_MODE_SINGLE
      && singleChannel >= CHANNEL_PICK_MIN && singleChannel <= CHANNEL_PICK_MAX) {
    coreSingleChannel = singleChannel;
  }
  customChannelIndex = 0;
  fullHopIndex = 0;
  applyInitialChannel();
  // Logs a `channels` change, see coreSetVendorMask(). obs_mode stays
  // promiscuous on this build.
  roostLogConfigChannels();
  dualPrintf("[bscope] scan mode -> %s ch=%u\n", channelModeName(), currentChannel);
}

// ============================================================
// JSON ESCAPE, for SSIDs, which are user-controlled bytes
// ============================================================

static size_t jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) break;
      dst[o++] = '\\'; dst[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      if (o + 6 >= cap) break;
      int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
      if (n <= 0 || (size_t)n >= cap - o) break;
      o += (size_t)n;
    } else {
      if (o + 1 >= cap) break;
      dst[o++] = c;
    }
  }
  dst[o] = '\0';
  return o;
}

// ============================================================
// CRC32  (zlib / SPIFFS-tool compatible polynomial 0xEDB88320)
// ============================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

// ============================================================
// DETECTION TABLE  (on-device storage, persisted to SPIFFS)
// ============================================================

typedef struct {
  char     mac[18];
  char     method[16];
  int8_t   rssi;
  uint8_t  channel;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint16_t count;
  char     ssid[33];
  // Proximity-alert state, session-only and absent from fySerializeDet(). A real
  // reading is never 0 dBm, so emaRssi == 0 means unseeded.
  int8_t   emaRssi;
  uint8_t  proxLatched;
  // A camera seen both ways sets both. `method` records only how the row was
  // first created. Session-only, spec C1-C2.
  uint8_t  seenDirect;
  uint8_t  seenIndirect;
} FYDetection;

static FYDetection fyDet[MAX_DETECTIONS];
int           fyDetCount       = 0;
uint16_t      fyDroppedNew     = 0;
bool          fySpiffsReady    = false;
#if USE_SD
bool          fySDReady        = false;
#endif
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;
unsigned long fyLastTargetSeen  = 0;

#define DEDUPE_SLOTS 8
static struct {
  char mac[18];
  unsigned long ts;
} dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  strlcpy(dedupeTable[dedupeIdx].mac, macStr, 18);
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

// Detection tallies, described in core.h. Plain uint16_t, since only
// coreHandleAlert() writes them, from the loop()-context queue drain, and the
// display reads them from the same context.
uint16_t coreDirectFrames   = 0;
uint16_t coreIndirectFrames = 0;

static void tallyFrame(AlertType t) {
  uint16_t& n = (t == ALERT_OUI_ADDR1) ? coreIndirectFrames : coreDirectFrames;
  if (n < 0xFFFF) n++;
}

// Device counts, which overlap. A camera seen both ways counts in each, so the
// sum can exceed fyDetCount. Spec C1-C3.
uint16_t coreDirectDeviceCount() {
  uint16_t n = 0;
  for (int i = 0; i < fyDetCount; i++) if (fyDet[i].seenDirect) n++;
  return n;
}

uint16_t coreIndirectDeviceCount() {
  uint16_t n = 0;
  for (int i = 0; i < fyDetCount; i++) if (fyDet[i].seenIndirect) n++;
  return n;
}

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:           return "ssid_match";
    case ALERT_WILDCARD_PROBE: return "wildcard_probe";
    case ALERT_DIRECTED_PROBE: return "directed_probe";
    case ALERT_SURVEY:         return "operator_survey";   // spec O3
    // Every AlertType needs its own case. The default returns the vocabulary's
    // word for "no target matched", since detection_method has no "unknown".
    default:                   return "unmatched";
  }
}

// Returns the index of the new or updated entry, or -1 when the table is full.
// Sets outChirpWorthy when the MAC is new to this session, or when it went
// unseen for REDISCOVER_MS and so left RF range and came back. A board without
// a buzzer ignores outChirpWorthy.
static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch, const char* ssid,
                          bool direct, bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      // Latched and never cleared. A later frame of the other kind adds a
      // direction to the row.
      if (direct) fyDet[i].seenDirect   = 1;
      else        fyDet[i].seenIndirect = 1;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      // `ssid` is NULL or a printable name, since roostSsidPrintable() already
      // decided what an empty SSID means.
      if (ssid && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    // Full table, with no eviction and no wraparound. fyDroppedNew counts the
    // distinct devices refused, so the display can show the table is out of
    // room and not a quiet area. Repeat hits on MACs already in the table still
    // update above.
    //
    // On a USE_SD board coreHandleAlert() writes the wifi_obs row whatever this
    // returns, so the capture keeps every device. A board without SD loses
    // them.
    if (fyDroppedNew < 0xFFFF) fyDroppedNew++;
    if (outChirpWorthy) *outChirpWorthy = false;
    return -1;
  }
  FYDetection& d = fyDet[fyDetCount];
  strlcpy(d.mac,    mac,                       sizeof(d.mac));
  strlcpy(d.method, method ? method : "",      sizeof(d.method));
  d.rssi      = rssi;
  d.channel   = ch;
  d.firstSeen = now;
  d.lastSeen  = now;
  d.count     = 1;
  // proximityEvaluate() seeds this from a direct hit. `rssi` may come from an
  // oui_addr1 hit, which measures the AP path.
  d.emaRssi     = 0;
  d.proxLatched = 0;
  d.seenDirect   = direct ? 1 : 0;
  d.seenIndirect = direct ? 0 : 1;
  if (ssid) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else      d.ssid[0] = '\0';
  fyDetCount++;
  fyDirty = true;
  if (outChirpWorthy) *outChirpWorthy = true;
  return fyDetCount - 1;
}

// ============================================================
// SPIFFS SESSION PERSISTENCE, a CRC-checked envelope format
// ============================================================
//
// On-disk format, in two parts:
//   1. An envelope line, `{"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n`
//   2. The payload, `[{"mac":...},...]`, exactly B bytes, with CRC32 == X
//
// Atomic write procedure:
//   1. Compute payload size + CRC (pass 1)
//   2. Write envelope + payload to /session.tmp (pass 2)
//   3. Re-validate /session.tmp from disk
//   4. Remove /session.json, rename tmp → main (with copy+delete fallback)
//
// Boot-time recovery:
//   - Try /session.json. If missing or CRC-invalid, try /session.tmp.
//   - Copy whichever validates to /prev_session.json, then delete both.

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"method\":\"%s\",\"rssi\":%d,\"channel\":%u,"
      "\"first\":%lu,\"last\":%lu,\"count\":%u,\"ssid\":\"%s\"}",
      d.mac, d.method, d.rssi, (unsigned)d.channel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen, (unsigned)d.count,
      ssidEsc);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static uint32_t fyComputePayloadCRC(size_t& outBytes) {
  char line[384];
  uint32_t crc = 0;
  outBytes = 0;
  crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    crc = fyCRC32Update(crc, (const uint8_t*)line, n);
    outBytes += n;
  }
  crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
  return crc;
}

// Minimal envelope parser. Finds the bytes and crc fields by substring search,
// in any order, and rejects anything missing either one.
static bool fyParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
  const char* b = strstr(hdr, "\"bytes\":");
  const char* c = strstr(hdr, "\"crc\":\"0x");
  if (!b || !c) return false;
  b += 8;
  long long bv = 0;
  if (sscanf(b, "%lld", &bv) != 1 || bv < 0) return false;
  c += 9;
  unsigned cv = 0;
  if (sscanf(c, "%x", &cv) != 1) return false;
  outBytes = (size_t)bv;
  outCrc   = (uint32_t)cv;
  return true;
}

static bool fyValidateSessionFile(const char* path) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  String hdr = f.readStringUntil('\n');
  if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

  size_t   expectedBytes = 0;
  uint32_t expectedCRC   = 0;
  if (!fyParseEnvelope(hdr.c_str(), expectedBytes, expectedCRC)) {
    f.close(); return false;
  }

  size_t bodyOffset = hdr.length() + 1;
  size_t fileSize   = f.size();
  if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
  if ((fileSize - bodyOffset) != expectedBytes) { f.close(); return false; }

  uint8_t buf[256];
  uint32_t crc = 0;
  size_t remaining = expectedBytes;
  while (remaining > 0) {
    int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) break;
    crc = fyCRC32Update(crc, buf, (size_t)n);
    remaining -= (size_t)n;
  }
  f.close();
  return (remaining == 0 && crc == expectedCRC);
}

static bool fySpiffsCopy(const char* src, const char* dst) {
  File s = SPIFFS.open(src, "r");
  if (!s) return false;
  File d = SPIFFS.open(dst, "w");
  if (!d) { s.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  s.close();
  d.close();
  return ok;
}

static bool fyAtomicPromote(const char* src, const char* dst) {
  if (SPIFFS.rename(src, dst)) return true;
  if (!fySpiffsCopy(src, dst)) return false;
  SPIFFS.remove(src);
  return true;
}

void fySaveSession() {
  if (!fySpiffsReady) return;
  if (!fyDirty && fyDetCount == fyLastSaveCount) return;

  size_t   payloadBytes = 0;
  uint32_t crc          = fyComputePayloadCRC(payloadBytes);
  int      savedCount   = fyDetCount;

  File f = SPIFFS.open(FY_SESSION_TMP, "w");
  if (!f) {
    dualPrintf("[bscope] save failed: cannot open %s\n", FY_SESSION_TMP);
    return;
  }
  f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);

  char line[384];
  size_t wrote = 0;
  f.write((uint8_t*)"[", 1); wrote++;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    f.write((uint8_t*)line, n);
    wrote += n;
  }
  f.write((uint8_t*)"]", 1); wrote++;
  f.close();

  if (wrote != payloadBytes) {
    dualPrintf("[bscope] save WARNING: wrote %u expected %u - aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }

  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintln("[bscope] save verify FAILED - old session preserved");
    return;
  }

  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[bscope] promote FAILED - data in %s for recovery\n", FY_SESSION_TMP);
    return;
  }

  fyLastSaveAt    = millis();
  fyLastSaveCount = savedCount;
  fyDirty         = false;
  dualPrintf("[bscope] session saved: %d det, %u bytes, crc=0x%08lX\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

// Promotes any valid session file from the last boot to /prev_session.json,
// then starts this boot with an empty table.
void fyPromotePrevSession() {
  if (!fySpiffsReady) return;

  const char* source = nullptr;
  if      (fyValidateSessionFile(FY_SESSION_FILE)) source = FY_SESSION_FILE;
  else if (fyValidateSessionFile(FY_SESSION_TMP))  source = FY_SESSION_TMP;

  if (!source) {
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
    dualPrintln("[bscope] no valid prior session to promote");
    return;
  }

  if (!fySpiffsCopy(source, FY_PREV_FILE)) {
    dualPrintf("[bscope] failed to promote %s -> %s\n", source, FY_PREV_FILE);
    return;
  }
  if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
  if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

  File v = SPIFFS.open(FY_PREV_FILE, "r");
  size_t sz = v ? v.size() : 0;
  if (v) v.close();
  dualPrintf("[bscope] prior session promoted from %s (%u bytes)\n",
             source, (unsigned)sz);
}

// ============================================================
// AUTOSAVE
// ============================================================

void autosaveTick() {
  if (!fySpiffsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

static unsigned long lastHeartbeat = 0;

void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    if (fyDroppedNew) {
      // The table is full and dropping devices. Say so every heartbeat, since
      // det= stays at MAX_DETECTIONS and looks like a quiet area.
      dualPrintf("[bscope] scanning (ch=%u mode=%s det=%d TABLE FULL, missed=%u)\n",
                    currentChannel, channelModeName(), fyDetCount,
                    (unsigned)fyDroppedNew);
    } else {
      dualPrintf("[bscope] scanning (ch=%u mode=%s det=%d)\n",
                    currentChannel, channelModeName(), fyDetCount);
    }
    lastHeartbeat = millis();
  }
}

// ============================================================
// SERIAL COMMANDS. Core handles the verbs every board shares. Each board
// composes its own `status` in printStatus(), from the extern state below,
// because the fields differ by board.
// ============================================================

void dumpCurrentSession() {
  dualPrintf("[bscope] dump: %d detections\n", fyDetCount);
  Serial.write('[');
  char line[384];
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) Serial.write(',');
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n > 0) Serial.write((uint8_t*)line, n);
  }
  Serial.write("]\n");
}

void dumpSpiffsFile(const char* path) {
  if (!fySpiffsReady || !SPIFFS.exists(path)) {
    dualPrintf("[bscope] dump: %s not found\n", path);
    return;
  }
  File f = SPIFFS.open(path, "r");
  if (!f) { dualPrintf("[bscope] dump: cannot open %s\n", path); return; }
  dualPrintf("[bscope] dump: %s (%u bytes)\n", path, (unsigned)f.size());
  uint8_t buf[256];
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, (size_t)n);
  Serial.write('\n');
  f.close();
}

// Shared line tokenizer, described in core.h. Accumulates Serial bytes in a
// static buffer until a newline, then splits the line into a lowercased verb
// (the first token) and a trimmed argument (the rest). Drops bytes past the end
// of the buffer.
bool coreReadSerialCommand(const char** verb, const char** arg) {
  static char line[64];
  static size_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      len = 0;
      // Split verb / arg on the first run of whitespace.
      char* p = line;
      while (*p == ' ' || *p == '\t') p++;          // leading space
      char* v = p;
      while (*p && *p != ' ' && *p != '\t') {
        *p = (char)tolower((unsigned char)*p);       // lowercase the verb in place
        p++;
      }
      if (*p) { *p++ = '\0'; }                        // terminate verb
      while (*p == ' ' || *p == '\t') p++;           // skip to arg
      *verb = v;
      *arg  = p;                                      // "" when no argument
      return true;                                    // one line ready (even if blank)
    }
    if (len < sizeof(line) - 1) line[len++] = c;      // else drop excess chars
  }
  return false;
}

// Maps a nav-direction word to its NavEvent. Returns NAV_NONE for anything else.
static NavEvent navEventFromWord(const char* w) {
  if (!strcmp(w, "up"))     return NAV_UP;
  if (!strcmp(w, "down"))   return NAV_DOWN;
  if (!strcmp(w, "select")) return NAV_SELECT;
  if (!strcmp(w, "back"))   return NAV_BACK;
  if (!strcmp(w, "mark"))   return NAV_MARK;
  return NAV_NONE;
}

#if DEBUG_OUI_CENSUS
// Defined down with the sniffer it instruments, which sits below this handler.
static void censusDump();
#endif

static void gpsEchoFor(uint32_t ms);

bool coreHandleSerialCommand(const char* verb, const char* arg) {
  if (!strcmp(verb, "dump")) { dumpCurrentSession(); return true; }
  if (!strcmp(verb, "prev")) { dumpSpiffsFile(FY_PREV_FILE); return true; }
  // Tone tests. Each prints an acknowledgement, so success looks different from
  // a silent failure. The help listing's gate hides them on a board without a
  // buzzer, which reports them as unknown verbs.
#if USE_BUZZER
  if (!strcmp(verb, "chirp"))  {
    dualPrintln("[bscope] playing detection chirp"); corePlayDetectChirp();    return true; }
  if (!strcmp(verb, "prox"))   {
    dualPrintln("[bscope] playing proximity chirp"); corePlayProximityChirp(); return true; }
  if (!strcmp(verb, "jingle")) {
    dualPrintln("[bscope] playing boot sound");      corePlayStartupJingle();  return true; }
  if (!strcmp(verb, "crow"))   {
    dualPrintln("[bscope] playing crow call");       corePlayCrowCall();       return true; }
  if (!strcmp(verb, "hawk"))   {
    dualPrintln("[bscope] playing hawk call");       corePlayHawkCall();       return true; }
#endif
#if DEBUG_OUI_CENSUS
  if (!strcmp(verb, "census")) { censusDump(); return true; }
#endif
  if (!strcmp(verb, "frames")) {
    static const struct { uint8_t st; const char* name; } kNames[] = {
      {0,"assoc_req"}, {1,"assoc_resp"}, {2,"reassoc_req"}, {3,"reassoc_resp"},
      {4,"probe_req"}, {5,"probe_resp"}, {8,"beacon"},      {11,"auth"},
      {12,"deauth"},   {10,"disassoc"},
    };
    dualPrintf("[frames] delivered=%lu candidate=%lu (after type/len/rssi)\n",
               (unsigned long)coreSeenFrames, (unsigned long)coreCandidateFrames);
    dualPrintln("[frames] mgmt subtype        seen   matched");
    for (size_t i = 0; i < sizeof(kNames)/sizeof(kNames[0]); i++)
      dualPrintf("[frames]   %-16s %7lu %9lu\n", kNames[i].name,
                 (unsigned long)coreMgmtSeen[kNames[i].st],
                 (unsigned long)coreMgmtMatched[kNames[i].st]);
    return true;
  }
  if (!strcmp(verb, "nmea")) {
    const bool off = arg && !strcmp(arg, "off");
    gpsEchoFor(off ? 0 : 30000);
    dualPrintln(off ? "[nmea] echo off"
                    : "[nmea] echoing raw sentences for 30s ($GxGSV carries "
                      "satellites in view and C/N0)");
    return true;
  }
  if (!strcmp(verb, "nav")) {
    NavEvent ev = navEventFromWord(arg);
    if (ev == NAV_NONE) {
      dualPrintln("[bscope] nav needs: up|down|select|back|mark");
    } else {
      coreInjectNav(ev);
      dualPrintf("[bscope] nav injected: %s\n", arg);
    }
    return true;
  }
  return false;
}

void corePrintSerialHelp() {
  dualPrintln("  dump              dump current session (JSON)");
  dualPrintln("  prev              dump previous session (JSON)");
  dualPrintln("  nav <up|down|select|back|mark>  inject a nav event");
  dualPrintln("  nmea [off]        echo raw GPS sentences for 30s");
  dualPrintln("  frames            mgmt subtypes seen vs matched");
#if DEBUG_OUI_CENSUS
  dualPrintln("  census            distinct OUIs heard (bench debug build)");
#endif
#if USE_BUZZER
  dualPrintln("  chirp             play detection chirp (tone test)");
  dualPrintln("  prox              play proximity chirp (tone test)");
  dualPrintln("  jingle            play boot sound (tone test)");
  dualPrintln("  crow              play the crow call (tone test)");
  dualPrintln("  hawk              play the hawk call (tone test)");
#endif
}

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 32

volatile uint32_t coreQueueDrops = 0;
volatile uint8_t  coreQueueDepthMax = 0;

uint8_t coreAlertQueueSize() { return ALERT_QUEUE_SIZE; }

// Each radio's queue and rate-limit tables exist only while that radio runs.
// Start allocates them and stop frees them, so the idle radio holds no buffer
// memory. A failed allocation leaves the radio running, reports on the
// console, and turns its matches into counted drops [G1].
//
// `psramOk` lets a buffer go to PSRAM when the board has it. Only a buffer the
// 802.11 RX callback never touches may, since that callback runs from IRAM
// while flash writes have the cache, and with it PSRAM, switched off.
volatile uint32_t coreRadioAllocFails = 0;

static void* radioAlloc(size_t n, const char* what, bool psramOk = false) {
  void* p = psramOk ? heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                    : nullptr;
  if (!p) p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) coreRadioAllocFails = coreRadioAllocFails + 1;
  if (!p)
    dualPrintf("[bscope] %s: %u bytes unavailable, its matches count as "
               "dropped\n", what, (unsigned)n);
  return p;
}

// nullptr while the 802.11 sniffer is down.
static AlertEntry* volatile alertQueue = nullptr;
static volatile size_t alertHead = 0;  // written by callback
static volatile size_t alertTail = 0;  // read by loop()
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;

volatile bool sniffingStopped = false;

void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac,
                             const FrameMeta* fm,
                             int8_t rssi, uint8_t ch,
                             const RoostSsid* ssid, const char* kind,
                             const char* fsubtype) {
  portENTER_CRITICAL_ISR(&queueMux);
  if (!alertQueue) {
    // A running sniffer without its queue lost the frame. A stopped one, which
    // the `inject` command can reach, had nothing to lose.
    if (!sniffingStopped) coreQueueDrops = coreQueueDrops + 1;
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full, loop() is behind
    // A matched frame lost outright, which never reaches the log. fyDroppedNew
    // counts something else, a full display table on a board that still writes
    // the row. This count feeds device_event buffer_full and
    // observations_dropped.
    coreQueueDrops = coreQueueDrops + 1;
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  memcpy((void*)e->mac, mac, 6);
  // Stamped in the callback, so the row records the frame's arrival time.
  e->uptimeMs = millis();
  if (fm) {
    memcpy((void*)e->addr1, fm->addr1, 6);
    memcpy((void*)e->addr2, fm->addr2, 6);
    memcpy((void*)e->addr3, fm->addr3, 6);
    e->seq      = fm->seq;
    e->fcFlags  = fm->fcFlags;
    e->frameLen = fm->frameLen;
    strncpy((char*)e->bbFormat, fm->bbFormat, 7); ((char*)e->bbFormat)[7] = '\0';
  } else {
    memset((void*)e->addr1, 0, 6);
    memset((void*)e->addr2, 0, 6);
    memset((void*)e->addr3, 0, 6);
    e->seq = 0; e->fcFlags = 0; e->frameLen = 0;
    ((char*)e->bbFormat)[0] = '\0';
  }

  // Copy the whole struct. `len` and `present` tell an absent SSID element from
  // a present empty one, and a string copy drops both.
  if (ssid) *(RoostSsid*)&e->ssid = *ssid;
  else      memset((void*)&e->ssid, 0, sizeof(RoostSsid));

  if (kind)     { strncpy((char*)e->frameKind,    kind,     11); ((char*)e->frameKind)[11]    = '\0'; }
  else           { ((char*)e->frameKind)[0] = '\0'; }

  if (fsubtype) { strncpy((char*)e->frameSubtype, fsubtype, 15); ((char*)e->frameSubtype)[15] = '\0'; }
  else           { ((char*)e->frameSubtype)[0] = '\0'; }

  alertHead = next;
  // High-water mark, for sizing the queue against real load. Each entry holds
  // the IE lists, so this shows how much headroom remains.
  uint8_t depth = (uint8_t)((alertHead + ALERT_QUEUE_SIZE - alertTail) % ALERT_QUEUE_SIZE);
  if (depth > coreQueueDepthMax) coreQueueDepthMax = depth;
  portEXIT_CRITICAL_ISR(&queueMux);
}

// ============================================================
// OPERATOR SURVEY WINDOW. Contract in core.h, rules in spec O1-O7 [D9]. The
// promiscuous callback reads the flag, and only loop() writes it.
// ============================================================

const uint16_t SURVEY_OPTIONS_S[SURVEY_OPTION_COUNT] = { 10, 20, 30 };
volatile uint16_t coreSurveySecs = SURVEY_OPTIONS_S[0];

// ---- Per-MAC rate limit -----------------------------------------------------
//
// One row per MAC per time window, applied before a frame reaches the queue. The survey window and infra matches each
// hold their own table. Target matches never reach either. Spec O6 [D9] and M8
// [D17].
#ifndef SURVEY_DEDUPE_SLOTS
#define SURVEY_DEDUPE_SLOTS 256
#endif
#ifndef SURVEY_DEDUPE_MS
#define SURVEY_DEDUPE_MS 2000
#endif
#ifndef INFRA_DEDUPE_SLOTS
#define INFRA_DEDUPE_SLOTS 64
#endif
#ifndef INFRA_DEDUPE_MS
#define INFRA_DEDUPE_MS 10000
#endif

uint32_t coreInfraDedupeMs() { return INFRA_DEDUPE_MS; }

#ifndef BLE_LIMIT_SLOTS
#define BLE_LIMIT_SLOTS 512
#endif

// The sniffer attaches the 802.11 tables while it runs, and the BLE scan
// attaches the BLE tables. See mac_limit.h for the algorithm.
static DRAM_ATTR MacLimit surveyLimit = {};
static DRAM_ATTR MacLimit infraLimit  = {};
static MacLimit bleSurveyLimit = {};
static MacLimit bleInfraLimit  = {};

// `psramOk` follows radioAlloc(). Only the BLE tables may take it.
static void limitStart(MacLimit* l, uint16_t slots, uint32_t windowMs,
                       const char* what, bool psramOk) {
  if (l->at) return;
  macLimitAttach(l, radioAlloc(MAC_LIMIT_BYTES(slots), what, psramOk),
                 slots, windowMs);
}

static void limitStop(MacLimit* l) { free(macLimitDetach(l)); }

// Returns 1 when the RX callback should queue a row for the address, 0 for no
// match, and -1 for an infra match the per-MAC limit holds back. A -1 still
// keeps the frame out of the survey path.
static int IRAM_ATTR matchForQueue(const uint8_t* mac) {
  const int i = matchOuiIndex(mac);
  if (i < 0) return 0;
  if (oui_table[i].cls == OUI_CLASS_INFRA
      && !macLimitAllow(&infraLimit, mac, millis())) return -1;
  return 1;
}

static volatile bool     surveyActive  = false;
static unsigned long     surveyEndsAt  = 0;
// Captured when the window opens, so the report covers the window alone. coreQueueDrops counts frames that never reached the log.
static uint32_t          surveyDropsAt = 0;
volatile uint32_t        coreSurveyRows = 0;
volatile uint32_t        coreSurveySuppressed = 0;

bool coreSurveyActive() { return surveyActive; }


uint32_t coreSurveyRemainingMs() {
  if (!surveyActive) return 0;
  unsigned long now = millis();
  return (surveyEndsAt > now) ? (uint32_t)(surveyEndsAt - now) : 0;
}

void coreSurveyStart() {
  // Without an SD log survey rows have no sink, so no window opens (spec O1). A
  // plain condition, so both arms compile on every board.
  if (!USE_SD) {
    dualPrintln("[bscope] SURVEY unavailable: no SD log on this board");
    return;
  }
  // A press during an open window restarts its full duration, so the
  // operator's last press decides when it ends.
  if (!surveyActive) {
    surveyDropsAt        = coreQueueDrops + coreBleQueueDrops;
    coreSurveyRows       = 0;
    coreSurveySuppressed = 0;
    // Cleared per window, so a device seen at an earlier mark still yields a
    // first row here (spec O6). Unlocked, so this clear must finish before
    // surveyActive goes true, since that flag gates the survey limit in the
    // callback.
    macLimitReset(&surveyLimit);
    macLimitReset(&bleSurveyLimit);
  }
  surveyEndsAt = millis() + (unsigned long)coreSurveySecs * 1000UL;
  surveyActive = true;   // must stay after the reset above
  dualPrintf("[bscope] SURVEY open: unfiltered capture for %us\n",
             (unsigned)coreSurveySecs);
}

void coreSurveyTick() {
  if (!surveyActive) return;
  if ((long)(millis() - surveyEndsAt) < 0) return;
  surveyActive = false;

  uint32_t drops = coreQueueDrops + coreBleQueueDrops - surveyDropsAt;
  // Drops count every frame the queue refused during the window, matched ones
  // included, so the figure is an upper bound on what the window missed. Spec
  // O7.
  dualPrintf("[bscope] SURVEY closed: %u rows, %u frames rate-limited,"
             " %u dropped (queue full), %u evictions\n",
             (unsigned)coreSurveyRows, (unsigned)coreSurveySuppressed,
             (unsigned)drops,
             (unsigned)(surveyLimit.evictions + bleSurveyLimit.evictions));
#if USE_SD
  if (drops) {
#if ROOST_CAP_BLE
    const RoostComponent comp =
        coreRadioMode == RADIO_MODE_BLE ? ROOST_COMP_BLE0 : ROOST_COMP_WIFI0;
#else
    const RoostComponent comp = ROOST_COMP_WIFI0;
#endif
    roostLogDeviceEvent(comp, "buffer_full", drops, "during_survey");
  }
#endif
}

bool coreDequeueAlert(AlertEntry& out) {
  portENTER_CRITICAL(&queueMux);
  if (!alertQueue || alertTail == alertHead) {
    portEXIT_CRITICAL(&queueMux);
    return false;
  }
  memcpy(&out, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
  alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
  portEXIT_CRITICAL(&queueMux);
  return true;
}

// ============================================================
// 802.11 HEADER
// ============================================================

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// Maps 802.11 frame type+subtype to a short log string.
// Called from IRAM. Returns only string literals, with no allocation.
static const char* IRAM_ATTR frameSubtypeStr(wifi_promiscuous_pkt_type_t pkt_type,
                                              uint8_t ftype, uint8_t subtype) {
  if (pkt_type == WIFI_PKT_DATA) return "data";
  if (ftype != 0) return "ctrl";  // control frames
  switch (subtype) {
    case 0:  return "assoc_req";
    case 1:  return "assoc_resp";
    case 2:  return "reassoc_req";
    case 3:  return "reassoc_resp";
    case 4:  return "probe_req";
    case 5:  return "probe_resp";
    case 8:  return "beacon";
    case 9:  return "atim";
    case 10: return "disassoc";
    case 11: return "auth";
    case 12: return "deauth";
    case 13: return "action";
    default: return "unknown";
  }
}

// Raw sniffer counters, described in core.h. The callback increments them
// without a lock, since a lost count under contention costs nothing.
volatile uint32_t coreSeenFrames      = 0;
volatile uint32_t coreCandidateFrames = 0;

// Management subtype histogram, the only instrument that sees frames the
// matcher rejected. Everything else records matches, where a subtype that never
// arrives looks the same as one that arrives unmatched. The callback counts at
// two points to separate them.
//
// DRAM_ATTR and no flash on the path, the same constraint as the OUI census.
DRAM_ATTR volatile uint32_t coreMgmtSeen[16]    = {0};
DRAM_ATTR volatile uint32_t coreMgmtMatched[16] = {0};

// ============================================================
// OUI CENSUS, a field instrument that only DEBUG_OUI_CENSUS compiles in.
// Records every distinct OUI the callback sees, and the `census` verb dumps it.
// Spec G2-G3.
//
// DRAM_ATTR throughout and no flash reads on the record path, the same
// constraint as oui_table, since this runs in the promiscuous callback.
// ============================================================

#ifndef DEBUG_OUI_CENSUS
#define DEBUG_OUI_CENSUS 0
#endif

#if DEBUG_OUI_CENSUS
// Sized for a drive, not a bench. 192 rows costs ~1.1KB of DRAM.
#ifndef CENSUS_MAX
#define CENSUS_MAX 192
#endif

// Which address field an OUI turned up in. The census records both, spec G2.
#define CENSUS_ROLE_ADDR2 0x01
#define CENSUS_ROLE_ADDR1 0x02

static DRAM_ATTR uint8_t  censusOui[CENSUS_MAX][3];
static DRAM_ATTR uint16_t censusHits[CENSUS_MAX];
static DRAM_ATTR uint8_t  censusRole[CENSUS_MAX];
static DRAM_ATTR uint16_t censusUsed = 0;
static DRAM_ATTR uint16_t censusOverflow = 0;

// Linear scan. A full table costs 192 three-byte compares per frame, small next
// to the parse that follows.
static void IRAM_ATTR censusRecord(const uint8_t* mac, uint8_t role) {
  for (uint16_t i = 0; i < censusUsed; i++) {
    if (censusOui[i][0] == mac[0] && censusOui[i][1] == mac[1] &&
        censusOui[i][2] == mac[2]) {
      if (censusHits[i] < 0xFFFF) censusHits[i]++;
      censusRole[i] |= role;
      return;
    }
  }
  if (censusUsed >= CENSUS_MAX) { censusOverflow++; return; }
  censusOui[censusUsed][0] = mac[0];
  censusOui[censusUsed][1] = mac[1];
  censusOui[censusUsed][2] = mac[2];
  censusHits[censusUsed]   = 1;
  censusRole[censusUsed]   = role;
  censusUsed++;
}

// Marks rows the matcher would accept. Also flags locally administered
// prefixes, since the matcher misses a randomised camera MAC.
static void censusDump() {
  dualPrintf("[bscope] census: %u distinct OUIs (%u dropped, table full)\n",
             (unsigned)censusUsed, (unsigned)censusOverflow);
  for (uint16_t i = 0; i < censusUsed; i++) {
    uint8_t mac[6] = { censusOui[i][0], censusOui[i][1], censusOui[i][2], 0, 0, 0 };
    char role[8];
    snprintf(role, sizeof(role), "%s%s",
             (censusRole[i] & CENSUS_ROLE_ADDR2) ? "tx" : "  ",
             (censusRole[i] & CENSUS_ROLE_ADDR1) ? "/rx" : "   ");
    dualPrintf("  %02x:%02x:%02x  hits=%-6u %s %s%s\n",
               censusOui[i][0], censusOui[i][1], censusOui[i][2],
               (unsigned)censusHits[i], role,
               (censusOui[i][0] & 0x02) ? "LAA " : "",
               (matchOuiRaw(mac) >= 0) ? "<-- TARGET" : "");
  }
}
#endif

void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped) return;
  // Ahead of every filter below, so this counts what the radio delivered. Zero
  // here means the driver delivers nothing. Read-modify-write, since C++20
  // deprecates ++ on a volatile.
  coreSeenFrames = coreSeenFrames + 1;

#if PROCESS_MGMT_FRAMES && PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif PROCESS_MGMT_FRAMES
  if (type != WIFI_PKT_MGMT) return;
#elif PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_DATA) return;
#else
  return;  // nothing configured to process
#endif

  wifi_promiscuous_pkt_t*      pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;
  wifi_ieee80211_mac_hdr_t*    hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;

  // Ahead of the RSSI gate, since a frame the threshold discards still arrived.
  // The gap against coreMgmtMatched separates "never arrived" from "arrived
  // unmatched".
  if (type == WIFI_PKT_MGMT) {
    const uint8_t st_ = (uint8_t)((hdr->frame_ctrl >> 4) & 0x0F);
    coreMgmtSeen[st_] = coreMgmtSeen[st_] + 1;
  }

  if (rssi < RSSI_MIN) return;
  // Past the type, length and RSSI guards. The gap from coreSeenFrames
  // isolates those guards.
  coreCandidateFrames = coreCandidateFrames + 1;

#if DEBUG_OUI_CENSUS
  censusRecord(hdr->addr2, CENSUS_ROLE_ADDR2);
  // The same multicast guard as the real addr1 path. addr1 is broadcast on
  // beacons and would bury the table in `ff:ff:ff`.
  if (!isMulticast(hdr->addr1)) censusRecord(hdr->addr1, CENSUS_ROLE_ADDR1);
#endif

  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver

  // Capture everything the row needs now. The driver frees its buffer once
  // this callback returns.
  FrameMeta fm;
  memcpy(fm.addr1, hdr->addr1, 6);
  memcpy(fm.addr2, hdr->addr2, 6);
  memcpy(fm.addr3, hdr->addr3, 6);
  fm.seq      = hdr->seq_ctrl;
  fm.fcFlags  = (uint16_t)((hdr->frame_ctrl >> 8) & 0xFF);
  fm.frameLen = (uint16_t)pkt->rx_ctrl.sig_len;
  // sig_mode 0 is non-HT, where the rate tells DSSS/CCK from OFDM. The IDF's
  // wifi_phy_rate_t puts the 1, 2, 5.5 and 11 Mbps rates at 0x00-0x07.
  switch (pkt->rx_ctrl.sig_mode) {
    case 1:  strcpy(fm.bbFormat, "ht");  break;
    case 3:  strcpy(fm.bbFormat, "vht"); break;
    default: strcpy(fm.bbFormat, pkt->rx_ctrl.rate <= 0x07 ? "11b" : "11g"); break;
  }

  uint8_t fc0       = hdr->frame_ctrl & 0xFF;
  uint8_t ftype     = (fc0 >> 2) & 0x03;
  uint8_t subtype   = (fc0 >> 4) & 0x0F;
  const char* fsub  = frameSubtypeStr(type, ftype, subtype);

  // The management body, bounded once for every consumer below. The driver's
  // sig_len still counts the FCS the hardware stripped, so parsing to it reads
  // four bytes of checksum as another element. roostIeParseLen() removes them.
  const size_t kHdr    = sizeof(wifi_ieee80211_mac_hdr_t);
  const size_t parseLen = roostIeParseLen(pkt->rx_ctrl.sig_len,
                                          pkt->rx_ctrl.sig_len);
  const uint8_t* body  = pkt->payload + kHdr;
  const size_t bodyLen = parseLen > kHdr ? parseLen - kHdr : 0;

  // Every IE-bearing subtype, whichever branch below handles the frame. A
  // declared column holds data whenever the radio reported it (spec 7.1).
  RoostSsid ssid;
  roostIeSsidCapture(body, bodyLen, ftype, subtype, &ssid);

  // --- addr2 OUI check, the transmitter ---
  //
  // A probe request from a matched OUI splits into wildcard_probe, the
  // DeFlockJoplin signature with a zero-length SSID IE, and directed_probe.
  // Every other frame from the OUI raises oui_addr2. docs/detection_methods.md
  // describes each method and its field results.

  // Set beside every enqueue below. The survey path writes a row only for a
  // frame no matcher claimed, so an open window never duplicates a matched
  // frame's rows.
  bool anyAlert = false;

  const int m2 = matchForQueue(hdr->addr2);
  if (m2 < 0) anyAlert = true;
  if (m2 > 0) {
    // Counted here, after the match and before the branch below decides what
    // kind of alert it is. A subtype that appears in coreMgmtSeen, appears
    // here, and still produces no row points to a fault between here and the
    // log.
    if (type == WIFI_PKT_MGMT)
      coreMgmtMatched[subtype] = coreMgmtMatched[subtype] + 1;

    bool emitted = false;
    if (type == WIFI_PKT_MGMT) {
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        // No FCS retry, since the bound above already removed the checksum.
        // This reads the result of the walk above.
        if (ssid.present && ssid.len == 0) {
          enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, &fm, rssi, ch,
                       &ssid, "probe_req", fsub);
          emitted = true;
          anyAlert = true;
        } else if (ssid.present) {
          // Directed probe. The probed name identifies the configured backhaul
          // network, the field target SSID lists draw from.
          enqueueAlert(ALERT_DIRECTED_PROBE, hdr->addr2, &fm, rssi, ch,
                       &ssid, "probe_req", fsub);
          emitted = true;
          anyAlert = true;
        }
      }
    }
    if (!emitted) {
      enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, &fm, rssi, ch, &ssid, "addr2", fsub);
      anyAlert = true;
    }
  }

#if CHECK_ADDR1
  // addr1, the receiver, catches Flock stations that appear only as the
  // destination of probe responses and data frames, because their burst-sleep
  // duty cycle sends nothing in the capture window. The multicast guard is
  // mandatory, since addr1 is broadcast on beacons and other broadcasts.
  //
  // addr2, the AP that sent the probe response, goes in mac2 so analysis can
  // use the AP's position as a proxy for the camera. The RSSI measures the
  // AP-to-scanner path, so triangulation cannot use it directly.
  const int m1 = isMulticast(hdr->addr1) ? 0 : matchForQueue(hdr->addr1);
  if (m1 < 0) anyAlert = true;
  if (m1 > 0) {
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, &fm, rssi, ch, &ssid, "addr1", fsub);
    anyAlert = true;
  }
#endif

#if CHECK_ADDR3
  // addr3 fallback, for management frames only. Catches a randomised addr2
  // when addr3 holds the real BSSID OUI.
  const int m3 = (type == WIFI_PKT_MGMT) ? matchForQueue(hdr->addr3) : 0;
  if (m3 < 0) anyAlert = true;
  if (m3 > 0) {
    enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, &fm, rssi, ch, &ssid, "addr3", fsub);
    anyAlert = true;
  }
#endif

#if ENABLE_SSID_MATCH
  // The walker above extracted the name, using each subtype's fixed-field
  // offset. This branch only decides whether it matches.
  const char* ssidName = roostSsidPrintable(&ssid);
  if (ssidName && matchSsidKeyword(ssidName)) {
    const char* frameKind = (subtype == 8)   ? "beacon"
                          : (subtype == 5)   ? "probe_resp"
                          : (subtype == 4)   ? "probe_req"
                                             : "mgmt";
    enqueueAlert(ALERT_SSID, hdr->addr2, &fm, rssi, ch, &ssid, frameKind, fsub);
    anyAlert = true;
  }
#endif

  // Survey window, for frames no matcher claimed. Keyed on addr2, which is both
  // the row's identity and what the per-MAC cap counts. Spec O3, O6.
  if (surveyActive && !anyAlert) {
    if (macLimitAllow(&surveyLimit, hdr->addr2, millis())) {
      enqueueAlert(ALERT_SURVEY, hdr->addr2, &fm, rssi, ch, &ssid, "survey", fsub);
      coreSurveyRows = coreSurveyRows + 1;
    } else {
      coreSurveySuppressed = coreSurveySuppressed + 1;
    }
  }
}

// ============================================================
// TIME SOURCE. A runtime priority chain of GPS once a module locks, then NTP
// over WiFi with stored station credentials, then millis() since boot. A
// cold-start GPS cannot lock at boot, so coreTimeSync() bridges the pre-lock
// window with an NTP join. GPS takes over when it locks, because gpsTick() sets
// the GPS anchor and coreTimestampStr() prefers GPS over NTP. On a board with no
// GPS module the NTP anchor stays the source. All three anchors live here. See
// core.h.
// ============================================================

#if HAS_GPS
#include <TinyGPS++.h>

static TinyGPSPlus gpsParser;
static bool        gpsReady        = false;
static bool        gpsTimeAnchored = false;
// Latches the first refused clock, so the wait prints once per boot. Never
// cleared.
static bool        gpsAnchorRefused = false;
static uint32_t    gpsAnchorUnix   = 0;
static uint32_t    gpsAnchorMs     = 0;
bool   gpsHasFix = false;
double gpsLat    = 0.0;
double gpsLng    = 0.0;

static uint32_t gpsToUnix(uint16_t year, uint8_t month, uint8_t day,
                           uint8_t hour, uint8_t minute, uint8_t second) {
  static const uint8_t dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++)
    days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  for (uint8_t m = 0; m < month - 1; m++)
    days += dpm[m] + (m == 1 && leap ? 1 : 0);
  days += day - 1;
  return days * 86400UL + hour * 3600UL + minute * 60UL + second;
}

static void unixToIso(uint32_t unix, char* buf, size_t len) {
  uint32_t s   = unix % 60; unix /= 60;
  uint32_t min = unix % 60; unix /= 60;
  uint32_t hr  = unix % 24; unix /= 24;
  uint32_t days = unix;
  uint16_t year = 1970;
  while (true) {
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    uint16_t yd = leap ? 366 : 365;
    if (days < yd) break;
    days -= yd; year++;
  }
  static const uint8_t dpm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  uint8_t month = 1;
  for (; month <= 12; month++) {
    uint8_t md = dpm[month-1] + (month == 2 && leap ? 1 : 0);
    if (days < md) break;
    days -= md;
  }
  snprintf(buf, len, "%04u-%02u-%02uT%02lu:%02lu:%02luZ",
           year, month, (uint8_t)(days + 1), hr, min, s);
}

// The HardwareSerial wired to the GPS module. Defaults to Serial2. A board
// overrides it when Serial2 has another job, such as the debug mirror, since two
// begin() calls on one UART conflict.
#ifndef GPS_SERIAL
#define GPS_SERIAL Serial2
#endif

static void gpsSetup() {
  // Power, reset, and wakeup control pins are specific to modules that expose
  // them. A board with a bare always-on module, wired straight to 3V3 with no
  // control lines, leaves these undefined and the whole block compiles out.
#ifdef GPS_VGNSS_CTRL
  pinMode(GPS_VGNSS_CTRL, OUTPUT); digitalWrite(GPS_VGNSS_CTRL, LOW);  // active-LOW rail enable
  delay(50);
#endif
#ifdef GPS_RST_PIN
  pinMode(GPS_RST_PIN, OUTPUT); digitalWrite(GPS_RST_PIN, LOW); delay(10);
  digitalWrite(GPS_RST_PIN, HIGH);
#endif
#ifdef GPS_WAKEUP_PIN
  pinMode(GPS_WAKEUP_PIN, OUTPUT); digitalWrite(GPS_WAKEUP_PIN, HIGH);
#endif
  delay(100);  // allow the module to boot before UART traffic
  GPS_SERIAL.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  gpsReady = true;
  dualPrintln("[bscope] GPS serial started (waiting for fix)");
}

// Raw sentence echo for antenna bring-up. The parsed counters cannot tell a
// dead antenna feed from a weak signal, because both leave every field empty.
// $GxGSV reports satellites in view and their C/N0, which separates them.
// Time-limited, so it cannot stay on in the field.
static uint32_t gpsEchoUntil = 0;
static char     gpsEchoLine[100];
static uint8_t  gpsEchoLen  = 0;

static void gpsEchoFor(uint32_t ms) {
  gpsEchoUntil = ms ? millis() + ms : 0;
  gpsEchoLen   = 0;
}

// Echoes whole sentences, since a per-byte print costs more than the 9600 baud
// it reads. Drops a sentence longer than the buffer.
static void gpsEchoByte(char c) {
  if (c == '\r') return;
  if (c == '\n') {
    gpsEchoLine[gpsEchoLen] = '\0';
    if (gpsEchoLen) dualPrintf("[nmea] %s\n", gpsEchoLine);
    gpsEchoLen = 0;
    return;
  }
  if (gpsEchoLen < sizeof(gpsEchoLine) - 1) gpsEchoLine[gpsEchoLen++] = c;
}

// Non-blocking. Drains whatever bytes arrived since the last call into the
// parser.
static void gpsTick() {
  if (!gpsReady) return;
  if (gpsEchoUntil && (int32_t)(millis() - gpsEchoUntil) >= 0) {
    gpsEchoUntil = 0;
    dualPrintln("[nmea] echo off");
  }
  while (GPS_SERIAL.available()) {
    const char c = (char)GPS_SERIAL.read();
    gpsParser.encode(c);
    if (gpsEchoUntil) gpsEchoByte(c);
  }

  static unsigned long gpsLastDiag = 0;
  if (millis() - gpsLastDiag >= 5000) {
    // ok counts sentences that passed checksum and bad those that failed.
    // Together they separate garbage on the wire (ok near 0, bad climbing) from
    // valid NMEA with no fix yet (ok climbing, fixsent 0). fixsent counts only
    // fix-carrying sentences, so it stays 0 until a lock.
    dualPrintf("[gps] chars=%lu ok=%lu bad=%lu fixsent=%lu fix=%d sats=%d\n",
               (unsigned long)gpsParser.charsProcessed(),
               (unsigned long)gpsParser.passedChecksum(),
               (unsigned long)gpsParser.failedChecksum(),
               (unsigned long)gpsParser.sentencesWithFix(),
               gpsHasFix ? 1 : 0,
               gpsParser.satellites.isValid() ? (int)gpsParser.satellites.value() : -1);
    gpsLastDiag = millis();
  }

  // One gps_track row per fix, at the GPS's own 1 Hz rather than per loop().
  static uint32_t gpsLastRowMs = 0;
  if (gpsHasFix && millis() - gpsLastRowMs >= 1000) {
    gpsLastRowMs = millis();
    roostLogGpsFix();
  }

  if (gpsParser.location.isValid() &&
      gpsParser.location.age() < GPS_FIX_MAX_AGE_MS) {
    gpsHasFix = true;
    gpsLat    = gpsParser.location.lat();
    gpsLng    = gpsParser.location.lng();
    if (!gpsTimeAnchored &&
        gpsParser.date.isValid() && gpsParser.time.isValid()) {
      const uint32_t unix_ =
          gpsToUnix(gpsParser.date.year(), gpsParser.date.month(),
                    gpsParser.date.day(), gpsParser.time.hour(),
                    gpsParser.time.minute(), gpsParser.time.second());
      // A receiver reports position from the ranging solution but date only
      // once it has decoded the almanac subframe, and until then it publishes
      // its own epoch. isValid() is true for that default even alongside a good
      // position fix, so plausibility is the only test that catches it.
      //
      // A capture cannot predate the build that produced it, so the build
      // stamp is a floor no correct clock fails. Until a time clears it the
      // session has no anchor, with an empty timestamp_utc, the boot-numbered
      // directory and clock_source "none".
      if (unix_ < BIRDOSCOPE_BUILD_UNIX) {
        if (!gpsAnchorRefused) {
          gpsAnchorRefused = true;
          dualPrintf("[gps] refusing a clock older than this build "
                     "(%lu < %lu) - staying unanchored until the date decodes\n",
                     (unsigned long)unix_, (unsigned long)BIRDOSCOPE_BUILD_UNIX);
#if USE_SD
          roostLogDeviceEvent(ROOST_COMP_GNSS0, "config_error", unix_,
                              "gps time precedes build");
#endif
        }
      } else {
        gpsAnchorUnix   = unix_;
        gpsAnchorMs     = millis();
        gpsTimeAnchored = true;
        dualPrintln("[gps] UTC time anchor set");
#if USE_SD
        roostSessionAnchor();
#endif
      }
    }
  } else {
    gpsHasFix = false;
  }
}

// Parser health counters for the GPS detail screen, the same values as the [gps]
// serial diagnostic line. good = sentences that passed checksum, bad = failed
// checksum, fixSent = sentences carrying a fix, sats = satellites in view
// (-1 if the module hasn't reported any yet).
void coreGpsStats(unsigned long& good, unsigned long& bad,
                  unsigned long& fixSent, int& sats) {
  good    = (unsigned long)gpsParser.passedChecksum();
  bad     = (unsigned long)gpsParser.failedChecksum();
  fixSent = (unsigned long)gpsParser.sentencesWithFix();
  sats    = gpsParser.satellites.isValid() ? (int)gpsParser.satellites.value() : -1;
}

// GPS presence probe, the runtime check for a wired module. Drains the UART
// into the parser for up to GPS_PRESENCE_PROBE_MS and returns true as soon as a
// checksum-valid NMEA sentence arrives. The test uses passedChecksum(), since a
// floating RX pin frames line noise as bytes and charsProcessed() climbs with
// no module attached. A healthy module answers in about 1 s, and only a missing
// one costs the full window.
static bool gpsProbePresent() {
  unsigned long start = millis();
  while (millis() - start < GPS_PRESENCE_PROBE_MS) {
    while (GPS_SERIAL.available()) gpsParser.encode(GPS_SERIAL.read());
    if (gpsParser.passedChecksum() > 0) return true;
    delay(10);
  }
  return false;
}

#endif  // HAS_GPS

// ============================================================
// WIFI STATION CREDENTIALS (see core.h). `{"ssid","pass"}` on SPIFFS, set from
// the web console. Only the NTP fallback below reads them. Always compiled,
// since any board can fall back to NTP when its GPS module is absent.
// ============================================================

bool coreWifiCredsHave() {
  return fySpiffsReady && SPIFFS.exists(WIFI_CREDS_FILE);
}

bool coreWifiCredsLoad(String& ssid, String& pass) {
  ssid = String(); pass = String();
  if (!fySpiffsReady || !SPIFFS.exists(WIFI_CREDS_FILE)) return false;
  File f = SPIFFS.open(WIFI_CREDS_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    dualPrintf("[bscope] wifi creds parse failed: %s\n", err.c_str());
    return false;
  }
  ssid = String((const char*)(doc["ssid"] | ""));
  pass = String((const char*)(doc["pass"] | ""));
  return ssid.length() > 0;
}

bool coreWifiCredsSave(const char* ssid, const char* pass) {
  if (!fySpiffsReady)   { dualPrintln("[bscope] wifi creds save: no SPIFFS");        return false; }
  if (!ssid || !*ssid)  { dualPrintln("[bscope] wifi creds save: empty SSID rejected"); return false; }
  JsonDocument doc;
  doc["ssid"] = ssid;
  doc["pass"] = pass ? pass : "";
  File f = SPIFFS.open(WIFI_CREDS_FILE, "w");
  if (!f) { dualPrintln("[bscope] wifi creds save: open failed"); return false; }
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  if (ok) dualPrintf("[bscope] wifi creds saved (ssid=%s)\n", ssid);   // SSID only, never log the pass
  else    dualPrintln("[bscope] wifi creds save: write failed");
  return ok;
}

// ============================================================
// PERSISTED SETTINGS. Web-console tuning that survives a power cycle, in one
// shared JSON file.
//
// setup() must call coreSettingsLoad() once after SPIFFS is up, since nothing
// loads it lazily. Without that call the compiled-in defaults stay in force,
// though saving and reading back still work within one boot.
// ============================================================

void coreSetEnvDensity(uint8_t density) {
  if (density < DENSITY_COUNT) coreEnvDensity = density;   // ignore, don't clamp
}

void coreSetRssiAt1mDbm(int8_t dbm) {
  // Clamp, so a mistyped value cannot produce absurd ranges.
  if (dbm > RSSI_AT_1M_MAX) dbm = RSSI_AT_1M_MAX;
  if (dbm < RSSI_AT_1M_MIN) dbm = RSSI_AT_1M_MIN;
  coreRssiAt1mDbm = dbm;
}

void coreNudgeRssiAt1mDbm(int8_t db) {
  // int, since an int8_t wraps on a large step and clamps to the wrong end.
  int v = (int)coreRssiAt1mDbm + (int)db;
  if (v > RSSI_AT_1M_MAX) v = RSSI_AT_1M_MAX;
  if (v < RSSI_AT_1M_MIN) v = RSSI_AT_1M_MIN;
  coreRssiAt1mDbm = (int8_t)v;
}

bool coreSettingsLoad() {
  if (!fySpiffsReady || !SPIFFS.exists(SETTINGS_FILE)) return false;
  File f = SPIFFS.open(SETTINGS_FILE, "r");
  if (!f) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    dualPrintf("[bscope] settings parse failed: %s\n", err.c_str());
    return false;
  }
  coreSetEnvDensity((uint8_t)(doc["density"] | (int)DENSITY_MEDIUM));
  coreSetRssiAt1mDbm((int8_t)(doc["rssi_1m"] | (int)RSSI_AT_1M));
  coreSetProxRingM((uint8_t)(doc["prox_m"] | (int)PROX_RING_M));
  dualPrintf("[bscope] settings loaded (density=%s n=%.2f rssi_1m=%ddBm prox=%um)\n",
             envDensityName(coreEnvDensity), corePathLossExponent(),
             (int)coreRssiAt1mDbm, (unsigned)coreProxRingM);
  return true;
}

bool coreSettingsSave() {
  if (!fySpiffsReady) { dualPrintln("[bscope] settings save: no SPIFFS"); return false; }
  JsonDocument doc;
  doc["density"] = (int)coreEnvDensity;
  doc["rssi_1m"] = (int)coreRssiAt1mDbm;
  doc["prox_m"]  = (int)coreProxRingM;
  File f = SPIFFS.open(SETTINGS_FILE, "w");
  if (!f) { dualPrintln("[bscope] settings save: open failed"); return false; }
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  if (ok) dualPrintf("[bscope] settings saved (density=%s rssi_1m=%ddBm prox=%um)\n",
                     envDensityName(coreEnvDensity), (int)coreRssiAt1mDbm,
                     (unsigned)coreProxRingM);
  else    dualPrintln("[bscope] settings save: write failed");
  return ok;
}

void coreWifiCredsClear() {
  if (fySpiffsReady && SPIFFS.exists(WIFI_CREDS_FILE)) {
    SPIFFS.remove(WIFI_CREDS_FILE);
    dualPrintln("[bscope] wifi creds cleared");
  }
}

// ============================================================
// NTP FALLBACK. Joins the saved station network, pulls UTC, and anchors time
// through the libc clock. Runs only from coreTimeSync(), and only while GPS has
// no anchor. Without credentials it returns without touching the radio, so the
// sniffer inits from cold. After a join attempt it always leaves WiFi off.
// ============================================================

static bool     ntpTimeAnchored = false;
// The anchor moment. The manifest needs this pair to place every pre-anchor
// row after the fact.
static uint32_t ntpAnchorUnix = 0;
static uint32_t ntpAnchorMs   = 0;

static void ntpSync() {
  String ssid, pass;
  if (!coreWifiCredsLoad(ssid, pass)) {
    dualPrintln("[bscope] no saved WiFi network - timestamping from boot");
    return;   // WiFi stays down, so the sniffer starts from a clean radio
  }

  WiFi.mode(WIFI_STA);

  // Scan before the blocking join. On a GPS board this runs every boot, and on
  // the move the saved network is usually out of range, so a short scan skips
  // to millis() without spending NTP_JOIN_TIMEOUT_MS first. A failed scan says
  // nothing about range, so the join still runs, best effort.
  dualPrintf("[bscope] scanning for \"%s\"...\n", ssid.c_str());
  int  n       = WiFi.scanNetworks();
  bool inRange = (n < 0);   // scan failed → attempt join anyway
  for (int i = 0; i < n && !inRange; i++)
    if (WiFi.SSID(i) == ssid) inRange = true;
  if (n >= 0) WiFi.scanDelete();
  if (!inRange) {
    dualPrintln("[bscope] saved network not in range - timestamping from boot");
    WiFi.mode(WIFI_OFF);
    return;
  }

  dualPrintf("[bscope] joining \"%s\" for NTP time sync...\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < NTP_JOIN_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.status() != WL_CONNECTED) {
    dualPrintln("[bscope] WiFi join timed out - timestamping from boot");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    ntpTimeAnchored = true;
    ntpAnchorUnix   = (uint32_t)time(nullptr);
    ntpAnchorMs     = millis();
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    dualPrintf("[bscope] time synced: %s\n", buf);
  } else {
    dualPrintln("[bscope] NTP sync failed - timestamping from boot");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void coreTimeSync() {
#if HAS_GPS
  gpsSetup();
  if (gpsProbePresent())
    dualPrintln("[bscope] GPS module present - it will master timing once it locks");
  else
    dualPrintln("[bscope] no GPS module detected");
  // GPS cannot lock this early, since a cold start takes far longer than the
  // boot probe, so NTP bridges the pre-lock window either way. GPS takes over
  // when it locks (see TIME SOURCE). Without a saved network ntpSync() returns
  // at once, and timestamps count from millis() until GPS locks.
  if (!gpsTimeAnchored) ntpSync();
#else
  ntpSync();   // joins only with stored credentials
#endif
}

void coreTick() {
#if HAS_GPS
  gpsTick();
#endif
  // NTP is a one-shot join at boot via coreTimeSync(), so there is nothing to poll.
}

bool coreTimeAnchored() {
#if HAS_GPS
  if (gpsTimeAnchored) return true;
#endif
  return ntpTimeAnchored;
}

static void coreTimestampStr(char* buf, size_t len) {
#if HAS_GPS
  if (gpsTimeAnchored) {
    uint32_t nowUnix = gpsAnchorUnix + (millis() - gpsAnchorMs) / 1000;
    unixToIso(nowUnix, buf, len);
    return;
  }
#endif
  if (ntpTimeAnchored) {   // only read the libc clock once NTP has set it
    time_t now = time(nullptr);
    struct tm tmInfo;
    gmtime_r(&now, &tmInfo);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tmInfo);
    return;
  }
  snprintf(buf, len, "%lums", millis());
}

// ============================================================
// SD CARD
//
// roost_session.cpp writes the SD log, a session directory of roost record
// files.
// ============================================================

// ============================================================
// SERIAL JSON EMISSION
// ============================================================
//
// Emits one JSON object per detection per line over USB CDC serial, in the
// upstream flock-you schema. GPS comes from this board's own fix, and a board
// without GPS emits `"gps":null`.

static void emitDetectionJSON(const char* mac, const char* method,
                              int8_t rssi, uint8_t ch, const char* ssid,
                              const char* apMac) {
  char ssidEsc[33 * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
  char oui[9];
  uint8_t mbytes[6] = {0};
  sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
  ouiFromMac(mbytes, oui, sizeof(oui));

  // ap_mac appears only on an oui_addr1 hit, and names the AP whose probe
  // response revealed the camera. Its position, from wardriving data, bounds
  // the camera location.
  char apMacField[28];
  if (apMac && apMac[0])
    snprintf(apMacField, sizeof(apMacField), "\"%s\"", apMac);
  else
    snprintf(apMacField, sizeof(apMacField), "null");

#if HAS_GPS
  if (gpsHasFix) {
    dualPrintf(
        "{\"event\":\"detection\","
        "\"detection_method\":\"wifi_%s\","
        "\"protocol\":\"wifi_2_4ghz\","
        "\"mac_address\":\"%s\","
        "\"oui\":\"%s\","
        "\"device_name\":\"\","
        "\"rssi\":%d,"
        "\"channel\":%u,"
        "\"frequency\":%u,"
        "\"ssid\":\"%s\","
        "\"ap_mac\":%s,"
        "\"gps\":{\"latitude\":%.6f,\"longitude\":%.6f}}\n",
        method, mac, oui, rssi,
        (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc,
        apMacField, gpsLat, gpsLng);
    return;
  }
#endif
  dualPrintf(
      "{\"event\":\"detection\","
      "\"detection_method\":\"wifi_%s\","
      "\"protocol\":\"wifi_2_4ghz\","
      "\"mac_address\":\"%s\","
      "\"oui\":\"%s\","
      "\"device_name\":\"\","
      "\"rssi\":%d,"
      "\"channel\":%u,"
      "\"frequency\":%u,"
      "\"ssid\":\"%s\","
      "\"ap_mac\":%s,"
      "\"gps\":null}\n",
      method, mac, oui, rssi,
      (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc, apMacField);
}

// ============================================================
// RSSI TO DISTANCE. Log-distance path loss, with Environment Density picking n
// and coreRssiAt1mDbm as the reference level.
//
// Callers skip addr1 hits, whose RSSI describes the AP-to-scanner path, and
// coreHandleAlert() already does. Both settings are runtime and persisted, so
// the result depends on more than the board config. RSSI_AT_1M and the
// PATH_LOSS_N_* presets near the top of this file are only defaults.
// docs/distance_estimation.md documents the model, calibration, and the
// invariants that fail quietly.
// ============================================================

volatile uint8_t coreEnvDensity = DENSITY_MEDIUM;
volatile int8_t  coreRssiAt1mDbm = RSSI_AT_1M;

// Set after the dedupe gate, so it tracks only readings the user saw.
static volatile int8_t lastDetectionRssi = 0;

int8_t coreLastDetectionRssi() { return lastDetectionRssi; }

// Indexed by EnvDensity. Order must match the enum in core.h.
static const float DENSITY_N[DENSITY_COUNT] = {
  PATH_LOSS_N_LOW, PATH_LOSS_N_MED, PATH_LOSS_N_HIGH,
};

float corePathLossExponent() {
  uint8_t d = coreEnvDensity;
  return DENSITY_N[(d < DENSITY_COUNT) ? d : DENSITY_MEDIUM];
}

const char* envDensityName(uint8_t density) {
  switch (density) {
    case DENSITY_LOW:    return "low";
    case DENSITY_MEDIUM: return "medium";
    case DENSITY_HIGH:   return "high";
    default:             return "unknown";
  }
}

float coreRssiToDistanceM(int8_t rssi) {
  return powf(10.0f, ((float)coreRssiAt1mDbm - (float)rssi)
                     / (10.0f * corePathLossExponent()));
}

// Defined with the notification module below. coreHandleAlert() calls it
// directly.
static void notifyDetection(bool chirpWorthy, bool rangeable, int8_t vendor);
static void notifyProximity(int8_t vendor);

// ============================================================
// PROXIMITY ALERT, a latched range ring described in core.h. Three things stop
// one ring from turning into a stream of chirps. The EMA absorbs the 6-10 dB
// multipath swing, the latch makes a crossing an event, and the hysteresis band
// stops the latch re-arming on leftover jitter. docs/alerts.md, under the
// proximity ring, gives what the constants must absorb.
// ============================================================

const uint8_t PROX_RING_OPTIONS[PROX_RING_OPTION_COUNT] = { 0, 10, 25, 50, 100 };

volatile uint8_t coreProxRingM = PROX_RING_M;

void coreSetProxRingM(uint8_t metres) {
  coreProxRingM = metres;
  // Every latch is relative to the old ring, so a stale one would silence the
  // first crossing of the new one.
  for (int i = 0; i < fyDetCount; i++) fyDet[i].proxLatched = 0;
}

int coreProxRingIndex() {
  for (int i = 0; i < PROX_RING_OPTION_COUNT; i++)
    if (PROX_RING_OPTIONS[i] == coreProxRingM) return i;
  return -1;
}

// Updates the smoothed RSSI for one detection and returns true when this
// reading is the inward crossing of the ring.
static bool proximityEvaluate(int idx, AlertType type, int8_t rssi) {
  if (idx < 0) return false;                    // table full, no row to hold state
  if (coreProxRingM == 0) return false;         // ring off
  // addr1 measures the AP that answered the probe, not the target. Same
  // exclusion coreHandleAlert() already applies to distM.
  if (type == ALERT_OUI_ADDR1) return false;

  FYDetection& d = fyDet[idx];
  bool seeding = (d.emaRssi == 0);
  if (seeding) {
    d.emaRssi = rssi;
  } else {
    // The step floors at 1 dB. A bare shift truncates differences under
    // 2^PROX_EMA_SHIFT to zero, so the average would never reach a steady
    // reading. Computed in int space so the intermediate cannot wrap.
    int diff = (int)rssi - (int)d.emaRssi;
    int step = diff >> PROX_EMA_SHIFT;
    if (step == 0 && diff != 0) step = (diff > 0) ? 1 : -1;
    d.emaRssi = (int8_t)((int)d.emaRssi + step);
  }

  float dist  = coreRssiToDistanceM(d.emaRssi);
  float ring  = (float)coreProxRingM;
  bool  inside = dist < ring;

  if (!inside) {
    if (dist > ring * (PROX_HYST_PCT / 100.0f)) d.proxLatched = 0;
    return false;
  }
  if (d.proxLatched) return false;
  d.proxLatched = 1;
  // A row seeded inside the ring latches silently, since the new-detection
  // chirp fires for the same frame.
  return !seeding;
}

// ============================================================
// coreHandleAlert(), the shared middle of drainAlertQueue(). It updates the
// detection table, appends the SD log row, applies the dedupe gate, prints the
// DETECT line, emits JSON and notifies on the LED and buzzer. Boards call it
// once per dequeued AlertEntry and use the result for display state.
// ============================================================

CoreAlertResult coreHandleAlert(const AlertEntry& e) {
  CoreAlertResult r{};
  macToStr(e.mac, r.macStr, sizeof(r.macStr));
  const char* method = alertTypeToMethod(e.type);

  // Log-only, so this returns ahead of the detection table, the tallies,
  // fyLastTargetSeen, the dedupe slots and every output path (spec O4).
  // `suppressed` already tells a board not to update its display from a row.
  if (e.type == ALERT_SURVEY) {
#if USE_SD
    roostLogWifiObs(e, method);
#endif
    r.detIdx     = -1;
    r.suppressed = true;
    r.type       = e.type;
    return r;
  }

  // Re-matched here to keep AlertEntry small. -1 for an ALERT_SSID hit, which
  // matched on name.
  const int ouiIdx = matchOuiIndex(e.mac);
  r.vendor = ouiIdx < 0 ? -1 : (int8_t)oui_table[ouiIdx].vendor;

  // An infra match writes its row and nothing else. It takes no detection
  // table slot, refreshes no active-target state and never alerts, spec M8.
  if (ouiIdx >= 0 && oui_table[ouiIdx].cls == OUI_CLASS_INFRA) {
#if USE_SD
    roostLogWifiObs(e, method);
#endif
    r.detIdx     = -1;
    r.suppressed = true;
    r.type       = e.type;
    return r;
  }

  char apMacStr[18] = "";
  if (e.type == ALERT_OUI_ADDR1) macToStr(e.addr2, apMacStr, sizeof(apMacStr));

  float distM = (e.type != ALERT_OUI_ADDR1) ? coreRssiToDistanceM(e.rssi) : -1.0f;

  bool chirpWorthy = false;
  // Direct when the camera itself transmitted, so the RSSI describes the path
  // to it. Every type except an addr1 hit is direct.
  bool direct = (e.type != ALERT_OUI_ADDR1);
  // The detection table feeds the display and holds a printable name. The
  // record column takes the octets and the length. Convert once here, so no
  // consumer decides for itself what an empty SSID means.
  int idx = fyAddDetection(r.macStr, method, e.rssi, e.channel,
                            roostSsidPrintable(&e.ssid),
                            direct, &chirpWorthy);

#if USE_SD
  roostLogWifiObs(e, method);
#endif

  // Refresh unconditionally, since a device counts as active even when the
  // dedupe gate below rate-limits its serial/JSON/display output.
  fyLastTargetSeen = millis();

  // The tallies likewise count what the radio heard, ahead of the dedupe gate.
  tallyFrame(e.type);

  r.detIdx      = idx;
  r.count       = (idx >= 0) ? (uint16_t)fyDet[idx].count : 0;
  r.chirpWorthy = chirpWorthy;
  r.rssi        = e.rssi;
  r.channel     = e.channel;
  r.distM       = distM;
  r.type        = e.type;
  strlcpy(r.frameKind, e.frameKind, sizeof(r.frameKind));
  ouiFromMac(e.mac, r.oui, sizeof(r.oui));

  // Ahead of the dedupe gate, which would swallow the crossing, and outside
  // it, since that gate owns the DETECT line and the JSON emit. Skipped when
  // the new-detection chirp is already firing for this frame.
  if (proximityEvaluate(idx, e.type, e.rssi) && !chirpWorthy) {
    notifyProximity(r.vendor);
  }

  if (shouldSuppressDuplicate(r.macStr)) {
    r.suppressed = true;
    return r;
  }
  r.suppressed = false;
  lastDetectionRssi = e.rssi;   // calibration reference, see coreLastDetectionRssi()

  // The printable name, once, for every display consumer below.
  const char* name = roostSsidPrintable(&e.ssid);

  if (e.type == ALERT_SSID) {
    dualPrintf("[bscope] DETECT-SSID type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u count=%d\n",
               e.frameKind, r.macStr, name ? name : "", e.rssi, e.channel,
               (int)r.count);
  } else {
    dualPrintf("[bscope] DETECT-OUI mac=%s oui=%s rssi=%d ch=%u addr=%s count=%d%s%s\n",
               r.macStr, r.oui, e.rssi, e.channel,
               e.frameKind[0] ? e.frameKind : "addr2", (int)r.count,
               name ? " ssid=" : "", name ? name : "");
  }

  emitDetectionJSON(r.macStr, method, e.rssi, e.channel, name, apMacStr);
  // distM is -1 for exactly the addr1 hits, so it serves as the predicate.
  notifyDetection(r.chirpWorthy, r.distM >= 0.0f, r.vendor);
  return r;
}

// ============================================================
// NOTIFICATIONS, LED (NeoPixel) and buzzer.
//
// A detection shows two facts at once. Colour shows the vendor, so the fleet is
// readable without the panel, and pulse count shows whether the MAC is new.
// ledTick() steps the pulse trains, since notifyDetection() runs in the alert
// drain path and must not block. The detection and heartbeat paths honour
// coreLedEnabled and coreBuzzerEnabled. The boot jingle, RGB cycle and replay
// hooks ignore them. See docs/alerts.md.
// ============================================================

// Per-vendor detection colours, overridable per board alongside the other
// LED_COLOR_* values. See docs/alerts.md.
#ifndef LED_COLOR_FLOCK_R
#define LED_COLOR_FLOCK_R 0
#define LED_COLOR_FLOCK_G 0
#define LED_COLOR_FLOCK_B 180
#endif
#ifndef LED_COLOR_AXON_R
#define LED_COLOR_AXON_R  180
#define LED_COLOR_AXON_G  150
#define LED_COLOR_AXON_B  0
#endif
#ifndef LED_COLOR_AXIS_R
#define LED_COLOR_AXIS_R  0
#define LED_COLOR_AXIS_G  160
#define LED_COLOR_AXIS_B  160
#endif
#ifndef LED_COLOR_UTILITY_R
#define LED_COLOR_UTILITY_R 160
#define LED_COLOR_UTILITY_G 0
#define LED_COLOR_UTILITY_B 160
#endif

// Blink train state, stepped by ledTick() so a multi-pulse pattern never blocks
// the alert drain path.
static unsigned long ledNextAt = 0;   // 0 = idle, no train running
static uint8_t  ledPulsesLeft = 0;
static bool     ledLit        = false;
static uint8_t  ledR = 0, ledG = 0, ledB = 0;
static unsigned ledPulseMs = 0;

// Runtime alert gates (declared in core.h), flipped live from the Alerts menu.
// Enabled by default each boot, session-only, and not persisted.
bool coreBuzzerEnabled = true;
bool coreLedEnabled    = true;

#if USE_LED
#include <Adafruit_NeoPixel.h>
static Adafruit_NeoPixel neopixel(1, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

static inline void ledSet(uint8_t r, uint8_t g, uint8_t b) {
#if USE_LED
  neopixel.setPixelColor(0, neopixel.Color(r, g, b));
  neopixel.show();
#endif
}

// Equal on/off intervals of ledPulseMs until ledPulsesLeft reaches zero, then
// parks the LED off and goes idle. coreNotifyTick() calls it every loop().
static void ledTick() {
#if USE_LED
  if (!ledNextAt) return;                              // idle
  if ((long)(millis() - ledNextAt) < 0) return;        // interval not up
  if (ledLit) {
    ledSet(0, 0, 0);
    ledLit = false;
    if (ledPulsesLeft == 0) { ledNextAt = 0; return; } // train finished
  } else {
    ledSet(ledR, ledG, ledB);
    ledLit = true;
    ledPulsesLeft--;
  }
  ledNextAt = millis() + ledPulseMs;
  if (!ledNextAt) ledNextAt = 1;                       // 0 is the idle sentinel
#endif
}

// Replaces any train still running, so the LED shows the newest detection.
static void ledBlink(uint8_t r, uint8_t g, uint8_t b, unsigned ms, uint8_t pulses) {
#if USE_LED
  if (pulses == 0) return;
  ledR = r; ledG = g; ledB = b;
  ledPulseMs = ms;
  ledPulsesLeft = pulses;
  ledLit = false;
  ledNextAt = 1;    // any nonzero past time, so the tick below lights pulse one
  ledTick();        // light it now instead of up to one loop() later
#endif
}

static void ledFlash(uint8_t r, uint8_t g, uint8_t b, unsigned ms) {
  ledBlink(r, g, b, ms, 1);
}

// Two fast ascending beeps, played on the first sighting of a MAC.
static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

// Three descending beeps on a ring crossing, against the new-detection
// chirp's two ascending. Tones default to NEW_CHIRP_*, so a board that tuned
// its chirp for its own piezo gets a matching one here.
#if USE_BUZZER
#ifndef PROX_CHIRP_HI_HZ
#define PROX_CHIRP_HI_HZ  NEW_CHIRP_HI_HZ
#endif
#ifndef PROX_CHIRP_MID_HZ
#define PROX_CHIRP_MID_HZ NEW_CHIRP_LO_HZ
#endif
#ifndef PROX_CHIRP_LO_HZ
#define PROX_CHIRP_LO_HZ  (NEW_CHIRP_LO_HZ * 3 / 4)
#endif
#ifndef PROX_CHIRP_NOTE_MS
#define PROX_CHIRP_NOTE_MS NEW_CHIRP_NOTE_MS
#endif
#ifndef PROX_CHIRP_GAP_MS
#define PROX_CHIRP_GAP_MS  NEW_CHIRP_GAP_MS
#endif
#endif

static void proximityChirp() {
#if USE_BUZZER
  static const uint16_t notes[3] = { PROX_CHIRP_HI_HZ, PROX_CHIRP_MID_HZ, PROX_CHIRP_LO_HZ };
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, notes[i]); delay(PROX_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
    if (i < 2) delay(PROX_CHIRP_GAP_MS);
  }
#endif
}

// Silent despite the name. Pulses the LED purple while a target stays in range,
// last seen within HB_DEVICE_ACTIVE_MS. Uncalled on screen models, spec A1.
__attribute__((unused))
static void heartbeatBeep() {
#if USE_LED
  if (!coreLedEnabled) return;   // LED off in the Alerts menu
  ledFlash(LED_COLOR_HB_R, LED_COLOR_HB_G, LED_COLOR_HB_B, LED_FLASH_MS);
#endif
}

// ---- Boot call --------------------------------------------------------------
//
// Approximates bird calls on a single square-wave element by cadence and pitch
// glide, with a frequency wobble standing in for rasp. Each call plays in the
// element's efficient band, transposed from its true pitch.
// Spec A5. docs/alerts.md covers the acoustics and the tuning knobs.
#define BOOT_SOUND_JINGLE 0   // six-note descending motif, the original
#define BOOT_SOUND_CROW   1   // "ca-CAW ca-CAW"
#define BOOT_SOUND_HAWK   2   // "kee-ahrrr", a single descending scream

#ifndef BOOT_SOUND
#define BOOT_SOUND BOOT_SOUND_CROW
#endif
// Wobble depth as a percentage of the current frequency, and the sweep step.
// The step doubles as the wobble period, so halving it doubles the rasp rate.
#ifndef BIRD_RASP_PCT
#define BIRD_RASP_PCT 7
#endif
#ifndef BIRD_STEP_MS
#define BIRD_STEP_MS  6
#endif

// One syllable, a glide from f0 to f1 over ms, roughened by raspPct. Blocking
// like every other player here, so only boot and serial context call it.
static void birdSyllable(uint16_t f0, uint16_t f1, uint16_t ms, uint8_t raspPct) {
#if USE_BUZZER
  const uint16_t steps = ms / BIRD_STEP_MS;
  if (!steps) return;
  for (uint16_t i = 0; i < steps; i++) {
    int32_t f = (int32_t)f0 + (((int32_t)f1 - (int32_t)f0) * (int32_t)i) / (int32_t)steps;
    if (raspPct && (i & 1)) f -= (f * (int32_t)raspPct) / 100;
    tone(BUZZER_PIN, (unsigned)f);
    delay(BIRD_STEP_MS);
  }
  noTone(BUZZER_PIN);
#endif
}

// A clipped grace note into a longer accented one that falls away, twice. The
// repeat makes it read as a call.
static void crowCall() {
  for (int i = 0; i < 2; i++) {
    birdSyllable(1450, 1330,  60, BIRD_RASP_PCT);   // "ca"
    delay(30);
    birdSyllable(2000, 1500, 190, BIRD_RASP_PCT);   // "CAW"
    if (i == 0) delay(170);
  }
}

// A thin rising attack breaking into a long descending scream. Closer to a pure
// glide than a caw, so it survives the element with less distortion.
static void hawkCall() {
  birdSyllable(2300, 2650,  90, 0);
  birdSyllable(2650, 1500, 420, BIRD_RASP_PCT / 2);
}

static void legacyJingle() {
#if USE_BUZZER
  // First 6 notes of SMB World 1-2 (underground). Koji Kondo's descending
  // pattern, C5 C4 A4 A3 G#4 G#3, in alternating-octave pairs.
  static const uint16_t notes[6] = { 523, 262, 440, 220, 415, 208 };
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#endif
}

static void startupBeep() {
#if BOOT_SOUND == BOOT_SOUND_CROW
  crowCall();
#elif BOOT_SOUND == BOOT_SOUND_HAWK
  hawkCall();
#else
  legacyJingle();
#endif
}

// Public hooks (declared in core.h) that let serial and web commands replay the
// buzzer sounds on demand for tone tuning. Thin wrappers over the static
// players above. Both no-op on a board without a buzzer.
void corePlayDetectChirp()    { newDetectChirp(); }
void corePlayStartupJingle()  { startupBeep(); }
void corePlayProximityChirp() { proximityChirp(); }
void corePlayCrowCall()       { crowCall(); }
void corePlayHawkCall()       { hawkCall(); }

// Uncalled on any board with a display, per spec A1. A display-less board
// re-enables it by calling heartbeatTick() from coreNotifyTick().
// notifyDetection() keeps fyLastHeartbeatAt current, so the phase survives.

// Last time the heartbeat pulse fired. When no target appears for
// HB_DEVICE_ACTIVE_MS the heartbeat stops until the next new detection.
static unsigned long fyLastHeartbeatAt = 0;

__attribute__((unused))
static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;                           // never seen one
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;    // gone silent
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;   // too soon
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

// Flock blue, Axon yellow. An unmatched vendor (an `ssid_keyword` hit, which has
// no OUI to attribute) keeps the generic detection colour.
#if USE_LED
static void vendorLedColor(int8_t vendor, uint8_t& r, uint8_t& g, uint8_t& b) {
  switch (vendor) {
    case VENDOR_FLOCK:   r = LED_COLOR_FLOCK_R;   g = LED_COLOR_FLOCK_G;   b = LED_COLOR_FLOCK_B;   break;
    case VENDOR_AXON:    r = LED_COLOR_AXON_R;    g = LED_COLOR_AXON_G;    b = LED_COLOR_AXON_B;    break;
    case VENDOR_AXIS:    r = LED_COLOR_AXIS_R;    g = LED_COLOR_AXIS_G;    b = LED_COLOR_AXIS_B;    break;
    case VENDOR_UTILITY: r = LED_COLOR_UTILITY_R; g = LED_COLOR_UTILITY_G; b = LED_COLOR_UTILITY_B; break;
    default:           r = LED_COLOR_R;       g = LED_COLOR_G;       b = LED_COLOR_B;       break;
  }
}
#endif

// A new MAC chirps and blinks twice, and a repeat blinks once in silence. See
// docs/alerts.md.
//
// `rangeable` gates the buzzer and never the LED, spec A2 and A3.
static void notifyDetection(bool chirpWorthy, bool rangeable, int8_t vendor) {
  if (chirpWorthy && rangeable) {
    if (coreBuzzerEnabled) newDetectChirp();   // buzzer muted in the Alerts menu
    // Reset the heartbeat phase so the first follow-up beep lands
    // HB_BEEP_INTERVAL_MS after the initial chirp, not mid-window.
    fyLastHeartbeatAt = millis();
  }
#if USE_LED
  if (coreLedEnabled) {
    uint8_t r, g, b;
    vendorLedColor(vendor, r, g, b);
    ledBlink(r, g, b, LED_FLASH_MS, chirpWorthy ? 2 : 1);
  }
#else
  (void)vendor;
#endif
}

// A ring crossing pulses three times, against two for new and one for repeat.
// Colour still shows the vendor.
static void notifyProximity(int8_t vendor) {
  if (coreBuzzerEnabled) proximityChirp();
#if USE_LED
  if (coreLedEnabled) {
    uint8_t r, g, b;
    vendorLedColor(vendor, r, g, b);
    ledBlink(r, g, b, LED_FLASH_MS, 3);
  }
#else
  (void)vendor;
#endif
}

void coreNotifyBoot() {
#if USE_LED
  neopixel.begin();
  neopixel.setBrightness(128);
  ledSet(0, 0, 0);
#endif

  startupBeep();

#if USE_LED
  // RGB sanity check. Cycles red, green, blue so a wiring or dead-pixel fault
  // is obvious.
  ledSet(255, 0,   0);   delay(200);
  ledSet(0,   255, 0);   delay(200);
  ledSet(0,   0,   255); delay(200);
  ledSet(0,   0,   0);
  ledFlash(LED_COLOR_BOOT_R, LED_COLOR_BOOT_G, LED_COLOR_BOOT_B, 200);
#endif
}

void coreNotifyTick() {
  // heartbeatTick() deliberately absent on screen models, spec A1.
  ledTick();        // turn off LED after LED_FLASH_MS
}

void coreLedBlink(uint8_t r, uint8_t g, uint8_t b,
                  uint8_t count, unsigned on_ms, unsigned off_ms) {
#if USE_LED
  for (uint8_t i = 0; i < count; i++) {
    ledSet(r, g, b);  delay(on_ms);
    ledSet(0, 0, 0);  delay(off_ms);
  }
  // Abandon any train in flight so ledTick() won't relight after this.
  ledNextAt = 0;
  ledPulsesLeft = 0;
  ledLit = false;
#endif
}

// ============================================================
// INPUT, plain debounced buttons
// ============================================================

#if HAS_BUTTONS
static bool          btn1LastState  = HIGH;
static unsigned long btn1LastChange = 0;
static bool          btn2LastState  = HIGH;
static unsigned long btn2LastChange = 0;
#endif

InputEvent coreInputTick() {
#if HAS_BUTTONS
  static bool initialized = false;
  if (!initialized) {
    pinMode(BTN_PIN_1, INPUT_PULLUP);
    pinMode(BTN_PIN_2, INPUT_PULLUP);
    initialized = true;
  }

  unsigned long now = millis();
  InputEvent ev = INPUT_NONE;

  bool b1 = digitalRead(BTN_PIN_1);
  if (b1 != btn1LastState && now - btn1LastChange > BTN_DEBOUNCE_MS) {
    btn1LastChange = now;
    btn1LastState  = b1;
    if (b1 == LOW) ev = INPUT_TOGGLE_SCREEN;   // pressed (active-low, INPUT_PULLUP)
  }

  bool b2 = digitalRead(BTN_PIN_2);
  if (b2 != btn2LastState && now - btn2LastChange > BTN_DEBOUNCE_MS) {
    btn2LastChange = now;
    btn2LastState  = b2;
    if (b2 == LOW) ev = INPUT_MANUAL_MARK;
  }

  return ev;
#else
  return INPUT_NONE;
#endif
}

// ============================================================
// SEMANTIC NAV LAYER, described in core.h. The serial injector (coreInjectNav)
// and the three- or four-button schemes (coreNavTick) feed a small event queue.
// coreNavApply() runs the screen-carousel state machine.
// ============================================================

#define NAV_QUEUE_SIZE 8
static NavEvent navQueue[NAV_QUEUE_SIZE];
static uint8_t  navQHead = 0, navQTail = 0;

void coreInjectNav(NavEvent ev) {
  uint8_t next = (uint8_t)((navQTail + 1) % NAV_QUEUE_SIZE);
  if (next == navQHead) return;          // full, drop the newest to avoid overwrite
  navQueue[navQTail] = ev;
  navQTail = next;
}

static NavEvent navQPop() {
  if (navQHead == navQTail) return NAV_NONE;
  NavEvent ev = navQueue[navQHead];
  navQHead = (uint8_t)((navQHead + 1) % NAV_QUEUE_SIZE);
  return ev;
}

NavEvent coreNavTick() {
#if NAV_BTN_COUNT
  // Per-button edge + long-press tracker. Index i = BTN_(i+1). A short press
  // fires on release, so the tracker can tell it from a long one. A long press
  // fires the moment it crosses NAV_LONG_PRESS_MS while still held, so
  // MARK/BACK feel immediate. Buttons are active-LOW (INPUT_PULLUP).
  static bool          initialized = false;
  static bool          last[NAV_BTN_COUNT];
  static unsigned long changedAt[NAV_BTN_COUNT];
  static unsigned long pressedAt[NAV_BTN_COUNT];
  static bool          longFired[NAV_BTN_COUNT];
  static bool          holdFired[NAV_BTN_COUNT];
  static unsigned long lastBackMs[NAV_BTN_COUNT];   // 0 = no press pending a pair
#if NAV_SCHEME_4BTN
  // A dedicated BACK button frees BTN_3 of its long press.
  static const uint8_t  pins[4]    = { BTN_PIN_1, BTN_PIN_2, BTN_PIN_3, BTN_PIN_4 };
  static const NavEvent shortEv[4] = { NAV_UP,   NAV_DOWN, NAV_SELECT, NAV_BACK };
  static const NavEvent longEv[4]  = { NAV_MARK, NAV_NONE, NAV_NONE,   NAV_NONE };  // NAV_NONE = no long action
#else
  static const uint8_t  pins[3]    = { BTN_PIN_1, BTN_PIN_2, BTN_PIN_3 };
  static const NavEvent shortEv[3] = { NAV_UP,   NAV_DOWN, NAV_SELECT };
  static const NavEvent longEv[3]  = { NAV_MARK, NAV_NONE, NAV_BACK   };  // NAV_NONE = no long action
#endif

  if (!initialized) {
    for (int i = 0; i < NAV_BTN_COUNT; i++) {
      pinMode(pins[i], INPUT_PULLUP);
      last[i] = HIGH; changedAt[i] = 0; pressedAt[i] = 0;
      longFired[i] = false; holdFired[i] = false; lastBackMs[i] = 0;
    }
    initialized = true;
  }

  unsigned long now = millis();
  for (int i = 0; i < NAV_BTN_COUNT; i++) {
    bool lvl = digitalRead(pins[i]);
    if (lvl != last[i] && now - changedAt[i] > BTN_DEBOUNCE_MS) {   // debounced edge
      changedAt[i] = now;
      last[i]      = lvl;
      if (lvl == LOW) {                         // press
        pressedAt[i] = now;
        longFired[i] = false;
        holdFired[i] = false;
      } else if (!longFired[i]) {               // release without a prior long → short
        coreInjectNav(shortEv[i]);
        // Two Back presses inside NAV_BACK_DOUBLE_MS also emit NAV_BACK_HOLD.
        if (shortEv[i] == NAV_BACK) {
          if (lastBackMs[i] != 0 && now - lastBackMs[i] <= NAV_BACK_DOUBLE_MS) {
            coreInjectNav(NAV_BACK_HOLD);
            lastBackMs[i] = 0;                  // consume, no triple-press retrigger
          } else {
            lastBackMs[i] = now;
          }
        }
      }
    }
    // Long press fires once, while still held, as soon as the threshold passes.
    if (last[i] == LOW && !longFired[i] && longEv[i] != NAV_NONE
        && now - pressedAt[i] >= NAV_LONG_PRESS_MS) {
      coreInjectNav(longEv[i]);
      longFired[i] = true;
    }
    // Whichever button carries BACK, on either scheme, also emits NAV_BACK_HOLD
    // once held this long. Setting longFired suppresses the short-press BACK on
    // release, so a 4-button hold is never also a click. A 3-button BACK has
    // already fired by this point, and its only consumer discards it.
    if (last[i] == LOW && !holdFired[i]
        && (shortEv[i] == NAV_BACK || longEv[i] == NAV_BACK)
        && now - pressedAt[i] >= NAV_EXIT_HOLD_MS) {
      coreInjectNav(NAV_BACK_HOLD);
      holdFired[i] = true;
      longFired[i] = true;
    }
    if (lastBackMs[i] != 0 && now - lastBackMs[i] > NAV_BACK_DOUBLE_MS) lastBackMs[i] = 0;
  }
#endif
  return navQPop();
}

// ------------------------------------------------------------
// SCREEN CAROUSEL AND MENU DRILL-IN. Up and Down cycle the top-level screens.
// SELECT on a menu screen opens its option list, and selecting Single or
// Proximity opens a picker. MARK works at every level. See docs/menu_ux.md.
// ------------------------------------------------------------

ScreenId  coreCurrentScreen = SCREEN_OVERVIEW;
MenuState coreMenuState      = MENU_NONE;
int       coreMenuSel        = 0;

int              coreWipeConfirmCount = 0;
static WipeScope g_wipeScope          = WIPE_NONE;
WipeScope coreWipeSelectedScope() { return g_wipeScope; }

NavAction coreNavApply(NavEvent ev) {
  // Available on every screen and menu except an armed wipe, where only Select
  // and Back work and a mark would write a row the wipe then erases.
  if (ev == NAV_MARK)
    return (coreMenuState == MENU_CONFIRM_WIPE) ? NAV_ACT_NONE : NAV_ACT_MARK;

  switch (coreMenuState) {

  case MENU_NONE:
    switch (ev) {
      case NAV_UP:      // advance forward through the screens (wraps at SCREEN_COUNT)
        coreCurrentScreen = coreStepScreen(coreCurrentScreen, +1);
        return NAV_ACT_REDRAW;
      case NAV_DOWN:    // go back a screen (wraps)
        coreCurrentScreen = coreStepScreen(coreCurrentScreen, -1);
        return NAV_ACT_REDRAW;
      case NAV_SELECT:
        if (coreCurrentScreen == SCREEN_SCAN_MODES) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = coreScanModeIndex();      // start on the active mode
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_TARGETS) {
          coreMenuState = MENU_LIST;
          const int t   = coreTargetIndex();
          coreMenuSel   = (t >= 0) ? t : 0;           // start on the active set
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_RADIO) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = (int)coreRadioMode;         // the enum is the row index
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_ALERTS) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = 0;                          // start on the Buzzer row
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_CONFIG) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = 1;                          // Off, since the portal stays closed while browsing Detect
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_WIPE) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = 0;                          // the narrower scope, device only
          return NAV_ACT_REDRAW;
        }
        return NAV_ACT_NONE;                           // info screens have nothing to select
      default:
        return NAV_ACT_NONE;                           // BACK at top level does nothing
    }

  case MENU_LIST: {
    // Row count of the open list, one entry per menu screen.
    int n;
    switch (coreCurrentScreen) {
      case SCREEN_SCAN_MODES: n = 3; break;
      case SCREEN_TARGETS:    n = TARGET_ROW_COUNT; break;
      case SCREEN_RADIO:      n = RADIO_MODE_COUNT; break;
      case SCREEN_ALERTS:     n = 3; break;   // Buzzer / LED / Proximity
      default:                n = 2; break;   // CONFIG / WIPE
    }
    switch (ev) {
      case NAV_UP:   coreMenuSel = (coreMenuSel + n - 1) % n; return NAV_ACT_REDRAW;
      case NAV_DOWN: coreMenuSel = (coreMenuSel + 1) % n;     return NAV_ACT_REDRAW;
      case NAV_BACK: coreMenuState = MENU_NONE;               return NAV_ACT_REDRAW;
      case NAV_SELECT:
        if (coreCurrentScreen == SCREEN_ALERTS) {
          // Toggle the highlighted gate and stay in the list, so the operator
          // can flip several rows before Back steps out.
          if (coreMenuSel == 0)      coreBuzzerEnabled = !coreBuzzerEnabled;  // Buzzer
          else if (coreMenuSel == 1) coreLedEnabled    = !coreLedEnabled;     // LED
          else {
            // Proximity is a range, so it opens a picker, like Single and its
            // channel picker.
            coreMenuState = MENU_PICK_PROX;
            const int p   = coreProxRingIndex();
            coreMenuSel   = (p >= 0) ? p : 0;
          }
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_RADIO) {
          // Act-and-close. coreSetRadioMode() ignores a repeat of the active
          // mode.
          if (coreMenuSel >= 0 && coreMenuSel < RADIO_MODE_COUNT)
            coreSetRadioMode((RadioMode)coreMenuSel);
          coreMenuState = MENU_NONE;
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_TARGETS) {
          // Act-and-close, like Scan Mode. Switching the mask is a single byte
          // store, so the sniffer keeps running across the change.
          if (coreMenuSel >= 0 && coreMenuSel < TARGET_ROW_COUNT) {
            coreSetVendorMask(TARGET_MASKS[coreMenuSel]);
            dualPrintf("[bscope] targets -> %s\n", coreTargetRowName(coreMenuSel));
          }
          coreMenuState = MENU_NONE;
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_SCAN_MODES) {
          if (coreMenuSel == 0) {
            coreSetScanMode(CHANNEL_MODE_CUSTOM, 0);   coreMenuState = MENU_NONE;
          } else if (coreMenuSel == 1) {
            coreSetScanMode(CHANNEL_MODE_FULL_HOP, 0); coreMenuState = MENU_NONE;
          } else {                                     // Single → open the channel picker
            coreMenuState = MENU_PICK_CHANNEL;
            coreMenuSel   = coreSingleChannel;
          }
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_WIPE) {
          // Selecting a scope only arms the confirmation. The wipe waits for
          // WIPE_CONFIRM_PRESSES more Selects.
          g_wipeScope          = (coreMenuSel == 1) ? WIPE_DEVICE_AND_CARD : WIPE_DEVICE;
          coreWipeConfirmCount = 0;
          coreMenuState        = MENU_CONFIRM_WIPE;
          return NAV_ACT_REDRAW;
        }
        // CONFIG
        coreMenuState = MENU_NONE;
        if (coreMenuSel == 0) return NAV_ACT_ADMIN;    // On → enter Admin
        return NAV_ACT_REDRAW;                          // Off → close the menu
      default:
        return NAV_ACT_NONE;
    }
  }

  case MENU_CONFIRM_WIPE:
    switch (ev) {
      case NAV_SELECT:
        if (++coreWipeConfirmCount >= WIPE_CONFIRM_PRESSES) return NAV_ACT_WIPE;
        return NAV_ACT_REDRAW;
      case NAV_BACK:
        // Any Back abandons the whole gesture rather than dropping one press,
        // so a cancel is never one press short of a wipe.
        coreWipeConfirmCount = 0;
        coreMenuState        = MENU_LIST;
        return NAV_ACT_REDRAW;
      default:
        return NAV_ACT_NONE;   // Up and Down do nothing while a wipe is armed
    }

  case MENU_PICK_PROX:
    switch (ev) {
      // Wraps, since this is a short option list. The channel picker dials a
      // number with meaningful ends.
      case NAV_UP:
        coreMenuSel = (coreMenuSel + PROX_RING_OPTION_COUNT - 1) % PROX_RING_OPTION_COUNT;
        return NAV_ACT_REDRAW;
      case NAV_DOWN:
        coreMenuSel = (coreMenuSel + 1) % PROX_RING_OPTION_COUNT;
        return NAV_ACT_REDRAW;
      case NAV_SELECT:
        if (coreMenuSel >= 0 && coreMenuSel < PROX_RING_OPTION_COUNT) {
          coreSetProxRingM(PROX_RING_OPTIONS[coreMenuSel]);
          // Persisted at once. It is a calibration setting like density and
          // rssi_1m, where the Buzzer and LED rows are session gates.
          coreSettingsSave();
          dualPrintf("[bscope] proximity ring -> %um\n", (unsigned)coreProxRingM);
        }
        coreMenuState = MENU_NONE;
        return NAV_ACT_REDRAW;
      case NAV_BACK:
        coreMenuState = MENU_LIST;                      // back up to the option list
        coreMenuSel   = 2;                              // Proximity highlighted
        return NAV_ACT_REDRAW;
      default:
        return NAV_ACT_NONE;
    }

  case MENU_PICK_CHANNEL:
    switch (ev) {
      case NAV_UP:
        if (coreMenuSel < CHANNEL_PICK_MAX) coreMenuSel++;
        return NAV_ACT_REDRAW;
      case NAV_DOWN:
        if (coreMenuSel > CHANNEL_PICK_MIN) coreMenuSel--;
        return NAV_ACT_REDRAW;
      case NAV_SELECT:
        coreSetScanMode(CHANNEL_MODE_SINGLE, (uint8_t)coreMenuSel);
        coreMenuState = MENU_NONE;
        return NAV_ACT_REDRAW;
      case NAV_BACK:
        coreMenuState = MENU_LIST;                      // back up to the option list
        coreMenuSel   = 2;                              // Single highlighted
        return NAV_ACT_REDRAW;
      default:
        return NAV_ACT_NONE;
    }
  }
  return NAV_ACT_NONE;
}

// Anytime BOOT double-press detector, described in core.h. Watches for two
// debounced presses within BOOT_DOUBLE_PRESS_MS and fires once per gesture.
// Works at any time, so the operator can hop between Detect and Admin
// repeatedly. BOOT is active-LOW (INPUT_PULLUP, idles HIGH).
bool coreAdminTriggerCheck() {
#if !BOOT_ADMIN_TRIGGER
  return false;
#else
  const unsigned long kDebounceMs = 40;
  static bool          armed       = false;   // first-call pin init done
  static int           lastLvl     = HIGH;
  static unsigned long lastEdgeMs  = 0;
  static unsigned long lastPressMs = 0;       // time of the previous accepted press (0 = none pending)

  unsigned long now = millis();
  if (!armed) {
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    lastLvl = digitalRead(BOOT_BTN_PIN);
    armed   = true;
  }

  int lvl = digitalRead(BOOT_BTN_PIN);
  if (lvl != lastLvl && now - lastEdgeMs > kDebounceMs) {   // debounced edge
    lastEdgeMs = now;
    lastLvl    = lvl;
    if (lvl == LOW) {                                       // a press
      if (lastPressMs != 0 && now - lastPressMs <= BOOT_DOUBLE_PRESS_MS) {
        lastPressMs = 0;                                    // consume, no triple-press retrigger
        return true;
      }
      lastPressMs = now;                                    // first press, wait for the second
    }
  }
  // Expire a lone first press so it can't pair with a much-later press.
  if (lastPressMs != 0 && now - lastPressMs > BOOT_DOUBLE_PRESS_MS) lastPressMs = 0;
  return false;
#endif
}

// Raw-IDF promiscuous capture bring-up, described in core.h. coreRadioStart()
// calls it in 2.4 GHz mode, so it re-runs when webPortalStop() releases the AP,
// since the portal deinits this driver to hand the radio to Arduino WiFi.
// Idempotent from a clean or deinit'd driver. Reports each failed bring-up step
// and continues, spec G1, so a dead radio cannot report itself healthy.
#define WIFI_TRY(call)                                                  \
  do {                                                                  \
    esp_err_t _e = (call);                                              \
    if (_e != ESP_OK)                                                   \
      dualPrintf("[bscope] %s -> %s\n", #call, esp_err_to_name(_e));    \
  } while (0)

void coreWifiSnifferStart() {
  // Before the driver allocates, while the largest block is still free.
  if (!alertQueue) {
    AlertEntry* q = (AlertEntry*)radioAlloc(ALERT_QUEUE_SIZE * sizeof(AlertEntry),
                                            "alert queue");
    portENTER_CRITICAL(&queueMux);
    alertHead = alertTail = 0;
    alertQueue = q;
    portEXIT_CRITICAL(&queueMux);
  }
  limitStart(&surveyLimit, SURVEY_DEDUPE_SLOTS, SURVEY_DEDUPE_MS, "survey limit", false);
  limitStart(&infraLimit, INFRA_DEDUPE_SLOTS, INFRA_DEDUPE_MS, "infra limit", false);

  // Left unwrapped. It returns ESP_ERR_INVALID_STATE whenever the loop already
  // exists, the normal path when webPortalStop() hands the radio back.
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  WIFI_TRY(esp_wifi_init(&cfg));
  WIFI_TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_NULL));
  WIFI_TRY(esp_wifi_start());

  // Its esp_wifi_set_channel() runs before promiscuous mode starts and can fail
  // there legitimately. updateChannelMode() sets the channel a hop later.
  applyInitialChannel();

  wifi_promiscuous_filter_t filt = {
    .filter_mask = 0
#if PROCESS_MGMT_FRAMES
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if PROCESS_DATA_FRAMES
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
  };
  WIFI_TRY(esp_wifi_set_promiscuous_filter(&filt));
  WIFI_TRY(esp_wifi_set_promiscuous_rx_cb(&wifiSniffer));
  WIFI_TRY(esp_wifi_set_promiscuous(true));
  sniffingStopped = false;
}

void coreWifiSnifferStop() {
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  esp_wifi_stop();
  esp_wifi_deinit();

  // The callback has stopped, so the rows still queued are the last ones.
  // They reach the log before the queue goes.
  AlertEntry e;
  while (coreDequeueAlert(e)) coreHandleAlert(e);
  portENTER_CRITICAL(&queueMux);
  AlertEntry* q = alertQueue;
  alertQueue = nullptr;
  portEXIT_CRITICAL(&queueMux);
  free(q);
  limitStop(&surveyLimit);
  limitStop(&infraLimit);
}

// BLE counters exist on every build, so `status` and the manifest read the same
// fields whether or not the board scans.
#ifndef BLE_QUEUE_SIZE
#define BLE_QUEUE_SIZE 64
#endif

volatile uint32_t coreBleReports = 0;
volatile uint32_t coreBlePhy1M = 0;
volatile uint32_t coreBlePhyCoded = 0;
volatile uint32_t coreBleQueueDrops = 0;
volatile uint8_t  coreBleQueueDepthMax = 0;

uint8_t coreBleQueueSize() { return BLE_QUEUE_SIZE; }

BleDetection coreBleDet[MAX_BLE_DETECTIONS];
uint16_t     coreBleDetCount  = 0;
uint16_t     coreBleDetMissed = 0;
int16_t      coreBleDetLast   = -1;

#if HAS_BLE_SCAN

// BLE capture needs extended advertising for the coded PHY and every extended
// report. A legacy-only build misses both, so the build refuses it.
#if !defined(CONFIG_BT_NIMBLE_EXT_ADV) || CONFIG_BT_NIMBLE_EXT_ADV != 1
#error "CONFIG_BT_NIMBLE_EXT_ADV=1 is required for BLE capture"
#endif

// Window equal to interval is a 100% duty cycle. Radio modes are exclusive, so
// nothing shares the dwell.
#ifndef BLE_SCAN_INTERVAL_MS
#define BLE_SCAN_INTERVAL_MS 100
#endif
#ifndef BLE_SCAN_WINDOW_MS
#define BLE_SCAN_WINDOW_MS 100
#endif

static portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;

// ---- BLE detection table ---------------------------------------------------
//
// Fed from coreBleDrain() in loop() context, the same context that draws it.

// Records a target row and alerts on it. A new device, or one silent for
// REDISCOVER_MS, chirps. Repeats inside ALERT_COOLDOWN_MS stay silent, through
// the gate the 802.11 path uses. BLE has no proximity ring and no distance
// estimate, since the path-loss calibration is the 802.11 one. Spec B4.
static bool bleRecordDetection(const BleObsEntry& e) {
  char macStr[18];
  macToStr(e.mac, macStr, sizeof(macStr));
  const char* key = e.id[0] ? e.id : macStr;
  const uint32_t now = millis();

  bool chirpWorthy = false;
  int idx = -1;
  for (int i = 0; i < coreBleDetCount; i++) {
    if (strcmp(coreBleDet[i].key, key) == 0) { idx = i; break; }
  }
  if (idx >= 0) {
    BleDetection& d = coreBleDet[idx];
    chirpWorthy = (now - d.lastSeen) > REDISCOVER_MS;
    if (d.count < 0xFFFF) d.count++;
    d.lastSeen = now;
    d.rssi     = e.rssi;
    strlcpy(d.mac, macStr, sizeof(d.mac));
  } else if (coreBleDetCount < MAX_BLE_DETECTIONS) {
    idx = coreBleDetCount++;
    BleDetection& d = coreBleDet[idx];
    strlcpy(d.key, key, sizeof(d.key));
    strlcpy(d.mac, macStr, sizeof(d.mac));
    d.vendor    = e.vendor;
    d.accessory = e.accessory;
    d.rssi      = e.rssi;
    d.count     = 1;
    d.firstSeen = now;
    d.lastSeen  = now;
    chirpWorthy = true;
  } else {
    // No eviction, so a full table counts what it refused [S4].
    if (coreBleDetMissed < 0xFFFF) coreBleDetMissed++;
    return false;
  }
  coreBleDetLast   = (int16_t)idx;
  fyLastTargetSeen = now;

  if (shouldSuppressDuplicate(key)) return true;
  dualPrintf("[bscope] DETECT-BLE %s key=%s mac=%s vendor=%s%s rssi=%d count=%u\n",
             e.method, key, macStr, vendorName((uint8_t)e.vendor),
             e.accessory ? " accessory" : "", (int)e.rssi,
             (unsigned)coreBleDet[idx].count);
  notifyDetection(chirpWorthy, true, e.vendor);
  return true;
}

// ---- BLE row ring -----------------------------------------------------------
//
// NimBLE host task to loop(), apart from the 802.11 alert queue. A target row
// may take any free slot. The ring refuses an infra row once it is three
// quarters full, so infra sheds first under load. Spec B3.
// The ring and the infra limiter exist only while the BLE scan runs.
static BleObsEntry* bleQueue = nullptr;
static size_t       bleHead = 0;   // written by the NimBLE host task
static size_t       bleTail = 0;   // read by loop()


static void bleEnqueue(const BleObsEntry& e, bool target) {
  portENTER_CRITICAL(&bleMux);
  if (!bleQueue) {   // the scan runs without its ring when allocation failed
    coreBleQueueDrops = coreBleQueueDrops + 1;
    portEXIT_CRITICAL(&bleMux);
    return;
  }
  const size_t depth = (bleHead + BLE_QUEUE_SIZE - bleTail) % BLE_QUEUE_SIZE;
  const size_t limit = target ? BLE_QUEUE_SIZE - 1 : (BLE_QUEUE_SIZE * 3) / 4;
  if (depth >= limit) {
    coreBleQueueDrops = coreBleQueueDrops + 1;
    portEXIT_CRITICAL(&bleMux);
    return;
  }
  bleQueue[bleHead] = e;
  bleHead = (bleHead + 1) % BLE_QUEUE_SIZE;
  if (depth + 1 > coreBleQueueDepthMax) coreBleQueueDepthMax = (uint8_t)(depth + 1);
  portEXIT_CRITICAL(&bleMux);
}

bool coreBleDrain() {
  BleObsEntry e;
  bool changed = false;
  for (;;) {
    portENTER_CRITICAL(&bleMux);
    if (!bleQueue || bleTail == bleHead) { portEXIT_CRITICAL(&bleMux); break; }
    e = bleQueue[bleTail];
    bleTail = (bleTail + 1) % BLE_QUEUE_SIZE;
    portEXIT_CRITICAL(&bleMux);
#if USE_SD
    roostLogBleObs(e);
#endif
    if (e.target) changed |= bleRecordDetection(e);
  }
  return changed;
}

static const char* blePhyName(uint8_t phy) {
  switch (phy) {
    case BLE_HCI_LE_PHY_1M:    return "1m";
    case BLE_HCI_LE_PHY_2M:    return "2m";
    case BLE_HCI_LE_PHY_CODED: return "coded";
    default:                   return "";
  }
}

// The registry's ble_pdu_type. getAdvType() returns the HCI report event type,
// so the cases use BLE_HCI_ADV_RPT_EVTYPE_*. The ADV_TYPE_* family overlaps it
// numerically and would label every scan response adv_direct_ind.
static const char* blePduTypeName(const NimBLEAdvertisedDevice *d) {
  if (!d->isLegacyAdvertisement()) return "ext_adv_ind";
  switch (d->getAdvType()) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:     return "adv_ind";
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:     return "adv_direct_ind";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:    return "adv_scan_ind";
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND: return "adv_nonconn_ind";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:    return "scan_rsp";
    default:                                 return "unknown";
  }
}

// Runs on the NimBLE host task. Counts, matches and queues advertisements, and
// queues an unmatched one only inside a survey window, spec B2.
class BleScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    const uint8_t phy = d->getPrimaryPhy();
    portENTER_CRITICAL(&bleMux);
    coreBleReports = coreBleReports + 1;
    if (phy == BLE_HCI_LE_PHY_CODED)   coreBlePhyCoded = coreBlePhyCoded + 1;
    else if (phy == BLE_HCI_LE_PHY_1M) coreBlePhy1M = coreBlePhy1M + 1;
    portEXIT_CRITICAL(&bleMux);

    // NimBLE stores the address little-endian. Reverse it to wire order for the
    // OUI table.
    uint8_t mac[6];
    const uint8_t *val = d->getAddress().getVal();
    for (int i = 0; i < 6; i++) mac[i] = val[5 - i];

    const std::vector<uint8_t> &payload = d->getPayload();
    BleMatch m;
    const char* method;
    bool target;
    m.vendor = -1; m.accessory = false; m.id[0] = '\0';
    if (coreBleMatch(mac, d->getAddress().getType(),
                     payload.data(), payload.size(), &m)) {
      if (m.infra && !macLimitAllow(&bleInfraLimit, mac, millis())) return;
      if (!m.infra) {
        portENTER_CRITICAL(&bleMux);
        coreBleMatched = coreBleMatched + 1;
        portEXIT_CRITICAL(&bleMux);
      }
      method = m.method;
      target = !m.infra;
    } else {
      if (!surveyActive) return;
      if (!macLimitAllow(&bleSurveyLimit, mac, millis())) {
        coreSurveySuppressed = coreSurveySuppressed + 1;
        return;
      }
      coreSurveyRows = coreSurveyRows + 1;
      method = "operator_survey";   // spec O3
      target = false;
    }

    // Refuses a payload that does not fit and never truncates it [L7].
    if (payload.size() > BLE_ADV_MAX) {
      portENTER_CRITICAL(&bleMux);
      coreBleQueueDrops = coreBleQueueDrops + 1;
      portEXIT_CRITICAL(&bleMux);
      return;
    }

    BleObsEntry e;
    e.uptimeMs     = millis();
    memcpy(e.mac, mac, 6);
    e.addrType     = d->getAddress().getType();
    e.rssi         = (int8_t)d->getRSSI();
    e.hasTxPower   = d->haveTXPower();
    e.txPower      = e.hasTxPower ? (int8_t)d->getTXPower() : 0;
    e.extended     = !d->isLegacyAdvertisement();
    e.sid          = e.extended ? d->getSetId() : 0;
    e.method       = method;
    e.target       = target;
    e.vendor       = m.vendor;
    e.accessory    = m.accessory;
    strlcpy(e.id, m.id, sizeof(e.id));
    e.pduType      = blePduTypeName(d);
    e.phyPrimary   = blePhyName(phy);
    e.phySecondary = e.extended ? blePhyName(d->getSecondaryPhy()) : "";
    e.payloadLen   = (uint16_t)payload.size();
    memcpy(e.payload, payload.data(), payload.size());
    bleEnqueue(e, target);
  }
};

static BleScanCb g_bleScanCb;
static NimBLEScan *g_bleScan = nullptr;

static void bleScanStart() {
  // Before NimBLE allocates, while the largest block is still free. All three
  // prefer PSRAM, since only the NimBLE host task and loop() touch them.
  if (!bleQueue) {
    BleObsEntry* q = (BleObsEntry*)radioAlloc(BLE_QUEUE_SIZE * sizeof(BleObsEntry),
                                              "ble ring", true);
    portENTER_CRITICAL(&bleMux);
    bleHead = bleTail = 0;
    bleQueue = q;
    portEXIT_CRITICAL(&bleMux);
  }
  limitStart(&bleInfraLimit, BLE_LIMIT_SLOTS, INFRA_DEDUPE_MS, "ble infra limit", true);
  limitStart(&bleSurveyLimit, BLE_LIMIT_SLOTS, SURVEY_DEDUPE_MS, "ble survey limit", true);
  NimBLEDevice::init("");
  g_bleScan = NimBLEDevice::getScan();
  g_bleScan->setScanCallbacks(&g_bleScanCb, /*wantDuplicates=*/true);
  // Passive, since active scanning transmits a SCAN_REQ that makes the device
  // detectable.
  g_bleScan->setActiveScan(false);
  g_bleScan->setInterval(BLE_SCAN_INTERVAL_MS);
  g_bleScan->setWindow(BLE_SCAN_WINDOW_MS);
  // Every advertisement, since repeat sightings form the RSSI series that
  // path-loss fitting needs.
  g_bleScan->setDuplicateFilter(false);
  g_bleScan->setMaxResults(0);   // the callback is the only output
  g_bleScan->setPhy(NimBLEScan::SCAN_ALL);
  g_bleScan->start(0, false);    // 0 = no duration, no restart on completion
}

// `drain` logs the rows still queued. Device wipe passes false, since it is about
// to erase the card those rows would land on.
static void bleScanStop(bool drain) {
  if (g_bleScan) {
    g_bleScan->stop();
    g_bleScan = nullptr;
  }
  NimBLEDevice::deinit(true);

  // The callback has stopped, so the rows still queued are the last ones.
  if (drain) (void)coreBleDrain();
  portENTER_CRITICAL(&bleMux);
  BleObsEntry* q = bleQueue;
  bleQueue = nullptr;
  portEXIT_CRITICAL(&bleMux);
  free(q);
  limitStop(&bleInfraLimit);
  limitStop(&bleSurveyLimit);
}

#else   // HAS_BLE_SCAN

bool coreBleDrain() { return false; }
static void     bleScanStart() {}
static void     bleScanStop(bool) {}

#endif  // HAS_BLE_SCAN

RadioMode coreRadioMode = RADIO_MODE_WIFI;

const char* radioModeName(RadioMode mode) {
  switch (mode) {
    case RADIO_MODE_WIFI: return "wifi";
    case RADIO_MODE_BLE:  return "ble";
    default:              return "unknown";
  }
}

static uint16_t sampledRate(uint32_t count, uint32_t *lastMs,
                            uint32_t *lastCount, uint16_t *rate) {
  const uint32_t now = millis();
  const uint32_t dt = now - *lastMs;   // wrap-safe
  if (dt >= 1000) {
    *rate = (uint16_t)(((uint64_t)(count - *lastCount) * 1000u) / dt);
    *lastCount = count;
    *lastMs = now;
  }
  return *rate;
}

uint16_t coreSeenRate() {
  static uint32_t lastMs = 0, lastCount = 0;
  static uint16_t rate = 0;
  return sampledRate(coreSeenFrames, &lastMs, &lastCount, &rate);
}

uint16_t coreBleRate() {
  static uint32_t lastMs = 0, lastCount = 0;
  static uint16_t rate = 0;
  return sampledRate(coreBleReports, &lastMs, &lastCount, &rate);
}

bool coreScreenVisible(ScreenId s) {
  // The channel plan is 802.11 only. BLE covers its three advertising channels
  // itself.
  if (s == SCREEN_SCAN_MODES) return coreRadioMode == RADIO_MODE_WIFI;
  if (s == SCREEN_RADIO)      return HAS_BLE_SCAN;
  return true;
}

// Steps to the next visible screen, wrapping. Returns `from` when no other
// screen is visible.
ScreenId coreStepScreen(ScreenId from, int dir) {
  int s = (int)from;
  for (int i = 0; i < SCREEN_COUNT; i++) {
    s = (s + dir + SCREEN_COUNT) % SCREEN_COUNT;
    if (coreScreenVisible((ScreenId)s)) return (ScreenId)s;
  }
  return from;
}

void coreRadioStart() {
  switch (coreRadioMode) {
    case RADIO_MODE_BLE: bleScanStart();         break;
    default:             coreWifiSnifferStart(); break;
  }
}

void coreRadioStop() {
  switch (coreRadioMode) {
    case RADIO_MODE_BLE: bleScanStop(true);     break;
    default:             coreWifiSnifferStop(); break;
  }
}

// Logs the row in the setter [L1], and roostLogConfigChange() handles L6. The
// early return keeps a repeat of the active mode from restarting a working
// driver.
void coreSetRadioMode(RadioMode mode) {
  if (mode >= RADIO_MODE_COUNT || mode == coreRadioMode) return;
  if (mode == RADIO_MODE_BLE && !HAS_BLE_SCAN) {
    dualPrintln("[bscope] radio: this board has no BLE capture");
    return;
  }
  coreRadioStop();
  coreRadioMode = mode;
  coreRadioStart();
  if (!coreScreenVisible(coreCurrentScreen))
    coreCurrentScreen = coreStepScreen(coreCurrentScreen, +1);
  roostLogConfigRadioMode();
  dualPrintf("[bscope] radio -> %s\n", radioModeName(coreRadioMode));
}

// ============================================================
// SESSION PROVENANCE
//
// What the manifest needs that only core knows. It lives here so the GPS
// parser, the OUI table and the clock anchor stay private to this translation
// unit.
// ============================================================

void coreGpsFix(CoreGpsFix* o) {
  memset(o, 0, sizeof(*o));
  o->source  = "none";
  o->fixType = "none";
#if HAS_GPS
  if (gpsHasFix) {
    o->valid = true;
    o->lat = gpsLat;
    o->lon = gpsLng;
    const uint32_t age = gpsParser.location.age();
    o->ageMs = age;
    // A re-reported last-known fix is not a measurement of where the device is
    // now, and the two must not look alike.
    o->source  = (age < GPS_FIX_MAX_AGE_MS) ? "device_fix" : "device_stale";
    o->fixType = gpsParser.altitude.isValid() ? "3d" : "2d";
    if (gpsParser.altitude.isValid()) { o->hasAlt = true; o->altM = gpsParser.altitude.meters(); }
    if (gpsParser.speed.isValid())    { o->hasSpeed = true; o->speedMps = gpsParser.speed.mps(); }
    if (gpsParser.course.isValid())   { o->hasCourse = true; o->courseDeg = gpsParser.course.deg(); }
    if (gpsParser.hdop.isValid())     { o->hasHdop = true; o->hdop = gpsParser.hdop.hdop(); }
    if (gpsParser.satellites.isValid()){ o->hasSats = true; o->sats = (uint8_t)gpsParser.satellites.value(); }
  }
#endif
}

const char* coreClockAnchor(uint32_t* anchorUnix, uint32_t* anchorUptimeMs) {
#if HAS_GPS
  if (gpsTimeAnchored) {
    if (anchorUnix)     *anchorUnix = gpsAnchorUnix;
    if (anchorUptimeMs) *anchorUptimeMs = gpsAnchorMs;
    return "gps";
  }
#endif
  if (ntpTimeAnchored) {
    if (anchorUnix)     *anchorUnix = ntpAnchorUnix;
    if (anchorUptimeMs) *anchorUptimeMs = ntpAnchorMs;
    return "ntp";
  }
  if (anchorUnix)     *anchorUnix = 0;
  if (anchorUptimeMs) *anchorUptimeMs = 0;
  return "none";
}

void coreUnixToIso(uint32_t unix, char* buf, size_t len) {
  unixToIso(unix, buf, len);
}

bool coreTimestampAt(uint32_t uptimeMs, char* buf, size_t len) {
  uint32_t au = 0, am = 0;
  if (strcmp(coreClockAnchor(&au, &am), "none") == 0) return false;
  // Shared, so the row arithmetic agrees with the manifest's and a pre-anchor
  // time returns false without underflowing. A row stamped before the anchor
  // leaves timestamp_utc empty, and ingest places it.
  return roostTimestampAt(au, am, uptimeMs, buf, len) != 0;
}

uint32_t coreBootCount() {
  static uint32_t n = 0;
  if (!n) {
    Preferences p;
    if (p.begin("bscope", false)) {
      n = p.getUInt("boots", 0) + 1;
      p.putUInt("boots", n);
      p.end();
    } else {
      n = 1;
    }
  }
  return n;
}

static uint32_t g_sessionSeq = 1;
uint32_t coreSessionSequence() { return g_sessionSeq; }

// The last four hex digits of coreDeviceSerial(), naming the unit in a session
// directory name. Empty where the hardware has no serial. Spec 6.2 and 7.
const char* coreDeviceTag() {
  static char tag[5] = "";
  static bool done = false;
  if (!done) {
    done = true;
    const char* sn = coreDeviceSerial();
    const size_t n = sn ? strlen(sn) : 0;
    if (n >= 4) snprintf(tag, sizeof(tag), "%s", sn + n - 4);
    else if (n)  snprintf(tag, sizeof(tag), "%s", sn);
  }
  return tag;
}

bool coreSessionDirName(char* buf, size_t len) {
  uint8_t mo = 1, dy = 1, yr = 70;
#if HAS_GPS
  if (gpsTimeAnchored && gpsParser.date.isValid()) {
    mo = gpsParser.date.month();
    dy = gpsParser.date.day();
    yr = (uint8_t)(gpsParser.date.year() % 100);
  } else
#endif
  {
    time_t now = time(nullptr);
    struct tm t;
    gmtime_r(&now, &t);
    mo = (uint8_t)(t.tm_mon + 1);
    dy = (uint8_t)t.tm_mday;
    yr = (uint8_t)((t.tm_year + 1900) % 100);
  }
  // YYMMDD so a listing sorts chronologically. The tag precedes it, so probing
  // settles the sequence within one unit. Spec 7.
  const char* tag = coreDeviceTag();
  for (uint8_t n = 1; n <= 99; n++) {
    if (tag[0]) snprintf(buf, len, "/" LOG_PREFIX "%s-%02u%02u%02u-%u",
                         tag, yr, mo, dy, n);
    else        snprintf(buf, len, "/" LOG_PREFIX "%02u%02u%02u-%u",
                         yr, mo, dy, n);
    if (!SD.exists(buf)) { g_sessionSeq = n; return true; }
  }
  // Refuse a name the loop just found taken. Renaming into an occupied
  // directory merges two boots under one manifest and leaves their rows
  // unattributable. Spec 6.2.
  buf[0] = '\0';
  g_sessionSeq = 0;
  return false;
}

// FNV-1a over the compiled table. Ingest compares it to tell that captures on
// either side of a table change are not comparable.
uint32_t coreOuiTableHash() {
  uint32_t h = 2166136261u;
  const uint8_t* p = (const uint8_t*)oui_table;
  for (size_t i = 0; i < sizeof(oui_table); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

const char* coreOwnMac() {
  static char s[18] = "";
  if (!s[0]) {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    macToStr(m, s, sizeof(s));
  }
  return s;
}

const char* coreDeviceSerial() {
  static char s[13] = "";
  if (!s[0]) {
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    snprintf(s, sizeof(s), "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  }
  return s;
}

const char* coreCountryCode() {
  static char cc[4] = "";
  if (!cc[0]) {
    wifi_country_t c;
    if (esp_wifi_get_country(&c) == ESP_OK) {
      cc[0] = c.cc[0]; cc[1] = c.cc[1]; cc[2] = '\0';
    } else {
      strcpy(cc, "01");
    }
  }
  return cc;
}

// The channel plan in effect, which says what was reachable, since the device
// hears nothing on a channel it never tunes. Single mode reports the channel
// the picker holds, not the build's compile-time default.
static void currentChannelPlan(const uint8_t** list, size_t* n) {
  switch (coreScanModeIndex()) {
    case 1:  *list = fullHopChannels;    *n = fullHopChannelCount; break;
    case 2:  *list = &coreSingleChannel; *n = 1;                   break;
    default: *list = customChannels;     *n = customChannelCount;  break;
  }
}

// The config_change `channels` value, which the registry declares a `list`
// and roost_value.h renders. A false return means the value did not fit, and the
// caller must not write it as an empty value.
// See vendor/jellybeans/roost_logging/runtime/roost_value.h.
bool coreChannelListRoost(char* buf, size_t len) {
  const uint8_t* list; size_t n;
  currentChannelPlan(&list, &n);
  RoostValue v;
  roostValueBegin(&v, buf, len);
  for (size_t i = 0; i < n; i++) roostValueAddUInt(&v, list[i]);
  return roostValueDone(&v) != 0;
}

// The web console's channel array. A JSON reader is not a roost reader, so this
// one truncates to stay parseable rather than refusing.
void coreChannelListJson(char* buf, size_t len) {
  const uint8_t* list; size_t n;
  currentChannelPlan(&list, &n);
  if (len < 3) { if (len) buf[0] = '\0'; return; }
  size_t o = 0;
  buf[o++] = '[';
  for (size_t i = 0; i < n; i++) {
    char item[8];
    const int w = i ? snprintf(item, sizeof(item), ",%u", (unsigned)list[i])
                    : snprintf(item, sizeof(item), "%u", (unsigned)list[i]);
    if (w < 0 || o + (size_t)w >= len - 1) break;
    memcpy(buf + o, item, (size_t)w);
    o += (size_t)w;
  }
  buf[o++] = ']';
  buf[o] = '\0';
}

// The config_change `vendor_mask` value, a registry `list` of vendor slugs,
// empty when the mask is clear. It tells a session with Axon masked out from one
// where Axon was absent. Never a placeholder like "none", and never a bitmask,
// which needs this build's bit assignments to read. See
// vendor/jellybeans/roost_logging/runtime/roost_value.h and
// vendor/jellybeans/roost_logging/docs/design_spec.md 6.5.
bool coreVendorMaskStr(char* buf, size_t len) {
  RoostValue v;
  roostValueBegin(&v, buf, len);
  for (int i = 0; i < VENDOR_COUNT; i++)
    if (coreVendorMask & (1u << i)) roostValueAddText(&v, kVendorSlugs[i]);
  return roostValueDone(&v) != 0;
}

// ============================================================
// DEVICE WIPE (see core.h). Writes the scope to NVS first and erases NVS last,
// so a boot finds only two states, no wipe requested or a wipe to resume.
// ============================================================

#define WIPE_NVS_NAMESPACE "bscope"
#define WIPE_NVS_KEY       "wipe"

#if USE_SD
// Recursion limit. Session directories sit one level below the root, so this is
// slack. The walk reports anything deeper and leaves it.
#define WIPE_SD_MAX_DEPTH 3
// Names held per pass, and the room each gets. Both stay small because these
// buffers sit on the stack at every level of the recursion.
#define WIPE_SD_BATCH    6
#define WIPE_SD_PATH_MAX 96

// Empties `path` and removes it, returning false if anything survived.
//
// Reads names in batches and deletes them after closing the listing, since an
// open iterator's position is undefined across a removal. A pass that removes
// nothing ends the walk, so an entry that refuses to unlink cannot spin the loop
// or block the rest of the card.
static bool wipeSdPurge(const char* path, uint8_t depth) {
  if (depth > WIPE_SD_MAX_DEPTH) {
    dualPrintf("[bscope] wipe: %s below depth %u, left in place\n",
               path, (unsigned)WIPE_SD_MAX_DEPTH);
    return false;
  }
  bool ok = true;
  for (;;) {
    char names[WIPE_SD_BATCH][WIPE_SD_PATH_MAX];
    bool isDir[WIPE_SD_BATCH];
    int  n = 0;

    File dir = SD.open(path);
    if (!dir) return false;
    if (!dir.isDirectory()) { dir.close(); return SD.remove(path); }
    for (File e = dir.openNextFile(); e && n < WIPE_SD_BATCH; e = dir.openNextFile()) {
      snprintf(names[n], WIPE_SD_PATH_MAX, "%s", e.path());
      isDir[n] = e.isDirectory();
      e.close();
      n++;
    }
    dir.close();
    if (n == 0) break;                                // emptied

    int removed = 0;
    for (int i = 0; i < n; i++) {
      if (isDir[i] ? wipeSdPurge(names[i], depth + 1) : SD.remove(names[i])) {
        removed++;
      } else {
        dualPrintf("[bscope] wipe: cannot remove %s\n", names[i]);
        ok = false;
      }
    }
    if (removed == 0) break;
  }
  if (strcmp(path, "/") == 0) return ok;              // the mount point itself stays
  return SD.rmdir(path) && ok;
}
#endif

static void wipeScopeRecord(WipeScope scope) {
  Preferences p;
  if (!p.begin(WIPE_NVS_NAMESPACE, false)) {
    dualPrintln("[bscope] wipe: NVS unavailable, no resume record");
    return;
  }
  p.putUChar(WIPE_NVS_KEY, (uint8_t)scope);
  p.end();
}

WipeScope coreWipePending() {
  Preferences p;
  if (!p.begin(WIPE_NVS_NAMESPACE, true)) return WIPE_NONE;
  const uint8_t v = p.getUChar(WIPE_NVS_KEY, (uint8_t)WIPE_NONE);
  p.end();
  if (v == (uint8_t)WIPE_DEVICE || v == (uint8_t)WIPE_DEVICE_AND_CARD)
    return (WipeScope)v;
  return WIPE_NONE;
}

void coreDeviceWipe(WipeScope scope) {
  if (scope != WIPE_DEVICE && scope != WIPE_DEVICE_AND_CARD) return;
  dualPrintf("[bscope] DEVICE WIPE starting (scope=%s)\n",
             scope == WIPE_DEVICE_AND_CARD ? "device+card" : "device");

  wipeScopeRecord(scope);

  // Silence every writer before removing what they write, or the next autosave
  // and manifest snapshot recreate files that were just deleted.
  esp_wifi_set_promiscuous(false);
  sniffingStopped = true;
  if (coreRadioMode == RADIO_MODE_BLE) bleScanStop(false);
  memset(fyDet, 0, sizeof(fyDet));
  fyDetCount      = 0;
  fyLastSaveCount = 0;
  fyDroppedNew    = 0;
  fyDirty         = false;
  memset(coreBleDet, 0, sizeof(coreBleDet));
  coreBleDetCount  = 0;
  coreBleDetMissed = 0;
  coreBleDetLast   = -1;
#if USE_SD
  roostSessionEnd();   // closes any open row files
#endif

#if USE_SD
  if (scope == WIPE_DEVICE_AND_CARD) {
    if (fySDReady) {
      dualPrintln("[bscope] wipe: emptying SD root");
      if (!wipeSdPurge("/", 0)) dualPrintln("[bscope] wipe: SD pass incomplete");
      else                      dualPrintln("[bscope] wipe: SD root emptied");
    } else {
      dualPrintln("[bscope] wipe: no SD card mounted, card skipped");
    }
  }
#endif

  // Format, so a file added later cannot survive the wipe.
  fySpiffsReady = false;
  SPIFFS.end();
  if (SPIFFS.format()) dualPrintln("[bscope] wipe: SPIFFS formatted");
  else                 dualPrintln("[bscope] wipe: SPIFFS format FAILED");

  // Last, and the step that clears the resume record. Also drops the WiFi
  // driver's own NVS entries, so the radio comes up from cold on the next boot.
  // The driver must be down first, since it holds handles into this partition.
  esp_wifi_stop();
  esp_wifi_deinit();
  nvs_flash_deinit();
  const esp_err_t err = nvs_flash_erase();
  if (err == ESP_OK) dualPrintln("[bscope] wipe: NVS erased");
  else               dualPrintf("[bscope] wipe: NVS erase FAILED (%d)\n", (int)err);
  nvs_flash_init();

  dualPrintln("[bscope] DEVICE WIPE complete");
}

void corePowerOff() {
  ledSet(0, 0, 0);
#if USE_BUZZER
  digitalWrite(BUZZER_PIN, LOW);
#endif
  dualPrintln("[bscope] powering off (deep sleep, reset to wake)");
  Serial.flush();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_deep_sleep_start();
}
