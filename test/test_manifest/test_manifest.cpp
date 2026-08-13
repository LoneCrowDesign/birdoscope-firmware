// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host tests for the shared manifest renderer.
//
// The manifest was written per-device, and caused drift with: rendering
// nothing at all, or writing straight at the card and leaving a truncated file.
// These assert the behaviour expected from shared reference.

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <unity.h>

#include "board_config.h"
#include "roost_registry.h"
#include "roost_sdlog.h"
#include "roost_manifest.h"

static const char* kMacs[] = {"02:00:5e:11:22:33"};

static void baseInfo(RoostSessionInfo* info, RoostFileDecl* decls,
                     size_t* nDecls) {
  memset(info, 0, sizeof(*info));
  *nDecls = roostDeclaredFiles(decls, ROOST_MAX_DECLARED_FILES);

  info->deviceModel = "birdoscope_analyze";
  info->deviceSerial = "02005e112233";
  info->hwRevision = "HWr0.1";
  info->fwVersion = "0.1.0 abc1234";
  info->builtAt = "2026-08-08T20:53Z";
  info->ownMacs = kMacs;
  info->numOwnMacs = 1;
  info->gnssCepM = 2.5f;
  info->sessionId = "bscope-8-8-26-5";
  info->sequence = 5;
  info->bootCount = 6;
  info->clockAnchored = 1;
  info->clockSource = ROOST_CLOCK_SOURCE_GPS;
  info->clockAnchorUnix = 1786222471u;
  info->clockAnchorUptimeMs = 3705u;
  info->ouiTableHash = 0x9f8b6ccfu;
  info->storageTier = "sd";
  info->files = decls;
  info->numFiles = *nDecls;
}

// --- The guarantee Analyze did not have ------------------------------------

// A buffer too small yields 0 and nothing usable. The caller must be able to
// tell "did not fit" from "wrote it", because a truncated manifest asserts a
// file set and a column list the session does not have.
void test_short_buffer_renders_nothing(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);

  char out[256];                      // nowhere near enough
  TEST_ASSERT_EQUAL_UINT(0, roostSessionJson(out, sizeof(out), &info));
}

// Every size from far too small to comfortably large: either 0, or a document
// that both starts and ends like one. Never a prefix.
void test_no_truncated_document_at_any_size(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);

  static char out[4096];
  for (size_t cap = 1; cap < sizeof(out); cap += 7) {
    memset(out, 0xAA, sizeof(out));
    const size_t w = roostSessionJson(out, cap, &info);
    if (!w) continue;
    TEST_ASSERT_TRUE(w < cap);
    TEST_ASSERT_EQUAL_INT('{', out[0]);
    TEST_ASSERT_EQUAL_INT('}', out[w - 1]);
    TEST_ASSERT_EQUAL_INT(0, out[w]);
  }
}

void test_renders_at_a_realistic_size(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);

  static char out[4096];
  const size_t w = roostSessionJson(out, sizeof(out), &info);
  TEST_ASSERT_TRUE(w > 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"registry_hash\":\"" ROOST_REGISTRY_HASH "\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"device_model\":\"birdoscope_analyze\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"wifi_obs.v2.csv\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"clock_source\":\"gps\""));
}

// --- Null is a claim, not a blank -----------------------------------------

// An unanchored session has no start and no anchor. Zero is a real uptime and
// a real, if absurd, unix time; absence is what actually happened.
void test_unanchored_session_writes_nulls_not_zeroes(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);
  info.clockAnchored = 0;

  static char out[4096];
  TEST_ASSERT_TRUE(roostSessionJson(out, sizeof(out), &info) > 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"started_utc\":null"));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"clock_anchor_unix\":null"));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"clock_anchor_uptime_ms\":null"));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"clock_source\":\"none\""));
  TEST_ASSERT_NULL(strstr(out, "\"clock_anchor_unix\":0"));
}

