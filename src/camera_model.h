#pragma once

// Pinhole camera with OpenCV (Brown–Conrady) lens distortion, built from a
// Calibration for any frame size. Plain math, no Arduino, so it can be
// compiled and tested on a PC.

#include "calibration.h"
#include <math.h>

struct CameraModel {
  double fx, fy, cx, cy; // pixels
  double dist[5];        // k1, k2, p1, p2, k3
};

// A calibration: intrinsics measured at width × height
struct Calibration {
  int width, height;
  double fx, fy, cx, cy;
  double dist[5];
};

// The compiled-in default from calibration.h. The firmware uses the one stored
// on the board (calibration_store.h) when there is one.
static constexpr Calibration CALIBRATION = {CALIB_WIDTH, CALIB_HEIGHT, CALIB_FX,
                                            CALIB_FY,    CALIB_CX,     CALIB_CY,
                                            CALIB_DIST};

// Estimate used while there is no calibration
static constexpr double FOCAL_PER_WIDTH = 1.0;

static inline bool camera_calibrated(const Calibration &cal) {
  return cal.fx > 0;
}

// Sanity limits for a calibration someone sends to the board. A real one has
// fx ≈ fy and its principal point inside the image; anything else is a typo or
// a calibration of something else.
static inline bool calibration_valid(const Calibration &c) {
  if (c.width < 1 || c.width > 4096 || c.height < 1 || c.height > 4096)
    return false;
  if (!(c.fx > 0 && c.fy > 0 && c.fx / c.fy > 0.5 && c.fx / c.fy < 2))
    return false;
  if (!(c.cx > 0 && c.cx < c.width && c.cy > 0 && c.cy < c.height))
    return false;
  for (double d : c.dist)
    if (!(fabs(d) <= 100)) // also rejects NaN
      return false;
  return true;
}

// Width, in w×h pixels, of the sensor's full 4:3 view. Other aspect ratios are
// centred crops of it (esp32-camera's OV5640 ratio_table): 1:1 and 5:4 keep
// the full height, 16:9 and 3:2 the full width.
static inline double full_view_width(int w, int h) {
  return fmax(w, h * 4.0 / 3.0);
}

// Intrinsics for a w×h frame
static inline CameraModel camera_model(int w, int h, const Calibration &cal) {
  const double F = full_view_width(w, h);
  if (!camera_calibrated(cal))
    return {FOCAL_PER_WIDTH * F, FOCAL_PER_WIDTH * F, w / 2.0, h / 2.0, {}};
  // Calibration → full-view coordinates → this frame's scale and crop
  const double Fc = full_view_width(cal.width, cal.height);
  const double k = F / Fc;
  const double cx_full = cal.cx + (Fc - cal.width) / 2;
  const double cy_full = cal.cy + (Fc * 0.75 - cal.height) / 2;
  const double *d = cal.dist;
  return {cal.fx * k,
          cal.fy * k,
          cx_full * k - (F - w) / 2,
          cy_full * k - (F * 0.75 - h) / 2,
          {d[0], d[1], d[2], d[3], d[4]}};
}

static inline bool has_distortion(const CameraModel &m) {
  for (double d : m.dist)
    if (d != 0)
      return true;
  return false;
}

// Remove lens distortion from pixel (u, v) in place, by the same fixed-point
// iteration as OpenCV's undistortPoints()
static inline void undistort_point(const CameraModel &m, double &u, double &v) {
  const double *d = m.dist;
  const double xd = (u - m.cx) / m.fx, yd = (v - m.cy) / m.fy;
  double x = xd, y = yd;
  for (int i = 0; i < 10; i++) {
    const double r2 = x * x + y * y;
    const double radial = 1 + r2 * (d[0] + r2 * (d[1] + r2 * d[4]));
    const double dx = 2 * d[2] * x * y + d[3] * (r2 + 2 * x * x);
    const double dy = d[2] * (r2 + 2 * y * y) + 2 * d[3] * x * y;
    x = (xd - dx) / radial;
    y = (yd - dy) / radial;
  }
  u = x * m.fx + m.cx;
  v = y * m.fy + m.cy;
}
