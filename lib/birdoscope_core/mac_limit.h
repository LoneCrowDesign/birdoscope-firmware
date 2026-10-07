// mac_limit.h - per-MAC time-windowed row cap, shared by both radios.
//
// Allows one row per MAC per window. When every probed slot holds another MAC,
// the limit evicts the least recently seen of them and counts the eviction if
// that MAC was still inside its window. An undersized table logs extra rows and
// never drops a device.
//
// The caller owns the storage and attaches it, so a radio holds its tables only
// while it runs. The 802.11 RX callback calls macLimitAllow() from IRAM, so a
// limit that callback uses needs internal RAM, never PSRAM.
//
// See decision_record.md D12, and spec O6 and M8 for the callers.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Bytes of storage a limit of `slots` entries needs.
#define MAC_LIMIT_BYTES(slots) ((size_t)(slots) * 11u)

typedef struct {
  uint32_t* at;          // nullptr while detached
  uint8_t (*mac)[6];
  uint8_t*  used;
  uint16_t  slots;
  uint32_t  windowMs;
  volatile uint32_t evictions;   // displaced entries still inside their window
} MacLimit;

// Points `l` at `block` and clears it. `block` must hold MAC_LIMIT_BYTES(slots)
// and be 4-byte aligned. A null `block` or zero `slots` leaves `l` detached.
void macLimitAttach(MacLimit* l, void* block, uint16_t slots, uint32_t windowMs);

// Detaches `l` and returns its block for the caller to free.
void* macLimitDetach(MacLimit* l);

// Clears every entry and the eviction count. Does nothing while detached.
void macLimitReset(MacLimit* l);

// True when this MAC is due a row. `nowMs` is millis(). Returns false while
// detached, so a failed allocation suppresses rows and never lets an uncapped
// stream through.
bool macLimitAllow(MacLimit* l, const uint8_t* mac, uint32_t nowMs);
