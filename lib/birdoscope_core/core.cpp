// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Arduino.h must come first (board_config.h's pin/channel-list defines use
// uint8_t/size_t). board_config.h must come before core.h: core.h has
// #ifndef fallbacks for USE_SD/HAS_GPS so it stays parseable standalone,
// and those fallbacks would silently win (locking in 0) if core.h were
// processed first on a board that actually sets them.
#include <Arduino.h>
#include "board_config.h"
#include "core.h"
#include "roost_session.h"
#include "esp_event.h"   // esp_event_loop_create_default() for coreWifiSnifferStart()
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <SD.h>
#include <ArduinoJson.h>   // wifi-creds file parse/serialize (SSID is user bytes)
#include <Preferences.h>   // boot_count, which must survive a power cut
#include "esp_mac.h"       // esp_read_mac() for own_macs and device_serial
#include "esp_event.h"

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

// The boot-time NTP fallback needs no board opt-in, because WiFi and time.h are
// universal on ESP32 and cost negligible flash. It joins WiFi only when GPS is
// absent and credentials are stored, so a GPS board with a healthy module never
// touches the radio here.
#ifndef WIFI_CREDS_FILE
#define WIFI_CREDS_FILE "/wifi.json"
#endif
// Persisted tuning the web console writes. Shared file rather than one per
// setting, so adding the next knob costs no extra loader.
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
// Environment Density presets: the path-loss exponent for each setting. Medium
// defers to PATH_LOSS_N so a board that already tuned that value still gets it,
// and so the default model is unchanged from before the presets existed.
#ifndef PATH_LOSS_N_LOW
#define PATH_LOSS_N_LOW  2.0f    // open ground, near line of sight
#endif
#ifndef PATH_LOSS_N_MED
#define PATH_LOSS_N_MED  PATH_LOSS_N
#endif
#ifndef PATH_LOSS_N_HIGH
#define PATH_LOSS_N_HIGH 3.5f    // dense urban, heavy obstruction
#endif
// Proximity-alert tuning, described where the module lives further down and in
// docs/alerts.md. Up here with the rest of the compiled-in defaults because
// coreSettingsLoad() seeds the ring from PROX_RING_M.
#ifndef PROX_RING_M
#define PROX_RING_M 25        // default ring in metres, 0 disables
#endif
#ifndef PROX_HYST_PCT
#define PROX_HYST_PCT 130     // clear the latch beyond this percent of the ring
#endif
#ifndef PROX_EMA_SHIFT
#define PROX_EMA_SHIFT 2      // alpha = 1/4: tracks a moving vehicle, ignores a null
#endif

// How long to join the saved network before giving up and timestamping from
// boot. board_config.h may override, and a few boards already define it.
#ifndef NTP_JOIN_TIMEOUT_MS
#define NTP_JOIN_TIMEOUT_MS 10000
#endif
// Presence probe: how long coreTimeSync() waits for a checksum-valid NMEA
// sentence before concluding no GPS module is wired and falling through to NTP.
#ifndef GPS_PRESENCE_PROBE_MS
#define GPS_PRESENCE_PROBE_MS 3000
#endif

// ============================================================
// BUILD IDENTITY: assembled once into a static buffer rather than built per
// call, since the boot banner, the web console and the display all want it.
// ============================================================

const char* coreBuildRev() { return BIRDOSCOPE_GIT_REV; }

const char* coreBuildIdentity() {
  static char buf[96];
  if (buf[0] == '\0') {
    snprintf(buf, sizeof(buf), "v%s %s (%s) built %s",
             BIRDOSCOPE_VERSION, BIRDOSCOPE_GIT_REV, BIRDOSCOPE_GIT_DATE,
             BIRDOSCOPE_BUILD_TS);
  }
  return buf;
}

// ============================================================
// TARGET OUI TABLE. Shared target data, not board config. One flat table
// tagged by vendor rather than a table per vendor: a single scan in the
// promiscuous hot path, and the match reports which vendor hit, which the
// logging side needs to distinguish Flock from Axon.
//
// Flock entries contributed by @NitekryDPaul + Michael/DeFlockJoplin field
// research: https://github.com/DeflockJoplin/flock-you
// Axon entries transcribed from the IEEE registry
// (https://standards-oui.ieee.org), covering the Axon Enterprise, VieVu and
// Fusus assignments. The two Fusus ones are MA-M /28 blocks, which is the whole
// reason the nibbles field exists.
//
// DRAM_ATTR is functional, not decorative. matchOuiRaw() is IRAM_ATTR and
// runs from the WiFi promiscuous callback, which must not depend on flash being
// readable: SPIFFS autosave writes make the flash mapping briefly unavailable,
// and a fetch from memory-mapped .rodata during that window faults. The
// previous oui_bytes[][] avoided this by being uninitialised .bss, written at
// boot by precompileOuis().
//
// Dropping const is NOT sufficient on its own. GCC promotes a static array it
// can prove is never written into .rodata regardless, which puts it back in
// flash; verified by checking the symbol's section in the .elf. DRAM_ATTR
// forces .dram0.data. Do not remove it, and do not add const.
// ============================================================

// nibbles: how many hex digits of the prefix are significant.
//   6 = MA-L, a 24-bit OUI, b[3] unused
//   7 = MA-M, a 28-bit prefix, high nibble of b[3] significant
typedef struct {
  uint8_t b[4];
  uint8_t nibbles;
  uint8_t vendor;
} OuiEntry;

