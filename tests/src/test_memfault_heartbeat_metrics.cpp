//! @file
//!
//! @brief

#include "CppUTest/MemoryLeakDetectorMallocMacros.h"
#include "CppUTest/MemoryLeakDetectorNewMacros.h"
#include "CppUTest/TestHarness.h"
#include "CppUTestExt/MockSupport.h"

extern "C" {
  #include <string.h>
  #include <stddef.h>

  #include "fakes/fake_memfault_platform_metrics_locking.h"
  #include "memfault/core/event_storage.h"
  #include "memfault/core/platform/core.h"
  #include "memfault/metrics/metrics.h"
  #include "memfault/metrics/platform/overrides.h"
  #include "memfault/metrics/platform/timer.h"
  #include "memfault/metrics/serializer.h"
  #include "memfault/metrics/utils.h"


  static void (*s_serializer_check_cb)(void) = NULL;

  static uint64_t s_fake_time_ms = 0;
  uint64_t memfault_platform_get_time_since_boot_ms(void) {
    return s_fake_time_ms;
  }

  static void prv_fake_time_set(uint64_t new_fake_time_ms) {
    s_fake_time_ms = new_fake_time_ms;
  }

  static void prv_fake_time_incr(uint64_t fake_time_delta_ms) {
    s_fake_time_ms += fake_time_delta_ms;
  }

  static const sMemfaultEventStorageImpl *s_fake_event_storage_impl;
}

bool memfault_platform_metrics_timer_boot(uint32_t period_sec,
                                          MemfaultPlatformTimerCallback callback) {
  return mock().actualCall(__func__)
      .withParameter("period_sec", period_sec)
      .returnBoolValueOrDefault(true);
}


#define FAKE_STORAGE_SIZE 100

bool memfault_metrics_heartbeat_serialize(const sMemfaultEventStorageImpl *storage_impl) {
  mock().actualCall(__func__);
  if (s_serializer_check_cb != NULL) {
    s_serializer_check_cb();
  }

  return true;
}

size_t memfault_metrics_heartbeat_compute_worst_case_storage_size(void) {
  return (size_t)mock().actualCall(__func__).returnIntValueOrDefault(FAKE_STORAGE_SIZE);
}

TEST_GROUP(MemfaultHeartbeatMetrics){
  void setup() {
    s_fake_time_ms = 0;
    s_serializer_check_cb = NULL;
    fake_memfault_metrics_platorm_locking_reboot();
    static uint8_t s_storage[FAKE_STORAGE_SIZE];
    mock().strictOrder();

    // Check that by default the heartbeat interval is once / hour
    mock().expectOneCall("memfault_platform_metrics_timer_boot").withParameter("period_sec", 3600);

    s_fake_event_storage_impl = memfault_events_storage_boot(
        &s_storage, sizeof(s_storage));
    mock().expectOneCall("memfault_metrics_heartbeat_compute_worst_case_storage_size");

    int rv = memfault_metrics_boot(s_fake_event_storage_impl);
    LONGS_EQUAL(0, rv);
    mock().checkExpectations();
    // We should test all the types of available metrics so if this
    // fails it means there's a new type we aren't yet covering
    LONGS_EQUAL(kMemfaultMetricType_NumTypes, memfault_metrics_heartbeat_get_num_metrics());
  }
  void teardown() {
    // dump the final result & also sanity test that this routine works
    memfault_metrics_heartbeat_debug_print();
    CHECK(fake_memfault_platform_metrics_lock_calls_balanced());
    mock().checkExpectations();
    mock().clear();
  }
};

TEST(MemfaultHeartbeatMetrics, Test_BootStorageTooSmall) {
  // Check that by default the heartbeat interval is once / hour
  mock().expectOneCall("memfault_platform_metrics_timer_boot").withParameter("period_sec", 3600);

  // reboot metrics with storage that is too small to actually hold an event
  // this should result in a warning being emitted
  mock().expectOneCall("memfault_metrics_heartbeat_compute_worst_case_storage_size")
      .andReturnValue(FAKE_STORAGE_SIZE + 1);

  int rv = memfault_metrics_boot(s_fake_event_storage_impl);
  LONGS_EQUAL(-5, rv);
  mock().checkExpectations();
}

TEST(MemfaultHeartbeatMetrics, Test_TimerInitFailed) {
  // Nothing else should happen if timer initialization failed for some reason
  mock().expectOneCall("memfault_platform_metrics_timer_boot")
      .withParameter("period_sec", 3600)
      .andReturnValue(false);

  int rv = memfault_metrics_boot(s_fake_event_storage_impl);
  LONGS_EQUAL(-6, rv);
  mock().checkExpectations();
}

