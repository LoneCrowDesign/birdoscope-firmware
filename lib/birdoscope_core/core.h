// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Shared detection engine for all birdoscope board targets. PlatformIO links
// everything under lib/ into every env, so each env compiles this once against
// its include/boards/<name>/board_config.h, which the env's -I flag reaches.
// Board peripherals (display, buzzer/LED, buttons) and setup()/loop() live in
// the display family's src/main_*.cpp.
#pragma once

#include <Arduino.h>
#include "esp_wifi.h"

// Firmware version. Bump on release.
#ifndef BIRDOSCOPE_VERSION
#define BIRDOSCOPE_VERSION "0.1.0"
#endif

// ============================================================
// BUILD IDENTITY
//
// The commit this binary came from. Someone bumps the version above by hand,
// so it says nothing about what a device runs between releases.
// tools/git_version.py stamps these, from [common] in platformio.ini. A build
// with no git reports "unknown". See tools/git_version.py for the rev format.
// ============================================================

#ifndef BIRDOSCOPE_GIT_REV
#define BIRDOSCOPE_GIT_REV "unknown"
#endif
#ifndef BIRDOSCOPE_GIT_DATE
#define BIRDOSCOPE_GIT_DATE "unknown"
#endif
#ifndef BIRDOSCOPE_BUILD_DATE
#define BIRDOSCOPE_BUILD_DATE "unknown"
#endif
#ifndef BIRDOSCOPE_BUILD_TS
#define BIRDOSCOPE_BUILD_TS "unknown"
#endif

// One line naming version, commit and build time. Static storage.
const char* coreBuildIdentity();

// Just the git rev, for a display row with no space for the rest.
const char* coreBuildRev();

// ============================================================
// DETECTION TALLIES
//
// Matching frames, split by whether the target itself transmitted (direct) or
// an AP answered its probe (indirect, the addr1 hits that read `dst:via AP`).
// They count frames, not chirps or devices. coreHandleAlert() increments them
// ahead of the repeat-suppression gate and regardless of any alert setting, so
// they show whether the detection path is alive on a drive with few alerts.
// Infra matches do not count. Both saturate at 0xFFFF. See
// docs/detection_methods.md.
// ============================================================

extern uint16_t coreDirectFrames;
extern uint16_t coreIndirectFrames;

// Cameras observed each way, in devices, spec C1-C3. A camera seen both ways
// counts in both, so the sum can exceed fyDetCount. Both walk the table, so
// call them once per display refresh.
uint16_t coreDirectDeviceCount();
uint16_t coreIndirectDeviceCount();

// ============================================================
// RAW SNIFFER COUNTERS
//
// These count non-matching traffic too, so they tell a dead radio from a quiet
// one where the tallies above cannot. coreSeenFrames counts everything the
// driver hands up, and coreCandidateFrames counts what passes the type, length
// and RSSI_MIN guards. Spec S2. The promiscuous callback writes them and loop()
// reads them, hence volatile. Both wrap.
// ============================================================

extern volatile uint32_t coreSeenFrames;
extern volatile uint32_t coreCandidateFrames;

// Management frames by subtype, indexed by the 802.11 subtype nibble (4 is a
// probe request, 5 a probe response, 8 a beacon). `Seen` counts every one the
// driver delivered, ahead of the RSSI gate, and `Matched` counts those whose
// addr2 holds a target OUI. Every other counter in this firmware records a
// match, so only these two tell a subtype that never arrives from one that
// arrives unmatched.
#define CORE_MGMT_SUBTYPE_COUNT 16
extern volatile uint32_t coreMgmtSeen[CORE_MGMT_SUBTYPE_COUNT];
extern volatile uint32_t coreMgmtMatched[CORE_MGMT_SUBTYPE_COUNT];

// Queued frames lost before the log, because the alert queue was full or never
// allocated. A non-zero count means the capture is short by that many frames.
extern volatile uint32_t coreQueueDrops;

// Load baselines for sizing the roost queue and flush policy. See
// docs/roost_logging.md, "Capture Performance". coreQueueDepthMax is the
// deepest the alert ring has reached. The roost writer reports write and flush
// figures through roostSessionStats().
extern volatile uint8_t  coreQueueDepthMax;
uint8_t coreAlertQueueSize();   // denominator for coreQueueDepthMax

// Each main_*.cpp includes board_config.h before this header. These guards
// keep core.h parseable on its own, for IDE tooling.
#ifndef USE_SD
#define USE_SD 0
#endif
#ifndef HAS_GPS
#define HAS_GPS 0
#endif
#ifndef HAS_BUTTONS
#define HAS_BUTTONS 0
#endif
// BLE capture through NimBLE, which needs a Bluetooth 5 controller for
// extended advertising. A board without one sets this to 0.
#ifndef HAS_BLE_SCAN
#define HAS_BLE_SCAN 1
#endif
#if ROOST_CAP_BLE && !HAS_BLE_SCAN
#error "ROOST_CAP_BLE needs HAS_BLE_SCAN"
#endif
// Open-file limit passed to SD.begin(). The roost writer holds every declared
// record file open and the manifest snapshot needs one more, which
// roost_session.cpp checks at build time.
#ifndef SD_MAX_OPEN_FILES
#define SD_MAX_OPEN_FILES 8
#endif

