// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// See roost_session.h. Numbered spec citations below (6.2, 7.1) refer to the
// contract, vendor/jellybeans/roost_logging/docs/design_spec.md. Lettered ones
// (L3, M9) refer to the firmware design spec.

#include "roost_session.h"

#if USE_SD

#include <Arduino.h>
#include <SD.h>
#include "ble_ad.h"

// ============================================================
// ARDUINO SD BACKEND
//
// The shared writer takes storage as function pointers, so a host can test its
// buffering. These are the device's implementations.
// ============================================================

// Every declared record file stays open for the session, and the manifest
// snapshot opens one more.
#if USE_SD && (ROOST_EMITS_WIFI_OBS + ROOST_EMITS_BLE_OBS + ROOST_EMITS_GPS_TRACK \
               + ROOST_EMITS_DEVICE_EVENT + ROOST_EMITS_CONFIG_CHANGE        \
               + ROOST_EMITS_OPERATOR_MARK + 1) > SD_MAX_OPEN_FILES
#error "SD_MAX_OPEN_FILES leaves the manifest snapshot no file handle"
#endif

static File g_files[SD_MAX_OPEN_FILES];
static bool g_used[SD_MAX_OPEN_FILES];

static int sdIoMkdir(void*, const char* path) {
  if (SD.exists(path)) return 0;
  return SD.mkdir(path) ? 0 : -1;
}

// Opens with FILE_APPEND. On ESP32 FILE_WRITE is "w" and truncates, which would
// destroy every pre-anchor row when a file reopens after the anchor rename.
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

// wifi_obs and ble_obs are the high-rate files. gps_track gets a smaller
// buffer because the device has no clean shutdown, and at 1 Hz a 4 KB block
// holds forty seconds of position. The three event records are write-through
// with no buffer.
static uint8_t g_bufWifi[4096];
static uint8_t g_bufTrack[1024];
#if ROOST_CAP_BLE
static uint8_t g_bufBle[4096];
#endif

static RoostSdLog  g_log;
static bool        g_open      = false;
static uint32_t    g_fixSeq    = 0;
static uint32_t    g_lastManifestMs = 0;
// Holds /bscope-TAG-boot-BOOT-K, which needs all 32 bytes at the largest u32
// boot_count. Keep it above 32.
static char        g_dir[40]   = "";
static bool        g_named     = false;
static bool        g_ended     = false;

// Watermarks for the degradation events in spec 6.4. reportDegradation()
// compares them on the manifest cadence, so no device_event goes out through
// the write that just failed.
static uint32_t    g_seenStorageErrors = 0;
static uint32_t    g_seenOverflowRows  = 0;
static uint32_t    g_seenQueueDrops    = 0;
static bool        g_inStorageError    = false;

// A session opens under its boot number, because rows precede the clock anchor
// and the date is unknown at first write. roostSessionAnchor() renames it to
// /bscope-TAG-YYMMDD-N once time anchors. An unanchored session keeps the boot
// name for good, with clock_source=none in its manifest.
//
// boot_count comes from NVS and survives a reflash, so each boot gets its own
// directory and manifest. The tag goes before "boot" because boot_count counts
// per device and two units collide on it alone. An empty tag gives
// /bscope-boot-N. Spec 6.2.
#define ROOST_DIR_BOOT_PREFIX "/" LOG_PREFIX
#define MANIFEST_SNAPSHOT_MS 15000

// Board revision for the manifest. A board config without a revision reports
// "unknown".
#ifndef HW_REVISION
#define HW_REVISION "unknown"
#endif

bool roostSessionOpen()      { return g_open; }
const char* roostSessionDir(){ return g_dir; }
uint32_t roostFixSeq()       { return g_fixSeq; }
bool roostHasFix()           { return gpsHasFix; }

