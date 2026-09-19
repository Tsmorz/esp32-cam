// IMU orientation history: lookup by time, interpolation, ring wrap-around.
// Run with: pio test -e native

#include "orientation_history.h"
#include <unity.h>

void setUp() {}
void tearDown() {}

// Rotation of `deg` about z
static void qz(float deg, float q[4]) {
  const float h = deg * (float)M_PI / 360.f;
  q[0] = cosf(h);
  q[1] = q[2] = 0;
  q[3] = sinf(h);
}

static float yaw_deg(const float q[4]) {
  return 2 * atan2f(q[3], q[0]) * 180.f / (float)M_PI;
}

static bool lookup(const OrientationHistory<16> &h, int64_t t, float out[4]) {
  OrientationSample a, b;
  if (!h.bracket(t, a, b))
    return false;
  orientation_interpolate(a, b, t, out);
  return true;
}

void test_empty_has_nothing() {
  OrientationHistory<16> h;
  float q[4];
  TEST_ASSERT_FALSE(lookup(h, 0, q));
}

void test_interpolates_between_samples() {
  OrientationHistory<16> h;
  float q[4], out[4];
  qz(0, q);
  h.push(1000000, q);
  qz(90, q);
  h.push(1010000, q);
  TEST_ASSERT_TRUE(lookup(h, 1005000, out));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 45.f, yaw_deg(out));
  TEST_ASSERT_TRUE(lookup(h, 1002500, out));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 22.5f, yaw_deg(out));
}

void test_exact_hit_and_unit_length() {
  OrientationHistory<16> h;
  float q[4], out[4];
  for (int i = 0; i < 10; i++) {
    qz(i * 10.f, q);
    h.push(1000000 + i * 10000, q);
  }
  TEST_ASSERT_TRUE(lookup(h, 1050000, out));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.f, yaw_deg(out));
  float n = 0;
  for (int i = 0; i < 4; i++)
    n += out[i] * out[i];
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.f, n);
}

void test_outside_the_history_is_refused() {
  OrientationHistory<16> h;
  float q[4], out[4];
  qz(0, q);
  h.push(1000000, q);
  h.push(1010000, q);
  TEST_ASSERT_FALSE(lookup(h, 900000, out));  // long before
  TEST_ASSERT_FALSE(lookup(h, 1200000, out)); // long after
  TEST_ASSERT_TRUE(lookup(h, 1015000, out));  // 5 ms past the newest: clamped
  TEST_ASSERT_TRUE(lookup(h, 995000, out));   // 5 ms before the oldest
}

void test_old_samples_fall_off_the_ring() {
  OrientationHistory<16> h;
  float q[4], out[4];
  for (int i = 0; i < 40; i++) { // 16 kept: i = 24..39
    qz((float)i, q);
    h.push(1000000 + i * 10000, q);
  }
  TEST_ASSERT_EQUAL_INT(16, h.size());
  TEST_ASSERT_FALSE(lookup(h, 1000000 + 5 * 10000, out)); // dropped
  TEST_ASSERT_TRUE(lookup(h, 1000000 + 30 * 10000 + 5000, out));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 30.5f, yaw_deg(out));
}

void test_takes_the_short_way_round() {
  OrientationHistory<16> h;
  float a[4], b[4], out[4];
  qz(10, a);
  qz(350, b); // −10°, written as the opposite-hemisphere quaternion
  h.push(0, a);
  h.push(10000, b);
  TEST_ASSERT_TRUE(lookup(h, 5000, out));
  const float yaw = yaw_deg(out);
  TEST_ASSERT_TRUE_MESSAGE(fabsf(yaw) < 1.f || fabsf(fabsf(yaw) - 360.f) < 1.f,
                           "went the long way round");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_empty_has_nothing);
  RUN_TEST(test_interpolates_between_samples);
  RUN_TEST(test_exact_hit_and_unit_length);
  RUN_TEST(test_outside_the_history_is_refused);
  RUN_TEST(test_old_samples_fall_off_the_ring);
  RUN_TEST(test_takes_the_short_way_round);
  return UNITY_END();
}