static DRAM_ATTR OuiEntry oui_table[] = {
  // --- Flock Safety ---
  // The only block IEEE assigns to Flock Safety itself. Every other prefix
  // below belongs to a module vendor, so this is the one entry that cannot
  // match a third party's hardware. See docs/detection_methods.md, "Target OUI
  // table provenance".
  {{0xB4,0x1E,0x52,0}, 6, VENDOR_FLOCK},

  // Original flock-you findings from @NitekryDPaul
  {{0x70,0xC9,0x4E,0}, 6, VENDOR_FLOCK}, {{0x3C,0x91,0x80,0}, 6, VENDOR_FLOCK},
  {{0xD8,0xF3,0xBC,0}, 6, VENDOR_FLOCK}, {{0x80,0x30,0x49,0}, 6, VENDOR_FLOCK},
  {{0xB8,0x35,0x32,0}, 6, VENDOR_FLOCK}, {{0x14,0x5A,0xFC,0}, 6, VENDOR_FLOCK},
  {{0x74,0x4C,0xA1,0}, 6, VENDOR_FLOCK}, {{0x08,0x3A,0x88,0}, 6, VENDOR_FLOCK},
  {{0x9C,0x2F,0x9D,0}, 6, VENDOR_FLOCK}, {{0xC0,0x35,0x32,0}, 6, VENDOR_FLOCK},
  {{0x94,0x08,0x53,0}, 6, VENDOR_FLOCK}, {{0xE4,0xAA,0xEA,0}, 6, VENDOR_FLOCK},
  {{0xF4,0x6A,0xDD,0}, 6, VENDOR_FLOCK}, {{0xF8,0xA2,0xD6,0}, 6, VENDOR_FLOCK},
  {{0x24,0xB2,0xB9,0}, 6, VENDOR_FLOCK}, {{0x00,0xF4,0x8D,0}, 6, VENDOR_FLOCK},
  {{0xD0,0x39,0x57,0}, 6, VENDOR_FLOCK}, {{0xE8,0xD0,0xFC,0}, 6, VENDOR_FLOCK},
  {{0xE0,0x4F,0x43,0}, 6, VENDOR_FLOCK}, {{0xB8,0x1E,0xA4,0}, 6, VENDOR_FLOCK},
  {{0x70,0x08,0x94,0}, 6, VENDOR_FLOCK}, {{0x58,0x8E,0x81,0}, 6, VENDOR_FLOCK},
  {{0xEC,0x1B,0xBD,0}, 6, VENDOR_FLOCK}, {{0x3C,0x71,0xBF,0}, 6, VENDOR_FLOCK},
  {{0x58,0x00,0xE3,0}, 6, VENDOR_FLOCK}, {{0x90,0x35,0xEA,0}, 6, VENDOR_FLOCK},
  {{0x5C,0x93,0xA2,0}, 6, VENDOR_FLOCK}, {{0x64,0x6E,0x69,0}, 6, VENDOR_FLOCK},
  {{0x48,0x27,0xEA,0}, 6, VENDOR_FLOCK}, {{0xA4,0xCF,0x12,0}, 6, VENDOR_FLOCK},
  // Locally-administered (0x82 has bit 0x02 set) and unregistered with IEEE,
  // which is what a derived virtual-interface MAC looks like. Field-tested by
  // Michael/DeFlockJoplin as catching cameras the original 30 missed, so it is
  // a live target, not stale data. matchOuiRaw()'s LAA fast-path exists for it:
  // see g_haveLaaTargets.
  {{0x82,0x6B,0xF2,0}, 6, VENDOR_FLOCK},

  // --- Axon Enterprise and acquired brands ---
  {{0x00,0x25,0xDF,0}, 6, VENDOR_AXON},   // Axon Enterprise (was TASER Intl)
  {{0xFC,0x01,0x9E,0}, 6, VENDOR_AXON},   // VieVu, acquired 2018
  {{0x7C,0x83,0x34,0x40}, 7, VENDOR_AXON},// Fusus MA-M /28, acquired 2024
  {{0x84,0xB3,0x86,0x50}, 7, VENDOR_AXON},// Fusus MA-M /28

  // --- Axis Communications ---
  // Surveillance cameras. Carried on the hypothesis that a target may ship 
  // under another registrant's block.
  // Expect commercial-install false positives: tag them, do not trust them.
  {{0x00,0x40,0x8C,0}, 6, VENDOR_AXIS}, {{0xAC,0xCC,0x8E,0}, 6, VENDOR_AXIS},
  {{0xB8,0xA4,0x4F,0}, 6, VENDOR_AXIS}, {{0xE8,0x27,0x25,0}, 6, VENDOR_AXIS},

  // --- Utility, Inc ---
  // BodyWorn / in-car law-enforcement video. The vendor's own registrations,
  // so unlike the Flock rows these cannot match a third party's hardware.
  {{0x00,0x09,0xBC,0}, 6, VENDOR_UTILITY}, {{0x00,0x16,0xED,0}, 6, VENDOR_UTILITY},

#ifdef BENCH_BAIT_OUI
  // Bench load generator, absent from the default build. A local OUI matched on
  // purpose so a stationary bench sees real matched traffic, which is the only
  // way to exercise the queue and the write path without driving. Sessions
  // captured with this are load measurements, not detections.
  {{BENCH_BAIT_OUI, 0}, 6, VENDOR_FLOCK},
#endif
};
static const size_t OUI_COUNT = sizeof(oui_table) / sizeof(oui_table[0]);

// Which vendors the matcher currently accepts, one bit per Vendor. A single
// aligned byte store is atomic on Xtensa, so the Targets menu can switch this
// live without stopping the sniffer or double-buffering the table.
volatile uint8_t coreVendorMask = VENDOR_MASK_ALL;

// True when any active target prefix is itself locally-administered. Gates the
// LAA fast-path in matchOuiRaw(): skipping randomised MACs wholesale is a big
// win, but it silently blocks an LAA target such as 82:6b:f2. Recomputed by
// precompileOuis() and by coreSetVendorMask(), since masking out a vendor can
// remove the last LAA target and re-enable the fast path.
//
// Defaults true, which is the fail-safe direction: before precompileOuis()
// runs we scan more than necessary, never less. Both mains call it before
// coreWifiSnifferStart(), so in practice it is correct by the time any frame
// arrives.
static DRAM_ATTR bool g_haveLaaTargets = true;

static void recomputeLaaTargets() {
  bool any = false;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if ((oui_table[i].b[0] & 0x02) &&
        (coreVendorMask & (1u << oui_table[i].vendor))) { any = true; break; }
  }
  g_haveLaaTargets = any;
}

// The config_change row belongs here rather than at the menu, or the next path
// that reaches a setter records nothing and the log stops describing the
// capture. Re-applying the same mask writes no row.
void coreSetVendorMask(uint8_t mask) {
  coreVendorMask = (uint8_t)(mask & VENDOR_MASK_ALL);
  recomputeLaaTargets();
  roostLogConfigVendorMask();
}

// Menu row order for SCREEN_TARGETS. Kept adjacent to coreTargetIndex() so the
// two cannot drift; screens.inc renders labels in the same order.
static const uint8_t TARGET_MASKS[3] = {
  (uint8_t)(1u << VENDOR_FLOCK),
  (uint8_t)(1u << VENDOR_AXON),
  VENDOR_MASK_ALL,
};

int coreTargetIndex() {
  for (int i = 0; i < 3; i++) {
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

const char* vendorName(uint8_t vendor) {
  switch (vendor) {
    case VENDOR_FLOCK:   return "flock";
    case VENDOR_AXON:    return "axon";
    case VENDOR_AXIS:    return "axis";
    case VENDOR_UTILITY: return "utility";
    default:             return "unknown";
  }
}

// The table is a byte literal now, so there is nothing left to parse. Retained
// for two reasons: both board mains call it in setup(), and the boot line tells
// you which target set you are about to drive around with.
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
  // Must honour the active mask: the `inject` command builds a synthetic target
  // MAC from this, and returning a Flock OUI while the Targets menu is set to
  // Axon would make the injected test frame miss.
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (coreVendorMask & (1u << oui_table[i].vendor)) {
      out[0] = oui_table[i].b[0];
      out[1] = oui_table[i].b[1];
      out[2] = oui_table[i].b[2];
      return;
    }
  }
  out[0] = oui_table[0].b[0];   // mask cleared entirely: fall back to entry 0
  out[1] = oui_table[0].b[1];
  out[2] = oui_table[0].b[2];
}

bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

