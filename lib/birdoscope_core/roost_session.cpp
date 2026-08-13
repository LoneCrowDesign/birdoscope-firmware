// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// See roost_session.h. Contract:
// vendor/jellybeans/roost_logging/docs/design_spec.md.

#include "roost_session.h"

#if USE_SD

#include <Arduino.h>
#include <SD.h>

// ============================================================
// ARDUINO SD BACKEND
//
// The shared writer takes storage as function pointers so its buffering can be
// tested on a host. This is the half only the device can supply.
// ============================================================

static File g_files[SD_MAX_OPEN_FILES];
static bool g_used[SD_MAX_OPEN_FILES];

static int sdIoMkdir(void*, const char* path) {
  if (SD.exists(path)) return 0;
  return SD.mkdir(path) ? 0 : -1;
}

// FILE_APPEND, never FILE_WRITE: on ESP32 FILE_WRITE is "w" and truncates, so
// reopening after the anchor rename would destroy every pre-anchor row.
static int sdIoOpen(void*, const char* path) {
  for (int i = 0; i < SD_MAX_OPEN_FILES; i++) {
    if (g_used[i]) continue;
    g_files[i] = SD.open(path, FILE_APPEND, true);
    if (!g_files[i]) return -1;
    g_used[i] = true;
    return i;
  }
  return -1;
}

static size_t sdIoSize(void*, int h) {
  if (h < 0 || h >= SD_MAX_OPEN_FILES || !g_used[h]) return 0;
  return (size_t)g_files[h].size();
}

static int sdIoWrite(void*, int h, const void* d, size_t n) {
  if (h < 0 || h >= SD_MAX_OPEN_FILES || !g_used[h]) return -1;
  return (int)g_files[h].write((const uint8_t*)d, n);
}

static int sdIoSync(void*, int h) {
  if (h < 0 || h >= SD_MAX_OPEN_FILES || !g_used[h]) return -1;
  g_files[h].flush();
  return 0;
}

static void sdIoClose(void*, int h) {
  if (h < 0 || h >= SD_MAX_OPEN_FILES || !g_used[h]) return;
  g_files[h].close();
  g_used[h] = false;
}

static uint32_t sdIoNow(void*) { return millis(); }

static const RoostSdIo kSdIo = {
  nullptr, sdIoMkdir, sdIoOpen, sdIoSize, sdIoWrite, sdIoSync, sdIoClose, sdIoNow,
};

// ============================================================
// SESSION STATE
// ============================================================

// wifi_obs is the only high-rate file. gps_track gets a smaller buffer on
// purpose: at 1 Hz a full 4 KB block is forty seconds of position, and this
// device's power is pulled rather than shut down. The three event records take
// no buffer at all and are write-through.
static uint8_t g_bufWifi[4096];
static uint8_t g_bufTrack[1024];

static RoostSdLog  g_log;
static bool        g_open      = false;
static uint32_t    g_fixSeq    = 0;
static uint32_t    g_lastManifestMs = 0;
static char        g_dir[32]   = "";
static bool        g_named     = false;
static bool        g_ended     = false;

// Watermarks for the degradation events in spec 6.4. Compared on the manifest
// cadence rather than raised from the failing write itself, which would try to
// write a device_event through the storage that just failed.
static uint32_t    g_seenStorageErrors = 0;
static uint32_t    g_seenOverflowRows  = 0;
static uint32_t    g_seenQueueDrops    = 0;
static bool        g_inStorageError    = false;

// A session opens under its boot number, because rows precede the clock anchor
// and the dated name is not knowable at first write. It is renamed to
// /bscope-M-D-YY-N once time anchors.
//
// Keyed by boot, not a fixed name: a fixed provisional name lets successive
// boots append to one directory, which makes their rows unattributable and
// leaves one manifest describing only the last. boot_count comes from NVS and
// survives a reflash, which is what makes it a usable key.
//
// This is a final name, not a placeholder. An unanchored session has no date to
// be given, so it keeps this one and says clock_source=none in its manifest.
#define ROOST_DIR_BOOT "/bscope-boot"
#define MANIFEST_SNAPSHOT_MS 15000