// The generated header derives the file list from this board's capability
// macros. A board declares capabilities and never lists its files.
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

  // The shared renderer spells every key, formats every timestamp and decides
  // every null. This device supplies the values.
  RoostSessionInfo info;
  memset(&info, 0, sizeof(info));
  info.deviceModel  = "birdoscope_analyze";
  info.deviceSerial = coreDeviceSerial();
  info.hwRevision   = HW_REVISION;
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
  // Null because this device applies no dedup to the log, and the file holds
  // every repeat observation. The cooldown ring gates only the display and the
  // detection JSON.
  info.dedupPolicy  = nullptr;
  info.storageTier  = "sd";

  // observations_suppressed is a true zero for the same reason. The two queue
  // drop counts are the only loss the writer cannot see, entries discarded
  // before the drain reached them.
  roostSessionCounters(&g_log, &info.counters, 0,
                       coreQueueDrops + coreBleQueueDrops);

  info.files    = g_decls;
  info.numFiles = g_declCount;

  // device_diagnostics holds counters the contract does not define, as manifest
  // v2 allows. It splits loss into queue drops, voided rows and buffer
  // overflows. queue_depth_max against queue_size tells an undersized queue
  // from a blocked drain. The voided total sums the writer's per-record
  // counts.
  uint32_t voided = 0;
  for (int i = 0; i < ROOST_REC_COUNT; i++)
    voided += roostSdRowsVoided(&g_log, (RoostRecord)i);

  // frames_seen and mgmt_* are the only counts of traffic the matcher rejected.
  // They tell a subtype the radio never received from one it received and did
  // not match.
  char diag[768];
  const int dn = snprintf(diag, sizeof(diag),
           "\"queue_drops\":%u,\"rows_voided\":%u,\"row_buffer_overflows\":%u,"
           "\"wifi_obs_written\":%u,\"wifi_obs_voided\":%u,"
           "\"queue_depth_max\":%u,\"queue_size\":%u,"
           "\"frames_seen\":%u,\"frames_candidate\":%u,"
           "\"mgmt_probe_req_seen\":%u,\"mgmt_probe_req_matched\":%u,"
           "\"mgmt_probe_resp_seen\":%u,\"mgmt_probe_resp_matched\":%u,"
           "\"mgmt_beacon_seen\":%u,\"mgmt_beacon_matched\":%u,"
           "\"ble_adv_seen\":%u,\"ble_adv_matched\":%u,"
           "\"ble_obs_written\":%u,\"ble_obs_voided\":%u,"
           "\"ble_queue_drops\":%u,\"ble_queue_depth_max\":%u,"
           "\"ble_queue_size\":%u",
           (unsigned)coreQueueDrops, (unsigned)voided,
           (unsigned)st.overflowRows,
           (unsigned)roostSdRowsWritten(&g_log, ROOST_REC_WIFI_OBS),
           (unsigned)roostSdRowsVoided(&g_log, ROOST_REC_WIFI_OBS),
           (unsigned)coreQueueDepthMax, (unsigned)coreAlertQueueSize(),
           (unsigned)coreSeenFrames, (unsigned)coreCandidateFrames,
           (unsigned)coreMgmtSeen[4],  (unsigned)coreMgmtMatched[4],
           (unsigned)coreMgmtSeen[5],  (unsigned)coreMgmtMatched[5],
           (unsigned)coreMgmtSeen[8],  (unsigned)coreMgmtMatched[8],
           (unsigned)coreBleReports, (unsigned)coreBleMatched,
           (unsigned)roostSdRowsWritten(&g_log, ROOST_REC_BLE_OBS),
           (unsigned)roostSdRowsVoided(&g_log, ROOST_REC_BLE_OBS),
           (unsigned)coreBleQueueDrops, (unsigned)coreBleQueueDepthMax,
           (unsigned)coreBleQueueSize());
  // A truncated block is invalid JSON and voids the whole manifest, so the
  // manifest omits a block that does not fit.
  if (dn < 0 || (size_t)dn >= sizeof(diag)) {
    dualPrintln("[roost] device_diagnostics did not fit - omitting the block");
    info.deviceDiagnostics = nullptr;
  } else {
    info.deviceDiagnostics = diag;
  }

  static char json[4096];
  const size_t n = roostSessionJson(json, sizeof(json), &info);
  // Render first, write second. A truncated manifest lists files and columns
  // the session lacks and fails validation for the whole capture, so a manifest
  // that does not fit leaves the previous snapshot in place.
  if (!n) {
    dualPrintln("[roost] manifest did not fit - keeping the previous one");
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/manifest.json", g_dir);
  // FILE_WRITE truncates. Each write replaces the manifest snapshot, unlike the
  // appended record files.
  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    // Reported once per session. The previous snapshot stays on the card.
    static bool reported = false;
    if (!reported) {
      reported = true;
      dualPrintln("[roost] manifest open failed - snapshot not written");
      roostLogDeviceEvent(ROOST_COMP_SYS, "storage_error", 1, "manifest open");
    }
    return;
  }
  f.write((const uint8_t*)json, n);
  f.flush();
  f.close();
}

