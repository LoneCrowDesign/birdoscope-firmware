// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host tests for this board's roost contract declaration.
//
// Asserted against the generated header rather than a recorded fixture. A
// fixture pins the bytes of one build and has to be regenerated whenever the
// registry grows; the checks here follow the registry automatically and catch
// the class of failure that actually occurs: a row whose fields do not line up
// with the header its manifest declares.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unity.h>

#include "board_config.h"
#include "roost_registry.h"

// Field separators, so a count is one less than the number of columns.
static int commas(const char* s) {
  int n = 0;
  for (; *s; s++) if (*s == ',') n++;
  return n;
}

static int columnsIn(RoostFieldMask mask, RoostRecord rec) {
  int n = 0;
  for (uint8_t i = 0; i < roostRecordFieldCount(rec); i++)
    if (mask & ROOST_F(i)) n++;
  return n;
}

// --- What this board declares ----------------------------------------------

void test_components_are_valid_and_distinct(void) {
  TEST_ASSERT_EQUAL_INT(1, roostComponentsValid());
  TEST_ASSERT_EQUAL_INT(3, ROOST_COMPONENT_COUNT);
  TEST_ASSERT_EQUAL_STRING("wifi0", roostComponentId(ROOST_COMP_WIFI0));
  TEST_ASSERT_EQUAL_STRING("gnss0", roostComponentId(ROOST_COMP_GNSS0));
  TEST_ASSERT_EQUAL_STRING("sys",   roostComponentId(ROOST_COMP_SYS));
}

// The radio reaches 2.4 GHz only. Without this an absent 5 GHz row is
// undecidable between a hardware limit and a channel plan that never went there.
void test_wifi_component_reaches_only_2_4(void) {
  TEST_ASSERT_EQUAL_UINT(ROOST_BAND_REACH_2_4,
                         roostComponentBandMask(ROOST_COMP_WIFI0));
}

// Five of six. ble_obs is out until a BLE capture mode exists; declaring it
// would put an empty ble_obs.v1.csv in every session, asserting BLE was
// reachable and nothing was heard.
void test_emitted_record_set(void) {
  TEST_ASSERT_TRUE(ROOST_EMITS_WIFI_OBS);
  TEST_ASSERT_TRUE(ROOST_EMITS_GPS_TRACK);
  TEST_ASSERT_TRUE(ROOST_EMITS_CONFIG_CHANGE);
  TEST_ASSERT_TRUE(ROOST_EMITS_DEVICE_EVENT);
  TEST_ASSERT_TRUE(ROOST_EMITS_OPERATOR_MARK);
  TEST_ASSERT_FALSE(ROOST_EMITS_BLE_OBS);
}

// A record this build emits but cannot fill the required set of would produce
// nothing but voided rows at runtime.
void test_column_masks_cover_required(void) {
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_WIFI_OBS,
                                              ROOST_WIFI_OBS_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_GPS_TRACK,
                                              ROOST_GPS_TRACK_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_CONFIG_CHANGE,
                                              ROOST_CONFIG_CHANGE_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_DEVICE_EVENT,
                                              ROOST_DEVICE_EVENT_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_OPERATOR_MARK,
                                              ROOST_OPERATOR_MARK_COLUMNS_MASK));
}

// auth_mode is reachable through ie_parse and has no producer, so it is
// excluded rather than left permanently empty. Capable but not recorded is a
// different statement from reachable-and-nothing-to-report.
void test_auth_mode_is_capable_but_excluded(void) {
  const RoostFieldMask f = ROOST_F(ROOST_WIFI_OBS_AUTH_MODE);
  TEST_ASSERT_TRUE(ROOST_WIFI_OBS_CAPABLE_MASK & f);
  TEST_ASSERT_FALSE(ROOST_WIFI_OBS_COLUMNS_MASK & f);
}

// capable must be a superset of columns, or the manifest claims a column the
// hardware cannot fill.
void test_capable_is_a_superset_of_columns(void) {
  TEST_ASSERT_EQUAL_UINT(ROOST_WIFI_OBS_COLUMNS_MASK,
      ROOST_WIFI_OBS_COLUMNS_MASK & ROOST_WIFI_OBS_CAPABLE_MASK);
  TEST_ASSERT_EQUAL_UINT(ROOST_GPS_TRACK_COLUMNS_MASK,
      ROOST_GPS_TRACK_COLUMNS_MASK & ROOST_GPS_TRACK_CAPABLE_MASK);
}

// --- Row against header, the check this file exists for ---------------------

