// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host tests for the shared 802.11 management-body walker.
//
// This is parsing attacker-supplied bytes with pointer arithmetic. 
// Two of these pin known failure modes: an FCS bound four bytes too long,
// which appends a bogus trailing element to every retained management
// frame, and a truncating IE list, which turns a dense beacon into a 
// false match.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unity.h>

#include "roost_ie.h"
#include "board_config.h"
#include "roost_registry.h"   // the row builder, for the escaping assertion below

// A beacon: 24-byte header, 12-byte fixed field, then SSID "Test", rates, and
// one vendor element.
static const uint8_t kBeacon[] = {
  0x80, 0x00, 0x00, 0x00,                          // fc: mgmt/beacon
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff,              // addr1
  0x04, 0x17, 0xb6, 0x01, 0x02, 0x03,              // addr2
  0x04, 0x17, 0xb6, 0x01, 0x02, 0x03,              // addr3
  0x00, 0x00,                                      // seq
  0,0,0,0,0,0,0,0,                                 // timestamp
  0x64, 0x00,                                      // beacon interval 100
  0x31, 0x04,                                      // capability info
  0x00, 0x04, 'T', 'e', 's', 't',                  // SSID
  0x01, 0x02, 0x82, 0x84,                          // rates
  0xdd, 0x05, 0x00, 0x50, 0xf2, 0x04, 0x01,        // vendor 00:50:f2 type 04
};
#define BODY      (kBeacon + 24)
#define BODY_LEN  (sizeof(kBeacon) - 24)
#define BEACON    0, 0x8

void test_ssid_is_extracted(void) {
  RoostSsid s;
  roostIeSsidCapture(BODY, BODY_LEN, BEACON, &s);
  TEST_ASSERT_EQUAL_UINT(1, s.present);
  TEST_ASSERT_EQUAL_UINT(4, s.len);
  TEST_ASSERT_EQUAL_STRING("Test", s.text);
}

// The gap this whole file exists to close on Analyze: a beacon carries an SSID
// and the row was leaving the column empty, which reads as "reachable, nothing
// to record" when the truth was "not parsed".
void test_beacon_and_probe_resp_both_carry_ssid(void) {
  RoostSsid s;
  TEST_ASSERT_TRUE(roostIeSubtypeHasIes(0, 0x8));   // beacon
  TEST_ASSERT_TRUE(roostIeSubtypeHasIes(0, 0x5));   // probe resp
  TEST_ASSERT_TRUE(roostIeSubtypeHasIes(0, 0x4));   // probe req
  roostIeSsidCapture(BODY, BODY_LEN, 0, 0x5, &s);
  TEST_ASSERT_EQUAL_UINT(4, s.len);
  // A data frame has no elements at all, and must not be walked as if it did.
  TEST_ASSERT_FALSE(roostIeSubtypeHasIes(2, 0x0));
  roostIeSsidCapture(BODY, BODY_LEN, 2, 0x0, &s);
  TEST_ASSERT_EQUAL_UINT(0, s.present);
  TEST_ASSERT_EQUAL_UINT(0, s.len);
}

// A hidden network broadcasts a zero-length SSID element. The column is empty
// either way, but present separates "the element was there and empty" from
// "there was no element", which is what decides wildcard_probe.
void test_hidden_ssid_is_present_and_empty(void) {
  uint8_t f[sizeof(kBeacon)];
  memcpy(f, kBeacon, sizeof(f));
  f[24 + 12 + 1] = 0;                     // SSID element, length 0
  RoostSsid s;
  roostIeSsidCapture(f + 24, BODY_LEN, BEACON, &s);
  TEST_ASSERT_EQUAL_UINT(1, s.present);   // not the same as no element
  TEST_ASSERT_EQUAL_UINT(0, s.len);
  TEST_ASSERT_EQUAL_STRING("", s.text);
}

