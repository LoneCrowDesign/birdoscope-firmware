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
#include "esp_event.h"   // esp_event_loop_create_default() for coreWifiSnifferStart()
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <SD.h>
#include <ArduinoJson.h>   // wifi-creds file parse/serialize (SSID is user bytes)
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
// TARGET OUI TABLE. Shared target data, not board config. One flat table
// tagged by vendor rather than a table per vendor: a single scan in the
// promiscuous hot path, and the match reports *which* vendor hit, which the
// logging side needs to distinguish Flock from Axon.
//
// Flock entries contributed by @NitekryDPaul + Michael/DeFlockJoplin field
// research: https://github.com/DeflockJoplin/flock-you
// Axon entries transcribed from the IEEE registry
// (https://standards-oui.ieee.org), covering the Axon Enterprise, VieVu and
// Fusus assignments. The two Fusus ones are MA-M /28 blocks, which is the whole
// reason the nibbles field exists.
//
// DRAM_ATTR is load-bearing, not decoration. matchOuiRaw() is IRAM_ATTR and
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

void coreSetVendorMask(uint8_t mask) {
  coreVendorMask = (uint8_t)(mask & VENDOR_MASK_ALL);
  recomputeLaaTargets();
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
    case VENDOR_FLOCK: return "flock";
    case VENDOR_AXON:  return "axon";
    default:           return "unknown";
  }
}

// The table is a byte literal now, so there is nothing left to parse. Retained
// because both board mains call it in setup() and because a boot-time summary
// of what the firmware is actually hunting is worth the two lines when you are
// about to drive around testing a new target set.
void precompileOuis() {
  uint16_t per[VENDOR_COUNT] = {0};
  uint16_t laa = 0;
  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (oui_table[i].vendor < VENDOR_COUNT) per[oui_table[i].vendor]++;
    if (oui_table[i].b[0] & 0x02) laa++;
  }
  recomputeLaaTargets();
  dualPrintf("[bscope] targets: %u flock, %u axon (%u total, %u locally-administered)\n",
             (unsigned)per[VENDOR_FLOCK], (unsigned)per[VENDOR_AXON],
             (unsigned)OUI_COUNT, (unsigned)laa);
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
    // Prefix compare first: it rejects almost every frame on byte 0, so the
    // mask test only runs on an actual prefix hit.
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

uint16_t channelFreqMhz(uint8_t ch) {
  return (ch >= 1 && ch <= 14) ? (uint16_t)(2407 + 5 * ch) : 0;
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

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:           return "ssid";
    case ALERT_WILDCARD_PROBE: return "wildcard_probe";
    default:                   return "unknown";
  }
}