// Spec 6.4 makes a write failure or a full buffer a device_event as well as a
// counter. This runs on the snapshot cadence after the flush, so the
// write-through event file reports a failing card outside the write that
// failed.
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

  // With the card healthy, the device loses an observation when the row buffer
  // has no room or a queue fills before the drain reaches it.
  const uint32_t overflow = st.overflowRows;
  const uint32_t drops    = coreQueueDrops + coreBleQueueDrops;
  if (overflow > g_seenOverflowRows || drops > g_seenQueueDrops) {
    roostLogDeviceEvent(ROOST_COMP_SYS, "buffer_full",
                        (overflow - g_seenOverflowRows) + (drops - g_seenQueueDrops),
                        overflow > g_seenOverflowRows ? "row buffer" : "queue");
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
#if ROOST_CAP_BLE
  roostSdAttachBuffer(&g_log, ROOST_REC_BLE_OBS,   g_bufBle,   sizeof(g_bufBle));
#endif

  // Each session takes a directory that does not exist yet. boot_count
  // restarts at 1 when an erase clears NVS, and an earlier unanchored session
  // may already hold the name.
  const unsigned boot = (unsigned)coreBootCount();
  const char* tag = coreDeviceTag();
  const char* sep = tag[0] ? "-" : "";
  bool free_ = false;
  for (unsigned k = 0; k < 100 && !free_; k++) {
    if (k) snprintf(g_dir, sizeof(g_dir), ROOST_DIR_BOOT_PREFIX "%s%sboot-%u-%u",
                    tag, sep, boot, k);
    else   snprintf(g_dir, sizeof(g_dir), ROOST_DIR_BOOT_PREFIX "%s%sboot-%u",
                    tag, sep, boot);
    free_ = !SD.exists(g_dir);
  }
  // With every name taken, no session opens. Two boots in one directory leave
  // their rows unattributable. Spec 6.2.
  if (!free_) {
    dualPrintln("[roost] no free session name - refusing to share one");
    return false;
  }
  if (!roostSdOpenSession(&g_log, g_dir, g_decls, g_declCount)) {
    dualPrintln("[roost] session open failed - no rows will be written");
    return false;
  }
  g_open = true;

  // Duplicate component ids need a runtime check, because the preprocessor
  // cannot compare string literals.
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

  char want[40];
  if (!coreSessionDirName(want, sizeof(want))) {
    // No dated name is free today, so the session keeps its boot name. The
    // manifest records the anchor either way. Spec 6.2.
    dualPrintln("[roost] no free dated name today - keeping the boot name");
    roostLogDeviceEvent(ROOST_COMP_SYS, "config_error", 0, "no free session name");
    g_named = true;                   // do not retry on every tick
    writeManifest();
    return;
  }

  // FatFs cannot rename an open object, so the files close first and reopen
  // after. Reopening appends without a second header, and each record type
  // stays one continuous file across the anchor.
  roostSdCloseSession(&g_log);
  if (SD.rename(g_dir, want)) {
    snprintf(g_dir, sizeof(g_dir), "%s", want);
    g_named = true;
  } else {
    // Capture continues under the provisional name. The manifest still records
    // the anchor, so the session stays placeable.
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
  // Reads the writer's count, the same one the manifest reports.
  if (fixes) *fixes = roostSdRowsWritten(&g_log, ROOST_REC_GPS_TRACK);
}

// ============================================================
// ROW WRITERS
// ============================================================

// Fills the columns every record shares. timestamp_utc stays empty until the
// clock anchors, and the manifest's anchor triple places the row from uptime_ms
// afterwards.
//
// fix_seq sits between uptime_ms and cap_component in every record that has
// it, so this function writes it. RoostRow cannot go back to a column it has
// passed, and gps_track requires fix_seq.
static void setCommon(RoostRow* w, uint8_t iTs, uint8_t iUp, uint8_t iFix,
                      uint8_t iComp, uint32_t uptimeMs, RoostComponent comp,
                      uint32_t fixSeq) {
  char ts[24];
  if (coreTimestampAt(uptimeMs, ts, sizeof(ts))) roostRowSetText(w, iTs, ts);
  roostRowSetUInt(w, iUp, uptimeMs);
  if (fixSeq) roostRowSetUInt(w, iFix, fixSeq);
  roostRowSetText(w, iComp, roostComponentId(comp));
}

// A refused row is missing a required column and never reaches the card, which
// downstream cannot tell from a quiet capture. The first refusal per record
// type goes to the serial line as well as the manifest counter.
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
  // A mac column takes the raw bytes. RoostRow refuses text on it, and the
  // record requires mac, so a text write voids the row.
  roostRowSetMac(&w, ROOST_WIFI_OBS_MAC, e.mac);
  roostRowSetEnumByName(&w, ROOST_WIFI_OBS_DETECTION_METHOD, method);
  if (e.frameSubtype[0])
    roostRowSetEnumByName(&w, ROOST_WIFI_OBS_FRAME_SUBTYPE, e.frameSubtype);
  roostRowSetInt(&w, ROOST_WIFI_OBS_RSSI, e.rssi);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_CHANNEL, e.channel);
  // The shared helper derives the band. It leaves the column empty for a
  // channel it cannot place.
  const RoostChannelBand cb = roostBandForChannel(e.channel);
  if (cb.known) roostRowSetEnum(&w, ROOST_WIFI_OBS_BAND, cb.band);
  // Written whenever the frame had an SSID element, whatever the alert type. A
  // beacon broadcasts its SSID without a name-bearing alert. Uses the length,
  // since the octets may contain 0x00 and a cloaked name is all zeros. A
  // zero-length element and a missing one both render empty, and ingest tells
  // them apart by frame_subtype (spec 7.1).
  if (e.ssid.len)
    roostRowSetTextN(&w, ROOST_WIFI_OBS_SSID, e.ssid.text, e.ssid.len);
  // Addresses go by position. Roles vary per frame, and the pipeline derives
  // them from type and subtype.
  roostRowSetMac(&w, ROOST_WIFI_OBS_ADDR1, e.addr1);
  roostRowSetMac(&w, ROOST_WIFI_OBS_ADDR2, e.addr2);
  roostRowSetMac(&w, ROOST_WIFI_OBS_ADDR3, e.addr3);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_SEQ, e.seq);
  // Only the flags byte. frame_subtype already holds the type and subtype.
  const uint8_t fc = (uint8_t)e.fcFlags;
  roostRowSetHex(&w, ROOST_WIFI_OBS_FC_FLAGS, &fc, 1);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_FRAME_LEN, e.frameLen);
  if (e.bbFormat[0])
    roostRowSetEnumByName(&w, ROOST_WIFI_OBS_BB_FORMAT, e.bbFormat);

  if (w.unknownEnums)
    roostLogDeviceEvent(ROOST_COMP_SYS, "vocabulary_error", w.unknownEnums, "wifi_obs");
  finishAndAppend(&w, ROOST_REC_WIFI_OBS, row);
}

