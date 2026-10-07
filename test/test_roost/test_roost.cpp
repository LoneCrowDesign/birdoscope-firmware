// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host tests for this board's roost contract declaration.
//
// The checks read the generated registry header, so they follow the registry
// as it grows. They assert that a row's fields line up with the header its
// manifest declares.

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
  TEST_ASSERT_EQUAL_INT(4, ROOST_COMPONENT_COUNT);
  TEST_ASSERT_EQUAL_STRING("wifi0", roostComponentId(ROOST_COMP_WIFI0));
  TEST_ASSERT_EQUAL_STRING("ble0",  roostComponentId(ROOST_COMP_BLE0));
  TEST_ASSERT_EQUAL_STRING("gnss0", roostComponentId(ROOST_COMP_GNSS0));
  TEST_ASSERT_EQUAL_STRING("sys",   roostComponentId(ROOST_COMP_SYS));
}

// The radio reaches 2.4 GHz only. With the band mask, a reader can tell a
// hardware limit from a channel plan that never visited 5 GHz.
void test_wifi_component_reaches_only_2_4(void) {
  TEST_ASSERT_EQUAL_UINT(ROOST_BAND_REACH_2_4,
                         roostComponentBandMask(ROOST_COMP_WIFI0));
}

// All six records. radio_mode tells an empty ble_obs apart from a radio
// switched off.
void test_emitted_record_set(void) {
  TEST_ASSERT_TRUE(ROOST_EMITS_WIFI_OBS);
  TEST_ASSERT_TRUE(ROOST_EMITS_GPS_TRACK);
  TEST_ASSERT_TRUE(ROOST_EMITS_CONFIG_CHANGE);
  TEST_ASSERT_TRUE(ROOST_EMITS_DEVICE_EVENT);
  TEST_ASSERT_TRUE(ROOST_EMITS_OPERATOR_MARK);
  TEST_ASSERT_TRUE(ROOST_EMITS_BLE_OBS);
}

// A BLE survey row writes operator_survey, per spec O3. The ble_obs
// restriction must allow it, or the row builder voids every survey row.
void test_ble_obs_accepts_survey_rows(void) {
  TEST_ASSERT_TRUE(roostValueAllowed(ROOST_REC_BLE_OBS,
                                     ROOST_BLE_OBS_DETECTION_METHOD,
                                     ROOST_DETECTION_METHOD_OPERATOR_SURVEY));
  TEST_ASSERT_TRUE(roostValueAllowed(ROOST_REC_BLE_OBS,
                                     ROOST_BLE_OBS_DETECTION_METHOD,
                                     ROOST_DETECTION_METHOD_BLE_MFR));
  TEST_ASSERT_FALSE(roostValueAllowed(ROOST_REC_BLE_OBS,
                                      ROOST_BLE_OBS_DETECTION_METHOD,
                                      ROOST_DETECTION_METHOD_OUI_ADDR2));
}

// Every emitted record's columns must cover its required fields, or the row
// builder voids every row.
void test_column_masks_cover_required(void) {
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_WIFI_OBS,
                                              ROOST_WIFI_OBS_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_BLE_OBS,
                                              ROOST_BLE_OBS_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_GPS_TRACK,
                                              ROOST_GPS_TRACK_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_CONFIG_CHANGE,
                                              ROOST_CONFIG_CHANGE_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_DEVICE_EVENT,
                                              ROOST_DEVICE_EVENT_COLUMNS_MASK));
  TEST_ASSERT_TRUE(roostMaskSatisfiesRequired(ROOST_REC_OPERATOR_MARK,
                                              ROOST_OPERATOR_MARK_COLUMNS_MASK));
}

// auth_mode is reachable through ie_parse and has no producer. See
// ROOST_WIFI_OBS_EXCLUDE in board_config.h.
void test_auth_mode_is_capable_but_excluded(void) {
  const RoostFieldMask f = ROOST_F(ROOST_WIFI_OBS_AUTH_MODE);
  TEST_ASSERT_TRUE(ROOST_WIFI_OBS_CAPABLE_MASK & f);
  TEST_ASSERT_FALSE(ROOST_WIFI_OBS_COLUMNS_MASK & f);
}

// capable must be a superset of columns, or the manifest claims a column the
// build cannot fill.
void test_capable_is_a_superset_of_columns(void) {
  TEST_ASSERT_EQUAL_UINT(ROOST_WIFI_OBS_COLUMNS_MASK,
      ROOST_WIFI_OBS_COLUMNS_MASK & ROOST_WIFI_OBS_CAPABLE_MASK);
  TEST_ASSERT_EQUAL_UINT(ROOST_GPS_TRACK_COLUMNS_MASK,
      ROOST_GPS_TRACK_COLUMNS_MASK & ROOST_GPS_TRACK_CAPABLE_MASK);
}