bool roostSessionOpen()      { return g_open; }
const char* roostSessionDir(){ return g_dir; }
uint32_t roostFixSeq()       { return g_fixSeq; }
bool roostHasFix()           { return gpsHasFix; }

// Derived from this board's capability macros by the generated header. A board
// declares capabilities; it never restates which files it writes.
static RoostFileDecl g_decls[ROOST_MAX_DECLARED_FILES];
static size_t        g_declCount = 0;

static void buildDecls() {
  g_declCount = roostDeclaredFiles(g_decls, ROOST_MAX_DECLARED_FILES);
}

// ============================================================
// MANIFEST
// ============================================================

static void writeManifest() {
  if (!g_open) return;

  uint32_t anchorUnix = 0, anchorUptime = 0;
  const char* clockSrc = coreClockAnchor(&anchorUnix, &anchorUptime);

  RoostSdStats st;
  roostSdGetStats(&g_log, &st);

  // Everything the contract defines is rendered by the shared renderer from
  // these facts. This device spells no key, formats no timestamp and decides
  // nothing about null: those were the parts that differed between devices.
  RoostSessionInfo info;
  memset(&info, 0, sizeof(info));
  info.deviceModel  = "birdoscope_analyze";
  info.deviceSerial = coreDeviceSerial();
  info.hwRevision   = "HWr0.1";
  info.fwVersion    = BIRDOSCOPE_VERSION " " BIRDOSCOPE_GIT_REV;
  info.builtAt      = BIRDOSCOPE_BUILD_TS;

  const char* macs[1] = { coreOwnMac() };
  info.ownMacs    = macs;
  info.numOwnMacs = 1;
  info.gnssCepM   = 2.5f;

  info.sessionId           = g_dir + 1;
  info.sequence            = coreSessionSequence();
  info.bootCount           = coreBootCount();
  info.clockAnchored       = anchorUnix != 0;
  info.clockSource         = roostClockSourceByName(clockSrc);
  info.clockAnchorUnix     = anchorUnix;
  info.clockAnchorUptimeMs = anchorUptime;
  info.endedUptimeMs       = g_ended ? millis() : 0;

  info.ouiTableHash = coreOuiTableHash();
  info.ieTableHash  = nullptr;   // no IE matcher table in this build
  // Null is load-bearing: this device applies no dedup to the log, so every
  // repeat observation is in the file. The cooldown ring gates the display and
  // the detection JSON, not wifi_obs.
  info.dedupPolicy  = nullptr;
  info.storageTier  = "sd";

  // observations_suppressed is a true zero for the same reason, not a stand-in
  // for a figure this device does not keep. coreQueueDrops is the only loss the
  // writer cannot see: alerts discarded before the drain reached them.
  roostSessionCounters(&g_log, &info.counters, 0, coreQueueDrops);

  info.files    = g_decls;
  info.numFiles = g_declCount;

  // Loss split into its three causes, which the contract has no key for:
  // queue drops, refused rows and buffer overflows. Manifest v2 names
  // device_diagnostics as where a device puts a counter the contract does not
  // define.
  //
  // queue_depth_max against queue_size is what says whether a drop count means
  // the ring is undersized or that something blocked the drain. Without it in
  // the artifact, a capture cannot answer that question after the fact.
  // Summed from the writer's per-record counts, not a tally kept beside them.
  uint32_t voided = 0;
  for (int i = 0; i < ROOST_REC_COUNT; i++)
    voided += roostSdRowsVoided(&g_log, (RoostRecord)i);

  // frames_seen and mgmt_* are the only figures here describing traffic the
  // matcher rejected. Without them a subtype that never reached the radio and
  // one that reached it and matched nothing produce the same capture, which is
  // what made a missing probe_req unanswerable from the artifact alone.
  char diag[512];
  const int dn = snprintf(diag, sizeof(diag),
           "\"queue_drops\":%u,\"rows_voided\":%u,\"row_buffer_overflows\":%u,"
           "\"wifi_obs_written\":%u,\"wifi_obs_voided\":%u,"
           "\"queue_depth_max\":%u,\"queue_size\":%u,"
           "\"frames_seen\":%u,\"frames_candidate\":%u,"
           "\"mgmt_probe_req_seen\":%u,\"mgmt_probe_req_matched\":%u,"
           "\"mgmt_probe_resp_seen\":%u,\"mgmt_probe_resp_matched\":%u,"
           "\"mgmt_beacon_seen\":%u,\"mgmt_beacon_matched\":%u",
           (unsigned)coreQueueDrops, (unsigned)voided,
           (unsigned)st.overflowRows,
           (unsigned)roostSdRowsWritten(&g_log, ROOST_REC_WIFI_OBS),
           (unsigned)roostSdRowsVoided(&g_log, ROOST_REC_WIFI_OBS),
           (unsigned)coreQueueDepthMax, (unsigned)coreAlertQueueSize(),
           (unsigned)coreSeenFrames, (unsigned)coreCandidateFrames,
           (unsigned)coreMgmtSeen[4],  (unsigned)coreMgmtMatched[4],
           (unsigned)coreMgmtSeen[5],  (unsigned)coreMgmtMatched[5],
           (unsigned)coreMgmtSeen[8],  (unsigned)coreMgmtMatched[8]);
  // A truncated block is invalid JSON and voids the whole manifest, so it is
  // dropped rather than written short. The return was discarded before this.
  if (dn < 0 || (size_t)dn >= sizeof(diag)) {
    dualPrintln("[roost] device_diagnostics did not fit - omitting the block");
    info.deviceDiagnostics = nullptr;
  } else {
    info.deviceDiagnostics = diag;
  }

  static char json[4096];
  const size_t n = roostSessionJson(json, sizeof(json), &info);
  // Render first, write second. A manifest that did not fit is not written at
  // all: a truncated one asserts a file set and a column list the session does
  // not have, and it fails validation for the whole capture.
  if (!n) {
    dualPrintln("[roost] manifest did not fit - keeping the previous one");
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/manifest.json", g_dir);
  // FILE_WRITE truncates, which is right here and wrong everywhere else in
  // this file: the manifest is a snapshot to be replaced, not a log to append.
  File f = SD.open(path, FILE_WRITE);
  if (!f) return;
  f.write((const uint8_t*)json, n);
  f.flush();
  f.close();
}

// Spec 6.4: a write failure or a full buffer is a device_event, not just a
// counter. Raised here on the snapshot cadence, after the flush, so a failing
// card is reported by the write-through path rather than from inside the write
// that failed.
static void reportDegradation() {
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);

  if (st.storageErrors > g_seenStorageErrors) {
    roostLogDeviceEvent(ROOST_COMP_SYS, "storage_error",
                        st.storageErrors - g_seenStorageErrors, nullptr);
    g_seenStorageErrors = st.storageErrors;
    g_inStorageError = true;
  } else if (g_inStorageError) {
    roostLogDeviceEvent(ROOST_COMP_SYS, "storage_recovered", 0, nullptr);
    g_inStorageError = false;
  }

  // Two ways to lose an observation with the card healthy: the row buffer had
  // no room, or the alert queue filled before the drain reached it.
  const uint32_t overflow = st.overflowRows;
  const uint32_t drops    = coreQueueDrops;
  if (overflow > g_seenOverflowRows || drops > g_seenQueueDrops) {
    roostLogDeviceEvent(ROOST_COMP_SYS, "buffer_full",
                        (overflow - g_seenOverflowRows) + (drops - g_seenQueueDrops),
                        overflow > g_seenOverflowRows ? "row buffer" : "alert queue");
    g_seenOverflowRows = overflow;
    g_seenQueueDrops   = drops;
  }
}