// The fleet logging contract, after the board's capability and component
// declarations. Deliberately unguarded, so a board that has not declared them
// fails to build here.
#include "roost_registry.h"
// The fleet's buffered session writer, header-only and platform-free.
// roost_session.cpp supplies the Arduino SD backend.
#include "roost_sdlog.h"
// The fleet's manifest renderer. It produces bytes and this device decides
// where they go. Devices never spell their own key names, timestamp formats or
// counters.
#include "roost_manifest.h"
// The clock anchor arithmetic. See roost_time.h for the pre-anchor case.
#include "roost_time.h"
// The fleet's host-tested 802.11 management-body walker. Boards parse frame
// bodies through it, never with their own pointer arithmetic.
#include "roost_ie.h"
// The channel-to-band derivation. `band` is a computed column, and every
// device computes it here. See spec 3.1.
#include "roost_channel.h"
// The `list` and `map` value encodings. A registry key's declared type fixes
// its rendering, so the device builds values here.
#include "roost_value.h"
// GPIO for the BOOT button the Admin-mode trigger reads. GPIO0 on every
// current board. Override per board.
#ifndef BOOT_BTN_PIN
#define BOOT_BTN_PIN 0
#endif

// ============================================================
// ALERT TYPES
//
// The promiscuous callback's queue and the board code reading
// coreHandleAlert()'s result share these.
// ============================================================

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,   // needs ENABLE_SSID_MATCH=1
  ALERT_WILDCARD_PROBE  = 4,
  // A probe request from a target OUI with a non-empty SSID. Direct, like every
  // type but ALERT_OUI_ADDR1. Separate from ALERT_OUI_ADDR2 so the log keeps
  // the probed name.
  ALERT_DIRECTED_PROBE  = 5,
  // A frame that matched no target, captured only while an operator survey
  // window is open. It reaches the log and nothing else, with no detection
  // table entry, tally, display or notification. See coreSurveyStart().
  ALERT_SURVEY          = 6,
} AlertType;

// Frame facts recorded by position, never by role. A role name varies per
// frame, so it cannot be a column, and the pipeline derives roles from type
// and subtype. The promiscuous callback fills this while the frame still
// exists.
typedef struct {
  uint8_t  addr1[6], addr2[6], addr3[6];
  uint16_t seq;
  uint16_t fcFlags;
  uint16_t frameLen;
  char     bbFormat[8];   // a roost bb_format value, or empty
} FrameMeta;

typedef struct {
  AlertType type;
  uint8_t   mac[6];       // the matched device
  uint8_t   addr1[6], addr2[6], addr3[6];
  uint16_t  seq;
  uint16_t  fcFlags;
  uint16_t  frameLen;
  // The frame's receive time. loop() drains the queue and can fall arbitrarily
  // far behind under load.
  uint32_t  uptimeMs;
  int8_t    rssi;
  uint8_t   channel;
  char      bbFormat[8];
  // The walker's whole result. An SSID is arbitrary octets, so a bare string
  // loses its length and whether the element was present. See RoostSsid in
  // roost_ie.h.
  RoostSsid ssid;
  char      frameKind[12];
  char      frameSubtype[16];
} AlertEntry;

// One GPS fix, flattened out of TinyGPS++ so the session writer needs no
// parser. Each `has*` is false when the module did not report the field, and
// the writer then leaves its column empty.
typedef struct {
  bool   valid;
  double lat, lon;
  float  altM, speedMps, courseDeg, hdop;
  uint8_t sats;
  uint32_t ageMs;
  bool   hasAlt, hasSpeed, hasCourse, hasHdop, hasSats;
  const char* source;    // a roost position_source value
  const char* fixType;   // a roost fix_type value
} CoreGpsFix;
void coreGpsFix(CoreGpsFix* out);

// Clock anchor triple for the manifest. Returns a roost clock_source value and
// fills the pair that makes every pre-anchor row retroactively placeable as
// anchor_unix + (uptime_ms - anchor_uptime_ms).
const char* coreClockAnchor(uint32_t* anchorUnix, uint32_t* anchorUptimeMs);
void coreUnixToIso(uint32_t unix, char* buf, size_t len);
// ISO-8601 for a row observed at `uptimeMs`. False when the clock has never
// anchored, and the caller then leaves the column empty.
bool coreTimestampAt(uint32_t uptimeMs, char* buf, size_t len);

// Session identity and provenance for the manifest.
// Fills buf with the next free /bscope-TAG-YYMMDD-N. False when the day's 99
// names are all taken, and the session then keeps its boot name.
bool     coreSessionDirName(char* buf, size_t len);
uint32_t coreSessionSequence();
uint32_t coreBootCount();
uint32_t coreOuiTableHash();     // captures either side of a table change are not comparable
const char* coreDeviceSerial();
// The last four hex digits of the serial, naming the unit in a session
// directory name. Empty where the hardware has no serial. Spec 6.2.
const char* coreDeviceTag();
const char* coreOwnMac();
const char* coreCountryCode();
// Both render a registry `list` for config_change and return false when the
// value did not fit. The caller must not treat false as an empty value.
bool coreChannelListRoost(char* buf, size_t len);
bool coreVendorMaskStr(char* buf, size_t len);
void coreChannelListJson(char* buf, size_t len);

// Result of coreHandleAlert(), with everything a board needs to update its
// display and fire its own LED, buzzer or chirp feedback. Core knows nothing of
// those peripherals. Boards gate display and feedback on !suppressed.
// A rate-limited hit still fills detIdx, count, chirpWorthy, macStr, oui and
// distM. A survey or infra row returns suppressed with detIdx -1.
typedef struct {
  bool      suppressed;
  int       detIdx;
  uint16_t  count;
  bool      chirpWorthy;   // true for brand-new MACs or REDISCOVER_MS-silent rediscoveries
  char      macStr[18];
  char      oui[9];
  int8_t    rssi;
  uint8_t   channel;
  float     distM;         // RSSI-distance estimate in metres, -1 for addr1 hits (see coreRssiToDistanceM)
  int8_t    vendor;        // matching Vendor, or -1 when the OUI is not a target (SSID / wildcard-probe hits)
  AlertType type;
  char      frameKind[12];
} CoreAlertResult;

