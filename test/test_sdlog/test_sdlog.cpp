// Copyright (C) 2026 Lone Crow Design, LLC
// Licensed under the MIT License. See LICENSE.
//
// Host tests for the shared buffered writer, against a memory backend.
//
// The buffering is what got captures wrong: syncing per row loses most matched
// observations under load. These assert the properties the fix has to have,
// none of which are visible on the device.
//
// See docs/roost_logging.md, "Why flush cadence is a trade rather than a
// maximum".

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <unity.h>

#include "board_config.h"
#include "roost_registry.h"
#include "roost_sdlog.h"

// --- Memory backend --------------------------------------------------------

#define FAKE_FILES 8
#define FAKE_CAP   65536

struct Fake {
  char     path[FAKE_FILES][160];
  char     data[FAKE_FILES][FAKE_CAP];
  size_t   len[FAKE_FILES];
  size_t   synced[FAKE_FILES];   // bytes durable at the last sync
  int      used;
  int      syncs;
  int      writes;
  int      mkdirs;
  uint32_t clock;
  uint32_t stallMs;              // charged to the next write
  int      failWritesFrom;       // -1 = never
  size_t   shortWriteTo;         // 0 = full writes; else cap each write
};
static Fake g_fake;

static void fakeReset(void) {
  memset(&g_fake, 0, sizeof(g_fake));
  g_fake.failWritesFrom = -1;
}
static int fakeMkdir(void*, const char*) { g_fake.mkdirs++; return 0; }
static int fakeOpen(void*, const char* p) {
  for (int i = 0; i < g_fake.used; i++)          // reopen appends
    if (!strcmp(g_fake.path[i], p)) return i;
  if (g_fake.used >= FAKE_FILES) return -1;
  int h = g_fake.used++;
  snprintf(g_fake.path[h], sizeof(g_fake.path[h]), "%s", p);
  return h;
}
static size_t fakeSize(void*, int h) { return g_fake.len[h]; }
static int fakeWrite(void*, int h, const void* d, size_t n) {
  if (g_fake.failWritesFrom >= 0 && g_fake.writes >= g_fake.failWritesFrom) return -1;
  g_fake.writes++;
  g_fake.clock += g_fake.stallMs;
  if (g_fake.shortWriteTo && n > g_fake.shortWriteTo) n = g_fake.shortWriteTo;
  if (g_fake.len[h] + n > FAKE_CAP) return -1;
  memcpy(g_fake.data[h] + g_fake.len[h], d, n);
  g_fake.len[h] += n;
  return (int)n;
}
static int fakeSync(void*, int h) { g_fake.syncs++; g_fake.synced[h] = g_fake.len[h]; return 0; }
static void fakeClose(void*, int) {}
static uint32_t fakeNow(void*) { return g_fake.clock; }

static const RoostSdIo kIo = {
  nullptr, fakeMkdir, fakeOpen, fakeSize, fakeWrite, fakeSync, fakeClose, fakeNow,
};

// --- Fixture ---------------------------------------------------------------

static uint8_t g_bufWifi[4096];
static uint8_t g_bufTrack[1024];
static RoostSdLog g_log;

static RoostFileDecl decls[5];
static size_t declCount;

static void buildDecls(void) {
  declCount = 0;
  decls[declCount++] = { ROOST_REC_WIFI_OBS,      ROOST_WIFI_OBS_COLUMNS_MASK,      ROOST_WIFI_OBS_CAPABLE_MASK };
  decls[declCount++] = { ROOST_REC_GPS_TRACK,     ROOST_GPS_TRACK_COLUMNS_MASK,     ROOST_GPS_TRACK_CAPABLE_MASK };
  decls[declCount++] = { ROOST_REC_CONFIG_CHANGE, ROOST_CONFIG_CHANGE_COLUMNS_MASK, ROOST_CONFIG_CHANGE_CAPABLE_MASK };
  decls[declCount++] = { ROOST_REC_DEVICE_EVENT,  ROOST_DEVICE_EVENT_COLUMNS_MASK,  ROOST_DEVICE_EVENT_CAPABLE_MASK };
  decls[declCount++] = { ROOST_REC_OPERATOR_MARK, ROOST_OPERATOR_MARK_COLUMNS_MASK, ROOST_OPERATOR_MARK_CAPABLE_MASK };
}

// Analyze's intended attachment: the high-rate file buffered, gps_track given a
// smaller buffer for durability, the three event files write-through.
static void openSession(void) {
  fakeReset();
  buildDecls();
  roostSdInit(&g_log, &kIo);
  roostSdAttachBuffer(&g_log, ROOST_REC_WIFI_OBS,  g_bufWifi,  sizeof(g_bufWifi));
  roostSdAttachBuffer(&g_log, ROOST_REC_GPS_TRACK, g_bufTrack, sizeof(g_bufTrack));
  TEST_ASSERT_TRUE(roostSdOpenSession(&g_log, "/bscope-8-8-26-1", decls, declCount));
}

