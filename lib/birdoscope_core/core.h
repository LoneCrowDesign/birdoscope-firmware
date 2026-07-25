// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Shared detection engine for all birdoscope board targets. Compiled once
// per env (PlatformIO auto-links everything under lib/ to every env) against
// that board's include/boards/<name>/board_config.h, included via -I before
// this header is reachable. Board-specific peripherals (display, buzzer/LED,
// buttons) and setup()/loop() orchestration stay in each board's
// src/main_<board>.cpp.
#pragma once

#include <Arduino.h>
#include "esp_wifi.h"

// Firmware version, surfaced by the web portal's `status` command and any
// board that wants to print it. Bump on release.
#ifndef BIRDOSCOPE_VERSION
#define BIRDOSCOPE_VERSION "0.1.0"
#endif

// board_config.h must already be included before this header (both
// main_*.cpp do `#include "board_config.h"` then `#include "core.h"`).
// These guards keep core.h safe to parse standalone, for IDE tooling.
#ifndef USE_SD
#define USE_SD 0
#endif
#ifndef HAS_GPS
#define HAS_GPS 0
#endif
#ifndef HAS_LORA_MODEM
#define HAS_LORA_MODEM 0
#endif
#ifndef HAS_BUTTONS
#define HAS_BUTTONS 0
#endif
// GPIO for the BOOT button used by the Admin-mode trigger. GPIO0, the
// strapping/BOOT button, on every current board. Override per board.
#ifndef BOOT_BTN_PIN
#define BOOT_BTN_PIN 0
#endif

// ============================================================
// ALERT TYPES: shared by the promiscuous callback's queue and by board
// orchestration code that reads coreHandleAlert()'s result.
// ============================================================

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,   // only ever enqueued when ENABLE_SSID_MATCH=1
  ALERT_WILDCARD_PROBE  = 4,
} AlertType;

typedef struct {
  AlertType type;
  uint8_t   mac[6];
  uint8_t   mac2[6];    // addr2 for ALERT_OUI_ADDR1: AP that sent the probe response; zeros otherwise
  int8_t    rssi;
  uint8_t   channel;
  char      ssid[33];
  char      frameKind[12];
  char      frameSubtype[16];
} AlertEntry;

// Result of coreHandleAlert(). Carries everything a board needs to update its own
// display state and fire board-specific feedback (LED/buzzer/chirp), without
// core knowing those peripherals exist. detIdx/count/chirpWorthy/macStr/oui/
// distM are populated even when suppressed=true (rate-limited), but boards
// should gate display/feedback updates on !suppressed to match the existing
// per-board behavior (rate-limited hits never touched the display before).
typedef struct {
  bool      suppressed;
  int       detIdx;
  uint16_t  count;
  bool      chirpWorthy;   // true for brand-new MACs or REDISCOVER_MS-silent rediscoveries
  char      macStr[18];
  char      oui[9];
  int8_t    rssi;
  uint8_t   channel;
  float     distM;         // RSSI-distance estimate, -1 when HAS_GPS or not applicable (addr1 hits)
  AlertType type;
  char      frameKind[12];
} CoreAlertResult;

// ============================================================
// OUTPUT
// ============================================================

void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void dualPrintln(const char* str);

// ============================================================
// MAC / OUI HELPERS
// ============================================================

void macToStr(const uint8_t* mac, char* buf, size_t len);
void ouiFromMac(const uint8_t* mac, char* buf, size_t len);
void precompileOuis();
bool matchOuiRaw(const uint8_t* mac);
bool isMulticast(const uint8_t* mac);

// Returns the first entry in the target OUI table (3 bytes). Used to build a
// synthetic but realistic target MAC for the `inject` command.
void coreGetFirstTargetOui(uint8_t out[3]);

// ============================================================
// CHANNEL HOPPING
// ============================================================

extern uint8_t currentChannel;
void applyInitialChannel();
void updateChannelMode();
const char* channelModeName();
uint16_t channelFreqMhz(uint8_t ch);

// Runtime scan mode. `CHANNEL_MODE` is the board's build-time default, and the
// Scan Mode menu switches this live in RAM, resetting to the default on reboot.
// Custom and Full hop their board-defined channel lists. Single locks to one
// channel (`coreSingleChannel`). coreNavApply()'s menu applies the mode change,
// and boards only read these for rendering.
extern uint8_t coreSingleChannel;   // channel Single mode locks to (display + picker)
int coreScanModeIndex();            // active mode as a menu list index: 0=Custom, 1=Full, 2=Single

