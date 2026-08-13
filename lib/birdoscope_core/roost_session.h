// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Device glue for the roost session format: the Arduino SD backend, the
// session lifecycle, one writer per record type, and the manifest.
//
// The contract is vendor/jellybeans/roost_logging/docs/design_spec.md and the
// buffering lives in the shared roost_sdlog.h. Everything here is what only this
// device
// can supply: which pins the card is on, where its clock comes from, and what
// its configuration is called.
//
// A session is a directory. It opens under a provisional name because rows
// precede the clock anchor, and is renamed once the anchor lands, so each
// record type is one continuous file rather than a pre/post pair to stitch.
#pragma once

#include <stdint.h>
#include <stddef.h>

// Before core.h, which pulls in the generated registry and hard-errors on any
// capability this board has not declared.
#include "board_config.h"
#include "core.h"

// Opens the session directory and every declared file. Call once, after the
// card has mounted. Returns false if the card is absent or unusable, in which
// case every append counts as a drop rather than faulting.
bool roostSessionBegin();

// Renames the provisional directory to /bscope-M-D-YY-N and records the clock
// anchor triple in the manifest. Call once, the moment time anchors. A no-op
// if the session never opened or time never anchors.
void roostSessionAnchor();

// Rewrites the manifest on a snapshot cadence, so a session that loses power
// still leaves the most recent counters. Call from loop().
void roostSessionTick();

// Flushes and closes every file. Not reached on a power cut, which is the
// point of the snapshot cadence above.
void roostSessionEnd();

bool roostSessionOpen();          // a session directory exists and is writable
const char* roostSessionDir();    // current directory name, provisional or final

// --- Record writers --------------------------------------------------------
//
// Each builds its row through RoostRow and hands it to the shared writer, so
// canonical order, quoting, MAC case and required-field enforcement are the
// row builder's job rather than each call site's.

// One matched Wi-Fi observation. `uptimeMs` is when the frame was received,
// not when this call was made: the queue is drained from loop() and can run
// arbitrarily far behind under load.
void roostLogWifiObs(const AlertEntry& e, const char* method);

// One GPS fix, written whether or not anything was observed. A gap in
// observations against a continuous track is a field observation; the same gap
// with no track is an unknown.
void roostLogGpsFix();

// The current fix sequence, or 0 before the first fix. Observation rows carry
// it rather than repeating the position.
uint32_t roostFixSeq();
bool     roostHasFix();

// One runtime setting taking a new value. Re-applying a setting to its current
// value writes nothing, or a settings screen floods a file meant to be sparse.
//
// `component` is what the setting governs, never `sys` by default: a reader
// cannot tell which radio changed otherwise. See docs/roost_logging.md,
// "Component attribution".
void roostLogConfigChange(RoostComponent component,
                          const char* setting, const char* value);

// Writes every runtime setting with its boot value, so the file is
// self-contained from its first row.
void roostLogConfigBoot();

// The two settings this build can change mid-session. Called from inside the
// setters, not from the menu, so a new path to a setter cannot reintroduce the
// gap. Both render a value that can refuse, which is why they are not plain
// roostLogConfigChange() calls. See
// vendor/jellybeans/roost_logging/runtime/roost_value.h.
void roostLogConfigChannels();
void roostLogConfigVendorMask();

// One device-level event: boot, clock anchor, storage failure, buffer full.
// `detail` may be null. Write-through, so an event explaining a failure
// reaches the card before the failure gets worse.
//
// `component` is what the event is about, which is not always what noticed it.
void roostLogDeviceEvent(RoostComponent component,
                         const char* kind, uint32_t count, const char* detail);

// The operator flagged a place. The row's existence is the information.
void roostLogOperatorMark();

// Counters for the status line and the manifest.
void roostSessionStats(uint32_t* rowsWritten, uint32_t* rowsDropped,
                       uint32_t* worstFlushMs, uint32_t* fixes);
