#pragma once

// A short history of the IMU's orientation, so the orientation at the moment
// a camera frame was captured can be looked up afterwards (tag detection
// finishes 0.2–2 s after the capture). No Arduino, tested on the host.
//
// Not thread-safe: the owner locks around push() and bracket().

#include <math.h>
#include <stdint.h>

struct OrientationSample {
  int64_t t_us;
  float q[4]; // [w, x, y, z]
};

template <int N> class OrientationHistory {
public:
  void push(int64_t t_us, const float q[4]) {
    OrientationSample &s = _s[_head];
    s.t_us = t_us;
    for (int i = 0; i < 4; i++)
      s.q[i] = q[i];
    _head = (_head + 1) % N;
    if (_n < N)
      _n++;
  }

  // The two samples around t_us. Within `slack_us` of either end it clamps to
  // that end (a frame captured a moment after the newest sample, say). False
  // if t_us is outside the history or nothing has been pushed.
  bool bracket(int64_t t_us, OrientationSample &a, OrientationSample &b,
               int64_t slack_us = 20000) const {
    if (_n == 0)
      return false;
    const OrientationSample &oldest = at(0), &newest = at(_n - 1);
    if (t_us < oldest.t_us - slack_us || t_us > newest.t_us + slack_us)
      return false;
    if (t_us <= oldest.t_us) {
      a = b = oldest;
      return true;
    }
    if (t_us >= newest.t_us) {
      a = b = newest;
      return true;
    }
    int lo = 0, hi = _n - 1; // at(lo).t <= t_us < at(hi).t
    while (hi - lo > 1) {
      const int mid = (lo + hi) / 2;
      if (at(mid).t_us <= t_us)
        lo = mid;
      else
        hi = mid;
    }
    a = at(lo);
    b = at(hi);
    return true;
  }

  int size() const { return _n; }

private:
  // i-th oldest sample
  const OrientationSample &at(int i) const {
    return _s[(_head - _n + i + N) % N];
  }
  OrientationSample _s[N] = {};
  int _head = 0, _n = 0;
};

// Orientation at t_us between two bracketing samples (shortest-arc slerp)
static inline void orientation_interpolate(const OrientationSample &a,
                                           const OrientationSample &b,
                                           int64_t t_us, float out[4]) {
  if (b.t_us <= a.t_us) {
    for (int i = 0; i < 4; i++)
      out[i] = a.q[i];
    return;
  }
  float u = (float)(t_us - a.t_us) / (float)(b.t_us - a.t_us);
  u = u < 0 ? 0 : (u > 1 ? 1 : u);
  float dot = 0, qb[4];
  for (int i = 0; i < 4; i++)
    dot += a.q[i] * b.q[i];
  const float sign = dot < 0 ? -1.f : 1.f; // q and −q are the same rotation
  for (int i = 0; i < 4; i++)
    qb[i] = sign * b.q[i];
  dot *= sign;
  float wa, wb;
  if (dot > 0.9995f) { // nearly parallel: lerp, then normalise
    wa = 1 - u;
    wb = u;
  } else {
    const float theta = acosf(dot), s = sinf(theta);
    wa = sinf((1 - u) * theta) / s;
    wb = sinf(u * theta) / s;
  }
  float n = 0;
  for (int i = 0; i < 4; i++) {
    out[i] = wa * a.q[i] + wb * qb[i];
    n += out[i] * out[i];
  }
  n = 1.f / sqrtf(n);
  for (int i = 0; i < 4; i++)
    out[i] *= n;
}