// ============================================================
// OUTPUT
// ============================================================

void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void dualPrintln(const char* str);

// ============================================================
// RADIO MODE
//
// Which radio captures. The modes are mutually exclusive, and the menu row
// index is the enum value. It selects which stack is up, so the RX path never
// reads it. Assign it only through coreSetRadioMode(). See design_spec.md
// "Radio mode".
// ============================================================

typedef enum : uint8_t {
  RADIO_MODE_WIFI = 0,   // 802.11 promiscuous capture
  RADIO_MODE_BLE  = 1,   // BLE scan
  RADIO_MODE_COUNT
} RadioMode;

extern RadioMode coreRadioMode;

// ============================================================
// BLE MATCHING
//
// Manufacturer data first, then the OUI table on public addresses only. See
// spec M5 to M9.
// ============================================================

#define BLE_MATCH_ID_MAX 20

typedef struct {
  int8_t      vendor;     // Vendor, or -1 when nothing matched
  bool        accessory;  // proves target equipment present, not a camera [M3]
  bool        infra;      // OUI_CLASS_INFRA hit, which never alerts [M8]
  const char* method;     // roost detection_method, or "unmatched"
  char        id[BLE_MATCH_ID_MAX];   // rule-supplied serial, else empty
} BleMatch;

// True when the advertisement matched a target. `addrType` is the HCI value,
// `payload` the raw AD data. Fills `out` whether or not it matched.
bool coreBleMatch(const uint8_t* mac, uint8_t addrType,
                  const uint8_t* payload, size_t payloadLen, BleMatch* out);

// Advertisements that matched a target, for the panel and `status`. Excludes
// infra hits, spec M8.
extern volatile uint32_t coreBleMatched;

// One queued ble_obs row. The NimBLE host task queues it and coreBleDrain()
// writes it. The string fields point at static registry spellings. Spec B1 to
// B4.
#define BLE_ADV_MAX 255

typedef struct {
  uint32_t    uptimeMs;
  uint8_t     mac[6];
  uint8_t     addrType;      // HCI value
  int8_t      rssi;
  int8_t      txPower;
  bool        hasTxPower;
  bool        extended;      // extended advertising, which alone carries `sid`
  uint8_t     sid;
  const char* method;        // roost detection_method
  const char* pduType;       // roost ble_pdu_type
  const char* phyPrimary;    // roost ble_phy, or "" when unreported
  const char* phySecondary;
  bool        target;        // a primary or accessory match, which the table holds
  int8_t      vendor;        // Vendor, or -1
  bool        accessory;
  char        id[BLE_MATCH_ID_MAX];   // rule-supplied serial, else empty
  uint16_t    payloadLen;
  uint8_t     payload[BLE_ADV_MAX];
} BleObsEntry;

// BLE rows lost before the log, to a full ring or an advertisement longer than
// BLE_ADV_MAX. The manifest adds them to coreQueueDrops as observations
// dropped.
extern volatile uint32_t coreBleQueueDrops;

// Radio buffers that failed to allocate at a radio start. Each failure leaves
// that radio's matches counting as drops.
extern volatile uint32_t coreRadioAllocFails;

// Deepest the BLE ring has been, the BLE counterpart of coreQueueDepthMax.
extern volatile uint8_t coreBleQueueDepthMax;
uint8_t coreBleQueueSize();   // denominator for coreBleQueueDepthMax

// Writes every queued BLE row and feeds target rows to the BLE detection table.
// Returns true when the table changed, so the board redraws. Boards call it
// from loop().
bool coreBleDrain();

// BLE detection table, display-only like fyDet [C5]. Holds target matches,
// keyed by the rule's serial where it supplies one, else by MAC, so a rotating
// address with a serial stays one device. It lasts the session, survives radio
// switches and never persists. Spec B4.
#ifndef MAX_BLE_DETECTIONS
#define MAX_BLE_DETECTIONS 64
#endif

typedef struct {
  char     key[18];      // serial, or the MAC string
  char     mac[18];      // the most recent advertising address
  int8_t   vendor;
  bool     accessory;
  int8_t   rssi;
  uint16_t count;
  uint32_t firstSeen;
  uint32_t lastSeen;
} BleDetection;

extern BleDetection coreBleDet[MAX_BLE_DETECTIONS];
extern uint16_t     coreBleDetCount;
extern uint16_t     coreBleDetMissed;   // devices refused by a full table
extern int16_t      coreBleDetLast;     // most recently updated index, or -1

// Per-MAC window on infra rows, both radios, spec M8.
uint32_t coreInfraDedupeMs();

// Frames or advertisements per second over the last sampling window, for spec
// S2. Each resamples at most once a second, so the rate always covers at least
// a second.
uint16_t coreSeenRate();
uint16_t coreBleRate();

// BLE scan counters. The NimBLE host task writes them and loop() reads them.
// `coreBlePhy1M` and `coreBlePhyCoded` size the coded-PHY share, which decides
// whether a PHY control is worth offering. `status` reports them, per S1.
extern volatile uint32_t coreBleReports;
extern volatile uint32_t coreBlePhy1M;
extern volatile uint32_t coreBlePhyCoded;

// Stops the outgoing radio before starting the incoming one, and writes the
// config_change row. Re-applying the current mode does nothing, and a board
// without HAS_BLE_SCAN refuses BLE. Call it only outside Admin mode.
void coreSetRadioMode(RadioMode mode);