// ============================================================
// LIFECYCLE
// ============================================================

bool roostSessionBegin() {
  if (!fySDReady) return false;
  buildDecls();
  roostSdInit(&g_log, &kSdIo);
  roostSdAttachBuffer(&g_log, ROOST_REC_WIFI_OBS,  g_bufWifi,  sizeof(g_bufWifi));
  roostSdAttachBuffer(&g_log, ROOST_REC_GPS_TRACK, g_bufTrack, sizeof(g_bufTrack));

  // Never adopt an existing directory. boot_count restarts at 1 if NVS is
  // erased, which would otherwise reopen an earlier unanchored session and
  // merge two captures under one manifest - the defect this key exists to
  // prevent.
  const unsigned boot = (unsigned)coreBootCount();
  bool free_ = false;
  for (unsigned k = 0; k < 100 && !free_; k++) {
    if (k) snprintf(g_dir, sizeof(g_dir), ROOST_DIR_BOOT "-%u-%u", boot, k);
    else   snprintf(g_dir, sizeof(g_dir), ROOST_DIR_BOOT "-%u", boot);
    free_ = !SD.exists(g_dir);
  }
  // Refuse rather than fall back to a name already in use. Sharing a container
  // makes rows unattributable rather than merely misnamed, because nothing in
  // the artifact says where one boot ended and the next began. Spec 6.2, D41.
  if (!free_) {
    dualPrintln("[roost] no free session name - refusing to share one");
    return false;
  }
  if (!roostSdOpenSession(&g_log, g_dir, g_decls, g_declCount)) {
    dualPrintln("[roost] session open failed - no rows will be written");
    return false;
  }
  g_open = true;

  // The one component check the preprocessor cannot make: it cannot compare
  // string literals to find two ids the same.
  if (!roostComponentsValid())
    roostLogDeviceEvent(ROOST_COMP_SYS, "config_error", 0, "components invalid");

  roostLogDeviceEvent(ROOST_COMP_SYS, "boot", coreBootCount(), coreBuildIdentity());
  roostLogConfigBoot();
  writeManifest();
  dualPrintf("[roost] session open at %s\n", g_dir);
  return true;
}