TEST(MemfaultHeartbeatMetrics, Test_UnsignedHeartbeatValue) {
  MemfaultMetricId key = MEMFAULT_METRICS_KEY(test_key_unsigned);
  int rv = memfault_metrics_heartbeat_set_unsigned(key, 100);
  LONGS_EQUAL(0, rv);

  rv = memfault_metrics_heartbeat_set_signed(key, 100);
  CHECK(rv != 0);

  rv = memfault_metrics_heartbeat_add(key, 1);
  LONGS_EQUAL(0, rv);
  rv = memfault_metrics_heartbeat_add(key, 1);
  LONGS_EQUAL(0, rv);
  rv = memfault_metrics_heartbeat_add(key, 2);
  LONGS_EQUAL(0, rv);
  uint32_t val = 0;
  rv = memfault_metrics_heartbeat_read_unsigned(key, &val);
  LONGS_EQUAL(0, rv);
  LONGS_EQUAL(104, val);

  // test clipping
  memfault_metrics_heartbeat_add(key, INT32_MAX);
  memfault_metrics_heartbeat_add(key, INT32_MAX);
  rv = memfault_metrics_heartbeat_read_unsigned(key, &val);
  LONGS_EQUAL(0, rv);
  LONGS_EQUAL(UINT32_MAX, val);
}

TEST(MemfaultHeartbeatMetrics, Test_SignedHeartbeatValue) {
  MemfaultMetricId key = MEMFAULT_METRICS_KEY(test_key_signed);
  int rv = memfault_metrics_heartbeat_set_signed(key, -100);
  LONGS_EQUAL(0, rv);

  // try wrong types
  rv = memfault_metrics_heartbeat_set_unsigned(key, 100);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_timer_stop(key);
  CHECK(rv != 0);

  rv = memfault_metrics_heartbeat_add(key, 1);
  LONGS_EQUAL(0, rv);
  rv = memfault_metrics_heartbeat_add(key, 1);
  LONGS_EQUAL(0, rv);
  rv = memfault_metrics_heartbeat_add(key, 2);
  LONGS_EQUAL(0, rv);
  int32_t val = 0;
  rv = memfault_metrics_heartbeat_read_signed(key, &val);
  LONGS_EQUAL(0, rv);
  LONGS_EQUAL(-96, val);

  memfault_metrics_heartbeat_add(key, INT32_MAX);
  memfault_metrics_heartbeat_add(key, INT32_MAX);
  rv = memfault_metrics_heartbeat_read_signed(key, &val);
  LONGS_EQUAL(0, rv);
  LONGS_EQUAL(INT32_MAX, val);

  memfault_metrics_heartbeat_set_signed(key, -100);
  memfault_metrics_heartbeat_add(key, INT32_MIN);
  rv = memfault_metrics_heartbeat_read_signed(key, &val);
  LONGS_EQUAL(0, rv);
  LONGS_EQUAL(INT32_MIN, val);
}

TEST(MemfaultHeartbeatMetrics, Test_TimerHeartBeatValueSimple) {
  MemfaultMetricId key = MEMFAULT_METRICS_KEY(test_key_timer);
  // no-op
  int rv = memfault_metrics_heartbeat_timer_stop(key);
  CHECK(rv != 0);

  // start the timer
  rv = memfault_metrics_heartbeat_timer_start(key);
  LONGS_EQUAL(0, rv);
  prv_fake_time_incr(10);
  // no-op
  rv = memfault_metrics_heartbeat_timer_start(key);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_add(key, 20);
  CHECK(rv != 0);

  rv = memfault_metrics_heartbeat_timer_stop(key);
  LONGS_EQUAL(0, rv);

  uint32_t val;
  memfault_metrics_heartbeat_timer_read(key, &val);
  LONGS_EQUAL(10, val);
}

TEST(MemfaultHeartbeatMetrics, Test_TimerHeartBeatValueRollover) {
  MemfaultMetricId key = MEMFAULT_METRICS_KEY(test_key_timer);

  prv_fake_time_set(0x80000000 - 9);

  int rv = memfault_metrics_heartbeat_timer_start(key);
  LONGS_EQUAL(0, rv);
  prv_fake_time_set(0x80000008);

  rv = memfault_metrics_heartbeat_timer_stop(key);
  LONGS_EQUAL(0, rv);

  uint32_t val;
  memfault_metrics_heartbeat_timer_read(key, &val);
  LONGS_EQUAL(17, val);
}

#define EXPECTED_HEARTBEAT_TIMER_VAL_MS  13

static void prv_serialize_check_cb(void) {
  MemfaultMetricId key = MEMFAULT_METRICS_KEY(test_key_timer);
  uint32_t val;
  memfault_metrics_heartbeat_timer_read(key, &val);
  LONGS_EQUAL(EXPECTED_HEARTBEAT_TIMER_VAL_MS, val);
}