// Lowercase slug matching the roost radio_mode vocabulary, or "unknown".
const char* radioModeName(RadioMode mode);

// ============================================================
// TARGET VENDORS
//
// The OUI table is one flat list tagged by vendor, so a match reports which
// vendor hit. `coreVendorMask` selects which vendors the matcher accepts, one
// bit per Vendor, and the Targets menu switches it. A single aligned 16-bit
// store is atomic on Xtensa, so the mask can change live without stopping the
// sniffer. VENDOR_COUNT must stay <= 16, since coreVendorMask is two bytes.
//
// Motorola covers Motorola Solutions, Avigilon Alta and WatchGuard Video, one
// corporate family. Analysis recovers the product line from the OUI, so the
// 21-character panel shows one Motorola slot for all three.
//
// Infra tags backhaul hardware that these deployments use and that is common
// everywhere else. Every Infra entry is OUI_CLASS_INFRA, which logs a row and
// never alerts. See spec M2 and M8.
// ============================================================

typedef enum : uint8_t {
  VENDOR_FLOCK    = 0,
  VENDOR_AXON     = 1,
  VENDOR_AXIS     = 2,
  VENDOR_UTILITY  = 3,
  VENDOR_MOTOROLA = 4,
  VENDOR_VERKADA  = 5,
  VENDOR_GENETEC  = 6,
  VENDOR_DALLY    = 7,   // Digital Ally
  VENDOR_INFRA    = 8,
  VENDOR_COUNT
} Vendor;

#define VENDOR_MASK_ALL ((uint16_t)((1u << VENDOR_COUNT) - 1))

// What a match proves, spec M1 to M3. The table errs wide, so each entry
// states how far to trust a match on it.
typedef enum : uint8_t {
  OUI_CLASS_PRIMARY   = 0,  // the target's own product
  OUI_CLASS_ACCESSORY = 1,  // target equipment present, not a camera
  OUI_CLASS_INFRA     = 2,  // corroborating only, common hardware
} OuiClass;

extern volatile uint16_t coreVendorMask;

// Sets the active vendor mask. Use it in place of assigning coreVendorMask,
// since it also recomputes whether any active target is locally administered,
// which controls the randomised-MAC fast path in matchOuiRaw(), and writes the
// config_change row.
void coreSetVendorMask(uint16_t mask);

// Active target set as the Targets menu's list index. Rows are parent
// categories, so one covers every subsidiary tagged to it. Returns -1 for any
// other mask, and the menu then shows no active marker. Vendors with no row of
// their own are reachable only through All, the boot default.
#define TARGET_ROW_COUNT 4

int coreTargetIndex();

// Lowercase label for a Targets menu row.
const char* coreTargetRowName(int row);

// Lowercase vendor slug, or "unknown". Stable strings, safe to log.
const char* vendorName(uint8_t vendor);

// ============================================================
// DISTANCE ESTIMATE
//
// A log-distance path-loss model with two user controls, one per term.
//
//   d = 10 ^ ((RSSI_1m - RSSI_measured) / (10 * n))
//
// Environment Density sets n, how fast signal fades with distance.
// coreRssiAt1mDbm sets the reference level it fades from, the reading a metre
// from a target, so an operator calibrates it by walking up to one and reading
// the number off the screen. Multipath alone swings instantaneous RSSI by 6-10
// dB, so accuracy stays coarse and the controls remove only systematic bias.
// docs/distance_estimation.md has the calibration procedure and ranges.
// ============================================================

// Order must match DENSITY_N in core.cpp.
typedef enum : uint8_t {
  DENSITY_LOW = 0,      // open ground, near line of sight
  DENSITY_MEDIUM,       // mixed suburban, scattered obstructions
  DENSITY_HIGH,         // dense urban, heavy obstruction
  DENSITY_COUNT
} EnvDensity;

#define RSSI_AT_1M_MIN -85
#define RSSI_AT_1M_MAX -20

extern volatile uint8_t coreEnvDensity;    // an EnvDensity, set through coreSetEnvDensity()
extern volatile int8_t  coreRssiAt1mDbm;   // expected RSSI at 1m, set through coreSetRssiAt1mDbm()

void coreSetEnvDensity(uint8_t density);   // ignores an out-of-range value
void coreSetRssiAt1mDbm(int8_t dbm);       // clamps to [RSSI_AT_1M_MIN, RSSI_AT_1M_MAX]

// Steps the reference, clamped the same way. Up reads farther.
void coreNudgeRssiAt1mDbm(int8_t db);

float corePathLossExponent();              // n for the active Density
const char* envDensityName(uint8_t density);   // "low" / "medium" / "high", safe to log

// Metres. Callers skip ALERT_OUI_ADDR1 hits, whose RSSI is the AP-to-scanner
// path. coreHandleAlert() already does, reporting -1 as CoreAlertResult::distM.
float coreRssiToDistanceM(int8_t rssi);

// Most recent non-suppressed detection's RSSI, or 0 before the first one.
int8_t coreLastDetectionRssi();

// coreSettingsSave() persists `{"density":N,"rssi_1m":N,"prox_m":N}`. Call
// coreSettingsLoad() from setup() once SPIFFS has mounted. Load returns false
// when the file is absent or unparseable, and the defaults stand.
bool coreSettingsLoad();
bool coreSettingsSave();

// ============================================================
// MAC / OUI HELPERS
// ============================================================

void macToStr(const uint8_t* mac, char* buf, size_t len);
void ouiFromMac(const uint8_t* mac, char* buf, size_t len);

// Validates the target table and prints a boot-time summary of what this
// firmware is hunting. Call once from setup().
void precompileOuis();