void roostSessionAnchor() {
  if (!g_open || g_named || !coreTimeAnchored()) return;

  uint32_t anchorUnix = 0, anchorUptime = 0;
  coreClockAnchor(&anchorUnix, &anchorUptime);
  roostLogDeviceEvent(ROOST_COMP_GNSS0, "clock_anchored", anchorUnix, nullptr);

  char want[32];
  if (!coreSessionDirName(want, sizeof(want))) {
    // The day's names are used up. Keep the boot name: a failed rename is not a
    // failed capture, and the manifest carries the anchor either way, so the
    // only thing lost is a cosmetic name. Spec 6.2.
    dualPrintln("[roost] no free dated name today - keeping the boot name");
    roostLogDeviceEvent(ROOST_COMP_SYS, "config_error", 0, "no free session name");
    g_named = true;                   // do not retry on every tick
    writeManifest();
    return;
  }

  // FatFs requires that a renamed object not be open, so close first and
  // reopen after. Reopening appends, so no header is re-emitted and each
  // record type stays one continuous file across the anchor.
  roostSdCloseSession(&g_log);
  if (SD.rename(g_dir, want)) {
    snprintf(g_dir, sizeof(g_dir), "%s", want);
    g_named = true;
  } else {
    // Keep capturing under the provisional name rather than losing the
    // session. The manifest still carries the anchor, so it remains placeable.
    dualPrintf("[roost] rename %s -> %s failed, staying put\n", g_dir, want);
    roostLogDeviceEvent(ROOST_COMP_SYS, "config_error", 0, "session rename failed");
  }
  if (!roostSdOpenSession(&g_log, g_dir, g_decls, g_declCount)) {
    dualPrintln("[roost] reopen after rename failed");
    g_open = false;
    return;
  }
  writeManifest();
  dualPrintf("[roost] session is %s\n", g_dir);
}

void roostSessionTick() {
  if (!g_open) return;
  const uint32_t now = millis();
  if (now - g_lastManifestMs < MANIFEST_SNAPSHOT_MS) return;
  g_lastManifestMs = now;
  roostSdFlushAll(&g_log);
  reportDegradation();
  writeManifest();
}