// Builds a wifi_obs row populating only the required fields. roostRowFinish()
// pads every other declared column, so the separator count must equal the
// header's exactly. This is the shifted-column failure, and nothing on the
// device would report it.
void test_wifi_obs_row_aligns_with_header(void) {
  char header[512];
  TEST_ASSERT_TRUE(roostHeader(header, sizeof(header), ROOST_REC_WIFI_OBS,
                               ROOST_WIFI_OBS_COLUMNS_MASK) > 0);

  static const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, 0x03};
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_UPTIME_MS, 1234u);
  roostRowSetText(&w, ROOST_WIFI_OBS_CAP_COMPONENT,
                  roostComponentId(ROOST_COMP_WIFI0));
  roostRowSetEnum(&w, ROOST_WIFI_OBS_OBS_MODE, ROOST_OBS_MODE_PROMISCUOUS);
  roostRowSetMac(&w, ROOST_WIFI_OBS_MAC, mac);
  roostRowSetInt(&w, ROOST_WIFI_OBS_RSSI, -82);
  TEST_ASSERT_TRUE(roostRowFinish(&w) > 0);

  TEST_ASSERT_EQUAL_INT(commas(header), commas(row));
  TEST_ASSERT_EQUAL_INT(columnsIn(ROOST_WIFI_OBS_COLUMNS_MASK,
                                  ROOST_REC_WIFI_OBS) - 1,
                        commas(row));
  TEST_ASSERT_EQUAL_UINT(0, w.unknownEnums);
}

// Every emitted record, not only the one with the most columns.
void test_every_record_row_aligns_with_header(void) {
  const RoostRecord recs[] = {
    ROOST_REC_WIFI_OBS, ROOST_REC_GPS_TRACK, ROOST_REC_CONFIG_CHANGE,
    ROOST_REC_DEVICE_EVENT, ROOST_REC_OPERATOR_MARK,
  };
  const RoostFieldMask masks[] = {
    ROOST_WIFI_OBS_COLUMNS_MASK, ROOST_GPS_TRACK_COLUMNS_MASK,
    ROOST_CONFIG_CHANGE_COLUMNS_MASK, ROOST_DEVICE_EVENT_COLUMNS_MASK,
    ROOST_OPERATOR_MARK_COLUMNS_MASK,
  };

  for (size_t i = 0; i < sizeof(recs) / sizeof(recs[0]); i++) {
    char header[512];
    TEST_ASSERT_TRUE(roostHeader(header, sizeof(header), recs[i], masks[i]) > 0);
    TEST_ASSERT_EQUAL_INT(columnsIn(masks[i], recs[i]) - 1, commas(header));
  }
}

// A row missing a required column must produce nothing rather than a short
// row that still parses. Here cap_component, obs_mode, mac and rssi are absent.
void test_row_missing_required_field_is_voided(void) {
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_UPTIME_MS, 1234u);
  TEST_ASSERT_EQUAL_UINT(0, roostRowFinish(&w));
  TEST_ASSERT_EQUAL_STRING("", row);
}

// Canonical order is enforced by the writer: a column already passed cannot be
// filled in afterwards, so a misordered writer yields nothing rather than a
// misaligned file.
void test_out_of_order_write_is_refused(void) {
  static const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, 0x03};
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetInt(&w, ROOST_WIFI_OBS_RSSI, -82);
  // mac is earlier in canonical order and has already been passed.
  TEST_ASSERT_EQUAL_INT(0, roostRowSetMac(&w, ROOST_WIFI_OBS_MAC, mac));
  TEST_ASSERT_EQUAL_UINT(0, roostRowFinish(&w));
}

// A name the vocabulary does not carry leaves the column empty and is counted,
// rather than voiding the row or writing an undeclared value. Unread, that
// reads as "nothing to record" when a producer has drifted from the registry.
void test_unknown_enum_name_is_counted(void) {
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetEnumByName(&w, ROOST_WIFI_OBS_FRAME_SUBTYPE, "mgmt_other");
  TEST_ASSERT_EQUAL_UINT(1, w.unknownEnums);
}