// Returns the matching Vendor, or -1 for no match. Honours coreVendorMask and
// the per-entry prefix length, so MA-M (28-bit) registrations match on the high
// nibble of byte 4.
int  matchOuiRaw(const uint8_t* mac);
bool isMulticast(const uint8_t* mac);

// Returns the first currently active entry in the target OUI table (3 bytes).
// The `inject` command builds a synthetic target MAC from it.
void coreGetFirstTargetOui(uint8_t out[3]);

// ============================================================
// CHANNEL HOPPING
// ============================================================

extern uint8_t currentChannel;
void applyInitialChannel();
void updateChannelMode();
const char* channelModeName();
uint16_t channelFreqMhz(uint8_t ch);

// Runtime scan mode. `CHANNEL_MODE` is the board's build-time default. The Scan
// Mode menu switches the mode live in RAM, and a reboot resets it. Custom and
// Full hop their board-defined channel lists. Single locks to
// `coreSingleChannel`. coreNavApply() applies the mode change, and boards only
// read these for rendering.
extern uint8_t coreSingleChannel;   // channel Single mode locks to (display + picker)
int coreScanModeIndex();            // active mode as a menu list index, 0=Custom, 1=Full, 2=Single

// ============================================================
// PROMISCUOUS CAPTURE
//
// wifiSniffer() fills the internal alert queue, and the board loop drains it
// through coreDequeueAlert().
// ============================================================

void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type);
bool coreDequeueAlert(AlertEntry& out);
extern volatile bool sniffingStopped;

// The board `inject` command calls this to push a synthetic alert through the
// real queue. `fm` may be null, for an alert with no frame behind it.
void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac,
                             const FrameMeta* fm, int8_t rssi, uint8_t ch,
                             const RoostSsid* ssid, const char* kind,
                             const char* fsubtype);

// ============================================================
// DETECTION TABLE + SD LOG + JSON EMIT
//
// Boards call coreHandleAlert() once per dequeued AlertEntry, then use the
// result for display and feedback.
// ============================================================

CoreAlertResult coreHandleAlert(const AlertEntry& e);

// ============================================================
// OPERATOR SURVEY WINDOW
//
// A bounded interval during which the firmware logs every frame the radio
// delivers, matched or not.
//
// Rows go to wifi_obs, or ble_obs under BLE capture, with
// detection_method=operator_survey, at most one per MAC per SURVEY_DEDUPE_MS.
// 802.11 frames still pass the frame-type and RSSI_MIN gates. No radio setting
// changes for the window's duration.
//
// Spec O1-O7 [D9] govern this. A board calls coreSurveyStart() on an operator
// mark and coreSurveyTick() once per loop().
// ============================================================

// Duration options in seconds, for a settings screen. Index 0 is the default.
#define SURVEY_OPTION_COUNT 3
extern const uint16_t SURVEY_OPTIONS_S[SURVEY_OPTION_COUNT];
extern volatile uint16_t coreSurveySecs;

// Opens the window, or extends it to a full duration if one is already open.
void coreSurveyStart();

// Closes the window once its duration has elapsed and reports what it caught.
// Boards call this once per loop(), and it does nothing while no window is
// open.
void coreSurveyTick();

bool coreSurveyActive();

// Window accounting. coreSurveyStart() zeroes these when a window opens and
// coreSurveyTick() reports them on close, spec O7. `Rows` counts survey frames
// offered to the queue and `Suppressed` counts frames the per-MAC limit held
// back. The close report takes evictions from each radio's survey limit.
extern volatile uint32_t coreSurveyRows;
extern volatile uint32_t coreSurveySuppressed;

// Milliseconds left in the open window, 0 when none is open.
uint32_t coreSurveyRemainingMs();

extern int  fyDetCount;
extern unsigned long fyLastTargetSeen;

// Distinct new MACs the detection table refused once it reached
// MAX_DETECTIONS. The table never evicts, so a full table holds fyDetCount
// still and this count shows the devices it missed. Repeat hits on known MACs
// still update. On a USE_SD board coreHandleAlert() writes every wifi_obs row
// regardless, so the capture loses nothing.
extern uint16_t fyDroppedNew;

// ============================================================
// SPIFFS SESSION PERSISTENCE
// ============================================================

void fySaveSession();
void fyPromotePrevSession();
extern bool fySpiffsReady;

// ============================================================
// SD LOG
// ============================================================

#if USE_SD
#include <SD.h>
extern bool fySDReady;
extern File sdLog;
#endif

#if HAS_GPS
extern bool   gpsHasFix;
extern double gpsLat;
extern double gpsLng;

// GPS parser health counters for the GPS detail screen, matching the [gps]
// serial diagnostic line. Good and bad checksum counts, fix-carrying sentences,
// and satellites in view, with sats -1 until the module reports any.
void coreGpsStats(unsigned long& good, unsigned long& bad,
                  unsigned long& fixSent, int& sats);
#endif

// ============================================================
// TIME SOURCE
//
// A runtime priority chain of GPS once a module locks, then NTP over WiFi if
// station credentials exist, then millis() since boot.
//
// coreTimeSync() runs once in setup() and blocks, because it must settle before
// the promiscuous radio comes up, the only window in which a WiFi STA join is
// safe. GPS has no lock this early, so NTP bridges the pre-lock window.
// coreTick(), which loop() calls every pass, drains the GPS UART and sets the
// GPS anchor the moment a fix lands, and timestamps prefer GPS over NTP from
// then on. A board with no module stays on NTP or millis. A boot probe tells
// checksum-valid NMEA from a floating UART to report whether a module is
// present.
// ============================================================