void roostSessionEnd() {
  if (!g_open) return;
  reportDegradation();
  roostLogDeviceEvent(ROOST_COMP_SYS, "shutdown", 0, nullptr);
  roostSdCloseSession(&g_log);
  g_ended = true;
  writeManifest();
  g_open = false;
}

void roostSessionStats(uint32_t* rowsWritten, uint32_t* rowsDropped,
                       uint32_t* worstFlushMs, uint32_t* fixes) {
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  if (rowsWritten) *rowsWritten = st.rowsWritten;
  if (rowsDropped) *rowsDropped = st.rowsDropped;
  if (worstFlushMs) *worstFlushMs = st.worstFlushMs;
  // From the writer, not a second tally beside it: two counters of one fact
  // disagree eventually, and the manifest reads the writer's.
  if (fixes) *fixes = roostSdRowsWritten(&g_log, ROOST_REC_GPS_TRACK);
}

// ============================================================
// ROW WRITERS
// ============================================================

// Fills the columns every record shares. timestamp_utc is left empty when the
// clock has not anchored; uptime_ms alone places the row, and the manifest's
// anchor triple resolves it afterwards. Never a stand-in value.
// fix_seq sits between uptime_ms and cap_component in every record that has
// it, so it is written here rather than by the caller: a column already passed
// cannot be revisited, and on gps_track it is required, so writing it late
// voids the row rather than misplacing a value.
static void setCommon(RoostRow* w, uint8_t iTs, uint8_t iUp, uint8_t iFix,
                      uint8_t iComp, uint32_t uptimeMs, RoostComponent comp,
                      uint32_t fixSeq) {
  char ts[24];
  if (coreTimestampAt(uptimeMs, ts, sizeof(ts))) roostRowSetText(w, iTs, ts);
  roostRowSetUInt(w, iUp, uptimeMs);
  if (fixSeq) roostRowSetUInt(w, iFix, fixSeq);
  roostRowSetText(w, iComp, roostComponentId(comp));
}

// A refusal means a required column was never written, so the row does not
// exist and nothing downstream can tell that from a quiet capture. The first
// refusal per record type is reported on the serial line rather than only
// reaching a manifest counter.
static bool finishAndAppend(RoostRow* w, RoostRecord rec, const char* row) {
  if (!roostRowFinish(w)) {
    if (!roostSdRowsVoided(&g_log, rec))
      dualPrintf("[roost] %s row refused by the builder: a required column was "
                 "never written\n", roostRecordName(rec));
    roostSdCountVoid(&g_log, rec);
    return false;
  }
  return roostSdAppend(&g_log, rec, row) != 0;
}

