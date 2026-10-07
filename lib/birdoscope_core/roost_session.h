// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Device glue for the roost session format, covering the Arduino SD backend,
// the session lifecycle, one writer per record type, and the manifest.
//
// The contract is vendor/jellybeans/roost_logging/docs/design_spec.md, and the
// buffering lives in the shared roost_sdlog.h. This file supplies what only
// this device can, its card pins, its clock and its setting names.
//
// A session is a directory. Rows precede the clock anchor, so the session
// opens under a provisional name and renames itself once the anchor lands.
// Each record type stays one continuous file across the rename.
#pragma once

#include <stdint.h>
#include <stddef.h>

// Include before core.h, which pulls in the generated registry and hard-errors
// on any capability this board has not declared.
#include "board_config.h"
#include "core.h"

// Opens the session directory and every declared file. Call once, after the
// card has mounted. Returns false if the card is absent or unusable, and every
// writer below then returns without writing.
bool roostSessionBegin();

// Renames the provisional directory to /bscope-TAG-YYMMDD-N and records the
// clock anchor triple in the manifest. Call once, the moment time anchors. Does
// nothing if the session never opened or time never anchors.
void roostSessionAnchor();

// Rewrites the manifest on a snapshot cadence, so a session that loses power
// still leaves the most recent counters. Call from loop().
void roostSessionTick();

// Flushes and closes every file. A power cut skips this, and
// roostSessionTick() covers that case.
void roostSessionEnd();

bool roostSessionOpen();          // a session directory exists and is writable
const char* roostSessionDir();    // current directory name, provisional or final

// --- Record writers --------------------------------------------------------
//
// Each builds its row through RoostRow and hands it to the shared writer.
// RoostRow enforces canonical order, quoting, MAC case and required fields.

// One Wi-Fi observation, matched or survey. The row's uptime_ms is the frame's
// receive time, since loop() drains the queue and can fall far behind under
// load.
void roostLogWifiObs(const AlertEntry& e, const char* method);

// One BLE advertisement, under the same timing rule. Writes nothing on a build
// without ROOST_CAP_BLE.
void roostLogBleObs(const BleObsEntry& e);

// One GPS fix, written whether or not a radio observed anything. A gap in
// observations against a continuous track is a field observation, and the same
// gap with no track is an unknown.
void roostLogGpsFix();

// The current fix sequence, or 0 before the first fix. Observation rows carry
// it in place of the position.
uint32_t roostFixSeq();
bool     roostHasFix();

// One runtime setting taking a new value. Re-applying a setting to its current
// value writes nothing, so a settings screen cannot flood a file meant to be
// sparse.
//
// `component` names what the setting governs, so a reader can tell which radio
// changed. Pass `sys` only for a setting that spans radios. See
// docs/roost_logging.md, "Component attribution".
void roostLogConfigChange(RoostComponent component,
                          const char* setting, const char* value);

// Writes every runtime setting with its boot value, so the file stands alone
// from its first row.
void roostLogConfigBoot();

// The settings this build can change mid-session. The setters call these, so
// every path to a setter logs the change. The channel plan and vendor mask
// render a value that can refuse, see
// vendor/jellybeans/roost_logging/runtime/roost_value.h.
void roostLogConfigChannels();
void roostLogConfigVendorMask();
void roostLogConfigRadioMode();

// One device-level event, such as boot, clock anchor, storage failure or a full
// buffer. `detail` may be null. The device_event file is write-through, so an
// event explaining a failure reaches the card before the failure gets worse.
//
// `component` names what the event is about, which may differ from what
// noticed it.
void roostLogDeviceEvent(RoostComponent component,
                         const char* kind, uint32_t count, const char* detail);

// The operator flagged a place. The row's existence is the information.
void roostLogOperatorMark();

// Counters for the status line and the manifest.
void roostSessionStats(uint32_t* rowsWritten, uint32_t* rowsDropped,
                       uint32_t* worstFlushMs, uint32_t* fixes);