// Returns the matching Vendor, or -1 for no match. Callers that only need a
// yes/no can test >= 0.
int IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  // Locally-administered (randomised) MACs have bit 1 of byte 0 set, and fixed
  // infrastructure normally never uses them, so skipping them wholesale avoids
  // scanning the table for most phone probe traffic. Only valid while no active
  // target is itself LAA: 82:6b:f2 is, and this early return silently blocked
  // it before g_haveLaaTargets existed. Byte 0 carries the LAA bit, so the
  // prefix compare below already keeps LAA inputs matching only LAA entries.
  if ((mac[0] & 0x02) && !g_haveLaaTargets) return -1;
  const uint8_t mask = coreVendorMask;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const OuiEntry& e = oui_table[i];
    // Prefix compare first: byte 0 rejects non-target frames, so the mask test
    // only runs on a prefix hit.
    if (mac[0] != e.b[0] || mac[1] != e.b[1] || mac[2] != e.b[2]) continue;
    if (!(mask & (1u << e.vendor)))                               continue;
    if (e.nibbles == 7 && ((mac[3] ^ e.b[3]) & 0xF0))             continue;
    return (int)e.vendor;
  }
  return -1;
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

// Runtime scan mode + Single-mode channel. Default to the board's build-time
// CHANNEL_MODE / SINGLE_CHANNEL. The Scan Mode menu switches them live in RAM,
// resetting to the default on reboot. This was a compile-time `#if CHANNEL_MODE`
// gate, and is now a runtime switch so the menu can change modes without
// reflashing.
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

// Display and JSON only: freq_mhz stopped being a logged column in wifi_obs v2,
// because channel and band already determine it. Channel 14 is the one the
// linear formula misses - 802.11 puts it at 2484 MHz, not 2477.
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
  // See coreSetVendorMask(). A mode switch is a `channels` change; obs_mode is
  // fixed promiscuous on this build and does not move with it.
  roostLogConfigChannels();
  dualPrintf("[bscope] scan mode -> %s ch=%u\n", channelModeName(), currentChannel);
}

// ============================================================
// JSON ESCAPE: only needed for SSIDs, which are user-controlled bytes
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
  // Proximity-alert state, session-only and absent from fySerializeDet(). 0
  // dBm is impossible for a real reading, so emaRssi is its own unseeded flag.
  int8_t   emaRssi;
  uint8_t  proxLatched;
  // Not exclusive: a camera seen both ways sets both. `method` cannot answer
  // this, recording only how the row was first created. Session-only, like the
  // proximity state above. Spec C1-C2.
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

// Detection tallies, described in core.h. Plain uint16_t rather than volatile:
// only coreHandleAlert() writes them, from the loop()-context queue drain, and
// the display reads them from the same context.
uint16_t coreDirectFrames   = 0;
uint16_t coreIndirectFrames = 0;

static void tallyFrame(AlertType t) {
  uint16_t& n = (t == ALERT_OUI_ADDR1) ? coreIndirectFrames : coreDirectFrames;
  if (n < 0xFFFF) n++;
}

// Devices, not frames, and the two overlap: a camera seen both ways counts in
// each, so the sum can exceed fyDetCount. Spec C1-C3.
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
    // The vocabulary's word for "no target matched". "unknown" is not in
    // detection_method's allowed set, so it would fail to resolve, empty a
    // column and raise vocabulary_error. Unreachable today; the default arm is
    // what the next alert type falls through.
    default:                   return "unmatched";
  }
}

// Returns index of entry (new or updated), or -1 if table is full.
// chirpWorthy = true when the caller should fire the ascending new-discovery
// chirp: either (a) MAC is brand new to this session, or (b) MAC is known
// but has not been seen in REDISCOVER_MS, meaning it left RF range and came
// back. A board without a buzzer ignores outChirpWorthy.
static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch, const char* ssid,
                          bool direct, bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      // Latched, never cleared: a later frame of the other kind adds a
      // direction rather than replacing one.
      if (direct) fyDet[i].seenDirect   = 1;
      else        fyDet[i].seenIndirect = 1;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      // NULL or a real name: roostSsidPrintable already decided, so this does
      // not second-guess what an empty SSID means.
      if (ssid && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    // Table full: no eviction, no wraparound. Count what we could not record so
    // the display can say so, because otherwise fyDetCount stops moving
    // and reads as "nothing new out here" rather than "out of room". Repeat
    // hits on MACs already in the table still update above, so this counts
    // distinct devices missed, not frames.
    //
    // On a USE_SD board no capture data is lost: the wifi_obs row is written
    // by coreHandleAlert() independent of this return value. On a board
    // without SD, these devices are genuinely gone.
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
  // Left unseeded rather than taking `rssi`, which may be an oui_addr1 hit
  // measuring the AP path. proximityEvaluate() seeds it from a direct one.
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
// SPIFFS SESSION PERSISTENCE: bulletproof envelope format
// ============================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},...]     (exactly B bytes, CRC32 == X)
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

// Minimal envelope parser: pulls bytes + crc fields by substring search.
// Robust to field reordering. Rejects anything without both required keys.
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

// Promote any valid session file from last boot into /prev_session.json, then
// start this boot with a fresh empty table. Preserves history across power cycles.
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
      // Table is full and has been dropping devices. Say so every heartbeat,
      // since det= is pinned at MAX_DETECTIONS and looks like a quiet area.
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
// SERIAL COMMANDS: core handles the shared verbs, whose dump format is
// identical on every board. `status` is genuinely per-board, since its fields
// differ, so each board composes its own printStatus() using the extern state
// below rather than a shared core implementation.
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

// Shared line tokenizer, described in core.h. Accumulates Serial bytes into a static
// buffer until a newline, then splits into a lowercased verb (first token) and
// a trimmed argument (remainder). Overlong lines are truncated, not overflowed.
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
  if (!strcmp(verb, "chirp"))  { corePlayDetectChirp();    return true; }  // tone test
  if (!strcmp(verb, "prox"))   { corePlayProximityChirp(); return true; }  // tone test
  if (!strcmp(verb, "jingle")) { corePlayStartupJingle();  return true; }  // tone test
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
  dualPrintln("  jingle            play boot jingle (tone test)");
#endif
}

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 32

volatile uint32_t coreQueueDrops = 0;
volatile uint8_t  coreQueueDepthMax = 0;

uint8_t coreAlertQueueSize() { return ALERT_QUEUE_SIZE; }

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
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
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full, loop() is behind
    // A matched frame lost outright: it never reaches the log. Distinct from
    // fyDroppedNew, which is a full display table on a board that still writes
    // the row. Becomes device_event buffer_full and observations_dropped.
    coreQueueDrops = coreQueueDrops + 1;
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  memcpy((void*)e->mac, mac, 6);
  // Stamped here, in the callback, so the row carries when the frame arrived
  // rather than when loop() got round to it.
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

  // Copied whole: a string copy drops `len` and `present`, which is what
  // separates an absent SSID element from a present empty one.
  if (ssid) *(RoostSsid*)&e->ssid = *ssid;
  else      memset((void*)&e->ssid, 0, sizeof(RoostSsid));

  if (kind)     { strncpy((char*)e->frameKind,    kind,     11); ((char*)e->frameKind)[11]    = '\0'; }
  else           { ((char*)e->frameKind)[0] = '\0'; }

  if (fsubtype) { strncpy((char*)e->frameSubtype, fsubtype, 15); ((char*)e->frameSubtype)[15] = '\0'; }
  else           { ((char*)e->frameSubtype)[0] = '\0'; }

  alertHead = next;
  // High-water mark, to size the queue against real load rather than a guess.
  // The roost row carries the IE lists, which widens every entry; this says
  // how much headroom there is to spend. Migration P5.
  uint8_t depth = (uint8_t)((alertHead + ALERT_QUEUE_SIZE - alertTail) % ALERT_QUEUE_SIZE);
  if (depth > coreQueueDepthMax) coreQueueDepthMax = depth;
  portEXIT_CRITICAL_ISR(&queueMux);
}

