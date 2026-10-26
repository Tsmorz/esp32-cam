#pragma once
#include <math.h>

// Magnetometer calibration: hard-iron offset and per-axis (diagonal soft-iron)
// scale, from the extremes seen while the board is turned through every
// direction. Header-only, no Arduino: host-tested.
//
// Nearby iron and currents (the ESP32, the camera, a battery) add a constant
// offset that can be larger than the Earth's field; uncorrected, heading is
// wrong by tens of degrees or barely moves.

struct MagCal {
  float offset[3] = {0, 0, 0}; // µT, subtracted
  float scale[3] = {1, 1, 1};  // then multiplied
  bool valid = false;
};

// The Earth's field is 25–65 µT, so a full turn sweeps an axis through at
// least ~50 µT; ask for 60 µT, or 80 % of the widest axis where the field is
// stronger.
static constexpr float MAG_CAL_MIN_SPAN_UT = 60.0f;
static constexpr int MAG_CAL_MIN_SAMPLES = 200; // ~5 s at 40 Hz

struct MagCalRun {
  float lo[3], hi[3];
  int n = 0;

  MagCalRun() { reset(); }

  void reset() {
    for (int i = 0; i < 3; i++) {
      lo[i] = INFINITY;
      hi[i] = -INFINITY;
    }
    n = 0;
  }

  void add(const float m[3]) {
    for (int i = 0; i < 3; i++) {
      if (m[i] < lo[i])
        lo[i] = m[i];
      if (m[i] > hi[i])
        hi[i] = m[i];
    }
    n++;
  }

  // 0…1: how much of the needed rotation has been seen, by the worst axis
  float progress() const {
    if (n < 2)
      return 0;
    float widest = 0, worst = 1;
    for (int i = 0; i < 3; i++)
      widest = fmaxf(widest, hi[i] - lo[i]);
    const float need = fmaxf(MAG_CAL_MIN_SPAN_UT, 0.8f * widest);
    for (int i = 0; i < 3; i++)
      worst = fminf(worst, (hi[i] - lo[i]) / need);
    if (n < MAG_CAL_MIN_SAMPLES)
      worst = fminf(worst, (float)n / MAG_CAL_MIN_SAMPLES);
    return fmaxf(0, fminf(1, worst));
  }

  // The calibration, if enough was seen and it looks like the Earth's field
  bool result(MagCal &out) const {
    if (progress() < 1)
      return false;
    float span[3], mean = 0;
    for (int i = 0; i < 3; i++) {
      span[i] = hi[i] - lo[i];
      mean += span[i] / 3;
    }
    const float field = mean / 2; // radius of the sphere, µT
    if (field < 15 || field > 150)
      return false;
    MagCal c;
    for (int i = 0; i < 3; i++) {
      c.offset[i] = (hi[i] + lo[i]) / 2;
      c.scale[i] = mean / span[i];
      if (c.scale[i] < 0.5f || c.scale[i] > 2.0f)
        return false; // one axis twice another: not a usable sweep
    }
    c.valid = true;
    out = c;
    return true;
  }
};

inline void mag_apply(const MagCal &c, const float raw[3], double out[3]) {
  for (int i = 0; i < 3; i++)
    out[i] = (raw[i] - c.offset[i]) * c.scale[i];
}