// Every required column of every record this board emits must be reachable
// with the setter its declared type calls for. Walks the types rather than
// naming columns, so a record gaining a required field is covered without
// this test being edited.
//
// This guards the registry, not the device writers: a device setting a typed
// column with the wrong setter is not reachable from a host test. Its value is
// in a required field being added later that some record cannot satisfy.
static void setByDeclaredType(RoostRow* w, RoostRecord rec, uint8_t idx) {
  static const uint8_t kMac[6] = {0x04, 0x17, 0xb6, 0x01, 0x02, 0x03};
  switch (roostFieldTypeOf(rec, idx)) {
    case ROOST_FT_TEXT:  roostRowSetText(w, idx, "x");        break;
    case ROOST_FT_MAC:   roostRowSetMac(w, idx, kMac);        break;
    case ROOST_FT_UINT:  roostRowSetUInt(w, idx, 1);          break;
    case ROOST_FT_INT:   roostRowSetInt(w, idx, -1);          break;
    case ROOST_FT_FLOAT: roostRowSetFloat(w, idx, 1.0);       break;
    case ROOST_FT_HEX:   roostRowSetHex(w, idx, kMac, 1);     break;
    case ROOST_FT_ENUM:  roostRowSetEnum(w, idx, 0);          break;
    default: break;
  }
}

void test_every_required_column_takes_its_declared_setter(void) {
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  const size_t n = roostDeclaredFiles(decls, ROOST_MAX_DECLARED_FILES);
  TEST_ASSERT_TRUE(n > 0);

  for (size_t i = 0; i < n; i++) {
    const RoostRecord rec = decls[i].record;
    char row[512];
    RoostRow w;
    roostRowBegin(&w, row, sizeof(row), rec, decls[i].columns);
    for (uint8_t c = 0; c < roostRecordFieldCount(rec); c++)
      if (decls[i].columns & ROOST_F(c)) setByDeclaredType(&w, rec, c);
    TEST_ASSERT_TRUE_MESSAGE(roostRowFinish(&w), roostRecordName(rec));
    TEST_ASSERT_TRUE_MESSAGE(row[0], roostRecordName(rec));
  }
}

// The refusal itself, pinned: a text setter on a mac column takes the row down
// rather than leaving one column empty.
void test_wrong_setter_class_voids_the_row(void) {
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_UPTIME_MS, 1);
  roostRowSetText(&w, ROOST_WIFI_OBS_CAP_COMPONENT, "wifi0");
  roostRowSetEnum(&w, ROOST_WIFI_OBS_OBS_MODE, 0);
  TEST_ASSERT_FALSE(roostRowSetText(&w, ROOST_WIFI_OBS_MAC, "04:17:b6:01:02:03"));
  roostRowSetInt(&w, ROOST_WIFI_OBS_RSSI, -40);
  TEST_ASSERT_FALSE(roostRowFinish(&w));
  TEST_ASSERT_EQUAL_INT(0, row[0]);
}

// The four spellings corrected in P3, asserted so a producer cannot drift back.
void test_producer_vocabulary_resolves(void) {
  const char* subtypes[] = {"probe_req", "probe_resp", "beacon", "ctrl",
                            "unknown", "atim", "action", "data"};
  for (size_t i = 0; i < sizeof(subtypes) / sizeof(subtypes[0]); i++) {
    char row[512];
    RoostRow w;
    roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                  ROOST_WIFI_OBS_COLUMNS_MASK);
    roostRowSetEnumByName(&w, ROOST_WIFI_OBS_FRAME_SUBTYPE, subtypes[i]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, w.unknownEnums, subtypes[i]);
  }

  const char* methods[] = {"oui_addr1", "oui_addr2", "oui_addr3", "ssid_match",
                           "wildcard_probe", "directed_probe"};
  for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
    char row[512];
    RoostRow w;
    roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                  ROOST_WIFI_OBS_COLUMNS_MASK);
    roostRowSetEnumByName(&w, ROOST_WIFI_OBS_DETECTION_METHOD, methods[i]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, w.unknownEnums, methods[i]);
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_components_are_valid_and_distinct);
  RUN_TEST(test_wifi_component_reaches_only_2_4);
  RUN_TEST(test_emitted_record_set);
  RUN_TEST(test_column_masks_cover_required);
  RUN_TEST(test_auth_mode_is_capable_but_excluded);
  RUN_TEST(test_capable_is_a_superset_of_columns);
  RUN_TEST(test_wifi_obs_row_aligns_with_header);
  RUN_TEST(test_every_record_row_aligns_with_header);
  RUN_TEST(test_row_missing_required_field_is_voided);
  RUN_TEST(test_out_of_order_write_is_refused);
  RUN_TEST(test_unknown_enum_name_is_counted);
  RUN_TEST(test_every_required_column_takes_its_declared_setter);
  RUN_TEST(test_wrong_setter_class_voids_the_row);
  RUN_TEST(test_producer_vocabulary_resolves);
  return UNITY_END();
}