// ended_utc is absent on a session that lost power, which is the signal that it
// did, so it is never filled in from the most recent snapshot.
void test_ended_utc_only_on_an_orderly_shutdown(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);

  static char out[4096];
  TEST_ASSERT_TRUE(roostSessionJson(out, sizeof(out), &info) > 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"ended_utc\":null"));

  info.endedUptimeMs = info.clockAnchorUptimeMs + 60000u;
  TEST_ASSERT_TRUE(roostSessionJson(out, sizeof(out), &info) > 0);
  TEST_ASSERT_NULL(strstr(out, "\"ended_utc\":null"));
}

// A shutdown before the clock anchored has no wall-clock end. The subtraction
// is unsigned, so without the guard it renders a timestamp 136 years out
// instead of saying it does not know.
void test_shutdown_before_the_anchor_has_no_end(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);
  info.endedUptimeMs = info.clockAnchorUptimeMs - 1000u;

  static char out[4096];
  TEST_ASSERT_TRUE(roostSessionJson(out, sizeof(out), &info) > 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"ended_utc\":null"));
}

// started_utc is derived from the anchor pair, never supplied, so it cannot
// disagree with the anchor it restates. 1786222471 - 3705ms lands 3s earlier.
void test_started_utc_is_derived_from_the_anchor(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);

  static char out[4096];
  TEST_ASSERT_TRUE(roostSessionJson(out, sizeof(out), &info) > 0);

  char want[24];
  roostUnixToIso(1786222471u - 3u, want, sizeof(want));
  char expect[64];
  snprintf(expect, sizeof(expect), "\"started_utc\":\"%s\"", want);
  TEST_ASSERT_NOT_NULL(strstr(out, expect));
}