static int fileFor(const char* suffix) {
  for (int i = 0; i < g_fake.used; i++)
    if (strstr(g_fake.path[i], suffix)) return i;
  return -1;
}

// --- Session start ---------------------------------------------------------

// Every declared file exists with its header, including ones this session may
// never observe. design_spec.md 6.2 step 4.
void test_session_creates_every_declared_file_with_a_header(void) {
  openSession();
  TEST_ASSERT_EQUAL_INT(1, g_fake.mkdirs);
  TEST_ASSERT_EQUAL_INT(5, g_fake.used);
  for (size_t i = 0; i < declCount; i++) {
    char name[48];
    roostFileName(name, sizeof(name), decls[i].record);
    int h = fileFor(name);
    TEST_ASSERT_TRUE_MESSAGE(h >= 0, name);
    char want[640];
    roostHeader(want, sizeof(want), decls[i].record, decls[i].columns);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, strncmp(g_fake.data[h], want, strlen(want)), name);
    // Durable immediately, not sitting in a buffer.
    TEST_ASSERT_TRUE(g_fake.synced[h] >= strlen(want));
  }
}

// --- Buffering, the fix for per-row syncing --------------------------------

// The defect in one assertion: rows must not reach the card one at a time.
void test_buffered_rows_do_not_sync_per_row(void) {
  openSession();
  const int syncsAfterOpen = g_fake.syncs;
  for (int i = 0; i < 20; i++) roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, "a,b,c");
  TEST_ASSERT_EQUAL_INT(syncsAfterOpen, g_fake.syncs);

  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(20, st.rowsWritten);
  TEST_ASSERT_EQUAL_UINT(0, st.rowsDropped);
}

// Rows are held until a block fills, then written once.
void test_buffer_flushes_when_full(void) {
  openSession();
  const int before = g_fake.syncs;
  char row[128];
  memset(row, 'x', sizeof(row) - 1);
  row[sizeof(row) - 1] = '\0';
  // 4096 / 130 = 31 rows per block.
  for (int i = 0; i < 31; i++) roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, row);
  TEST_ASSERT_EQUAL_INT(before, g_fake.syncs);
  for (int i = 0; i < 10; i++) roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, row);
  TEST_ASSERT_EQUAL_INT(before + 1, g_fake.syncs);
}

// Amortised sync count is what the measurement cares about: one per block, not
// one per row. Asserted exactly rather than as a range, since the ratio is the
// property under test.
void test_sync_count_is_amortised_over_a_block(void) {
  openSession();
  const int before = g_fake.syncs;

  char row[92];
  memset(row, 'y', sizeof(row) - 1);
  row[sizeof(row) - 1] = '\0';
  const size_t onDisk = strlen(row) + 2;              // + CRLF
  const size_t perBlock = sizeof(g_bufWifi) / onDisk;  // rows that fit a block

  const int rows = 500;
  for (int i = 0; i < rows; i++)
    TEST_ASSERT_TRUE(roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, row));

  const int syncs = g_fake.syncs - before;
  TEST_ASSERT_EQUAL_INT((int)(rows / perBlock), syncs);
  // The property, stated as the ratio it buys over syncing per row.
  TEST_ASSERT_TRUE(syncs * 40 < rows);

  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(rows, st.rowsWritten);
  TEST_ASSERT_EQUAL_UINT(0, st.rowsDropped);
}

// --- Write-through ---------------------------------------------------------

// A device_event saying storage just failed is useless sitting in RAM, and an
// operator mark is a press that does not come again.
void test_event_records_are_write_through(void) {
  openSession();
  int h = fileFor("device_event");
  const size_t before = g_fake.synced[h];
  TEST_ASSERT_TRUE(roostSdAppend(&g_log, ROOST_REC_DEVICE_EVENT, "1,2,3"));
  TEST_ASSERT_TRUE(g_fake.synced[h] > before);
  TEST_ASSERT_EQUAL_INT(0, memcmp(g_fake.data[h] + g_fake.len[h] - 7, "1,2,3\r\n", 7));
}

void test_operator_mark_is_write_through(void) {
  openSession();
  int h = fileFor("operator_mark");
  TEST_ASSERT_TRUE(roostSdAppend(&g_log, ROOST_REC_OPERATOR_MARK, "t,1,2,sys"));
  TEST_ASSERT_EQUAL_UINT(g_fake.len[h], g_fake.synced[h]);
}

// --- Durability ------------------------------------------------------------