// ============================================================
// PROMISCUOUS CAPTURE: fills the internal alert queue, which the board loop
// drains via coreDequeueAlert().
// ============================================================

void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type);
bool coreDequeueAlert(AlertEntry& out);
extern volatile bool sniffingStopped;

// Exposed for the board `inject` command, which pushes a synthetic alert
// through the real queue.
void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac,
                             const uint8_t* mac2, int8_t rssi, uint8_t ch,
                             const char* ssid, const char* kind,
                             const char* fsubtype);

// ============================================================
// DETECTION TABLE + SD LOG + JSON EMIT: the shareable middle of
// drainAlertQueue(). Boards call this once per dequeued AlertEntry, then use
// the returned result for display/feedback.
// ============================================================

CoreAlertResult coreHandleAlert(const AlertEntry& e);
extern int  fyDetCount;
extern unsigned long fyLastTargetSeen;

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
void sdSetup();
void sdTryNameLog();
// Exposed for board-specific manual-log rows, such as the MANUALALERT marker,
// that bypass the detection table.
void sdAppendRow(const char* mac, const char* method, const char* frameSubtype,
                  int8_t rssi, uint8_t channel, const char* ssid,
                  const char* apMac, float distM = -1.0f);
extern bool fySDReady;
extern File sdLog;
#endif

#if HAS_GPS
extern bool   gpsHasFix;
extern double gpsLat;
extern double gpsLng;

// GPS parser health counters for the GPS detail screen. Mirrors the [gps]
// serial diagnostic line: good/bad checksum counts, fix-carrying sentences, and
// satellites in view. sats is -1 if none have been reported yet.
void coreGpsStats(unsigned long& good, unsigned long& bad,
                  unsigned long& fixSent, int& sats);
#endif

// ============================================================
// TIME SOURCE: a runtime priority chain of GPS once a module locks, then NTP
// over WiFi if station credentials are stored, then millis() since boot.
//
// coreTimeSync() runs once in setup() and blocks, because it must settle before
// the promiscuous radio comes up, the only window in which a WiFi STA join is
// safe. GPS cannot have a lock this early, so the NTP join bridges the pre-lock
// window. coreTick(), polled every loop(), then drains the GPS UART and sets the
// GPS anchor the moment a fix lands, at which point GPS takes over, since
// coreTimestampStr() prefers GPS over NTP. A board with GPS onboard therefore
// rides NTP or millis until it locks, then masters off GPS. A board with no
// module stays on NTP or millis. A boot probe distinguishes checksum-valid NMEA
// from a floating UART to report whether a module is wired at all.
// ============================================================

void coreTimeSync();
void coreTick();
bool coreTimeAnchored();

// ============================================================
// WIFI STATION CREDENTIALS: a single saved network used only for the boot-time
// NTP fallback (never for the Admin SoftAP, which is its own identity). Stored
// on SPIFFS at WIFI_CREDS_FILE as {"ssid","pass"} so they are set from the web
// console instead of being compiled in. SSID is user-controlled bytes, so this
// goes through a real JSON parser rather than hand-rolled string ops.
//
// Load returns true only when a non-empty SSID is stored. Save persists and
// overwrites, rejecting an empty ssid. Have is a cheap presence check. Clear
// removes the file, backing the console "wifi-forget" verb.
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
// SERIAL COMMANDS: word-based, using brief noun/verb tokens rather than single
// letters, because the command set outgrew single characters. Core handles the
// shared verbs (dump/prev/chirp/jingle/nav). `status` is genuinely per-board,
// since its fields differ, so each board composes its own printStatus() from
// the extern state above
// (currentChannel, channelModeName(), fyDetCount, fySpiffsReady,
// sniffingStopped, coreTimeAnchored()).
//
// Boards drive it with coreReadSerialCommand() (one shared line tokenizer),
// call coreHandleSerialCommand() first, and handle their own verbs when it
// returns false. Commands are newline-terminated. The verb is lowercased and
// the argument is the trimmed remainder, or "" if there is none.
// ============================================================

void dumpCurrentSession();
void dumpSpiffsFile(const char* path);

// Non-blocking line reader: drains Serial into an internal buffer and, each
// time a full line arrives, returns true with `verb`/`arg` pointing at the
// parsed tokens (lowercased verb, remainder as arg). Returns false when no
// complete line is pending. Loop over it to process all buffered lines.
bool coreReadSerialCommand(const char** verb, const char** arg);