bool coreDequeueAlert(AlertEntry& out) {
  portENTER_CRITICAL(&queueMux);
  if (alertTail == alertHead) { portEXIT_CRITICAL(&queueMux); return false; }
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

// Raw sniffer counters, described in core.h. Plain increments in the callback:
// a lost count under contention costs nothing, and a lock here would be on the
// hot path.
volatile uint32_t coreSeenFrames      = 0;
volatile uint32_t coreCandidateFrames = 0;

// Management subtype histogram. The only instrument that sees a frame the
// matcher rejected: everything else in this firmware records matches, so a
// subtype that never arrives and one that arrives and is never matched leave
// the same artifact. Counted at two points so the two can be told apart.
//
// DRAM_ATTR and no flash on the path, same constraint as the OUI census.
DRAM_ATTR volatile uint32_t coreMgmtSeen[16]    = {0};
DRAM_ATTR volatile uint32_t coreMgmtMatched[16] = {0};

// ============================================================
// OUI CENSUS: field instrument, compiled out unless DEBUG_OUI_CENSUS is set.
// Records every distinct OUI the callback sees, dumped by the `census` verb.
// Spec G2-G3.
//
// DRAM_ATTR throughout and no flash reads on the record path, same constraint
// as oui_table, since this runs in the promiscuous callback.
// ============================================================

#ifndef DEBUG_OUI_CENSUS
#define DEBUG_OUI_CENSUS 0
#endif

#if DEBUG_OUI_CENSUS
// Sized for a drive, not a bench. 192 rows costs ~1.1KB of DRAM.
#ifndef CENSUS_MAX
#define CENSUS_MAX 192
#endif

// Which address field an OUI turned up in. Both are recorded, spec G2.
#define CENSUS_ROLE_ADDR2 0x01
#define CENSUS_ROLE_ADDR1 0x02

static DRAM_ATTR uint8_t  censusOui[CENSUS_MAX][3];
static DRAM_ATTR uint16_t censusHits[CENSUS_MAX];
static DRAM_ATTR uint8_t  censusRole[CENSUS_MAX];
static DRAM_ATTR uint16_t censusUsed = 0;
static DRAM_ATTR uint16_t censusOverflow = 0;

// Linear scan: a full table is 192 three-byte compares per frame, which is
// invisible next to the parse that follows.
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

// Marks rows the matcher would accept, and flags locally-administered
// prefixes: a randomised camera MAC reads as a miss otherwise.
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
  // Ahead of every filter below: counts what the radio delivered, not what
  // survived. Zero here means the driver is not feeding us.
  // Read-modify-write rather than ++, which C++20 deprecates on a volatile.
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

  // Ahead of the RSSI gate on purpose: a frame the threshold discards was
  // still delivered, and the gap against coreMgmtMatched is what separates
  // "never arrived" from "arrived and was not matched".
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
  // Same multicast guard the real addr1 path uses: addr1 is broadcast on
  // beacons, which would otherwise bury the table in ff:ff:ff.
  if (!isMulticast(hdr->addr1)) censusRecord(hdr->addr1, CENSUS_ROLE_ADDR1);
#endif

  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver

  // Everything the row needs from the frame, captured while it still exists:
  // the driver's buffer is gone once this callback returns.
  FrameMeta fm;
  memcpy(fm.addr1, hdr->addr1, 6);
  memcpy(fm.addr2, hdr->addr2, 6);
  memcpy(fm.addr3, hdr->addr3, 6);
  fm.seq      = hdr->seq_ctrl;
  fm.fcFlags  = (uint16_t)((hdr->frame_ctrl >> 8) & 0xFF);
  fm.frameLen = (uint16_t)pkt->rx_ctrl.sig_len;
  // sig_mode 0 is non-HT, where the rate says DSSS/CCK from OFDM: the IDF's
  // wifi_phy_rate_t puts the 1/2/5.5/11 Mbps rates at 0x00-0x07.
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
  // four bytes of checksum as another element; roostIeParseLen takes it off.
  const size_t kHdr    = sizeof(wifi_ieee80211_mac_hdr_t);
  const size_t parseLen = roostIeParseLen(pkt->rx_ctrl.sig_len,
                                          pkt->rx_ctrl.sig_len);
  const uint8_t* body  = pkt->payload + kHdr;
  const size_t bodyLen = parseLen > kHdr ? parseLen - kHdr : 0;

  // Every IE-bearing subtype, not only the branches that matched a target.
  // Left to the branches it was empty on beacons, and a declared column that is
  // always empty claims the radio had nothing to report (spec 7.1).
  RoostSsid ssid;
  roostIeSsidCapture(body, bodyLen, ftype, subtype, &ssid);

  // --- OUI check: addr2 (transmitter/source) ---
  //
  // For mgmt Probe Requests (type=0 subtype=4) from a matched OUI, tighten
  // to the DeFlockJoplin wildcard-probe signature: SSID IE (tag 0) length
  // must be zero. This reduces false positives dramatically (Michael's field
  // test: 11/12 true-positive with only 2 false-positives in Joplin).
  //
  // Non-probe frames from the same OUI still emit the broad ADDR2 alert.
  // See: https://github.com/DeflockJoplin/flock-you
  if (matchOuiRaw(hdr->addr2) >= 0) {
    // Counted here, after the match and before the branch below decides what
    // kind of alert it is. A subtype that appears in coreMgmtSeen, appears
    // here, and still produces no row is a fault between this point and the
    // log rather than a frame that never arrived.
    if (type == WIFI_PKT_MGMT)
      coreMgmtMatched[subtype] = coreMgmtMatched[subtype] + 1;

    bool emitted = false;
    if (type == WIFI_PKT_MGMT) {
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        // No FCS retry: the bound above already removed the checksum, so the
        // walk cannot run past the elements. The walk happened once, above;
        // this only reads its result.
        if (ssid.present && ssid.len == 0) {
          enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, &fm, rssi, ch,
                       &ssid, "probe_req", fsub);
          emitted = true;
        } else if (ssid.present) {
          // Directed probe: the probed name identifies configured backhaul
          // networks, which is the field a target's SSID list is built from.
          enqueueAlert(ALERT_DIRECTED_PROBE, hdr->addr2, &fm, rssi, ch,
                       &ssid, "probe_req", fsub);
          emitted = true;
        }
      }
    }
    if (!emitted) {
      enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, &fm, rssi, ch, &ssid, "addr2", fsub);
    }
  }