// Closing must not silently discard buffered rows; it is what runs before the
// session directory is renamed at the clock anchor.
void test_close_flushes_buffered_rows(void) {
  openSession();
  roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, "held,in,ram");
  int h = fileFor("wifi_obs");
  const size_t beforeLen = g_fake.len[h];
  roostSdCloseSession(&g_log);
  TEST_ASSERT_TRUE(g_fake.len[h] > beforeLen);
  TEST_ASSERT_EQUAL_UINT(g_fake.len[h], g_fake.synced[h]);
}

void test_append_after_close_is_a_counted_drop(void) {
  openSession();
  roostSdCloseSession(&g_log);
  TEST_ASSERT_FALSE(roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, "a,b"));
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(1, st.rowsDropped);
}

// A voided row from roostRowFinish() is an empty string. Writing it would put a
// blank line in the file, which parses as a row of empty columns.
void test_voided_row_is_dropped_not_written(void) {
  openSession();
  TEST_ASSERT_FALSE(roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, ""));
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(0, st.rowsWritten);
  TEST_ASSERT_EQUAL_UINT(1, st.rowsDropped);
}

// --- Reopen after the anchor rename ----------------------------------------

// The session directory is renamed once the clock anchors, which means closing
// every file and reopening it. Reopening must APPEND: ESP32 FILE_WRITE is "w"
// and truncates, which destroys every row captured before the anchor and leaves
// each file holding nothing but a freshly rewritten header.
void test_reopen_after_rename_keeps_earlier_rows(void) {
  openSession();
  roostSdAppend(&g_log, ROOST_REC_DEVICE_EVENT, "before,the,anchor");
  roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, "buffered,before,anchor");
  roostSdCloseSession(&g_log);

  int h = fileFor("device_event");
  const size_t afterClose = g_fake.len[h];
  TEST_ASSERT_TRUE(afterClose > 0);

  // Same paths, as a rename to the same fixture would leave them.
  TEST_ASSERT_TRUE(roostSdOpenSession(&g_log, "/bscope-8-8-26-1", decls, declCount));
  TEST_ASSERT_EQUAL_UINT(afterClose, g_fake.len[h]);       // nothing truncated
  TEST_ASSERT_TRUE(strstr(g_fake.data[h], "before,the,anchor") != NULL);

  // And exactly one header, not one per open.
  char want[640];
  roostHeader(want, sizeof(want), ROOST_REC_DEVICE_EVENT, decls[3].columns);
  const char* first = strstr(g_fake.data[h], want);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NULL(strstr(first + 1, want));

  // Rows still land after the reopen.
  TEST_ASSERT_TRUE(roostSdAppend(&g_log, ROOST_REC_DEVICE_EVENT, "after,the,anchor"));
  TEST_ASSERT_TRUE(strstr(g_fake.data[h], "after,the,anchor") != NULL);
}

// --- Partial writes --------------------------------------------------------

// A card that accepts fewer bytes than asked must not cause the landed bytes to
// be written twice. Retrying the whole buffer leaves a truncated line followed
// by a complete copy of the same block, which is corruption in the middle of
// the file: a line-oriented reader can drop a partial tail, but it cannot
// recover from a duplicated fragment, and the duplicate shifts every column on
// the line it lands in.
void test_short_write_consumes_only_what_landed(void) {
  openSession();
  int h = fileFor("wifi_obs");
  const size_t headerLen = g_fake.len[h];

  // Fill a block with identifiable rows, then let the card take only part.
  char row[92];
  memset(row, 'a', sizeof(row) - 1);
  row[sizeof(row) - 1] = '\0';
  g_fake.shortWriteTo = 1000;                 // of a 4096-byte block
  const size_t perBlock = sizeof(g_bufWifi) / (strlen(row) + 2);
  for (size_t i = 0; i < perBlock + 1; i++)
    roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, row);

  g_fake.shortWriteTo = 0;                    // card recovers
  roostSdFlushAll(&g_log);
  roostSdCloseSession(&g_log);

  // Every byte after the header is one of our rows, exactly once each: no
  // duplicated fragment, no gap.
  const size_t body = g_fake.len[h] - headerLen;
  TEST_ASSERT_EQUAL_UINT(0, body % (strlen(row) + 2));
  const char* p = g_fake.data[h] + headerLen;
  for (size_t i = 0; i < body / (strlen(row) + 2); i++) {
    const char* line = p + i * (strlen(row) + 2);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(line, row, strlen(row)), "row body");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(line + strlen(row), "\r\n", 2), "row terminator");
  }
}