void roostLogBleObs(const BleObsEntry& e) {
#if ROOST_CAP_BLE
  if (!g_open) return;
  // A full extended payload is 510 hex characters before any other column.
  char row[768];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_BLE_OBS,
                ROOST_BLE_OBS_COLUMNS_MASK);
  setCommon(&w, ROOST_BLE_OBS_TIMESTAMP_UTC, ROOST_BLE_OBS_UPTIME_MS,
            ROOST_BLE_OBS_FIX_SEQ, ROOST_BLE_OBS_CAP_COMPONENT,
            e.uptimeMs, ROOST_COMP_BLE0, g_fixSeq);
  // One row per advertisement, which the registry calls promiscuous.
  roostRowSetEnum(&w, ROOST_BLE_OBS_OBS_MODE, ROOST_OBS_MODE_PROMISCUOUS);
  roostRowSetMac(&w, ROOST_BLE_OBS_MAC, e.mac);
  roostRowSetEnumByName(&w, ROOST_BLE_OBS_ADDR_TYPE, bleAddrTypeName(e.addrType));
  roostRowSetEnumByName(&w, ROOST_BLE_OBS_DETECTION_METHOD, e.method);
  roostRowSetInt(&w, ROOST_BLE_OBS_RSSI, e.rssi);
  if (e.hasTxPower) roostRowSetInt(&w, ROOST_BLE_OBS_TX_POWER, e.txPower);
  uint8_t nameLen = 0;
  const uint8_t* name = bleLocalName(e.payload, e.payloadLen, &nameLen);
  if (name && nameLen)
    roostRowSetTextN(&w, ROOST_BLE_OBS_DEVICE_NAME, (const char*)name, nameLen);
  roostRowSetEnumByName(&w, ROOST_BLE_OBS_PDU_TYPE, e.pduType);
  if (e.phyPrimary[0])
    roostRowSetEnumByName(&w, ROOST_BLE_OBS_PHY_PRIMARY, e.phyPrimary);
  if (e.phySecondary[0])
    roostRowSetEnumByName(&w, ROOST_BLE_OBS_PHY_SECONDARY, e.phySecondary);
  if (e.extended) roostRowSetUInt(&w, ROOST_BLE_OBS_SID, e.sid);
  roostRowSetBool(&w, ROOST_BLE_OBS_ACTIVE_SCAN, 0);   // passive only, spec M9
  // The authoritative payload. Analysis re-derives every decoded column above
  // from it.
  roostRowSetHex(&w, ROOST_BLE_OBS_ADV_DATA_HEX, e.payload, e.payloadLen);

  if (w.unknownEnums)
    roostLogDeviceEvent(ROOST_COMP_SYS, "vocabulary_error", w.unknownEnums, "ble_obs");
  finishAndAppend(&w, ROOST_REC_BLE_OBS, row);