// Returns index of entry (new or updated), or -1 if table is full.
// chirpWorthy = true when the caller should fire the ascending new-discovery
// chirp: either (a) MAC is brand new to this session, or (b) MAC is known
// but has not been seen in REDISCOVER_MS, meaning it left RF range and came
// back. A board without a buzzer ignores outChirpWorthy.
static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch, const char* ssid,
                          bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      if (ssid && ssid[0] && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    // Table full: no eviction, no wraparound. Count what we could not record so
    // the display can say so, because otherwise fyDetCount simply stops moving
    // and reads as "nothing new out here" rather than "out of room". Repeat
    // hits on MACs already in the table still update above, so this counts
    // distinct devices missed, not frames.
    //
    // On a USE_SD board no capture data is lost: sdAppendRow() is called
    // unconditionally by coreHandleAlert(), independent of this return value.
    // On a board without SD, these devices are genuinely gone.
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
  if (ssid && ssid[0]) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else                 d.ssid[0] = '\0';
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
    dualPrintf("[bscope] save WARNING: wrote %u expected %u – aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }

  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintln("[bscope] save verify FAILED – old session preserved");
    return;
  }

  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[bscope] promote FAILED – data in %s for recovery\n", FY_SESSION_TMP);
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

bool coreHandleSerialCommand(const char* verb, const char* arg) {
  if (!strcmp(verb, "dump")) { dumpCurrentSession(); return true; }
  if (!strcmp(verb, "prev")) { dumpSpiffsFile(FY_PREV_FILE); return true; }
  if (!strcmp(verb, "chirp"))  { corePlayDetectChirp();   return true; }  // tone test
  if (!strcmp(verb, "jingle")) { corePlayStartupJingle(); return true; }  // tone test
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
#if USE_BUZZER
  dualPrintln("  chirp             play detection chirp (tone test)");
  dualPrintln("  jingle            play boot jingle (tone test)");
#endif
}

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 32

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;  // written by callback
static volatile size_t alertTail = 0;  // read by loop()
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;

volatile bool sniffingStopped = false;

void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac,
                             const uint8_t* mac2,
                             int8_t rssi, uint8_t ch,
                             const char* ssid, const char* kind,
                             const char* fsubtype) {
  portENTER_CRITICAL_ISR(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full, loop() is behind
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  memcpy((void*)e->mac, mac, 6);
  if (mac2) memcpy((void*)e->mac2, mac2, 6);
  else       memset((void*)e->mac2, 0,   6);

  if (ssid)     { strncpy((char*)e->ssid,         ssid,     32); ((char*)e->ssid)[32]         = '\0'; }
  else           { ((char*)e->ssid)[0] = '\0'; }

  if (kind)     { strncpy((char*)e->frameKind,    kind,     11); ((char*)e->frameKind)[11]    = '\0'; }
  else           { ((char*)e->frameKind)[0] = '\0'; }

  if (fsubtype) { strncpy((char*)e->frameSubtype, fsubtype, 15); ((char*)e->frameSubtype)[15] = '\0'; }
  else           { ((char*)e->frameSubtype)[0] = '\0'; }

  alertHead = next;
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
  if (ftype != 0) return "ctrl_other";  // control frames
  switch (subtype) {
    case 0:  return "assoc_req";
    case 1:  return "assoc_resp";
    case 2:  return "reassoc_req";
    case 3:  return "reassoc_resp";
    case 4:  return "probe_req";
    case 5:  return "probe_resp";
    case 8:  return "beacon";
    case 10: return "disassoc";
    case 11: return "auth";
    case 12: return "deauth";
    default: return "mgmt_other";
  }
}

static bool IRAM_ATTR extractSsidFromMgmtBody(const uint8_t* body, int len,
                                     char* outSsid, size_t outLen) {
  if (!body || len <= 0 || !outSsid || outLen == 0) return false;
  while (len >= 2) {
    uint8_t id = body[0], elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) {
      size_t n = (elen < (outLen - 1)) ? elen : (outLen - 1);
      memcpy(outSsid, body + 2, n);
      outSsid[n] = '\0';
      return true;
    }
    body += elen + 2; len -= elen + 2;
  }
  return false;
}

// Returns:
//   1  = wildcard SSID IE found (tag 0, length 0)  → Flock-style probe
//   0  = SSID IE found, non-zero length            → directed probe, not ours
//  -1  = no SSID IE found at all                   → caller should retry with
//                                                    FCS-stripped length, then bail
static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped) return;

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

  if (rssi < RSSI_MIN) return;

  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver

  uint8_t fc0       = hdr->frame_ctrl & 0xFF;
  uint8_t ftype     = (fc0 >> 2) & 0x03;
  uint8_t subtype   = (fc0 >> 4) & 0x0F;
  const char* fsub  = frameSubtypeStr(type, ftype, subtype);

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
    bool emitted = false;
    if (type == WIFI_PKT_MGMT) {
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        int sigLen  = (int)pkt->rx_ctrl.sig_len;
        int bodyLen = sigLen - (int)sizeof(wifi_ieee80211_mac_hdr_t);
        const uint8_t* body = pkt->payload + sizeof(wifi_ieee80211_mac_hdr_t);
        int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
        // FCS-trailer retry: only when the first parse found no SSID IE AT
        // ALL (-1). A found-but-nonzero (0) means a legit directed probe, so do
        // not retry, which would mis-classify.
        if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
        if (r == 1) {
          enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, nullptr, rssi, ch,
                       nullptr, "probe_req", fsub);
          emitted = true;
        } else if (r == 0) {
          // Directed probe (non-zero SSID IE), so extract and log the target SSID
          // so the analysis pipeline can identify configured backhaul networks.
          char ssid[33] = {0};
          extractSsidFromMgmtBody(body, bodyLen, ssid, sizeof(ssid));
          enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, nullptr, rssi, ch,
                       ssid[0] ? ssid : nullptr, "probe_req", fsub);
          emitted = true;
        }
      }
    }
    if (!emitted) {
      enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, nullptr, rssi, ch, nullptr, "addr2", fsub);
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
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, hdr->addr2, rssi, ch, nullptr, "addr1", fsub);
  }