// Every line the writer emits is whole. A buffer never holds a partial row, so
// a power cut can truncate the final line but cannot shift columns within one.
void test_buffer_never_holds_a_partial_row(void) {
  openSession();
  char row[300];
  memset(row, 'q', sizeof(row) - 1);
  row[sizeof(row) - 1] = '\0';
  for (int i = 0; i < 60; i++) roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, row);
  roostSdCloseSession(&g_log);

  int h = fileFor("wifi_obs");
  // Count terminators: one per row, and the file ends on one.
  size_t lines = 0;
  for (size_t i = 0; i + 1 < g_fake.len[h]; i++)
    if (g_fake.data[h][i] == '\r' && g_fake.data[h][i + 1] == '\n') lines++;
  TEST_ASSERT_EQUAL_UINT(61, lines);           // 60 rows + the header
  TEST_ASSERT_EQUAL_INT(0, memcmp(g_fake.data[h] + g_fake.len[h] - 2, "\r\n", 2));
}

// --- Failure is visible ----------------------------------------------------

void test_write_failure_counts_a_drop(void) {
  openSession();
  g_fake.failWritesFrom = g_fake.writes;   // fail everything from here
  TEST_ASSERT_FALSE(roostSdAppend(&g_log, ROOST_REC_DEVICE_EVENT, "1,2,3"));
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(1, st.rowsDropped);
  // storage_errors and observations_dropped are different manifest keys and
  // must not be one variable read twice: a failed write is both.
  TEST_ASSERT_EQUAL_UINT(1, st.storageErrors);
  TEST_ASSERT_EQUAL_UINT(0, st.overflowRows);
}

// The other half of the same split. A full buffer with the card healthy loses
// rows without a single failed write, and reporting it as a storage error
// would send a reader looking at the card instead of the buffer size.
void test_overflow_is_not_a_storage_error(void) {
  openSession();
  static char huge[2048];
  memset(huge, 'z', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';
  TEST_ASSERT_FALSE(roostSdAppend(&g_log, ROOST_REC_GPS_TRACK, huge));
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(1, st.overflowRows);
  TEST_ASSERT_EQUAL_UINT(0, st.storageErrors);
}

// A short write is a storage error even though bytes landed, because the
// stream is now behind and the next flush carries the remainder.
void test_short_write_counts_a_storage_error(void) {
  openSession();
  char row[64];
  memset(row, 'w', sizeof(row) - 1);
  row[sizeof(row) - 1] = '\0';
  g_fake.shortWriteTo = 100;            // every flush lands 100 bytes of a block
  for (int i = 0; i < 80; i++) roostSdAppend(&g_log, ROOST_REC_WIFI_OBS, row);
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_TRUE(st.storageErrors >= 1);
}

// A row that cannot fit the buffer at all must be dropped once, not retried
// forever. Guards against a spin on a pathological ie_ids value.
void test_row_larger_than_buffer_is_dropped(void) {
  openSession();
  static char huge[2048];
  memset(huge, 'z', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';
  TEST_ASSERT_FALSE(roostSdAppend(&g_log, ROOST_REC_GPS_TRACK, huge));  // 1 KB buffer
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_EQUAL_UINT(1, st.rowsDropped);
}

// worst_flush_ms is what sizes the alert queue, so it must observe the stall
// rather than the call count.
void test_worst_flush_is_recorded(void) {
  openSession();
  g_fake.stallMs = 246;                 // the measured Analyze stall
  roostSdAppend(&g_log, ROOST_REC_DEVICE_EVENT, "1,2,3");
  RoostSdStats st;
  roostSdGetStats(&g_log, &st);
  TEST_ASSERT_TRUE(st.worstFlushMs >= 246);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_session_creates_every_declared_file_with_a_header);
  RUN_TEST(test_buffered_rows_do_not_sync_per_row);
  RUN_TEST(test_buffer_flushes_when_full);
  RUN_TEST(test_sync_count_is_amortised_over_a_block);
  RUN_TEST(test_event_records_are_write_through);
  RUN_TEST(test_operator_mark_is_write_through);
  RUN_TEST(test_close_flushes_buffered_rows);
  RUN_TEST(test_append_after_close_is_a_counted_drop);
  RUN_TEST(test_voided_row_is_dropped_not_written);
  RUN_TEST(test_reopen_after_rename_keeps_earlier_rows);
  RUN_TEST(test_short_write_consumes_only_what_landed);
  RUN_TEST(test_buffer_never_holds_a_partial_row);
  RUN_TEST(test_write_failure_counts_a_drop);
  RUN_TEST(test_overflow_is_not_a_storage_error);
  RUN_TEST(test_short_write_counts_a_storage_error);
  RUN_TEST(test_row_larger_than_buffer_is_dropped);
  RUN_TEST(test_worst_flush_is_recorded);
  return UNITY_END();
}
