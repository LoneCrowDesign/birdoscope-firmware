// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host tests for the BLE decoding and per-MAC limiting modules.
//
//   pio test -e native
//
// Covers AD structure walking, the Axon serial rule, the manufacturer-data
// primitives and the per-MAC rate limiter, none of which need a radio.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unity.h>

#include "ble_ad.h"
#include "mac_limit.h"

void setUp(void) {}
void tearDown(void) {}

// --- Rate limiting --------------------------------------------------------

static uint32_t g_limitBlock[(MAC_LIMIT_BYTES(64) + 3) / 4];

static MacLimit newLimit(uint32_t windowMs, uint16_t slots = 64) {
  MacLimit l = {};
  macLimitAttach(&l, g_limitBlock, slots, windowMs);
  return l;
}

static void test_ratelimit_first_seen_allowed() {
  MacLimit rl = newLimit(10000);
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 1000));
}

static void test_ratelimit_suppresses_inside_window() {
  MacLimit rl = newLimit(10000);
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 1000));
  TEST_ASSERT_FALSE(macLimitAllow(&rl, mac, 5000));
  TEST_ASSERT_FALSE(macLimitAllow(&rl, mac, 10999));
}

// A device stays loggable across windows, so path-loss fitting gets RSSI from
// the whole pass.
static void test_ratelimit_reallows_after_window() {
  MacLimit rl = newLimit(10000);
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 1000));
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 11000));
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 21000));
}

// millis() wraps every ~49 days and a capture can outlive that, so the window
// check must hold across the wrap.
static void test_ratelimit_survives_millis_wrap() {
  MacLimit rl = newLimit(10000);
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  const uint32_t nearMax = 0xFFFFFFFFu - 5000;
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, nearMax));
  TEST_ASSERT_FALSE(macLimitAllow(&rl, mac, nearMax + 1000));
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, nearMax + 11000));  // wrapped
}

static void test_ratelimit_distinguishes_macs() {
  MacLimit rl = newLimit(10000);
  const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t b[6] = {1, 2, 3, 4, 5, 7};
  TEST_ASSERT_TRUE(macLimitAllow(&rl, a, 1000));
  TEST_ASSERT_TRUE(macLimitAllow(&rl, b, 1000));
  TEST_ASSERT_FALSE(macLimitAllow(&rl, a, 2000));
}

// A detached limit rejects every row, so a failed allocation never lets an
// uncapped stream through.
static void test_ratelimit_detached_refuses() {
  MacLimit rl = {};
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  TEST_ASSERT_FALSE(macLimitAllow(&rl, mac, 1000));
  macLimitReset(&rl);   // no-op, no crash
}

// A full probe window evicts its oldest entry. `evictions` counts it only while
// that entry is still inside its window.
static void test_ratelimit_counts_live_evictions() {
  MacLimit rl = newLimit(10000, 8);   // 8 slots, so the probe covers the table
  uint8_t mac[6] = {9, 9, 9, 0, 0, 0};
  for (uint8_t i = 0; i < 8; i++) { mac[5] = i; macLimitAllow(&rl, mac, 1000); }
  TEST_ASSERT_EQUAL_UINT32(0, rl.evictions);
  mac[5] = 100;
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 2000));
  TEST_ASSERT_EQUAL_UINT32(1, rl.evictions);
  mac[5] = 101;
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 30000));   // every entry has expired
  TEST_ASSERT_EQUAL_UINT32(1, rl.evictions);
}

// Reset forgets every MAC, which opens a fresh survey window per spec O6.
static void test_ratelimit_reset_forgets() {
  MacLimit rl = newLimit(10000);
  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 1000));
  macLimitReset(&rl);
  TEST_ASSERT_TRUE(macLimitAllow(&rl, mac, 2000));
}

// --- BLE ------------------------------------------------------------------

// 4D 03 02 then nine printable ASCII bytes.
static const uint8_t kAxonMfr[] = {0x4D, 0x03, 0x02, 'X', '8', '7', '1',
                                   '2', '3', '4', '5', '6', 0xAB, 0xCD};