// Dispatches a core-owned verb (dump/prev/chirp/jingle/nav <up|down|select|
// back|mark>). Returns true if handled, false so the board can try its own.
bool coreHandleSerialCommand(const char* verb, const char* arg);

// Prints the core-handled commands as indented help lines. Boards' own `help`
// prints their board-specific verbs (status/inject/log/…) then calls this, so
// the core command list lives in one place.
void corePrintSerialHelp();

// ============================================================
// LORA IDLE-PIN HOLDING: for a board with an unused LoRa modem sharing SPI bus
// pins with something else. The radio is never used. This holds NSS/RST high
// and BUSY/DIO1 as inputs so the idle radio does not interfere with the bus.
// Gated on HAS_LORA_MODEM, and a no-op otherwise.
// ============================================================

void coreLoraIdleInit();

// ============================================================
// NOTIFICATIONS: LED (NeoPixel) and buzzer feedback, gated on USE_LED/
// USE_BUZZER. coreHandleAlert() calls the detection half internally, so boards
// only need coreNotifyBoot() (once in setup(), after display init) and
// coreNotifyTick() (once per loop(), turns off the LED after LED_FLASH_MS
// and plays the audible heartbeat while a target is still in range).
// ============================================================

void coreNotifyBoot();
void coreNotifyTick();

// Runtime alert gates, toggled live from the on-device Alerts menu (SCREEN_
// ALERTS). Session-only, enabled by default every boot and not persisted, which
// matches the runtime scan mode. coreBuzzerEnabled gates the new-detection
// chirp and coreLedEnabled gates the detection and heartbeat LED flashes. The
// boot jingle, RGB cycle, and the chirp/jingle hooks all run unconditionally.
// Boards may also read these to render the current state.
extern bool coreBuzzerEnabled;
extern bool coreLedEnabled;

// Blocking status blink for boot-time signaling, such as the SD-not-found
// indicator: `count` on/off cycles of the given colour, leaving the LED off.
// Boot-context only, since it blocks with delay(). A no-op without USE_LED.
void coreLedBlink(uint8_t r, uint8_t g, uint8_t b,
                  uint8_t count, unsigned on_ms, unsigned off_ms);

// Replay the detection chirp or boot jingle on demand, without waiting for a
// real detection. Backs the "chirp" and "jingle" verbs on both the serial
// console and the web console. Both block via delay(), which suits either call
// site, since serial and WebConsole both dispatch from loop(). A no-op on a
// board with no buzzer, gated on USE_BUZZER inside.
void corePlayDetectChirp();
void corePlayStartupJingle();

// ============================================================
// INPUT: plain debounced buttons. Gated on HAS_BUTTONS, and always returns
// INPUT_NONE otherwise. BTN_PIN_1 toggles the active screen and BTN_PIN_2
// triggers a manual "area of interest" marker. The names stay abstract because
// physical button placement is not consistent board to board. Self-initializes
// both pins as INPUT_PULLUP on first call, so there is no separate init
// function for boards to remember.
// ============================================================

typedef enum { INPUT_NONE, INPUT_TOGGLE_SCREEN, INPUT_MANUAL_MARK } InputEvent;
InputEvent coreInputTick();

// ============================================================
// SEMANTIC NAV LAYER: a display-independent event grammar (UP / DOWN / SELECT /
// BACK / MARK) that the screen and menu state machine consumes, decoupled from
// the physical input hardware. The mapping from physical input to semantic
// event, meaning which button and short versus long press, lives in
// coreNavTick() behind the board's NAV_SCHEME. A later board revision can
// therefore swap the three buttons for an encoder or 5-way without touching the
// screen logic. Events also arrive from the serial nav injector
// (coreInjectNav), so the screen and menu machine can be driven over serial
// with no physical input. coreNavTick() reads the physical buttons only under
// NAV_SCHEME_3BTN. A 2-button board uses coreInputTick() instead. The injector
// works on any board.
//
// 3-button map: BTN_1 short=UP / long=MARK · BTN_2 short=DOWN · BTN_3
// short=SELECT / long=BACK. Manual-mark keeps a dedicated, always-available
// gesture on long BTN_1 rather than an overloaded context press.
// ============================================================

#ifndef NAV_SCHEME_3BTN
#define NAV_SCHEME_3BTN 0
#endif
#ifndef NAV_LONG_PRESS_MS
#define NAV_LONG_PRESS_MS 500   // hold >= this many ms = long press (BACK / MARK)
#endif