#endif

#if CHECK_ADDR3
  // addr3 fallback: catches cases where addr2 is randomised but addr3
  // carries the real BSSID OUI (management frames only).
  if (type == WIFI_PKT_MGMT && matchOuiRaw(hdr->addr3) >= 0) {
    enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, nullptr, rssi, ch, nullptr, "addr3", fsub);
  }
#endif

#if ENABLE_SSID_MATCH
  if (type == WIFI_PKT_MGMT) {
    if (ftype == 0) {
      int sigLen = pkt->rx_ctrl.sig_len - 4;  // strip 4-byte FCS
      if (sigLen < (int)sizeof(wifi_ieee80211_mac_hdr_t)) return;

      const uint8_t* mgmtBody    = nullptr;
      int            mgmtBodyLen = 0;
      const char*    frameKind   = nullptr;

      if (subtype == 8 || subtype == 5) {
        // Beacon / Probe Response: fixed params = 12 bytes after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        if (sigLen > off) {
          frameKind   = (subtype == 8) ? "beacon" : "probe_resp";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      } else if (subtype == 4) {
        // Probe Request: IEs follow directly after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t);
        if (sigLen > off) {
          frameKind   = "probe_req";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      }

      if (mgmtBody && mgmtBodyLen > 0) {
        char ssid[33] = {0};
        if (extractSsidFromMgmtBody(mgmtBody, mgmtBodyLen, ssid, sizeof(ssid))) {
          if (matchSsidKeyword(ssid)) {
            enqueueAlert(ALERT_SSID, hdr->addr2, nullptr, rssi, ch, ssid, frameKind, fsub);
          }
        }
      }
    }
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

// Non-blocking: drain whatever bytes arrived since last call into the parser.
static void gpsTick() {
  if (!gpsReady) return;
  while (GPS_SERIAL.available()) gpsParser.encode(GPS_SERIAL.read());

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

  if (gpsParser.location.isValid() &&
      gpsParser.location.age() < GPS_FIX_MAX_AGE_MS) {
    gpsHasFix = true;
    gpsLat    = gpsParser.location.lat();
    gpsLng    = gpsParser.location.lng();
    if (!gpsTimeAnchored &&
        gpsParser.date.isValid() && gpsParser.time.isValid()) {
      gpsAnchorUnix   = gpsToUnix(gpsParser.date.year(), gpsParser.date.month(),
                                   gpsParser.date.day(), gpsParser.time.hour(),
                                   gpsParser.time.minute(), gpsParser.time.second());
      gpsAnchorMs     = millis();
      gpsTimeAnchored = true;
      dualPrintln("[gps] UTC time anchor set");
#if USE_SD
      sdTryNameLog();
#endif
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

static bool ntpTimeAnchored = false;

static void ntpSync() {
  String ssid, pass;
  if (!coreWifiCredsLoad(ssid, pass)) {
    dualPrintln("[bscope] no saved WiFi network – timestamping from boot");
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
    dualPrintln("[bscope] saved network not in range – timestamping from boot");
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
    dualPrintln("[bscope] WiFi join timed out – timestamping from boot");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000)) {
    ntpTimeAnchored = true;
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    dualPrintf("[bscope] time synced: %s\n", buf);
  } else {
    dualPrintln("[bscope] NTP sync failed – timestamping from boot");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void coreTimeSync() {
#if HAS_GPS
  gpsSetup();
  if (gpsProbePresent())
    dualPrintln("[bscope] GPS module present – it will master timing once it locks");
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
// SD CARD: append-only CSV event log
//
// Architecture: SPIFFS holds the deduplicated crash-safe recovery table.
// The SD log is a separate artifact, a chronological raw event stream,
// one row per detection, written before the serial rate-limit gate so
// every hit is captured regardless of ALERT_COOLDOWN_MS.
// ============================================================

#if USE_SD

#if HAS_GPS
#define SD_LOG_HEADER "timestamp_utc,mac,method,frame_subtype,rssi,channel,ssid,ap_mac,lat,lon"
#else
#define SD_LOG_HEADER "timestamp,mac,method,frame_subtype,rssi,channel,ssid,ap_mac,dist_m"
#endif

static bool sdLogNamed = false;
File        sdLog;

void sdSetup() {
  bool existed = SD.exists(SD_LOG_FILE);
  sdLog = SD.open(SD_LOG_FILE, FILE_APPEND);
  if (!sdLog) {
    dualPrintln("[sd] failed to open log file");
    fySDReady = false;
    return;
  }
  if (!existed) {
    sdLog.println(SD_LOG_HEADER);
    sdLog.flush();
    dualPrintln("[sd] created " SD_LOG_FILE " with header");
  } else {
    dualPrintln("[sd] appending to existing " SD_LOG_FILE);
  }
}

// Called once a time anchor lands, either from gpsTick() when a GPS fix carries
// date and time, or right after coreTimeSync() returns when the NTP fallback
// synced. Closes the pre-anchor log.csv and opens a canonically-named
// LOG_PREFIX-M-D-YY-N.csv, so each session lands in its own dated file. No-op,
// staying on log.csv, if time never anchors.
void sdTryNameLog() {
  if (!fySDReady || !coreTimeAnchored() || sdLogNamed) return;

  // Anchor source is runtime, not build-time: a HAS_GPS board can be NTP-anchored
  // when the module is absent. In that case gpsParser.date is never valid, so read the
  // libc clock the NTP sync set instead. coreTimeAnchored() above guarantees one
  // of the two is live before we get here.
  uint8_t mo, dy, yr;
#if HAS_GPS
  if (gpsTimeAnchored) {
    if (!gpsParser.date.isValid()) return;
    mo = gpsParser.date.month();
    dy = gpsParser.date.day();
    yr = (uint8_t)(gpsParser.date.year() % 100);
  } else
#endif
  {
    time_t now = time(nullptr);
    struct tm tmInfo;
    gmtime_r(&now, &tmInfo);
    mo = (uint8_t)(tmInfo.tm_mon + 1);
    dy = (uint8_t)tmInfo.tm_mday;
    yr = (uint8_t)((tmInfo.tm_year + 1900) % 100);
  }

  char newPath[32];
  uint8_t n = 1;
  do {
    snprintf(newPath, sizeof(newPath), "/" LOG_PREFIX "%u-%u-%02u-%u.csv", mo, dy, yr, n);
    n++;
  } while (SD.exists(newPath) && n <= 99);

  if (sdLog) { sdLog.flush(); sdLog.close(); }

  sdLog = SD.open(newPath, FILE_WRITE);
  if (!sdLog) {
    dualPrintf("[sd] failed to open named log %s – falling back to %s\n", newPath, SD_LOG_FILE);
    sdLog = SD.open(SD_LOG_FILE, FILE_APPEND);
    return;
  }
  sdLog.println(SD_LOG_HEADER);
  sdLog.flush();
  sdLogNamed = true;
  dualPrintf("[sd] log opened as %s (pre-anchor rows in %s)\n", newPath, SD_LOG_FILE);
}

void sdAppendRow(const char* mac, const char* method, const char* frameSubtype,
                         int8_t rssi, uint8_t channel, const char* ssid,
                         const char* apMac, float distM) {
  if (!fySDReady) return;

  char timestamp[22];
  coreTimestampStr(timestamp, sizeof(timestamp));

  char row[210];
  // Escape any commas in the SSID by quoting the field.
#if HAS_GPS
  snprintf(row, sizeof(row), "%s,%s,%s,%s,%d,%u,\"%s\",%s,%.6f,%.6f",
           timestamp, mac, method, frameSubtype ? frameSubtype : "",
           (int)rssi, (unsigned)channel, ssid ? ssid : "",
           (apMac && apMac[0]) ? apMac : "",
           gpsHasFix ? gpsLat : 0.0, gpsHasFix ? gpsLng : 0.0);
#else
  char distStr[12] = "";
  if (distM >= 0.0f) snprintf(distStr, sizeof(distStr), "%.1f", distM);
  snprintf(row, sizeof(row), "%s,%s,%s,%s,%d,%u,\"%s\",%s,%s",
           timestamp, mac, method, frameSubtype ? frameSubtype : "",
           (int)rssi, (unsigned)channel, ssid ? ssid : "",
           (apMac && apMac[0]) ? apMac : "", distStr);
#endif

  if (!sdLog || !sdLog.println(row)) {
    // Write failed, so close and attempt one reopen before giving up.
    if (sdLog) sdLog.close();
    sdLog = SD.open(SD_LOG_FILE, FILE_APPEND);
    if (!sdLog || !sdLog.println(row)) {
      dualPrintln("[sd] write failed, disabling SD log");
      fySDReady = false;
      return;
    }
  }
  sdLog.flush();
}
#endif // USE_SD

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
// RSSI -> DISTANCE  (log-distance path loss model, non-GPS boards only)
// ============================================================
//
// Only meaningful for addr2/wildcard-probe hits. addr1 RSSI reflects
// AP->scanner path loss, not target->scanner, so it's never used here.

#if !HAS_GPS
static float rssiToDistanceM(int8_t rssi) {
  return powf(10.0f, ((float)RSSI_AT_1M - (float)rssi) / (10.0f * PATH_LOSS_N));
}
#endif

// Defined further down alongside the rest of the notification module,
// forward-declared here since coreHandleAlert() calls it directly instead
// of leaving detection feedback to the board.
static void notifyDetection(bool chirpWorthy);

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
  if (e.type == ALERT_OUI_ADDR1) macToStr(e.mac2, apMacStr, sizeof(apMacStr));

#if HAS_GPS
  float distM = -1.0f;
#else
  float distM = (e.type != ALERT_OUI_ADDR1) ? rssiToDistanceM(e.rssi) : -1.0f;
#endif

  bool chirpWorthy = false;
  int idx = fyAddDetection(r.macStr, method, e.rssi, e.channel,
                            (e.type == ALERT_SSID) ? e.ssid : nullptr,
                            &chirpWorthy);

#if USE_SD
  sdAppendRow(r.macStr, method, e.frameSubtype, e.rssi, e.channel,
              (e.type == ALERT_SSID) ? e.ssid : "", apMacStr, distM);
#endif

  // Refresh unconditionally, since a device counts as active even when the
  // dedupe gate below rate-limits its serial/JSON/display output.
  fyLastTargetSeen = millis();

  r.detIdx      = idx;
  r.count       = (idx >= 0) ? (uint16_t)fyDet[idx].count : 0;
  r.chirpWorthy = chirpWorthy;
  r.rssi        = e.rssi;
  r.channel     = e.channel;
  r.distM       = distM;
  r.type        = e.type;
  strlcpy(r.frameKind, e.frameKind, sizeof(r.frameKind));
  ouiFromMac(e.mac, r.oui, sizeof(r.oui));

  if (shouldSuppressDuplicate(r.macStr)) {
    r.suppressed = true;
    return r;
  }
  r.suppressed = false;

  if (e.type == ALERT_SSID) {
    dualPrintf("[bscope] DETECT-SSID type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u count=%d\n",
               e.frameKind, r.macStr, e.ssid, e.rssi, e.channel, (int)r.count);
  } else {
    dualPrintf("[bscope] DETECT-OUI mac=%s oui=%s rssi=%d ch=%u addr=%s count=%d\n",
               r.macStr, r.oui, e.rssi, e.channel,
               e.frameKind[0] ? e.frameKind : "addr2", (int)r.count);
  }

  emitDetectionJSON(r.macStr, method, e.rssi, e.channel,
                    (e.type == ALERT_SSID) ? e.ssid : "", apMacStr);
  notifyDetection(r.chirpWorthy);
  return r;
}

// ============================================================
// NOTIFICATIONS: LED (NeoPixel) and buzzer
// ============================================================

static volatile unsigned long ledOffAt = 0;

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

static void ledFlash(uint8_t r, uint8_t g, uint8_t b, unsigned ms) {
#if USE_LED
  ledSet(r, g, b);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;  // avoid the "off" sentinel
#endif
}

static void ledTick() {
#if USE_LED
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(0, 0, 0);
    ledOffAt = 0;
  }
#endif
}

// Two fast ascending beeps, played on the FIRST sighting of a MAC.
static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

// Two monotone beeps, a periodic heartbeat while at least one target is still
// in range (last seen within HB_DEVICE_ACTIVE_MS).
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
void corePlayDetectChirp()   { newDetectChirp(); }
void corePlayStartupJingle() { startupBeep(); }

// Heartbeat audio state: last time the heartbeat beep-pair was played. When
// nothing has been seen for HB_DEVICE_ACTIVE_MS the heartbeat stops until
// the next new detection.
static unsigned long fyLastHeartbeatAt = 0;

static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;                           // never seen one
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;    // gone silent
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;   // too soon
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

// Called from coreHandleAlert() for every non-suppressed detection.
//   - NEW MAC  → two fast ascending beeps (clearly distinct sound)
//   - REPEAT   → silent, since the heartbeat tick covers continued presence
// LED flashes on every emitted detection either way.
static void notifyDetection(bool chirpWorthy) {
  if (chirpWorthy) {
    if (coreBuzzerEnabled) newDetectChirp();   // Alerts menu: buzzer mute
    // Reset the heartbeat phase so the first follow-up beep lands
    // HB_BEEP_INTERVAL_MS after the initial chirp, not mid-window.
    fyLastHeartbeatAt = millis();
#if USE_LED
    if (coreLedEnabled) ledFlash(LED_COLOR_NEW_R, LED_COLOR_NEW_G, LED_COLOR_NEW_B, LED_FLASH_MS);
#endif
  } else {
#if USE_LED
    if (coreLedEnabled) ledFlash(LED_COLOR_R, LED_COLOR_G, LED_COLOR_B, LED_FLASH_MS);
#endif
  }
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
  heartbeatTick();  // audible beep-pair while a target is still in range
  ledTick();        // turn off LED after LED_FLASH_MS
}

void coreLedBlink(uint8_t r, uint8_t g, uint8_t b,
                  uint8_t count, unsigned on_ms, unsigned off_ms) {
#if USE_LED
  for (uint8_t i = 0; i < count; i++) {
    ledSet(r, g, b);  delay(on_ms);
    ledSet(0, 0, 0);  delay(off_ms);
  }
  ledOffAt = 0;   // fully off; clear any pending timed flash so ledTick() won't relight
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
      case NAV_UP:      // advance forward through the screens (1 -> 2 -> ... -> 7)
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
    const int n = (coreCurrentScreen == SCREEN_SCAN_MODES ||
                   coreCurrentScreen == SCREEN_TARGETS) ? 3 : 2;
    switch (ev) {
      case NAV_UP:   coreMenuSel = (coreMenuSel + n - 1) % n; return NAV_ACT_REDRAW;
      case NAV_DOWN: coreMenuSel = (coreMenuSel + 1) % n;     return NAV_ACT_REDRAW;
      case NAV_BACK: coreMenuState = MENU_NONE;               return NAV_ACT_REDRAW;
      case NAV_SELECT:
        if (coreCurrentScreen == SCREEN_ALERTS) {
          // Toggle the highlighted gate in place and stay in the list so both
          // rows can be flipped before long-Back pops out (unlike the act-and-
          // close Scan Mode / Config menus).
          if (coreMenuSel == 0) coreBuzzerEnabled = !coreBuzzerEnabled;   // Buzzer
          else                  coreLedEnabled    = !coreLedEnabled;      // LED
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
void coreWifiSnifferStart() {
  // esp_event_loop_create_default() is a no-op if the loop already exists.
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();

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
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
  esp_wifi_set_promiscuous(true);
  sniffingStopped = false;
}