void coreTimeSync();
void coreTick();
bool coreTimeAnchored();

// ============================================================
// WIFI STATION CREDENTIALS
//
// One saved network, used only for the boot-time NTP fallback. The Admin
// SoftAP has its own identity. The web console sets the credentials, and the
// firmware keeps them on SPIFFS at WIFI_CREDS_FILE as `{"ssid","pass"}`. The
// SSID is user-controlled bytes, so a real JSON parser reads it.
//
// Load returns true only when the file holds a non-empty SSID. Save persists
// and overwrites, rejecting an empty SSID. Have is a cheap presence check.
// Clear removes the file, backing the console "wifi-forget" verb.
// ============================================================

bool coreWifiCredsHave();
bool coreWifiCredsLoad(String& ssid, String& pass);
bool coreWifiCredsSave(const char* ssid, const char* pass);
void coreWifiCredsClear();

// ============================================================
// AUTOSAVE
// ============================================================

void autosaveTick();
void printHeartbeat();

// ============================================================
// SERIAL COMMANDS
//
// Commands are brief noun/verb words. Core handles the shared verbs that
// corePrintSerialHelp() lists. Each board composes its own `status` from the
// extern state above, since the fields differ per board.
//
// Boards read lines with coreReadSerialCommand(), pass each to
// coreHandleSerialCommand() first, and handle their own verbs when it returns
// false. Commands end in a newline.
// ============================================================

void dumpCurrentSession();
void dumpSpiffsFile(const char* path);

// Non-blocking line reader. Drains Serial into an internal buffer and returns
// true once a full line arrives, with `verb` lowercased and `arg` the trimmed
// remainder, or "" when there is none. Returns false when no complete line is
// pending. Loop over it to process every buffered line.
bool coreReadSerialCommand(const char** verb, const char** arg);

// Dispatches a core-owned verb. Returns true if handled, false so the board
// can try its own.
bool coreHandleSerialCommand(const char* verb, const char* arg);

// Prints the core-handled commands as indented help lines. A board's own
// `help` prints its own verbs, then calls this.
void corePrintSerialHelp();

// ============================================================
// NOTIFICATIONS
//
// LED (NeoPixel) and buzzer feedback, gated on USE_LED and USE_BUZZER.
// coreHandleAlert() calls the detection half itself, so boards only call
// coreNotifyBoot() once in setup(), after display init, and coreNotifyTick()
// once per loop() to step the LED pulse train. Screen boards leave the in-range
// heartbeat uncalled, spec A1.
// ============================================================

void coreNotifyBoot();
void coreNotifyTick();

// Runtime alert gates, toggled live from the Alerts menu (SCREEN_ALERTS). Both
// start enabled each boot and never persist, like the runtime scan mode.
// coreBuzzerEnabled gates the new-detection and proximity chirps, and
// coreLedEnabled gates their LED flashes. The boot jingle, RGB cycle and the
// on-demand replays ignore both.
extern bool coreBuzzerEnabled;
extern bool coreLedEnabled;

// Blocking status blink for boot-time signaling, such as the SD-not-found
// indicator. Runs `count` on/off cycles of the given colour and leaves the LED
// off. Call it only at boot, since it blocks in delay(). A no-op without
// USE_LED.
void coreLedBlink(uint8_t r, uint8_t g, uint8_t b,
                  uint8_t count, unsigned on_ms, unsigned off_ms);

// On-demand replays behind the "chirp", "jingle" and "prox" verbs on the
// serial and web consoles. They block in delay(), which both consoles allow
// because each dispatches from loop(). A no-op without USE_BUZZER.
void corePlayDetectChirp();
void corePlayStartupJingle();
void corePlayProximityChirp();

// The two bird calls behind the "crow" and "hawk" verbs, playable whichever one
// BOOT_SOUND selects, spec A5. See docs/alerts.md for the tuning knobs.
void corePlayCrowCall();
void corePlayHawkCall();

// ============================================================
// PROXIMITY ALERT
//
// A second chirp when a tracked target crosses inside a range ring, since the
// new-detection chirp fires once per MAC per REDISCOVER_MS and reads no RSSI.
// The ring is in metres, so `rssi_trim` stays the one calibration for both it
// and the `dst:` readout. Each MAC keeps a smoothed RSSI and a latch that
// clears only outside PROX_HYST_PCT of the ring. A crossing plays a sound and
// writes no log row. See docs/alerts.md.
// ============================================================

// Selectable rings, in metres. 0 is off. Order is the picker's row order.
#define PROX_RING_OPTION_COUNT 5
extern const uint8_t PROX_RING_OPTIONS[PROX_RING_OPTION_COUNT];

// Active ring in metres, 0 when off. coreSettingsSave() persists it as
// `prox_m`. Set it through coreSetProxRingM(), which also clears every latch.
extern volatile uint8_t coreProxRingM;
void coreSetProxRingM(uint8_t metres);

// Index into PROX_RING_OPTIONS for the active ring, or -1 when it matches no
// row.
int coreProxRingIndex();

// ============================================================
// INPUT
//
// Plain debounced buttons under HAS_BUTTONS, and INPUT_NONE otherwise.
// BTN_PIN_1 toggles the active screen and BTN_PIN_2 triggers a manual "area of
// interest" marker. The names stay abstract because physical button placement
// varies board to board. The first call sets both pins to INPUT_PULLUP, so
// boards have no separate init to call.
// ============================================================

typedef enum { INPUT_NONE, INPUT_TOGGLE_SCREEN, INPUT_MANUAL_MARK } InputEvent;
InputEvent coreInputTick();