static void test_ble_axon_serial_match() {
  char out[10];
  TEST_ASSERT_EQUAL_UINT(9, bleAxonSerial(kAxonMfr, sizeof(kAxonMfr), out,
                                             sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("X87123456", out);
}

static void test_ble_axon_serial_wrong_prefix() {
  uint8_t bad[sizeof(kAxonMfr)];
  memcpy(bad, kAxonMfr, sizeof(kAxonMfr));
  bad[1] = 0x04;
  char out[10];
  TEST_ASSERT_EQUAL_UINT(0, bleAxonSerial(bad, sizeof(bad), out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_ble_axon_serial_too_short() {
  char out[10];
  TEST_ASSERT_EQUAL_UINT(0, bleAxonSerial(kAxonMfr, 8, out, sizeof(out)));
}

// A non-printable byte means the prefix matched something that is not a
// serial.
static void test_ble_axon_serial_rejects_nonprintable() {
  uint8_t bad[sizeof(kAxonMfr)];
  memcpy(bad, kAxonMfr, sizeof(kAxonMfr));
  bad[5] = 0x01;
  char out[10];
  TEST_ASSERT_EQUAL_UINT(0, bleAxonSerial(bad, sizeof(bad), out, sizeof(out)));
}

// bleAddrTypeName maps identity-resolved types to the same two names.
static void test_ble_addr_type_names() {
  TEST_ASSERT_EQUAL_STRING("public", bleAddrTypeName(BLE_ADDR_PUBLIC));
  TEST_ASSERT_EQUAL_STRING("random", bleAddrTypeName(BLE_ADDR_RANDOM));
  TEST_ASSERT_EQUAL_STRING("public", bleAddrTypeName(BLE_ADDR_PUBLIC_ID));
  TEST_ASSERT_EQUAL_STRING("random", bleAddrTypeName(BLE_ADDR_RANDOM_ID));
}

// [len][type][data...], len covering the type byte.
static const uint8_t kAdPayload[] = {
    0x02, 0x01, 0x06,                    // flags
    0x05, 0x09, 'T', 'e', 's', 't',      // complete local name
    0x04, 0xFF, 0x4D, 0x03, 0x02,        // manufacturer specific
};

static void test_ble_ad_walk() {
  size_t cursor = 0;
  BleAdStructure ad;
  TEST_ASSERT_TRUE(bleNextAdStructure(kAdPayload, sizeof(kAdPayload), &cursor, &ad));
  TEST_ASSERT_EQUAL_UINT8(0x01, ad.type);
  TEST_ASSERT_EQUAL_UINT8(1, ad.len);
  TEST_ASSERT_TRUE(bleNextAdStructure(kAdPayload, sizeof(kAdPayload), &cursor, &ad));
  TEST_ASSERT_EQUAL_UINT8(0x09, ad.type);
  TEST_ASSERT_EQUAL_UINT8(4, ad.len);
  TEST_ASSERT_TRUE(bleNextAdStructure(kAdPayload, sizeof(kAdPayload), &cursor, &ad));
  TEST_ASSERT_EQUAL_UINT8(0xFF, ad.type);
  TEST_ASSERT_FALSE(bleNextAdStructure(kAdPayload, sizeof(kAdPayload), &cursor, &ad));
}

static void test_ble_find_ad_type() {
  uint8_t len = 0;
  const uint8_t *d = bleFindAdType(kAdPayload, sizeof(kAdPayload), 0xFF, &len);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL_UINT8(3, len);
  TEST_ASSERT_EQUAL_UINT8(0x4D, d[0]);
  TEST_ASSERT_NULL(bleFindAdType(kAdPayload, sizeof(kAdPayload), 0x16, &len));
  TEST_ASSERT_EQUAL_UINT8(0, len);
}

// A complete name wins over a shortened one in the same payload.
static void test_ble_local_name_prefers_complete() {
  const uint8_t p[] = {0x03, 0x08, 'A', 'b', 0x04, 0x09, 'A', 'b', 'c'};
  uint8_t len = 0;
  const uint8_t *n = bleLocalName(p, sizeof(p), &len);
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_EQUAL_UINT8(3, len);
  TEST_ASSERT_EQUAL_UINT8('c', n[2]);
  len = 0;
  n = bleLocalName(p, 4, &len);
  TEST_ASSERT_NOT_NULL(n);
  TEST_ASSERT_EQUAL_UINT8(2, len);
  TEST_ASSERT_NULL(bleLocalName(kAdPayload + 9, 5, &len));
}

// The walker stops at a declared length past the end without reading out of
// bounds.
static void test_ble_ad_walk_rejects_overrun() {
  uint8_t bad[] = {0x02, 0x01, 0x06, 0x20, 0x09, 'x'};
  size_t cursor = 0;
  BleAdStructure ad;
  TEST_ASSERT_TRUE(bleNextAdStructure(bad, sizeof(bad), &cursor, &ad));
  TEST_ASSERT_FALSE(bleNextAdStructure(bad, sizeof(bad), &cursor, &ad));
}

// --- Manufacturer-data primitives -----------------------------------------

static void test_mfr_company_id_is_little_endian() {
  const uint8_t axon[] = {0x4D, 0x03, 0x02, 'X'};
  TEST_ASSERT_EQUAL_INT(0x034D, bleMfrCompanyId(axon, sizeof(axon)));
  const uint8_t xuntong[] = {0xC8, 0x09, 0x00};
  TEST_ASSERT_EQUAL_INT(0x09C8, bleMfrCompanyId(xuntong, sizeof(xuntong)));
}

static void test_mfr_company_id_needs_two_bytes() {
  const uint8_t one[] = {0x4D};
  TEST_ASSERT_EQUAL_INT(-1, bleMfrCompanyId(one, sizeof(one)));
  TEST_ASSERT_EQUAL_INT(-1, bleMfrCompanyId(nullptr, 8));
}

static void test_mfr_ascii_id_extracts_at_offset() {
  char out[10];
  TEST_ASSERT_EQUAL_UINT(9, bleMfrAsciiId(kAxonMfr, sizeof(kAxonMfr), 3, 9,
                                          out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("X87123456", out);
}

// The printability check limits false positives, so a non-printable byte fails
// the whole extraction.
static void test_mfr_ascii_id_rejects_nonprintable() {
  uint8_t bad[sizeof(kAxonMfr)];
  memcpy(bad, kAxonMfr, sizeof(kAxonMfr));
  bad[6] = 0x7F;
  char out[10];
  TEST_ASSERT_EQUAL_UINT(0, bleMfrAsciiId(bad, sizeof(bad), 3, 9, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_mfr_ascii_id_refuses_a_short_field() {
  char out[10];
  TEST_ASSERT_EQUAL_UINT(0, bleMfrAsciiId(kAxonMfr, 8, 3, 9, out, sizeof(out)));
}

// cap must exceed idLen, or the terminator has nowhere to go.
static void test_mfr_ascii_id_refuses_an_exact_fit_buffer() {
  char out[9];
  TEST_ASSERT_EQUAL_UINT(0, bleMfrAsciiId(kAxonMfr, sizeof(kAxonMfr), 3, 9,
                                          out, sizeof(out)));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ratelimit_first_seen_allowed);
  RUN_TEST(test_ratelimit_suppresses_inside_window);
  RUN_TEST(test_ratelimit_reallows_after_window);
  RUN_TEST(test_ratelimit_survives_millis_wrap);
  RUN_TEST(test_ratelimit_distinguishes_macs);
  RUN_TEST(test_ratelimit_detached_refuses);
  RUN_TEST(test_ratelimit_counts_live_evictions);
  RUN_TEST(test_ratelimit_reset_forgets);
  RUN_TEST(test_ble_axon_serial_match);
  RUN_TEST(test_ble_axon_serial_wrong_prefix);
  RUN_TEST(test_ble_axon_serial_too_short);
  RUN_TEST(test_ble_axon_serial_rejects_nonprintable);
  RUN_TEST(test_ble_addr_type_names);
  RUN_TEST(test_ble_ad_walk);
  RUN_TEST(test_ble_find_ad_type);
  RUN_TEST(test_ble_ad_walk_rejects_overrun);
  RUN_TEST(test_ble_local_name_prefers_complete);
  RUN_TEST(test_mfr_company_id_is_little_endian);
  RUN_TEST(test_mfr_company_id_needs_two_bytes);
  RUN_TEST(test_mfr_ascii_id_extracts_at_offset);
  RUN_TEST(test_mfr_ascii_id_rejects_nonprintable);
  RUN_TEST(test_mfr_ascii_id_refuses_a_short_field);
  RUN_TEST(test_mfr_ascii_id_refuses_an_exact_fit_buffer);
  return UNITY_END();
}