// --- Row against header -----------------------------------------------------

// The test fills only the required fields of a wifi_obs row. roostRowFinish()
// pads every other declared column, so the row's separator count must equal the
// header's. No on-device check catches a shifted column.
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

// Each record's header carries one column per declared field.
void test_every_record_row_aligns_with_header(void) {
  const RoostRecord recs[] = {
    ROOST_REC_WIFI_OBS, ROOST_REC_BLE_OBS, ROOST_REC_GPS_TRACK,
    ROOST_REC_CONFIG_CHANGE, ROOST_REC_DEVICE_EVENT, ROOST_REC_OPERATOR_MARK,
  };
  const RoostFieldMask masks[] = {
    ROOST_WIFI_OBS_COLUMNS_MASK, ROOST_BLE_OBS_COLUMNS_MASK,
    ROOST_GPS_TRACK_COLUMNS_MASK, ROOST_CONFIG_CHANGE_COLUMNS_MASK,
    ROOST_DEVICE_EVENT_COLUMNS_MASK, ROOST_OPERATOR_MARK_COLUMNS_MASK,
  };

  for (size_t i = 0; i < sizeof(recs) / sizeof(recs[0]); i++) {
    char header[512];
    TEST_ASSERT_TRUE(roostHeader(header, sizeof(header), recs[i], masks[i]) > 0);
    TEST_ASSERT_EQUAL_INT(columnsIn(masks[i], recs[i]) - 1, commas(header));
  }
}

// A row missing a required column must produce nothing. This row lacks
// cap_component, obs_mode, mac and rssi.
void test_row_missing_required_field_is_voided(void) {
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetUInt(&w, ROOST_WIFI_OBS_UPTIME_MS, 1234u);
  TEST_ASSERT_EQUAL_UINT(0, roostRowFinish(&w));
  TEST_ASSERT_EQUAL_STRING("", row);
}

// The row builder enforces canonical order. It refuses a column it has already
// passed, and the row then yields nothing.
void test_out_of_order_write_is_refused(void) {
  static const uint8_t mac[6] = {0xb4, 0x1e, 0x52, 0x01, 0x02, 0x03};
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetInt(&w, ROOST_WIFI_OBS_RSSI, -82);
  // mac precedes rssi in canonical order, so the builder has already passed it.
  TEST_ASSERT_EQUAL_INT(0, roostRowSetMac(&w, ROOST_WIFI_OBS_MAC, mac));
  TEST_ASSERT_EQUAL_UINT(0, roostRowFinish(&w));
}

// A name outside the vocabulary leaves the column empty and increments
// unknownEnums. The row still writes.
void test_unknown_enum_name_is_counted(void) {
  char row[512];
  RoostRow w;
  roostRowBegin(&w, row, sizeof(row), ROOST_REC_WIFI_OBS,
                ROOST_WIFI_OBS_COLUMNS_MASK);
  roostRowSetEnumByName(&w, ROOST_WIFI_OBS_FRAME_SUBTYPE, "mgmt_other");
  TEST_ASSERT_EQUAL_UINT(1, w.unknownEnums);
}

// Every declared column of every record this board emits must accept the
// setter its declared type calls for. The test walks the declared types, so it
// covers a new required field with no edit. Its scope is the registry, since a
// host test cannot reach the device writers.
static void setByDeclaredType(RoostRow* w, RoostRecord rec, uint8_t idx) {
  static const uint8_t kMac[6] = {0x04, 0x17, 0xb6, 0x01, 0x02, 0x03};
  switch (roostFieldTypeOf(rec, idx)) {
    case ROOST_FT_TEXT:  roostRowSetText(w, idx, "x");        break;
    case ROOST_FT_MAC:   roostRowSetMac(w, idx, kMac);        break;
    case ROOST_FT_UINT:  roostRowSetUInt(w, idx, 1);          break;
    case ROOST_FT_INT:   roostRowSetInt(w, idx, -1);          break;
    case ROOST_FT_FLOAT: roostRowSetFloat(w, idx, 1.0);       break;
    case ROOST_FT_HEX:   roostRowSetHex(w, idx, kMac, 1);     break;
    case ROOST_FT_ENUM: {
      // The lowest value the record allows, since a restricted column refuses
      // the rest.
      int v = 0;
      while (v < 64 && !roostValueAllowed(rec, idx, v)) v++;
      roostRowSetEnum(w, idx, v);
      break;
    }
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

// A text setter on a mac column voids the whole row.
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

// These producer spellings must resolve in the vocabulary.
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
  RUN_TEST(test_ble_obs_accepts_survey_rows);
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