// ============================================================
// SEMANTIC NAV LAYER
//
// A display-independent event grammar (UP / DOWN / SELECT / BACK / MARK) that
// the screen and menu state machine consumes. coreNavTick() maps physical
// input to these events behind the board's NAV_SCHEME, so the screen logic
// never reads a button. The serial nav injector (coreInjectNav) feeds the same
// queue on any board. A 2-button board has no NAV_SCHEME and uses
// coreInputTick() instead.
//
// - 3-button map: BTN_1 short=UP / long=MARK, BTN_2 short=DOWN, BTN_3
//   short=SELECT / long=BACK.
// - 4-button map: as above, with BACK on BTN_4 short and no long press on
//   SELECT.
//
// Long BTN_1 is MARK under both schemes.
//
// Whichever button carries BACK emits NAV_BACK_HOLD when held for
// NAV_EXIT_HOLD_MS. Under the 4-button scheme two short BTN_4 presses within
// NAV_BACK_DOUBLE_MS also emit it. Only the Admin screen consumes it, so
// leaving the web portal takes a deliberate gesture.
//
// Under the 4-button scheme the hold cancels the short BACK that release would
// emit. Under the 3-button scheme BACK has already fired at NAV_LONG_PRESS_MS,
// and Admin ignores it. The double-press keeps both BACK events, which backs
// out of two nested menus on every other screen.
// ============================================================

#ifndef NAV_SCHEME_3BTN
#define NAV_SCHEME_3BTN 0
#endif
#ifndef NAV_SCHEME_4BTN
#define NAV_SCHEME_4BTN 0
#endif
#if NAV_SCHEME_3BTN && NAV_SCHEME_4BTN
#error "NAV_SCHEME_3BTN and NAV_SCHEME_4BTN are mutually exclusive"
#endif

// Physical buttons feeding coreNavTick(), and the flag the screen and menu code
// gates on. 0 means the board has no semantic nav and uses coreInputTick().
#if NAV_SCHEME_4BTN
#define NAV_BTN_COUNT 4
#elif NAV_SCHEME_3BTN
#define NAV_BTN_COUNT 3
#else
#define NAV_BTN_COUNT 0
#endif
#ifndef NAV_LONG_PRESS_MS
#define NAV_LONG_PRESS_MS 500   // hold >= this many ms = long press (BACK / MARK)
#endif
#ifndef NAV_BACK_DOUBLE_MS
#define NAV_BACK_DOUBLE_MS 600  // max gap between two Back presses = NAV_BACK_HOLD
#endif
#ifndef NAV_EXIT_HOLD_MS
#define NAV_EXIT_HOLD_MS 3000   // hold >= this many ms on Back = NAV_BACK_HOLD
#endif

typedef enum {
  NAV_NONE,
  NAV_UP,      // previous screen / menu item up
  NAV_DOWN,    // next screen / menu item down
  NAV_SELECT,  // enter menu / confirm selection
  NAV_BACK,    // exit menu (no change)
  NAV_MARK,    // manual "area of interest" marker
  NAV_BACK_HOLD,  // sustained hold or double press of Back, which leaves Admin
} NavEvent;

// Returns the next pending nav event, or NAV_NONE. Serial-injected events come
// first, then the physical buttons under a NAV_SCHEME. The first call sets up
// the pins. Poll once per loop, like coreInputTick().
NavEvent coreNavTick();

// Pushes a synthetic nav event into the queue coreNavTick() drains. The serial
// `nav` verb calls this on every board.
void coreInjectNav(NavEvent ev);

// ============================================================
// SCREENS + MENUS
//
// Core holds the screen carousel's state and each board renders
// coreCurrentScreen its own way, so every board shows the same screens.
// coreNavApply() feeds a NavEvent into the state machine and returns a
// NavAction for the board to act on. See docs/menu_ux.md.
// ============================================================

typedef enum {
  SCREEN_OVERVIEW,      // headline, detection count + channel + scanning/hit
  SCREEN_GPS,           // detail, GPS position / fix status
  SCREEN_DETECTIONS,    // detail, num detections / last detection MAC
  SCREEN_SCAN_DETAIL,   // detail, current channel, dwell, mode
  SCREEN_SCAN_MODES,    // menu, Custom Scan / Full Channel / Single
  SCREEN_TARGETS,       // menu, which vendors to match (Flock / Axon / Motorola / All)
  SCREEN_RADIO,         // menu, which radio captures (2.4GHz / BLE)
  SCREEN_ALERTS,        // menu, Buzzer mute/unmute + LED on/off (toggle in place)
  SCREEN_CONFIG,        // menu, web console On / Off (Admin entry)
  SCREEN_WIPE,          // menu, device wipe, device only or device + card
  SCREEN_COUNT,
} ScreenId;

extern ScreenId coreCurrentScreen;

// Side effects coreNavApply() asks the board to run, the parts that need
// board-specific code such as the SD row or portal start. coreNavApply()
// updates coreCurrentScreen itself.
typedef enum {
  NAV_ACT_NONE,
  NAV_ACT_REDRAW,   // screen/menu state changed, so the board redraws
  NAV_ACT_MARK,     // run the manual area-of-interest marker
  NAV_ACT_ADMIN,    // enter Admin (web portal), Config menu confirmed "On"
  NAV_ACT_WIPE,     // device wipe confirmed, board runs coreDeviceWipe()
} NavAction;

// Feeds one NavEvent into the screen/menu state machine. Updates
// coreCurrentScreen and returns the side effect, if any, for the board to run.
NavAction coreNavApply(NavEvent ev);

// False for a screen that does not apply to the active radio or board, such as
// the channel plan under BLE capture. coreStepScreen() skips these, so a hidden
// screen is unreachable.
bool coreScreenVisible(ScreenId s);
ScreenId coreStepScreen(ScreenId from, int dir);