void roostLogWifiObs(const AlertEntry& e, const char* method) {
  if (!g_open) return;
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  setCommon(&w, ROOST_WIFI_OBS_TIMESTAMP_UTC, ROOST_WIFI_OBS_UPTIME_MS,
            ROOST_WIFI_OBS_FIX_SEQ, ROOST_WIFI_OBS_CAP_COMPONENT,
            e.uptimeMs, ROOST_COMP_WIFI0, g_fixSeq);
  roostRowSetEnum(&w, ROOST_WIFI_OBS_OBS_MODE, ROOST_OBS_MODE_PROMISCUOUS);
  // A mac column takes the bytes, not a formatted string. The text setter is
  // refused on it, and mac is required, so that refusal voids the whole row.
  roostRowSetMac(&w, ROOST_WIFI_OBS_MAC, e.mac);
  roostRowSetEnumByName(&w, ROOST_WIFI_OBS_DETECTION_METHOD, method);
  if (e.frameSubtype[0])
    roostRowSetEnumByName(&w, ROOST_WIFI_OBS_FRAME_SUBTYPE, e.frameSubtype);
  roostRowSetInt(&w, ROOST_WIFI_OBS_RSSI, e.rssi);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_CHANNEL, e.channel);
  // Derived through the shared helper, never declared per device. A channel
  // this build cannot place leaves the column empty rather than guess a band.
  const RoostChannelBand cb = roostBandForChannel(e.channel);
  if (cb.known) roostRowSetEnum(&w, ROOST_WIFI_OBS_BAND, cb.band);
  // Whenever the frame carried one. The alert type does not decide this: a
  // beacon is not a name-bearing alert and still broadcasts its SSID. Length,
  // not strlen: the octets may contain 0x00 and a cloaked name is all zeros.
  // A zero-length element renders empty here, same as no element; frame_subtype
  // is what separates them at ingest (spec 7.1).
  if (e.ssid.len)
    roostRowSetTextN(&w, ROOST_WIFI_OBS_SSID, e.ssid.text, e.ssid.len);
  // By position, never by role. The pipeline derives roles from type and
  // subtype; a role name varies per frame and cannot be a column.
  roostRowSetMac(&w, ROOST_WIFI_OBS_ADDR1, e.addr1);
  roostRowSetMac(&w, ROOST_WIFI_OBS_ADDR2, e.addr2);
  roostRowSetMac(&w, ROOST_WIFI_OBS_ADDR3, e.addr3);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_SEQ, e.seq);
  // The flags byte only: the type/subtype half is already frame_subtype.
  const uint8_t fc = (uint8_t)e.fcFlags;
  roostRowSetHex(&w, ROOST_WIFI_OBS_FC_FLAGS, &fc, 1);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_FRAME_LEN, e.frameLen);
  if (e.bbFormat[0])
    roostRowSetEnumByName(&w, ROOST_WIFI_OBS_BB_FORMAT, e.bbFormat);

  if (w.unknownEnums)
    roostLogDeviceEvent(ROOST_COMP_SYS, "vocabulary_error", w.unknownEnums, "wifi_obs");
  finishAndAppend(&w, ROOST_REC_WIFI_OBS, row);
}

void roostLogGpsFix() {
  if (!g_open) return;
  g_fixSeq++;

  char row[256];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_GPS_TRACK,
                ROOST_GPS_TRACK_COLUMNS_MASK);
  const uint32_t now = millis();
  setCommon(&w, ROOST_GPS_TRACK_TIMESTAMP_UTC, ROOST_GPS_TRACK_UPTIME_MS,
            ROOST_GPS_TRACK_FIX_SEQ, ROOST_GPS_TRACK_CAP_COMPONENT,
            now, ROOST_COMP_GNSS0, g_fixSeq);

  CoreGpsFix fx;
  coreGpsFix(&fx);
  roostRowSetEnumByName(&w, ROOST_GPS_TRACK_POSITION_SOURCE, fx.source);
  if (fx.valid) {
    roostRowSetFloat(&w, ROOST_GPS_TRACK_LAT, fx.lat);
    roostRowSetFloat(&w, ROOST_GPS_TRACK_LON, fx.lon);
    if (fx.hasAlt)    roostRowSetFloat(&w, ROOST_GPS_TRACK_ALT_M, fx.altM);
    if (fx.hasSpeed)  roostRowSetFloat(&w, ROOST_GPS_TRACK_SPEED_MPS, fx.speedMps);
    if (fx.hasCourse) roostRowSetFloat(&w, ROOST_GPS_TRACK_COURSE_DEG, fx.courseDeg);
    if (fx.hasHdop)   roostRowSetFloat(&w, ROOST_GPS_TRACK_HDOP, fx.hdop);
    if (fx.hasSats)   roostRowSetUInt(&w, ROOST_GPS_TRACK_SATS, fx.sats);
    roostRowSetEnumByName(&w, ROOST_GPS_TRACK_FIX_TYPE, fx.fixType);
    roostRowSetUInt(&w, ROOST_GPS_TRACK_FIX_AGE_MS, fx.ageMs);
  }
  // position_source and fix_type are set by name, so they can drift from the
  // registry. Spec 6.3: an unexplained empty column reads as nothing to record.
  if (w.unknownEnums)
    roostLogDeviceEvent(ROOST_COMP_SYS, "vocabulary_error", w.unknownEnums, "gps_track");
  finishAndAppend(&w, ROOST_REC_GPS_TRACK, row);
}

