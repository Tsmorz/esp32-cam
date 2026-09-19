// The detections JSON: field set, timestamps, and the UDP-sized compact form.
// Run with: pio test -e native

#include "detections_json.h"
#include <string.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

static DetectionFrame frame(int tags) {
  DetectionFrame f = {};
  f.ms = 5000;
  f.seq = 42;
  f.t_us = 123456789;
  f.width = 800;
  f.height = 600;
  f.cam = {800, 800, 400, 300, {}};
  f.count = tags;
  for (int i = 0; i < tags; i++) {
    TagDetection &t = f.tags[i];
    t.id = 586 - i;
    t.tx = -0.12345;
    t.ty = -0.12345;
    t.tz = -1.23456;
    t.q[0] = 0.5f;
    t.q[1] = -0.5f;
    t.q[2] = -0.5f;
    t.q[3] = -0.5f;
    for (int c = 0; c < 4; c++)
      t.corners[c][0] = t.corners[c][1] = 1234.5f;
  }
  return f;
}

static const DetectionsJson FULL = {true, true, 0.165, 5200, 123999999};
static const DetectionsJson COMPACT = {false, true, 0.165, 5200, 123999999};

void test_reports_capture_time_and_now() {
  char buf[3072];
  const DetectionFrame f = frame(1);
  detections_json(f, FULL, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"seq\":42,"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"t_us\":123456789,"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"now_us\":123999999,"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"age_ms\":200,"));
}

void test_imu_orientation_or_null() {
  char buf[3072];
  DetectionFrame f = frame(0);
  detections_json(f, FULL, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"imu_q\":null,"));
  f.has_imu = true;
  f.imu_q[0] = 1;
  detections_json(f, FULL, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"imu_q\":[1.00000,0.00000,0.00000,"));
}

void test_nothing_yet() {
  char buf[3072];
  DetectionFrame f = {};
  detections_json(f, FULL, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"age_ms\":-1,"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"tags\":[]}"));
}

void test_full_has_corners_and_camera_compact_does_not() {
  char buf[3072];
  const DetectionFrame f = frame(2);
  detections_json(f, FULL, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"corners\":"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"cam\":{"));
  detections_json(f, COMPACT, buf, sizeof(buf));
  TEST_ASSERT_NULL(strstr(buf, "\"corners\":"));
  TEST_ASSERT_NULL(strstr(buf, "\"cam\":{"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"pose\":{"));
}

void test_full_frame_fits_the_http_buffer() {
  char buf[3072];
  const DetectionFrame f = frame(MAX_TAGS);
  const size_t n = detections_json(f, FULL, buf, sizeof(buf));
  TEST_ASSERT_LESS_THAN(sizeof(buf) - 1, n); // not truncated
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

void test_compact_frame_fits_one_datagram() {
  char buf[1472]; // Ethernet MTU − IP − UDP headers
  DetectionFrame f = frame(MAX_TAGS);
  f.has_imu = true;
  const size_t n = detections_json(f, COMPACT, buf, sizeof(buf));
  TEST_ASSERT_LESS_THAN(sizeof(buf) - 1, n);
  TEST_ASSERT_EQUAL_CHAR('}', buf[n - 1]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_reports_capture_time_and_now);
  RUN_TEST(test_imu_orientation_or_null);
  RUN_TEST(test_nothing_yet);
  RUN_TEST(test_full_has_corners_and_camera_compact_does_not);
  RUN_TEST(test_full_frame_fits_the_http_buffer);
  RUN_TEST(test_compact_frame_fits_one_datagram);
  return UNITY_END();
}