// A row stamped before the anchor cannot be placed forward from it. The
// subtraction is unsigned, so without the guard it renders ~50 days late.
void test_pre_anchor_row_has_no_timestamp(void) {
  char buf[ROOST_ISO_BUF];
  const uint32_t anchorUnix = 1786239282u, anchorUptime = 3408u;

  TEST_ASSERT_EQUAL_INT(0, roostTimestampAt(anchorUnix, anchorUptime, 3375u,
                                            buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);

  // One millisecond before is still before.
  TEST_ASSERT_EQUAL_INT(0, roostTimestampAt(anchorUnix, anchorUptime,
                                            anchorUptime - 1, buf, sizeof(buf)));
  // At the anchor, and after it, it places normally.
  TEST_ASSERT_TRUE(roostTimestampAt(anchorUnix, anchorUptime, anchorUptime,
                                    buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("2026-08-09T01:34:42Z", buf);
  TEST_ASSERT_TRUE(roostTimestampAt(anchorUnix, anchorUptime,
                                    anchorUptime + 60000u, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("2026-08-09T01:35:42Z", buf);
}

// No anchor at all: no row in the session has a wall-clock time.
void test_unanchored_clock_places_nothing(void) {
  char buf[ROOST_ISO_BUF];
  TEST_ASSERT_EQUAL_INT(0, roostTimestampAt(0, 0, 12345u, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_iso_conversion_is_correct(void) {
  char buf[24];
  roostUnixToIso(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1970-01-01T00:00:00Z", buf);
  roostUnixToIso(1786222471u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2026-08-08T20:54:31Z", buf);
  // A leap day, which is where a hand-rolled civil-date conversion goes wrong.
  roostUnixToIso(1709208000u, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2024-02-29T12:00:00Z", buf);
}

// --- Counters come from the writer ----------------------------------------

// observations_written is the observation family, not every row of every type:
// one counter answering both makes a total look healthy against an empty
// observation file.
void test_observations_written_excludes_non_observation_rows(void) {
  RoostSdLog g;
  memset(&g, 0, sizeof(g));

  RoostSessionCounters c;
  roostSessionCounters(&g, &c, 0, 0);
  TEST_ASSERT_EQUAL_UINT(0, c.observationsWritten);

  // Rows of every non-observation type must not move it.
  g.wrote[ROOST_REC_GPS_TRACK] = 2263;
  g.wrote[ROOST_REC_DEVICE_EVENT] = 13;
  g.wrote[ROOST_REC_CONFIG_CHANGE] = 7;
  roostSessionCounters(&g, &c, 0, 0);
  TEST_ASSERT_EQUAL_UINT(0, c.observationsWritten);
  TEST_ASSERT_EQUAL_UINT(2263, c.fixesWritten);

  g.wrote[ROOST_REC_WIFI_OBS] = 40;
  roostSessionCounters(&g, &c, 0, 0);
  TEST_ASSERT_EQUAL_UINT(40, c.observationsWritten);
}

// storage_errors is failed writes and observations_dropped is rows lost. Both
// devices had one variable answering both, so a full buffer read as a failing
// card. Not expressible now: the device supplies neither.
void test_storage_errors_cannot_be_wired_to_a_drop_count(void) {
  RoostSdLog g;
  memset(&g, 0, sizeof(g));
  g.stats.rowsDropped = 12;
  g.stats.overflowRows = 12;
  g.stats.storageErrors = 0;

  RoostSessionCounters c;
  roostSessionCounters(&g, &c, 0, 0);
  TEST_ASSERT_EQUAL_UINT(0, c.storageErrors);
  TEST_ASSERT_EQUAL_UINT(12, c.observationsDropped);
}

// A voided row is an observation that was made and did not reach the card, and
// it is invisible in the file by definition. Leaving it out of the drop count
// is how a capture that refused most of its rows reads as a quiet one.
void test_voided_rows_count_as_dropped(void) {
  RoostSdLog g;
  memset(&g, 0, sizeof(g));
  g.voided[ROOST_REC_WIFI_OBS] = 28873;

  RoostSessionCounters c;
  roostSessionCounters(&g, &c, 0, 799);
  TEST_ASSERT_EQUAL_UINT(28873 + 799, c.observationsDropped);
}

// --- Device diagnostics ----------------------------------------------------

void test_device_diagnostics_are_optional(void) {
  RoostSessionInfo info;
  RoostFileDecl decls[ROOST_MAX_DECLARED_FILES];
  size_t n;
  baseInfo(&info, decls, &n);

  static char out[4096];
  TEST_ASSERT_TRUE(roostSessionJson(out, sizeof(out), &info) > 0);
  TEST_ASSERT_NULL(strstr(out, "device_diagnostics"));

  info.deviceDiagnostics = "\"queue_drops\":799";
  const size_t w = roostSessionJson(out, sizeof(out), &info);
  TEST_ASSERT_TRUE(w > 0);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"device_diagnostics\":{\"queue_drops\":799}"));
  TEST_ASSERT_EQUAL_INT('}', out[w - 1]);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_short_buffer_renders_nothing);
  RUN_TEST(test_no_truncated_document_at_any_size);
  RUN_TEST(test_renders_at_a_realistic_size);
  RUN_TEST(test_unanchored_session_writes_nulls_not_zeroes);
  RUN_TEST(test_ended_utc_only_on_an_orderly_shutdown);
  RUN_TEST(test_shutdown_before_the_anchor_has_no_end);
  RUN_TEST(test_started_utc_is_derived_from_the_anchor);
  RUN_TEST(test_pre_anchor_row_has_no_timestamp);
  RUN_TEST(test_unanchored_clock_places_nothing);
  RUN_TEST(test_iso_conversion_is_correct);
  RUN_TEST(test_observations_written_excludes_non_observation_rows);
  RUN_TEST(test_storage_errors_cannot_be_wired_to_a_drop_count);
  RUN_TEST(test_voided_rows_count_as_dropped);
  RUN_TEST(test_device_diagnostics_are_optional);
  return UNITY_END();
}
