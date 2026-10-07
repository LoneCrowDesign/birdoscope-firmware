#include "mac_limit.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define MAC_LIMIT_IRAM IRAM_ATTR
#else
#define MAC_LIMIT_IRAM
#endif

// Hashes mainly the low three bytes, which vary within a vendor. macLimitAllow()
// probes linearly from the result.
static inline size_t MAC_LIMIT_IRAM macHash(const uint8_t* mac, size_t slots) {
  uint32_t h = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  h ^= (uint32_t)mac[0] << 7;
  h *= 2654435761u;
  return (size_t)(h % slots);
}

// Wrap-safe, because millis() rolls over about every 49 days and a capture can
// run longer.
static inline bool MAC_LIMIT_IRAM elapsedAtLeast(uint32_t now, uint32_t then,
                                                 uint32_t span) {
  return (uint32_t)(now - then) >= span;
}

void macLimitAttach(MacLimit* l, void* block, uint16_t slots, uint32_t windowMs) {
  l->slots     = slots;
  l->windowMs  = windowMs;
  l->evictions = 0;
  uint8_t* b = (uint8_t*)block;
  if (!b || !slots) {
    l->at = nullptr; l->mac = nullptr; l->used = nullptr;
    return;
  }
  l->at   = (uint32_t*)b;
  l->mac  = (uint8_t (*)[6])(b + (size_t)slots * 4);
  l->used = b + (size_t)slots * 10;
  memset(b, 0, MAC_LIMIT_BYTES(slots));
}

void* macLimitDetach(MacLimit* l) {
  void* b = l->at;
  l->at = nullptr; l->mac = nullptr; l->used = nullptr;
  return b;
}

void macLimitReset(MacLimit* l) {
  l->evictions = 0;
  if (l->used) memset(l->used, 0, l->slots);
}

bool MAC_LIMIT_IRAM macLimitAllow(MacLimit* l, const uint8_t* mac, uint32_t nowMs) {
  if (!l->used) return false;
  const size_t start = macHash(mac, l->slots);
  // Probe length cap, since this runs on every frame or advertisement.
  const size_t kProbe = 8;

  size_t   freeIdx   = l->slots;
  size_t   oldestIdx = start;
  uint32_t oldestAge = 0;

  for (size_t i = 0; i < kProbe; i++) {
    const size_t idx = (start + i) % l->slots;
    if (!l->used[idx]) {
      if (freeIdx == l->slots) freeIdx = idx;
      continue;
    }
    if (memcmp(l->mac[idx], mac, 6) == 0) {
      if (elapsedAtLeast(nowMs, l->at[idx], l->windowMs)) {
        l->at[idx] = nowMs;
        return true;
      }
      return false;
    }
    const uint32_t age = (uint32_t)(nowMs - l->at[idx]);
    if (age >= oldestAge) { oldestAge = age; oldestIdx = idx; }
  }

  // A MAC absent from the probed slots is new, so it gets a row.
  size_t idx;
  if (freeIdx != l->slots) {
    idx = freeIdx;
  } else {
    idx = oldestIdx;
    if (!elapsedAtLeast(nowMs, l->at[idx], l->windowMs))
      l->evictions = l->evictions + 1;
  }
  memcpy(l->mac[idx], mac, 6);
  l->at[idx]   = nowMs;
  l->used[idx] = 1;
  return true;
}