// Drill-in state for the menu screens.
//   MENU_NONE          browsing the carousel, where Up/Down move screens
//   MENU_LIST          an option list is open, coreMenuSel = highlighted index
//   MENU_PICK_CHANNEL  Single-mode channel picker, coreMenuSel = channel dialed
//   MENU_PICK_PROX     proximity-ring picker, coreMenuSel = PROX_RING_OPTIONS index
//   MENU_CONFIRM_WIPE  device-wipe confirmation, coreWipeConfirmCount = presses
// Boards read these to render the cursor and edit state. Outside MENU_NONE the
// carousel holds still, Up/Down move the highlight and Back steps out one
// level. MENU_CONFIRM_WIPE also ignores Up/Down and the manual mark, so only
// Select and Back reach an armed wipe.
typedef enum {
  MENU_NONE, MENU_LIST, MENU_PICK_CHANNEL, MENU_PICK_PROX, MENU_CONFIRM_WIPE
} MenuState;
extern MenuState coreMenuState;
extern int       coreMenuSel;

// ============================================================
// ADMIN-MODE TRIGGER
//
// A double-press of the BOOT button enters the web portal (Admin mode). Boards
// poll coreAdminTriggerCheck() at the top of each loop() while in Detect. The
// first call sets BOOT_BTN_PIN to INPUT_PULLUP, and the check returns true once
// for each debounced pair of presses within BOOT_DOUBLE_PRESS_MS. It works at
// any time after boot, so Detect and Admin can alternate without a reboot.
// web_portal.h covers the way back to Detect. Non-blocking.
// ============================================================

#ifndef BOOT_DOUBLE_PRESS_MS
#define BOOT_DOUBLE_PRESS_MS 600   // max gap between the two presses
#endif
// A board that reaches Admin from its menus sets this to 0. The check then
// always returns false and never claims BOOT_BTN_PIN.
#ifndef BOOT_ADMIN_TRIGGER
#define BOOT_ADMIN_TRIGGER 1
#endif
bool coreAdminTriggerCheck();

// ============================================================
// WIFI SNIFFER BRING-UP
//
// The raw-IDF promiscuous capture init, covering driver init, NULL mode, start,
// channel, and the promiscuous filter and callback. coreRadioStart() calls it
// under WiFi capture, including when webPortalStop() resumes Detect.
// The Admin web portal fully deinits the driver to hand the radio to Arduino
// WiFi for the SoftAP, and this brings it back up from clean. Clears
// sniffingStopped.
// ============================================================

void coreWifiSnifferStart();

// Full teardown of the raw promiscuous driver, leaving the clean state
// coreWifiSnifferStart() expects. Sets sniffingStopped.
void coreWifiSnifferStop();

// Starts or stops whichever radio coreRadioMode selects. Boards and
// webPortalStop() call these, so resuming Detect returns to the selected radio.
void coreRadioStart();
void coreRadioStop();

// ============================================================
// DEVICE WIPE
//
// Erases everything that identifies a unit or the places it has been, so an
// owner can sell, donate or hand on a device. SCREEN_WIPE reaches it, and
// three deliberate Select presses confirm it.
//
// WIPE_DEVICE clears onboard state only, for an owner who pulls and replaces
// the card. WIPE_DEVICE_AND_CARD also empties the card's root. Each scope
// erases these.
//   RAM     the 802.11 detection table, zeroed before any storage write
//   SPIFFS  formatted whole, so a file added later needs no edit here
//   NVS     erased whole, taking boot_count and the WiFi driver's own store
//   SD      every root entry, recursively, under WIPE_DEVICE_AND_CARD
//
// The eFuse MAC and serial survive, and coreDeviceTag() derives session
// directory names from them. Copies of a session taken off the device before
// a wipe still name the unit that made them.
//
// A card delete frees FAT entries and leaves the sectors intact. Treat it as
// tidying, not sanitization, and destroy or host-format a card whose contents
// matter.
//
// The wipe records its scope in NVS first and erases NVS last, so a power cut
// mid-wipe leaves the requested scope on record. Boards call
// coreWipePending() in setup() and finish the job before the sniffer starts,
// so a device never boots into a partial wipe and keeps logging.
// ============================================================

typedef enum {
  WIPE_NONE = 0,
  WIPE_DEVICE = 1,            // RAM + SPIFFS + NVS
  WIPE_DEVICE_AND_CARD = 2,   // the above, plus the SD card root
} WipeScope;

// Runs the erase and returns. The caller shows its own completion frame and
// then calls corePowerOff(). Stops the active radio and closes the roost
// session before touching storage, so nothing rewrites a removed file. Blocks
// for as long as the card takes.
void coreDeviceWipe(WipeScope scope);

// The scope of an interrupted wipe, or WIPE_NONE. Call once from setup() after
// SPIFFS and SD mount and before coreRadioStart(), and pass any other result
// straight back to coreDeviceWipe().
WipeScope coreWipePending();

// Blanks the LED and buzzer and enters deep sleep with every wake source
// disabled, the closest these boards come to power-off without a software
// power latch. Only reset or a power cycle brings the device back, so a wipe
// always ends here.
void corePowerOff();

// The scope highlighted on SCREEN_WIPE, which the confirmation keeps, and the
// Select presses so far (0 to 3). Boards read both to draw the confirmation
// overlay.
WipeScope coreWipeSelectedScope();
extern int coreWipeConfirmCount;

// Select presses required to arm a wipe, to prevent an accidental one.
#define WIPE_CONFIRM_PRESSES 3