// An AP cloaking its name pads the element to its real length with 0x00, so the
// first octet is NUL while the element is present and non-empty. A caller that
// tests text[0] records this as no SSID at all, which is a different fact.
void test_zero_padded_ssid_is_captured_not_dropped(void) {
  uint8_t f[sizeof(kBeacon)];
  memcpy(f, kBeacon, sizeof(f));
  memset(f + 24 + 12 + 2, 0x00, 4);       // 4-octet element, all NUL
  RoostSsid s;
  roostIeSsidCapture(f + 24, BODY_LEN, BEACON, &s);
  TEST_ASSERT_EQUAL_UINT(1, s.present);
  TEST_ASSERT_EQUAL_UINT(4, s.len);       // not 0, which strlen would have said
  TEST_ASSERT_EQUAL_UINT(0, s.text[0]);

  // And it reaches the column as escaped octets rather than an empty cell.
  char buf[256];
  RoostRow w;
  roostRowBegin(&w, buf, sizeof(buf), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetTextN(&w, ROOST_WIFI_OBS_SSID, s.text, s.len);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\\x00\\x00\\x00\\x00"));
}

void test_ie_list_preserves_frame_order(void) {
  char ids[ROOST_IE_IDS_BUF];
  TEST_ASSERT_TRUE(roostIeIdList(BODY, BODY_LEN, BEACON, ids, sizeof(ids)) > 0);
  TEST_ASSERT_EQUAL_STRING("0|1|221", ids);      // never sorted, never deduped

  char vend[ROOST_IE_VEND_BUF];
  TEST_ASSERT_TRUE(roostIeVendorList(BODY, BODY_LEN, BEACON, vend, sizeof(vend)) > 0);
  TEST_ASSERT_EQUAL_STRING("00:50:f2:04", vend);
}

// A prefix is not a shorter fingerprint, it is a different one.
void test_ie_list_refuses_rather_than_truncates(void) {
  char tight[5];                                  // fits "0|1", not "|221"
  TEST_ASSERT_EQUAL_UINT(0, roostIeIdList(BODY, BODY_LEN, BEACON, tight, sizeof(tight)));
  TEST_ASSERT_EQUAL_STRING("", tight);

  char vtight[8];                                 // "00:50:f2:04" needs 12
  TEST_ASSERT_EQUAL_UINT(0, roostIeVendorList(BODY, BODY_LEN, BEACON, vtight, sizeof(vtight)));
  TEST_ASSERT_EQUAL_STRING("", vtight);
}

void test_fixed_fields_are_read(void) {
  uint8_t cap[2];
  uint16_t interval = 0;
  TEST_ASSERT_TRUE(roostIeFixedFields(BODY, BODY_LEN, BEACON, cap, &interval));
  TEST_ASSERT_EQUAL_UINT(100, interval);
  TEST_ASSERT_EQUAL_UINT(0x31, cap[0]);
  TEST_ASSERT_EQUAL_UINT(0x04, cap[1]);

  // A probe request has no fixed field, so there is nothing to read and the
  // caller must not set the columns from whatever happens to be there.
  TEST_ASSERT_FALSE(roostIeFixedFields(BODY, BODY_LEN, 0, 0x4, cap, &interval));
  TEST_ASSERT_FALSE(roostIeFixedFields(BODY, 8, BEACON, cap, &interval));
}

// A declared length running past the body is malformed. Stop, never read on.
void test_walker_rejects_a_length_past_the_body(void) {
  uint8_t bad[sizeof(kBeacon)];
  memcpy(bad, kBeacon, sizeof(bad));
  bad[24 + 12 + 1] = 200;                         // SSID claims 200 bytes
  char ids[ROOST_IE_IDS_BUF];
  TEST_ASSERT_EQUAL_UINT(0, roostIeIdList(bad + 24, BODY_LEN, BEACON, ids, sizeof(ids)));
  RoostSsid s;
  roostIeSsidCapture(bad + 24, BODY_LEN, BEACON, &s);
  TEST_ASSERT_EQUAL_UINT(0, s.present);
}

// Every truncation of a real frame must terminate without reading past the end.
// Run under a sanitiser this is the test that catches an off-by-one.
void test_no_overrun_at_any_body_length(void) {
  char ids[ROOST_IE_IDS_BUF], vend[ROOST_IE_VEND_BUF];
  RoostSsid s;
  uint8_t cap[2];
  uint16_t interval;
  for (size_t n = 0; n <= BODY_LEN; n++) {
    roostIeSsidCapture(BODY, n, BEACON, &s);
    roostIeIdList(BODY, n, BEACON, ids, sizeof(ids));
    roostIeVendorList(BODY, n, BEACON, vend, sizeof(vend));
    roostIeFixedFields(BODY, n, BEACON, cap, &interval);
  }
  TEST_PASS();
}

// The driver's on-air length still counts the FCS the hardware already
// stripped, so parsing to it reads four bytes of checksum as the start of
// another element.
void test_parse_length_removes_the_fcs(void) {
  TEST_ASSERT_EQUAL_UINT(96, roostIeParseLen(100, 100));
  // Only what was actually copied may be parsed, whichever is smaller.
  TEST_ASSERT_EQUAL_UINT(46, roostIeParseLen(100, 50));
  TEST_ASSERT_EQUAL_UINT(46, roostIeParseLen(50, 100));
  // A frame at or under the FCS length leaves nothing, and must not wrap.
  TEST_ASSERT_EQUAL_UINT(0, roostIeParseLen(4, 4));
  TEST_ASSERT_EQUAL_UINT(0, roostIeParseLen(0, 0));
  TEST_ASSERT_EQUAL_UINT(0, roostIeParseLen(2, 2));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_ssid_is_extracted);
  RUN_TEST(test_beacon_and_probe_resp_both_carry_ssid);
  RUN_TEST(test_hidden_ssid_is_present_and_empty);
  RUN_TEST(test_zero_padded_ssid_is_captured_not_dropped);
  RUN_TEST(test_ie_list_preserves_frame_order);
  RUN_TEST(test_ie_list_refuses_rather_than_truncates);
  RUN_TEST(test_fixed_fields_are_read);
  RUN_TEST(test_walker_rejects_a_length_past_the_body);
  RUN_TEST(test_no_overrun_at_any_body_length);
  RUN_TEST(test_parse_length_removes_the_fcs);
  return UNITY_END();
}