typedef enum {
  NAV_NONE,
  NAV_UP,      // previous screen / menu item up
  NAV_DOWN,    // next screen / menu item down
  NAV_SELECT,  // enter menu / confirm selection
  NAV_BACK,    // exit menu (no change)
  NAV_MARK,    // manual "area of interest" marker (always available)
} NavEvent;

// Returns the next pending nav event, taking serial-injected events first and
// then the physical buttons under NAV_SCHEME_3BTN, or NAV_NONE. Self-inits its
// pins on first call. Poll once per loop, like coreInputTick().
NavEvent coreNavTick();

// Push a synthetic nav event into the same queue coreNavTick() drains. The
// serial `nav` verb calls this. Available on every board.
void coreInjectNav(NavEvent ev);

// ============================================================
// SCREENS + MENUS: the top-level screen carousel lives in core, holding state
// only, with each board rendering coreCurrentScreen its own way, so every board
// can show the same screens. coreNavApply() feeds a NavEvent into the state
// machine and returns a NavAction the board acts on (redraw the display, run
// the manual mark, enter Admin). See docs/menu_ux.md.
// ============================================================

typedef enum {
  SCREEN_OVERVIEW,      // headline: detection count + channel + scanning/hit
  SCREEN_GPS,           // detail: GPS position / fix status
  SCREEN_DETECTIONS,    // detail: num detections / last detection MAC
  SCREEN_SCAN_DETAIL,   // detail: current channel, dwell, mode
  SCREEN_SCAN_MODES,    // menu: Custom Scan / Full Channel Scan
  SCREEN_ALERTS,        // menu: Buzzer mute/unmute + LED on/off (toggle in place)
  SCREEN_CONFIG,        // menu: web console On / Off (Admin entry)
  SCREEN_COUNT,
} ScreenId;

extern ScreenId coreCurrentScreen;

// Board-observable side effects coreNavApply() asks the caller to run. The
// screen-state change itself is internal to coreCurrentScreen. These are the
// parts that need board-specific code, such as the SD row or portal start.
typedef enum {
  NAV_ACT_NONE,
  NAV_ACT_REDRAW,   // screen/menu state changed, board should redraw
  NAV_ACT_MARK,     // run the manual area-of-interest marker
  NAV_ACT_ADMIN,    // enter Admin (web portal), Config menu confirmed "On"
} NavAction;

// Feed one NavEvent into the screen/menu state machine. Updates
// coreCurrentScreen and returns the side effect (if any) for the board to run.
NavAction coreNavApply(NavEvent ev);

// Menu drill-in state for the two menu screens (SCAN_MODES / CONFIG):
//   MENU_NONE          browsing the carousel, where Up/Down move screens
//   MENU_LIST          an option list is open, coreMenuSel = highlighted index
//   MENU_PICK_CHANNEL  Single-mode channel picker, coreMenuSel = channel dialed
// Boards read these to render the cursor and edit state. While the state is not
// MENU_NONE the carousel is frozen, with Up/Down moving the highlight instead,
// and long-Back pops out.
typedef enum { MENU_NONE, MENU_LIST, MENU_PICK_CHANNEL } MenuState;
extern MenuState coreMenuState;
extern int       coreMenuSel;

// ============================================================
// ADMIN-MODE TRIGGER: a double-press of the BOOT button enters the web portal
// (Admin mode). coreAdminTriggerCheck() is polled at the top of each loop()
// while in Detect. It self-inits BOOT_BTN_PIN as INPUT_PULLUP on first call and
// returns true once when a debounced double-press completes, meaning two
// presses within BOOT_DOUBLE_PRESS_MS. It works at any time, not only at boot,
// so Detect and Admin can alternate repeatedly without a reboot. The portal
// releases back to Detect via a web-console command or an idle timeout, as
// described in web_portal.h. Non-blocking.
// ============================================================

#ifndef BOOT_DOUBLE_PRESS_MS
#define BOOT_DOUBLE_PRESS_MS 600   // max gap between the two presses
#endif
bool coreAdminTriggerCheck();

// ============================================================
// WIFI SNIFFER BRING-UP: the raw-IDF promiscuous capture init, covering driver
// init, NULL mode, start, channel, and the promiscuous filter and callback.
// Shared by every board's setup() and by webPortalStop() when it releases the
// AP and resumes Detect. Clears sniffingStopped. The Admin web portal fully
// deinits this driver to hand the radio to Arduino WiFi for the SoftAP, and
// this brings it back up from clean. Must run after the board's peripheral
// init, since it touches no display.
// ============================================================

void coreWifiSnifferStart();