#if CHECK_ADDR1
  // addr1 (receiver/destination): catches Flock STAs that appear only as the
  // dst of probe responses and data frames, never transmitting in the capture
  // window due to their burst-sleep duty cycle. Multicast guard is mandatory
  // here since addr1 is broadcast (ff:ff:ff:ff:ff:ff) in beacons/broadcasts.
  //
  // addr2 (the AP that sent this probe response) is passed as mac2 so the
  // analysis pipeline can look up the AP's position and use it as a camera
  // location proxy. The RSSI here reflects AP→scanner path loss, not
  // camera→scanner, so it can't be used for triangulation directly.
  if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1) >= 0) {
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, &fm, rssi, ch, &ssid, "addr1", fsub);
  }
#endif

#if CHECK_ADDR3
  // addr3 fallback: catches cases where addr2 is randomised but addr3
  // carries the real BSSID OUI (management frames only).
  if (type == WIFI_PKT_MGMT && matchOuiRaw(hdr->addr3) >= 0) {
    enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, &fm, rssi, ch, &ssid, "addr3", fsub);
  }
#endif

#if ENABLE_SSID_MATCH
  // The name was already extracted above, by the one walker that knows each
  // subtype's fixed-field offset. This branch only decides whether it matches.
  const char* ssidName = roostSsidPrintable(&ssid);
  if (ssidName && matchSsidKeyword(ssidName)) {
    const char* frameKind = (subtype == 8)   ? "beacon"
                          : (subtype == 5)   ? "probe_resp"
                          : (subtype == 4)   ? "probe_req"
                                             : "mgmt";
    enqueueAlert(ALERT_SSID, hdr->addr2, &fm, rssi, ch, &ssid, frameKind, fsub);
  }
#endif
}

// ============================================================
// TIME SOURCE: a runtime priority chain of GPS once a module locks, then NTP
// over WiFi if station creds are stored, then millis() since boot. GPS cannot
// lock at boot, because cold-start is slow, so coreTimeSync() bridges the
// pre-lock window with an NTP join. GPS then takes over automatically the moment
// it locks: gpsTick() sets the GPS anchor and coreTimestampStr() prefers GPS
// over NTP. On a board with no GPS module the NTP anchor stays the source. All
// three anchor implementations live here so boards never duplicate the math.
// See core.h.
// ============================================================

#if HAS_GPS
#include <TinyGPS++.h>

static TinyGPSPlus gpsParser;
static bool        gpsReady        = false;
static bool        gpsTimeAnchored = false;
// Latches the first refused clock so the wait is reported once, not on every
// fix. Never cleared: one report per boot is what the operator needs.
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

// Which HardwareSerial the GPS module is wired to. Defaults to Serial2, and a
// board overrides it when that UART is needed for something else, such as
// putting the debug mirror on Serial2 and GPS on Serial1 to avoid a
// double-begin() conflict on the same UART.
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
// dead antenna feed from a weak signal, because both leave every field empty;
// $GxGSV carries satellites in view and their C/N0, which separates them.
// Time-limited rather than a toggle, so it cannot be left on in the field.
static uint32_t gpsEchoUntil = 0;
static char     gpsEchoLine[100];
static uint8_t  gpsEchoLen  = 0;

static void gpsEchoFor(uint32_t ms) {
  gpsEchoUntil = ms ? millis() + ms : 0;
  gpsEchoLen   = 0;
}

// Assembled into whole sentences rather than echoed per byte: NMEA is
// line-oriented and a per-byte print would cost more than the 9600 baud it is
// reading. A sentence longer than the buffer is dropped, not split.
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

// Non-blocking: drain whatever bytes arrived since last call into the parser.
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
    // ok = valid NMEA sentences (passed checksum), bad = corrupt (failed),
    // together they tell garbage-on-the-wire (ok≈0, bad climbing) apart from
    // valid-NMEA-but-no-fix-yet (ok climbing, fixsent still 0). fixsent only
    // counts fix-carrying sentences, so it stays 0 until a lock.
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
      // A capture cannot predate the build that produced it, which makes the
      // build stamp a floor no correct clock can fail. Staying unanchored is a
      // designed state: empty timestamp_utc, the boot-numbered directory, and
      // clock_source "none". Adopting a wrong time is not.
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

// GPS presence probe: the runtime check for whether a module is actually wired.
// Drains the UART into the parser for up to GPS_PRESENCE_PROBE_MS and returns
// true the moment a checksum-valid NMEA sentence lands. passedChecksum(), rather
// than charsProcessed(), is the signal, because an open or floating RX pin frames
// line noise as bytes, so charsProcessed climbs even with no module. Noise cannot
// forge a valid `$…*XX` checksum. Returns fast on a healthy module, around 1s,
// and only burns the full window when GPS is genuinely absent.
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
// WIFI STATION CREDENTIALS (see core.h): {"ssid","pass"} on SPIFFS, set from
// the web console. Used only by the NTP fallback below. Always compiled: any
// board can fall back to NTP when its GPS module is absent.
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
// PERSISTED SETTINGS: web-console tuning that has to survive a power cycle, in
// one shared JSON file rather than one per setting, so the next knob costs no
// extra loader.
//
// coreSettingsLoad() runs once from setup() after SPIFFS is up, and nothing reads
// it lazily. Skip that call and the compiled-in defaults quietly stand, while
// saving and reading back still appear to work within a single boot.
// ============================================================

void coreSetEnvDensity(uint8_t density) {
  if (density < DENSITY_COUNT) coreEnvDensity = density;   // ignore, don't clamp
}

void coreSetRssiAt1mDbm(int8_t dbm) {
  // Clamped rather than rejected, so a mistyped value cannot produce absurd ranges.
  if (dbm > RSSI_AT_1M_MAX) dbm = RSSI_AT_1M_MAX;
  if (dbm < RSSI_AT_1M_MIN) dbm = RSSI_AT_1M_MIN;
  coreRssiAt1mDbm = dbm;
}

