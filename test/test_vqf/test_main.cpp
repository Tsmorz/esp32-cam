// BasicVQF with a magnetometer, and the compass calibration, on synthetic
// samples: a board held still at a known orientation in a known field.
#include "basicvqf.hpp"
#include "mag_cal.h"
#include <cmath>
#include <initializer_list>
#include <unity.h>

void setUp() {}
void tearDown() {}

static constexpr double TS = 1.0 / 52, MAG_TS = 1.0 / 40;
static const double G = 9.81;
// Earth frame East-North-Up; the field points north and down (~50 µT)
static const double FIELD_E[3] = {0.0, 20.0, -45.0};

// Rotation matrix (sensor → Earth) of yaw about z, then pitch about the new x
static void rot(double yaw, double tilt, double R[3][3]) {
  const double cy = cos(yaw), sy = sin(yaw), ct = cos(tilt), st = sin(tilt);
  const double Rz[3][3] = {{cy, -sy, 0}, {sy, cy, 0}, {0, 0, 1}};
  const double Rx[3][3] = {{1, 0, 0}, {0, ct, -st}, {0, st, ct}};
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) {
      R[i][j] = 0;
      for (int k = 0; k < 3; k++)
        R[i][j] += Rz[i][k] * Rx[k][j];
    }
}

// v in the sensor frame = Rᵀ · v_earth
static void to_sensor(const double R[3][3], const double e[3], double s[3]) {
  for (int i = 0; i < 3; i++)
    s[i] = R[0][i] * e[0] + R[1][i] * e[1] + R[2][i] * e[2];
}

// The rotation q describes, applied to v
static void qrot(const double q[4], const double v[3], double out[3]) {
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  const double R[3][3] = {
      {1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
      {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
      {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  for (int i = 0; i < 3; i++)
    out[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2];
}

// Largest angle [deg] between where q and R send the sensor's x and z axes
static double err_deg(const double q[4], const double R[3][3]) {
  double worst = 0;
  for (int a : {0, 2}) {
    const double e[3] = {a == 0 ? 1.0 : 0.0, 0.0, a == 2 ? 1.0 : 0.0};
    double got[3];
    qrot(q, e, got);
    const double dot = got[0] * R[0][a] + got[1] * R[1][a] + got[2] * R[2][a];
    worst = fmax(worst, acos(fmin(1.0, fmax(-1.0, dot))) * 180 / M_PI);
  }
  return worst;
}

// Still board at (yaw, tilt) for `seconds`; mag fused at its own rate
static void run(BasicVQF &f, double yaw, double tilt, double seconds,
                bool mag = true) {
  double R[3][3], acc[3], m[3];
  const double up[3] = {0, 0, G}, gyr[3] = {0, 0, 0};
  rot(yaw, tilt, R);
  to_sensor(R, up, acc);
  to_sensor(R, FIELD_E, m);
  double next_mag = 0;
  for (double t = 0; t < seconds; t += TS) {
    f.update(gyr, acc);
    if (mag && t >= next_mag) {
      f.updateMag(m);
      next_mag += MAG_TS;
    }
  }
}

void test_heading_follows_the_magnetometer() {
  for (double yaw_deg : {0.0, 60.0, -120.0, 175.0}) {
    BasicVQF f(TS, 3.0, MAG_TS);
    run(f, yaw_deg * M_PI / 180, 0, 6);
    double q[4], R[3][3];
    f.getQuat9D(q);
    rot(yaw_deg * M_PI / 180, 0, R);
    TEST_ASSERT_LESS_THAN_DOUBLE(0.5, err_deg(q, R));
  }
}

void test_heading_with_tilt() {
  BasicVQF f(TS, 3.0, MAG_TS);
  const double yaw = 40 * M_PI / 180, tilt = 35 * M_PI / 180;
  run(f, yaw, tilt, 15);
  double q[4], R[3][3];
  f.getQuat9D(q);
  rot(yaw, tilt, R);
  TEST_ASSERT_LESS_THAN_DOUBLE(1.0, err_deg(q, R));
}

void test_6d_ignores_the_magnetometer() {
  BasicVQF f(TS, 3.0, MAG_TS);
  run(f, 90 * M_PI / 180, 0, 6);
  double q[4], R[3][3];
  f.getQuat6D(q);
  rot(0, 0, R); // gyro-only heading stays where it started
  TEST_ASSERT_LESS_THAN_DOUBLE(0.5, err_deg(q, R));
}

void test_heading_tracks_a_turn() {
  // Converged at 0°, then the board is turned to 90°: the gyro carries the
  // turn (simulated by restarting at the new heading without gyro) — here we
  // check the slow tauMag path: after the fast start, a new heading is
  // reached within a few tauMag
  BasicVQF f(TS, 3.0, MAG_TS);
  run(f, 0, 0, 12); // past the fast initial convergence
  run(f, M_PI / 2, 0, 45);
  double q[4], R[3][3];
  f.getQuat9D(q);
  rot(M_PI / 2, 0, R);
  TEST_ASSERT_LESS_THAN_DOUBLE(2.0, err_deg(q, R));
}

// ── Compass calibration ─────────────────────────────────────────────────────
static const float OFFSET[3] = {-35.0f, 12.0f, 80.0f};
static const float GAIN[3] = {1.1f, 0.9f, 1.05f};

// Field of 50 µT seen through a hard-iron offset and per-axis gain
static void distorted(double yaw, double pitch, float out[3]) {
  const double v[3] = {cos(pitch) * cos(yaw), cos(pitch) * sin(yaw),
                       sin(pitch)};
  for (int i = 0; i < 3; i++)
    out[i] = OFFSET[i] + GAIN[i] * 50.0f * (float)v[i];
}

void test_calibration_recovers_offset_and_scale() {
  MagCalRun r;
  for (int p = -80; p <= 80; p += 10)
    for (int y = 0; y < 360; y += 10) {
      float m[3];
      distorted(y * M_PI / 180, p * M_PI / 180, m);
      r.add(m);
    }
  TEST_ASSERT_EQUAL_FLOAT(1.0f, r.progress());
  MagCal c;
  TEST_ASSERT_TRUE(r.result(c));
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_FLOAT_WITHIN(1.0f, OFFSET[i], c.offset[i]);
  // Corrected samples all have (about) the same length
  float lo = 1e9f, hi = 0;
  for (int y = 0; y < 360; y += 7) {
    float m[3];
    double out[3];
    distorted(y * M_PI / 180, 0.3, m);
    mag_apply(c, m, out);
    const float n =
        (float)sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    lo = fminf(lo, n);
    hi = fmaxf(hi, n);
  }
  TEST_ASSERT_LESS_THAN_FLOAT(0.03f, (hi - lo) / hi);
}

void test_calibration_needs_every_axis() {
  // Turning only about the vertical axis leaves z unexplored
  MagCalRun r;
  for (int k = 0; k < 400; k++) {
    float m[3];
    distorted(k * M_PI / 100, 0, m);
    r.add(m);
  }
  TEST_ASSERT_LESS_THAN_FLOAT(0.5f, r.progress());
  MagCal c;
  TEST_ASSERT_FALSE(r.result(c));
  TEST_ASSERT_FALSE(c.valid);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_heading_follows_the_magnetometer);
  RUN_TEST(test_heading_with_tilt);
  RUN_TEST(test_6d_ignores_the_magnetometer);
  RUN_TEST(test_heading_tracks_a_turn);
  RUN_TEST(test_calibration_recovers_offset_and_scale);
  RUN_TEST(test_calibration_needs_every_axis);
  return UNITY_END();
}