TEST(MemfaultHeartbeatMetrics, Test_TimerActiveWhenHeartbeatCollected) {
  MemfaultMetricId key = MEMFAULT_METRICS_KEY(test_key_timer);

  int rv = memfault_metrics_heartbeat_timer_start(key);
  LONGS_EQUAL(0, rv);
  prv_fake_time_incr(EXPECTED_HEARTBEAT_TIMER_VAL_MS);

  s_serializer_check_cb = &prv_serialize_check_cb;
  mock().expectOneCall("memfault_metrics_heartbeat_collect_data");
  mock().expectOneCall("memfault_metrics_heartbeat_serialize");
  memfault_metrics_heartbeat_debug_trigger();

  uint32_t val;
  memfault_metrics_heartbeat_timer_read(key, &val);
  LONGS_EQUAL(0, val);

  // timer should still be running
  prv_fake_time_incr(EXPECTED_HEARTBEAT_TIMER_VAL_MS);
  rv = memfault_metrics_heartbeat_timer_stop(key);
  LONGS_EQUAL(0, rv);
  memfault_metrics_heartbeat_timer_read(key, &val);
  LONGS_EQUAL(EXPECTED_HEARTBEAT_TIMER_VAL_MS, val);

  // the time is no longer running so an increment shouldn't be counted
  prv_fake_time_incr(EXPECTED_HEARTBEAT_TIMER_VAL_MS);
  s_serializer_check_cb = &prv_serialize_check_cb;
  mock().expectOneCall("memfault_metrics_heartbeat_collect_data");
  mock().expectOneCall("memfault_metrics_heartbeat_serialize");
  memfault_metrics_heartbeat_debug_trigger();

  memfault_metrics_heartbeat_timer_read(key, &val);
  LONGS_EQUAL(0, val);
}

TEST(MemfaultHeartbeatMetrics, Test_BadBoot) {
  int rv = memfault_metrics_boot(NULL);
  LONGS_EQUAL(-3, rv);
}

TEST(MemfaultHeartbeatMetrics, Test_KeyDNE) {
  // NOTE: Using the macro MEMFAULT_METRICS_KEY, it's impossible for a non-existent key to trigger a
  // compilation error
  MemfaultMetricId key = (MemfaultMetricId){ "non_existent_key" };

  int rv = memfault_metrics_heartbeat_set_signed(key, 0);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_set_unsigned(key, 0);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_add(key, INT32_MIN);
  CHECK(rv != 0);

  rv = memfault_metrics_heartbeat_timer_start(key);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_timer_stop(key);
  CHECK(rv != 0);

  int32_t vali32;
  rv = memfault_metrics_heartbeat_read_signed(key, NULL);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_read_signed(key, &vali32);
  CHECK(rv != 0);

  uint32_t valu32;
  rv = memfault_metrics_heartbeat_read_unsigned(key, NULL);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_read_unsigned(key, &valu32);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_timer_read(key, NULL);
  CHECK(rv != 0);
  rv = memfault_metrics_heartbeat_timer_read(key, &valu32);
  CHECK(rv != 0);
}

void memfault_metrics_heartbeat_collect_data(void) {
  mock().actualCall(__func__);
}

TEST(MemfaultHeartbeatMetrics, Test_HeartbeatCollection) {
  MemfaultMetricId keyi32 = MEMFAULT_METRICS_KEY(test_key_signed);
  MemfaultMetricId keyu32 = MEMFAULT_METRICS_KEY(test_key_unsigned);

  memfault_metrics_heartbeat_set_signed(keyi32, 200);
  memfault_metrics_heartbeat_set_unsigned(keyu32, 199);

  int32_t vali32;
  uint32_t valu32;

  // should fail if we read the wrong type
  int rv = memfault_metrics_heartbeat_read_signed(keyu32, &vali32);
  CHECK(rv != 0);

  rv = memfault_metrics_heartbeat_read_signed(keyi32, &vali32);
  LONGS_EQUAL(0, rv);
  rv = memfault_metrics_heartbeat_read_unsigned(keyu32, &valu32);
  LONGS_EQUAL(0, rv);

  LONGS_EQUAL(vali32, 200);
  LONGS_EQUAL(valu32, 199);

  mock().expectOneCall("memfault_metrics_heartbeat_collect_data");
  mock().expectOneCall("memfault_metrics_heartbeat_serialize");
  memfault_metrics_heartbeat_debug_trigger();
  mock().checkExpectations();

  // values should all be reset
  memfault_metrics_heartbeat_read_signed(keyi32, &vali32);
  memfault_metrics_heartbeat_read_unsigned(keyu32, &valu32);
  LONGS_EQUAL(vali32, 0);
  LONGS_EQUAL(valu32, 0);
}