// Last value written per setting. Spec 6.4: a setting re-applied to the value
// it already holds is not a change and writes nothing, so a menu that reasserts
// its state does not fill the file with rows saying nothing happened.
// Keyed on the pair, not on the setting alone: two components report the same
// setting under different values, and a setting-only key suppresses the second
// component's row as a repeat of the first's.
#define CFG_SLOTS ((int)ROOST_COMPONENT_COUNT * (int)ROOST_CONFIG_SETTING_COUNT)
static struct {
  RoostComponent comp;
  char           setting[24];
  char           value[64];
} g_cfg[CFG_SLOTS];
static size_t g_cfgUsed = 0;

static bool configUnchanged(RoostComponent comp, const char* setting,
                            const char* value) {
  for (size_t i = 0; i < g_cfgUsed; i++)
    if (g_cfg[i].comp == comp && strcmp(g_cfg[i].setting, setting) == 0)
      return strcmp(g_cfg[i].value, value) == 0;
  // A setting seen for the first time is a change: its boot row is what makes
  // config_change self-contained from its first row.
  return false;
}

// Called only once the row reached the card. Recording the value before the
// append means a refused row leaves the table claiming a value the artifact
// never carried, and every later write of it is then suppressed as a repeat.
static void configRemember(RoostComponent comp, const char* setting,
                           const char* value) {
  for (size_t i = 0; i < g_cfgUsed; i++) {
    if (g_cfg[i].comp != comp || strcmp(g_cfg[i].setting, setting) != 0) continue;
    strlcpy(g_cfg[i].value, value, sizeof(g_cfg[0].value));
    return;
  }
  if (g_cfgUsed < CFG_SLOTS) {
    g_cfg[g_cfgUsed].comp = comp;
    strlcpy(g_cfg[g_cfgUsed].setting, setting, sizeof(g_cfg[0].setting));
    strlcpy(g_cfg[g_cfgUsed].value, value, sizeof(g_cfg[0].value));
    g_cfgUsed++;
  }
}

void roostLogConfigChange(RoostComponent component,
                          const char* setting, const char* value) {
  if (!g_open) return;
  const char* v = value ? value : "";
  if (configUnchanged(component, setting, v)) return;
  char row[192];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_CONFIG_CHANGE,
                ROOST_CONFIG_CHANGE_COLUMNS_MASK);
  setCommon(&w, ROOST_CONFIG_CHANGE_TIMESTAMP_UTC, ROOST_CONFIG_CHANGE_UPTIME_MS,
            ROOST_CONFIG_CHANGE_FIX_SEQ, ROOST_CONFIG_CHANGE_CAP_COMPONENT,
            millis(), component, g_fixSeq);
  roostRowSetEnumByName(&w, ROOST_CONFIG_CHANGE_SETTING, setting);
  roostRowSetText(&w, ROOST_CONFIG_CHANGE_VALUE, v);
  if (w.unknownEnums)
    roostLogDeviceEvent(ROOST_COMP_SYS, "vocabulary_error", w.unknownEnums, setting);
  if (finishAndAppend(&w, ROOST_REC_CONFIG_CHANGE, row))
    configRemember(component, setting, v);
}

// The channel plan and the vendor mask are rendered rather than fixed, so both
// can refuse. An empty value means the setting does not apply on this build
// (spec L3), so a refusal must not be written as one: drop the row and say so.
void roostLogConfigChannels() {
  char buf[64];
  if (coreChannelListRoost(buf, sizeof(buf)))
    roostLogConfigChange(ROOST_COMP_WIFI0, "channels", buf);
  else
    roostLogDeviceEvent(ROOST_COMP_WIFI0, "config_error", 0, "channels too long");
}

void roostLogConfigVendorMask() {
  char buf[64];
  if (coreVendorMaskStr(buf, sizeof(buf)))
    roostLogConfigChange(ROOST_COMP_WIFI0, "vendor_mask", buf);
  else
    roostLogDeviceEvent(ROOST_COMP_WIFI0, "config_error", 0, "vendor_mask too long");
}