#else
  (void)e;
#endif
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
  // position_source and fix_type go in by name and can drift from the registry.
  // Spec 6.3 requires reporting the miss, since an unexplained empty column
  // reads as nothing to record.
  if (w.unknownEnums)
    roostLogDeviceEvent(ROOST_COMP_SYS, "vocabulary_error", w.unknownEnums, "gps_track");
  finishAndAppend(&w, ROOST_REC_GPS_TRACK, row);
}

// Last value written per component and setting. Spec 6.4 says a setting
// re-applied to its current value writes nothing, so a menu that reasserts its
// state adds no rows. The key includes the component because two components
// report the same setting with different values.
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
  // A setting seen for the first time counts as a change, which gives
  // config_change its boot row.
  return false;
}

// Call only after the row reaches the card, so the table holds only values the
// file contains.
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

// The channel plan and vendor mask render their values, so both can refuse. An
// empty value means the setting does not apply on this build (spec L3), so a
// refusal writes a config_error event in place of the row.
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

// Logs under sys, since the setting selects among radios. Spec L4.
void roostLogConfigRadioMode() {
  roostLogConfigChange(ROOST_COMP_SYS, "radio_mode", radioModeName(coreRadioMode));
}

void roostLogConfigBoot() {
  char buf[96];
  // Writes every setting in the vocabulary in registry order, empty where it
  // does not apply. The boot dump then has the same length on every device, and
  // an empty value always means not applicable. Spec L5.
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
  roostValueAddKeyUInt(&f, "infra_dedupe_ms", coreInfraDedupeMs());
  if (roostValueDone(&f))
    roostLogConfigChange(ROOST_COMP_WIFI0, "filters", buf);
  else
    roostLogDeviceEvent(ROOST_COMP_WIFI0, "config_error", 0, "filters too long");
#if ROOST_CAP_BLE
  roostLogConfigChange(ROOST_COMP_BLE0, "obs_mode", "promiscuous");
  roostValueBegin(&f, buf, sizeof(buf));
  roostValueAddKeyUInt(&f, "infra_dedupe_ms", coreInfraDedupeMs());
  if (roostValueDone(&f))
    roostLogConfigChange(ROOST_COMP_BLE0, "filters", buf);
  else
    roostLogDeviceEvent(ROOST_COMP_BLE0, "config_error", 0, "filters too long");
#endif
  roostLogConfigRadioMode();
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
  // The record requires event_kind, so an unknown kind voids the row. The
  // serial line is the only channel left to report it, since a device_event
  // here would recurse.
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
void roostLogBleObs(const BleObsEntry&) {}
void roostLogGpsFix() {}
void roostLogConfigChange(RoostComponent, const char*, const char*) {}
void roostLogConfigBoot() {}
void roostLogConfigChannels() {}
void roostLogConfigVendorMask() {}
void roostLogConfigRadioMode() {}
void roostLogDeviceEvent(RoostComponent, const char*, uint32_t, const char*) {}
void roostLogOperatorMark() {}
void roostSessionStats(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
  if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0;
}

#endif  // USE_SD
