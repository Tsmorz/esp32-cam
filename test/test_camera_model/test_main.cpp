// Camera model: estimate, calibration rescaling, lens undistortion.
// Run with: pio test -e native

#include "camera_model.h"
#include <stdlib.h>
#include <unity.h>

static const Calibration NONE = {800, 600, 0, 0, 0, 0, {0, 0, 0, 0, 0}};
// A plausible wide-angle calibration, as cv2.calibrateCamera would report it
static const Calibration CAL = {
    800, 600, 812.5, 810.0, 405.0, 297.0, {-0.28, 0.09, 0.001, -0.0015, 0.0}};

void setUp() {}
void tearDown() {}

static void expect_model(const CameraModel &m, double fx, double fy, double cx,
                         double cy) {
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, fx, m.fx);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, fy, m.fy);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, cx, m.cx);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, cy, m.cy);
}

void test_estimate_uses_full_view_width() {
  expect_model(camera_model(800, 600, NONE), 800, 800, 400, 300);
  // 240×240 is a crop of the 320×240 view; 1280×720 keeps the full width
  expect_model(camera_model(240, 240, NONE), 320, 320, 120, 120);
  expect_model(camera_model(1280, 720, NONE), 1280, 1280, 640, 360);
  TEST_ASSERT_FALSE(has_distortion(camera_model(800, 600, NONE)));
}

void test_calibration_rescales_to_other_sizes() {
  expect_model(camera_model(800, 600, CAL), 812.5, 810, 405, 297);
  expect_model(camera_model(1600, 1200, CAL), 1625, 1620, 810, 594);
  expect_model(camera_model(400, 300, CAL), 406.25, 405, 202.5, 148.5);
}

void test_calibration_follows_crops() {
  // 1:1 — full view is 320 wide, 40 columns cut from each side
  expect_model(camera_model(240, 240, CAL), 325, 324, 405 * 0.4 - 40,
               297 * 0.4);
  // 16:9 — full view is 1280×960, 120 rows cut from the top
  expect_model(camera_model(1280, 720, CAL), 1300, 1296, 648, 297 * 1.6 - 120);
}

void test_distortion_is_resolution_independent() {
  const CameraModel a = camera_model(800, 600, CAL),
                    b = camera_model(240, 240, CAL);
  for (int i = 0; i < 5; i++)
    TEST_ASSERT_EQUAL_DOUBLE(a.dist[i], b.dist[i]);
}

// OpenCV's forward model, independent of the code under test
static void distort(const CameraModel &m, double x, double y, double &u,
                    double &v) {
  const double *d = m.dist, r2 = x * x + y * y;
  const double rad = 1 + r2 * (d[0] + r2 * (d[1] + r2 * d[4]));
  u = (x * rad + 2 * d[2] * x * y + d[3] * (r2 + 2 * x * x)) * m.fx + m.cx;
  v = (y * rad + d[2] * (r2 + 2 * y * y) + 2 * d[3] * x * y) * m.fy + m.cy;
}

void test_undistort_inverts_distortion_across_the_frame() {
  const CameraModel m = camera_model(800, 600, CAL);
  srand(1);
  double worst = 0;
  for (int i = 0; i < 5000; i++) {
    const double x = (rand() / (double)RAND_MAX - 0.5) * 1.0; // whole frame
    const double y = (rand() / (double)RAND_MAX - 0.5) * 0.75;
    double u, v;
    distort(m, x, y, u, v);
    undistort_point(m, u, v);
    worst = fmax(worst, hypot(u - (x * m.fx + m.cx), v - (y * m.fy + m.cy)));
  }
  TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0, worst);
}

void test_undistort_without_distortion_is_identity() {
  const CameraModel m = camera_model(800, 600, NONE);
  double u = 123.4, v = 567.8;
  undistort_point(m, u, v);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 123.4, u);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 567.8, v);
}

// Whatever is in src/calibration.h must at least be self-consistent
void test_calibration_file_is_sane() {
  if (!camera_calibrated(CALIBRATION))
    TEST_IGNORE_MESSAGE("calibration.h not filled in — using the estimate");
  const Calibration &c = CALIBRATION;
  TEST_ASSERT_TRUE(c.width > 0 && c.height > 0);
  TEST_ASSERT_TRUE(c.fx > 0 && c.fy > 0);
  TEST_ASSERT_TRUE_MESSAGE(c.fx / c.fy > 0.8 && c.fx / c.fy < 1.25,
                           "fx and fy should be close for this sensor");
  TEST_ASSERT_TRUE_MESSAGE(c.cx > 0 && c.cx < c.width && c.cy > 0 &&
                               c.cy < c.height,
                           "principal point outside the calibration image");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_estimate_uses_full_view_width);
  RUN_TEST(test_calibration_rescales_to_other_sizes);
  RUN_TEST(test_calibration_follows_crops);
  RUN_TEST(test_distortion_is_resolution_independent);
  RUN_TEST(test_undistort_inverts_distortion_across_the_frame);
  RUN_TEST(test_undistort_without_distortion_is_identity);
  RUN_TEST(test_calibration_file_is_sane);
  return UNITY_END();
}