void coreNudgeRssiAt1mDbm(int8_t db) {
  // int, not int8_t: a large step wraps and clamps to the wrong end.
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
// NTP FALLBACK: joins the saved station network, pulls UTC, and anchors time
// via the libc clock. Runs only from coreTimeSync() when no GPS module was
// detected. The no-creds path returns without touching the radio, so the sniffer
// inits from cold. A join-attempted path always leaves WiFi OFF on exit.
// ============================================================

static bool     ntpTimeAnchored = false;
// The anchor moment, not just the fact of it: the manifest needs the pair
// that places every pre-anchor row retroactively.
static uint32_t ntpAnchorUnix = 0;
static uint32_t ntpAnchorMs   = 0;

static void ntpSync() {
  String ssid, pass;
  if (!coreWifiCredsLoad(ssid, pass)) {
    dualPrintln("[bscope] no saved WiFi network - timestamping from boot");
    return;   // never bring WiFi up: leave the radio clean for the sniffer
  }

  WiFi.mode(WIFI_STA);

  // Pre-scan before committing to the (blocking) join: on a GPS board this runs
  // every boot to bridge the pre-lock window, and when mobile the saved network
  // is usually out of range, so a short scan skips straight to millis() instead of
  // burning the full NTP_JOIN_TIMEOUT_MS of dead air before scanning starts. A
  // scan *failure* (not "absent") shouldn't permanently deny NTP, so fall through
  // and attempt the join best-effort in that case.
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
  // A GPS module can't have a lock this early (cold-start takes far longer than
  // the boot probe), so bridge the pre-lock window with NTP either way. If GPS
  // is onboard it takes over the instant it locks, since gpsTick() sets the
  // anchor and coreTimestampStr() prefers GPS over NTP, so this is a bridge
  // rather than a demotion.
  // Without a saved network ntpSync() is an instant no-op and we ride millis()
  // until (if) GPS locks.
  if (!gpsTimeAnchored) ntpSync();
#else
  ntpSync();   // joins only if credentials are stored; otherwise millis()
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
// SD logging is a session directory of roost record files, written in
// lib/birdoscope_core/roost_session.cpp. Only the load counters below live
// here: they measure the write path rather than format it.
// ============================================================

// ============================================================
// SERIAL JSON EMISSION
// ============================================================
//
// Emits one JSON object per detection, one per line, over USB CDC serial. Any
// serial consumer can ingest it, whether a downstream analysis tool reading the
// port or a plain terminal. Schema inherited from upstream flock-you. GPS, when
// present, comes from this board's own fix. A board without GPS emits "gps":null.

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

  // ap_mac: only present for oui_addr1, naming the AP whose probe response revealed
  // the camera. Its position (from wardriving data) bounds the camera location.
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
// RSSI -> DISTANCE: log-distance path loss, with Environment Density picking n
// and coreRssiAt1mDbm as the reference level.
//
// Callers skip addr1 hits, whose RSSI describes the AP->scanner path rather than
// target->scanner; coreHandleAlert() already does. Both settings are runtime and
// persisted, so this is not a pure function of the board config. RSSI_AT_1M and
// the PATH_LOSS_N_* presets near the top of this file are only the defaults.
// The model, calibration, and the invariants that fail quietly are documented
// in docs/distance_estimation.md.
// ============================================================

volatile uint8_t coreEnvDensity = DENSITY_MEDIUM;
volatile int8_t  coreRssiAt1mDbm = RSSI_AT_1M;

// Set after the dedupe gate, so it tracks readings the user was actually shown
// rather than every rate-limited repeat.
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

// Defined further down alongside the rest of the notification module,
// forward-declared here since coreHandleAlert() calls it directly instead
// of leaving detection feedback to the board.
static void notifyDetection(bool chirpWorthy, bool rangeable, int8_t vendor);
static void notifyProximity(int8_t vendor);

// ============================================================
// PROXIMITY ALERT: latched range ring, described in core.h. Three things keep
// one ring from becoming a stream of chirps: the EMA absorbs the 6-10 dB
// multipath swing, the latch makes a crossing an event rather than a state,
// and the hysteresis band stops the latch re-arming on what jitter is left.
// What the constants have to absorb is in docs/alerts.md, under the proximity
// ring.
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
  if (idx < 0) return false;                    // table full: no row to hold state
  if (coreProxRingM == 0) return false;         // ring off
  // addr1 measures the AP that answered the probe, not the target. Same
  // exclusion coreHandleAlert() already applies to distM.
  if (type == ALERT_OUI_ADDR1) return false;

  FYDetection& d = fyDet[idx];
  bool seeding = (d.emaRssi == 0);
  if (seeding) {
    d.emaRssi = rssi;
  } else {
    // Floored at 1 dB: a bare shift truncates differences under
    // 2^PROX_EMA_SHIFT to zero and the average parks short of the reading
    // forever, leaving a stationary target inside the ring silent. Computed in
    // int space so the intermediate cannot wrap.
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
  // A row seeded already inside latches silently: the new-detection chirp is
  // firing for the same frame.
  return !seeding;
}

// ============================================================
// coreHandleAlert: the shareable middle of drainAlertQueue(), covering detection
// table update, SD log append, dedupe gate, serial DETECT line, JSON emit,
// LED/buzzer notification. Boards call this once per dequeued AlertEntry
// and use the result for display state.
// ============================================================

CoreAlertResult coreHandleAlert(const AlertEntry& e) {
  CoreAlertResult r{};
  macToStr(e.mac, r.macStr, sizeof(r.macStr));
  const char* method = alertTypeToMethod(e.type);

  char apMacStr[18] = "";
  if (e.type == ALERT_OUI_ADDR1) macToStr(e.addr2, apMacStr, sizeof(apMacStr));

  float distM = (e.type != ALERT_OUI_ADDR1) ? coreRssiToDistanceM(e.rssi) : -1.0f;

  bool chirpWorthy = false;
  // Direct: the camera itself transmitted, so the RSSI describes the path to
  // it. Only an addr1 hit does not.
  bool direct = (e.type != ALERT_OUI_ADDR1);
  // The detection table is a display surface and holds a printable name; the
  // record column takes the octets and the length instead. One conversion,
  // here, rather than each consumer deciding what an empty SSID means.
  int idx = fyAddDetection(r.macStr, method, e.rssi, e.channel,
                            roostSsidPrintable(&e.ssid),
                            direct, &chirpWorthy);

#if USE_SD
  roostLogWifiObs(e, method);
#endif

  // Refresh unconditionally, since a device counts as active even when the
  // dedupe gate below rate-limits its serial/JSON/display output.
  fyLastTargetSeen = millis();

  // Same reasoning: the tallies count what the radio heard, not what the
  // device announced.
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
  // Re-matched rather than carried on the queue, keeping AlertEntry small. -1 for
  // an ALERT_SSID hit, which matched on name rather than OUI.
  r.vendor = (int8_t)matchOuiRaw(e.mac);

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
  lastDetectionRssi = e.rssi;   // calibration reference; see coreLastDetectionRssi()

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
  // distM is already -1 for exactly the addr1 hits, so it is the predicate
  // rather than a second copy of the type test.
  notifyDetection(r.chirpWorthy, r.distM >= 0.0f, r.vendor);
  return r;
}

// ============================================================
// NOTIFICATIONS: LED (NeoPixel) and buzzer.
//
// A detection encodes two facts at once. Colour carries the vendor, so which
// fleet was seen is readable without looking at the panel, and pulse count
// carries whether the MAC is new. Trains are stepped from ledTick() rather than
// blocking, since notifyDetection() runs in the alert drain path. Both the
// detection and heartbeat paths honour coreLedEnabled and coreBuzzerEnabled; the
// boot jingle, RGB cycle and on-demand replay hooks do not. See docs/alerts.md.
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

// Equal on/off intervals of ledPulseMs until ledPulsesLeft is exhausted, then
// parks the LED off and goes idle. Called from coreNotifyTick() every loop().
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
  ledNextAt = 1;    // any nonzero past time; the tick below lights pulse one
  ledTick();        // light it now instead of up to one loop() later
#endif
}

static void ledFlash(uint8_t r, uint8_t g, uint8_t b, unsigned ms) {
  ledBlink(r, g, b, ms, 1);
}

// Two fast ascending beeps, played on the FIRST sighting of a MAC.
static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

// Three descending beeps on a ring crossing, against the new-detection
// chirp's two ascending. Tones default off NEW_CHIRP_*, so a board that tuned
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

// Silent despite the name: a purple LED pulse while a target is still in range
// (last seen within HB_DEVICE_ACTIVE_MS). Uncalled on screen models, spec A1.
__attribute__((unused))
static void heartbeatBeep() {
#if USE_LED
  if (!coreLedEnabled) return;   // Alerts menu: LED off
  ledFlash(LED_COLOR_HB_R, LED_COLOR_HB_G, LED_COLOR_HB_B, LED_FLASH_MS);
#endif
}

static void startupBeep() {
#if USE_BUZZER
  // First 6 notes of SMB World 1-2 (underground). Koji Kondo's descending
  // pattern: C5 → C4 → A4 → A3 → G#4 → G#3 (alternating-octave pairs).
  static const uint16_t notes[6] = { 523, 262, 440, 220, 415, 208 };
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#endif
}

// Public hooks (declared in core.h) that let serial and web commands replay the
// buzzer sounds on demand for tone tuning. Thin wrappers over the static
// players above. Both no-op on a board without a buzzer.
void corePlayDetectChirp()    { newDetectChirp(); }
void corePlayStartupJingle()  { startupBeep(); }
void corePlayProximityChirp() { proximityChirp(); }

// Uncalled on any board with a display, per spec A1. Retained for display-less
// boards: call heartbeatTick() from coreNotifyTick() to re-enable.
// fyLastHeartbeatAt is kept current by notifyDetection() so the phase survives.

// Last time the heartbeat pulse fired. When nothing has been seen for
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

// New MAC chirps and blinks twice; a repeat is silent and blinks once. Colour
// carries vendor, pulse count carries new-versus-repeat. See docs/alerts.md.
//
// `rangeable` gates the buzzer only, never the LED: spec A2 and A3.
static void notifyDetection(bool chirpWorthy, bool rangeable, int8_t vendor) {
  if (chirpWorthy && rangeable) {
    if (coreBuzzerEnabled) newDetectChirp();   // Alerts menu: buzzer mute
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

// A ring crossing: three pulses against two-for-new and one-for-repeat, with
// colour still carrying the vendor.
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
  // RGB sanity check: cycle R → G → B so a wiring or dead-pixel fault is obvious.
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
// INPUT: plain debounced buttons
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
// SEMANTIC NAV LAYER, described in core.h. A small event queue fed by both the serial
// injector (coreInjectNav) and the physical buttons (coreNavTick, 3-button
// scheme only), plus the screen-carousel state machine (coreNavApply).
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
#if NAV_SCHEME_3BTN
  // Per-button edge + long-press tracker. Index 0/1/2 = BTN_1/2/3. A short
  // press fires on release, so it can be distinguished from a long. A long
  // press fires the moment it crosses NAV_LONG_PRESS_MS while still held, so
  // MARK/BACK feel immediate. Buttons are active-LOW (INPUT_PULLUP).
  static bool          initialized = false;
  static bool          last[3];
  static unsigned long changedAt[3];
  static unsigned long pressedAt[3];
  static bool          longFired[3];
  static const uint8_t  pins[3]    = { BTN_PIN_1, BTN_PIN_2, BTN_PIN_3 };
  static const NavEvent shortEv[3] = { NAV_UP,   NAV_DOWN, NAV_SELECT };
  static const NavEvent longEv[3]  = { NAV_MARK, NAV_NONE, NAV_BACK   };  // NAV_NONE = no long action

  if (!initialized) {
    for (int i = 0; i < 3; i++) {
      pinMode(pins[i], INPUT_PULLUP);
      last[i] = HIGH; changedAt[i] = 0; pressedAt[i] = 0; longFired[i] = false;
    }
    initialized = true;
  }

  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    bool lvl = digitalRead(pins[i]);
    if (lvl != last[i] && now - changedAt[i] > BTN_DEBOUNCE_MS) {   // debounced edge
      changedAt[i] = now;
      last[i]      = lvl;
      if (lvl == LOW) {                         // press
        pressedAt[i] = now;
        longFired[i] = false;
      } else if (!longFired[i]) {               // release without a prior long → short
        coreInjectNav(shortEv[i]);
      }
    }
    // Long press fires once, while still held, as soon as the threshold passes.
    if (last[i] == LOW && !longFired[i] && longEv[i] != NAV_NONE
        && now - pressedAt[i] >= NAV_LONG_PRESS_MS) {
      coreInjectNav(longEv[i]);
      longFired[i] = true;
    }
  }
#endif
  return navQPop();
}

// ------------------------------------------------------------
// SCREEN CAROUSEL + MENU DRILL-IN: top-level Up/Down cycle the six screens.
// SELECT on a menu screen (SCAN_MODES / CONFIG) drills into an option list, and
// selecting Single opens a channel picker. MARK is available at every level.
// See docs/menu_ux.md.
// ------------------------------------------------------------

ScreenId  coreCurrentScreen = SCREEN_OVERVIEW;
MenuState coreMenuState      = MENU_NONE;
int       coreMenuSel        = 0;

NavAction coreNavApply(NavEvent ev) {
  if (ev == NAV_MARK) return NAV_ACT_MARK;   // always available, any screen/menu

  switch (coreMenuState) {

  case MENU_NONE:
    switch (ev) {
      case NAV_UP:      // advance forward through the screens (wraps at SCREEN_COUNT)
        coreCurrentScreen = (ScreenId)((coreCurrentScreen + 1) % SCREEN_COUNT);
        return NAV_ACT_REDRAW;
      case NAV_DOWN:    // go back a screen (wraps)
        coreCurrentScreen = (ScreenId)((coreCurrentScreen + SCREEN_COUNT - 1) % SCREEN_COUNT);
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
        if (coreCurrentScreen == SCREEN_ALERTS) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = 0;                          // start on the Buzzer row
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_CONFIG) {
          coreMenuState = MENU_LIST;
          coreMenuSel   = 1;                          // Off, the portal is closed while browsing Detect
          return NAV_ACT_REDRAW;
        }
        return NAV_ACT_NONE;                           // info screens: nothing to select
      default:
        return NAV_ACT_NONE;                           // BACK at top level: nothing
    }

  case MENU_LIST: {
    // Row count of the open list, one entry per menu screen.
    int n;
    switch (coreCurrentScreen) {
      case SCREEN_SCAN_MODES: n = 3; break;
      case SCREEN_TARGETS:    n = 3; break;
      case SCREEN_ALERTS:     n = 3; break;   // Buzzer / LED / Proximity
      default:                n = 2; break;   // CONFIG
    }
    switch (ev) {
      case NAV_UP:   coreMenuSel = (coreMenuSel + n - 1) % n; return NAV_ACT_REDRAW;
      case NAV_DOWN: coreMenuSel = (coreMenuSel + 1) % n;     return NAV_ACT_REDRAW;
      case NAV_BACK: coreMenuState = MENU_NONE;               return NAV_ACT_REDRAW;
      case NAV_SELECT:
        if (coreCurrentScreen == SCREEN_ALERTS) {
          // Toggle the highlighted gate in place and stay in the list so both
          // rows can be flipped before long-Back pops out (unlike the act-and-
          // close Scan Mode / Config menus).
          if (coreMenuSel == 0)      coreBuzzerEnabled = !coreBuzzerEnabled;  // Buzzer
          else if (coreMenuSel == 1) coreLedEnabled    = !coreLedEnabled;     // LED
          else {
            // Proximity is a range rather than a gate, so it drills into a
            // picker instead of flipping. Same shape as Single → channel.
            coreMenuState = MENU_PICK_PROX;
            const int p   = coreProxRingIndex();
            coreMenuSel   = (p >= 0) ? p : 0;
          }
          return NAV_ACT_REDRAW;
        }
        if (coreCurrentScreen == SCREEN_TARGETS) {
          // Act-and-close, like Scan Mode. Switching the mask is a single byte
          // store, so the sniffer keeps running across the change.
          if (coreMenuSel >= 0 && coreMenuSel < 3) {
            coreSetVendorMask(TARGET_MASKS[coreMenuSel]);
            dualPrintf("[bscope] targets -> %s\n",
                       coreMenuSel == 0 ? "flock" : coreMenuSel == 1 ? "axon" : "all");
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
        // CONFIG
        coreMenuState = MENU_NONE;
        if (coreMenuSel == 0) return NAV_ACT_ADMIN;    // On → enter Admin
        return NAV_ACT_REDRAW;                          // Off → close the menu
      default:
        return NAV_ACT_NONE;
    }
  }

  case MENU_PICK_PROX:
    switch (ev) {
      // Wraps, unlike the channel picker: this is a short option list, not a
      // dialled number with meaningful ends.
      case NAV_UP:
        coreMenuSel = (coreMenuSel + PROX_RING_OPTION_COUNT - 1) % PROX_RING_OPTION_COUNT;
        return NAV_ACT_REDRAW;
      case NAV_DOWN:
        coreMenuSel = (coreMenuSel + 1) % PROX_RING_OPTION_COUNT;
        return NAV_ACT_REDRAW;
      case NAV_SELECT:
        if (coreMenuSel >= 0 && coreMenuSel < PROX_RING_OPTION_COUNT) {
          coreSetProxRingM(PROX_RING_OPTIONS[coreMenuSel]);
          // Persisted on the spot: it is calibration-class, like density and
          // rssi_1m, not a session gate like the Buzzer and LED rows above it.
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

// Anytime BOOT double-press detector, described in core.h. Watches for two debounced
// presses within BOOT_DOUBLE_PRESS_MS and fires once per gesture. Works at any
// time (not a post-boot window) so Detect↔Admin can be hopped repeatedly. BOOT
// is active-LOW (INPUT_PULLUP, idles HIGH).
bool coreAdminTriggerCheck() {
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
      lastPressMs = now;                                    // first press; wait for the second
    }
  }
  // Expire a lone first press so it can't pair with a much-later press.
  if (lastPressMs != 0 && now - lastPressMs > BOOT_DOUBLE_PRESS_MS) lastPressMs = 0;
  return false;
}

// Raw-IDF promiscuous capture bring-up, described in core.h. Factored out of every
// board's setup() so webPortalStop() can re-run it verbatim when it releases
// the AP and resumes Detect (the portal deinits this driver to hand the radio
// to Arduino WiFi). Idempotent from a clean/deinit'd driver state.
// Reports a failed bring-up step instead of aborting on it, spec G1. Without
// this a dead radio reports itself healthy: sniffing=1 and no frames.
#define WIFI_TRY(call)                                                  \
  do {                                                                  \
    esp_err_t _e = (call);                                              \
    if (_e != ESP_OK)                                                   \
      dualPrintf("[bscope] %s -> %s\n", #call, esp_err_to_name(_e));    \
  } while (0)

void coreWifiSnifferStart() {
  // Left unwrapped: returns ESP_ERR_INVALID_STATE whenever the loop already
  // exists, which is the normal path when webPortalStop() hands the radio back.
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  WIFI_TRY(esp_wifi_init(&cfg));
  WIFI_TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_NULL));
  WIFI_TRY(esp_wifi_start());

  // Its esp_wifi_set_channel() runs before promiscuous mode is enabled and can
  // fail there legitimately; updateChannelMode() sets the channel a hop later.
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

// ============================================================
// SESSION PROVENANCE
//
// What the manifest needs and only core can answer. Kept here rather than in
// roost_session.cpp so the GPS parser, the OUI table and the clock anchor stay
// private to this translation unit.
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
  // Shared, so the row arithmetic and the manifest's agree, and so the
  // pre-anchor case is refused rather than underflowing. A row stamped before
  // the anchor leaves timestamp_utc empty and is placed at ingest.
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
  for (uint8_t n = 1; n <= 99; n++) {
    snprintf(buf, len, "/" LOG_PREFIX "%u-%u-%02u-%u", mo, dy, yr, n);
    if (!SD.exists(buf)) { g_sessionSeq = n; return true; }
  }
  // Refuse rather than hand back a name the loop just proved exists. Renaming
  // into an occupied directory merges two boots under one manifest, which makes
  // their rows unattributable rather than merely misnamed. Spec 6.2.
  buf[0] = '\0';
  g_sessionSeq = 0;
  return false;
}

// FNV-1a over the compiled table. Captures either side of a table change are
// not comparable, and this is what says so at ingest.
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

// The direct answer to what was reachable: a device never heard on a channel
// it never tuned.
// The channel plan in effect. Single mode reports the channel the picker holds,
// not the build's compile-time default.
static void currentChannelPlan(const uint8_t** list, size_t* n) {
  switch (coreScanModeIndex()) {
    case 1:  *list = fullHopChannels;    *n = fullHopChannelCount; break;
    case 2:  *list = &coreSingleChannel; *n = 1;                   break;
    default: *list = customChannels;     *n = customChannelCount;  break;
  }
}

// The config_change `channels` value. The registry declares it a `list`, so the
// rendering is roost_value.h's and not this device's; false means it did not
// fit, which the caller must not write as an empty value.
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

// A session with no Axon rows may mean Axon was masked out rather than absent.
//
// The config_change `vendor_mask` value. A registry `list` of vendor names,
// empty when nothing is matched. Never a word like "none", which would be a
// value standing in for absence, and never a bitmask, which cannot be read
// without this build's bit assignments. See
// vendor/jellybeans/roost_logging/runtime/roost_value.h for the `list`
// rendering, and .../roost_logging/docs/design_spec.md 6.5 on placeholders.
bool coreVendorMaskStr(char* buf, size_t len) {
  static const char* kNames[VENDOR_COUNT] = { "flock", "axon", "axis", "utility" };
  RoostValue v;
  roostValueBegin(&v, buf, len);
  for (int i = 0; i < VENDOR_COUNT; i++)
    if (coreVendorMask & (1u << i)) roostValueAddText(&v, kNames[i]);
  return roostValueDone(&v) != 0;
}