void roostLogConfigBoot() {
  char buf[64];
  // Every setting in the vocabulary, in registry order, empty where it does not
  // apply: the boot dump is then a fixed length across the fleet and a reader
  // never has to tell "not applicable" from "this device forgot". Spec L5.
  roostLogConfigChange(ROOST_COMP_WIFI0, "obs_mode", "promiscuous");
  roostLogConfigChannels();
  roostLogConfigChange(ROOST_COMP_WIFI0, "country_code", coreCountryCode());
  snprintf(buf, sizeof(buf), "%u", (unsigned)CHANNEL_DWELL_MS);
  roostLogConfigChange(ROOST_COMP_WIFI0, "dwell_ms", buf);
  roostLogConfigChange(ROOST_COMP_WIFI0, "scan_period_ms", "");
  roostLogConfigVendorMask();

  RoostValue f;
  roostValueBegin(&f, buf, sizeof(buf));
  roostValueAddKeyInt(&f, "rssi_min", RSSI_MIN);
  roostValueAddKeyUInt(&f, "cooldown_ms", ALERT_COOLDOWN_MS);
  if (roostValueDone(&f))
    roostLogConfigChange(ROOST_COMP_WIFI0, "filters", buf);
  else
    roostLogDeviceEvent(ROOST_COMP_WIFI0, "config_error", 0, "filters too long");
}

void roostLogDeviceEvent(RoostComponent component,
                         const char* kind, uint32_t count, const char* detail) {
  if (!g_open) return;
  char row[256];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_DEVICE_EVENT,
                ROOST_DEVICE_EVENT_COLUMNS_MASK);
  setCommon(&w, ROOST_DEVICE_EVENT_TIMESTAMP_UTC, ROOST_DEVICE_EVENT_UPTIME_MS,
            ROOST_DEVICE_EVENT_FIX_SEQ, ROOST_DEVICE_EVENT_CAP_COMPONENT,
            millis(), component, g_fixSeq);
  roostRowSetEnumByName(&w, ROOST_DEVICE_EVENT_EVENT_KIND, kind);
  roostRowSetUInt(&w, ROOST_DEVICE_EVENT_EVENT_COUNT, count);
  if (detail) roostRowSetText(&w, ROOST_DEVICE_EVENT_EVENT_DETAIL, detail);
  // event_kind is required, so an unresolvable one voids the row rather than
  // emptying a column: report it on the serial line, which is the only channel
  // left. Never by recursing into this function with another unknown kind.
  if (w.unknownEnums)
    dualPrintf("[roost] device_event kind \"%s\" not in the registry\n", kind);
  finishAndAppend(&w, ROOST_REC_DEVICE_EVENT, row);
}

void roostLogOperatorMark() {
  if (!g_open) return;
  char row[128];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_OPERATOR_MARK,
                ROOST_OPERATOR_MARK_COLUMNS_MASK);
  setCommon(&w, ROOST_OPERATOR_MARK_TIMESTAMP_UTC, ROOST_OPERATOR_MARK_UPTIME_MS,
            ROOST_OPERATOR_MARK_FIX_SEQ, ROOST_OPERATOR_MARK_CAP_COMPONENT,
            millis(), ROOST_COMP_SYS, g_fixSeq);
  finishAndAppend(&w, ROOST_REC_OPERATOR_MARK, row);
}

#else   // no card on this build

bool roostSessionBegin() { return false; }
void roostSessionAnchor() {}
void roostSessionTick() {}
void roostSessionEnd() {}
bool roostSessionOpen() { return false; }
const char* roostSessionDir() { return ""; }
uint32_t roostFixSeq() { return 0; }
bool roostHasFix() { return false; }
void roostLogWifiObs(const AlertEntry&, const char*) {}
void roostLogGpsFix() {}
void roostLogConfigChange(RoostComponent, const char*, const char*) {}
void roostLogConfigBoot() {}
void roostLogConfigChannels() {}
void roostLogConfigVendorMask() {}
void roostLogDeviceEvent(RoostComponent, const char*, uint32_t, const char*) {}
void roostLogOperatorMark() {}
void roostSessionStats(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0;
}

#endif  // USE_SD
