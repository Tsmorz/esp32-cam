#pragma once
// BasicVQF — 6D (gyro + accel) and 9D (+ magnetometer) orientation, no bias
// estimation. Port of BasicVQF from Laidig & Seel (2023) "VQF: Highly
// Accurate IMU Orientation Estimation with Bias Estimation and Magnetic
// Disturbance Rejection", https://github.com/dlaidig/vqf
//
// The gyroscope is integrated into gyrQuat. Accelerometer samples are rotated
// into that almost-inertial frame *before* low-pass filtering, so the filter
// does not lag behind rotations. The filtered gravity direction then drives
// an inclination correction (accQuat). 6D output = accQuat ⊗ gyrQuat.
//
// The magnetometer only corrects heading: each sample is rotated into the 6D
// Earth frame, its horizontal direction gives the heading error, and a
// first-order filter (tauMag) tracks it as delta. 9D output = a rotation by
// delta about the vertical ⊗ the 6D output, in an East-North-Up frame.
// Without magnetometer samples yaw comes from the gyro alone and drifts, so
// gyro bias must be removed by the caller.

#include <cmath>
#include <cstring>

class BasicVQF {
public:
  // gyrTs: gyro sample period [s]; tauAcc: accel low-pass time constant [s];
  // magTs: magnetometer sample period [s] (≤ 0: same as the gyro); tauMag:
  // heading correction time constant [s]
  explicit BasicVQF(double gyrTs, double tauAcc = 3.0, double magTs = -1.0,
                    double tauMag = 9.0)
      : _ts(gyrTs), _tauAcc(tauAcc), _magTs(magTs > 0 ? magTs : gyrTs),
        _tauMag(tauMag), _kMag(gainFromTau(tauMag, _magTs)) {
    filterCoeffs(tauAcc, gyrTs, _accLpB, _accLpA);
    reset();
  }

  void reset() {
    _gyrQuat[0] = 1.0;
    _gyrQuat[1] = _gyrQuat[2] = _gyrQuat[3] = 0.0;
    std::memcpy(_accQuat, _gyrQuat, sizeof(_accQuat));
    _accLpState[0] = NAN; // NaN = still averaging the first tauAcc seconds
    _accLpState[1] = NAN;
    _lastAccLp[0] = _lastAccLp[1] = _lastAccLp[2] = 0.0;
    _delta = 0.0;
    _kMagInit = 1.0;
  }

  // gyr: angular velocity [rad/s]; acc: specific force [m/s²] (body frame)
  void update(const double gyr[3], const double acc[3]) {
    updateGyr(gyr);
    updateAcc(acc);
  }

  // mag: magnetic field in the same body frame as gyr/acc (any unit; hard- and
  // soft-iron corrected by the caller). [0, 0, 0] is ignored.
  void updateMag(const double mag[3]) {
    if (mag[0] == 0.0 && mag[1] == 0.0 && mag[2] == 0.0)
      return;
    double q6[4], magEarth[3];
    getQuat6D(q6);
    quatRotate(q6, mag, magEarth);
    double dis = wrap(std::atan2(magEarth[0], magEarth[1]) - _delta);
    double k = _kMag;
    // Converge quickly at first: the gain is at least 1/N for the first N
    // samples, until tauMag has passed
    if (_kMagInit != 0.0) {
      if (k < _kMagInit)
        k = _kMagInit;
      _kMagInit = _kMagInit / (_kMagInit + 1.0);
      if (_kMagInit * _tauMag < _magTs)
        _kMagInit = 0.0;
    }
    _delta = wrap(_delta + k * dis);
  }

  // Orientation of the sensor relative to the Earth frame (z up), [w, x, y, z]
  void getQuat6D(double out[4]) const { quatMul(_accQuat, _gyrQuat, out); }

  // As getQuat6D, with heading from the magnetometer: East-North-Up
  void getQuat9D(double out[4]) const {
    double q[4];
    getQuat6D(q);
    const double c = std::cos(_delta / 2), s = std::sin(_delta / 2);
    out[0] = c * q[0] - s * q[3];
    out[1] = c * q[1] - s * q[2];
    out[2] = c * q[2] + s * q[1];
    out[3] = c * q[3] + s * q[0];
  }

  // Heading correction the magnetometer applies to the 6D output [rad]
  double getDelta() const { return _delta; }

  // Converge quickly again, as after reset(), e.g. once the magnetometer's
  // calibration has changed (not in the reference implementation)
  void restartHeading() { _kMagInit = 1.0; }

private:
  double _ts, _tauAcc;
  double _magTs, _tauMag, _kMag;
  double _delta, _kMagInit;
  double _gyrQuat[4];
  double _accQuat[4];
  double _accLpB[3] = {}, _accLpA[2] = {};
  // [0..5] filter state for 3 axes; while initialising, [1] holds the sample
  // count and [2..4] the running sum.
  double _accLpState[6];
  double _lastAccLp[3];

  void updateGyr(const double gyr[3]) {
    double n = std::sqrt(gyr[0] * gyr[0] + gyr[1] * gyr[1] + gyr[2] * gyr[2]);
    if (n > 1e-9) {
      double half = n * _ts * 0.5;
      double s = std::sin(half) / n;
      double dq[4] = {std::cos(half), gyr[0] * s, gyr[1] * s, gyr[2] * s};
      quatMul(_gyrQuat, dq, _gyrQuat);
      quatNorm(_gyrQuat);
    }
  }

  void updateAcc(const double acc[3]) {
    if (acc[0] == 0.0 && acc[1] == 0.0 && acc[2] == 0.0)
      return;

    // Low-pass in the gyro-integrated (almost inertial) frame
    double accI[3];
    quatRotate(_gyrQuat, acc, accI);
    filterVec(accI, _lastAccLp);

    // Into the 6D Earth frame, then rotate so filtered gravity points along +z
    double accE[3];
    quatRotate(_accQuat, _lastAccLp, accE);
    double n =
        std::sqrt(accE[0] * accE[0] + accE[1] * accE[1] + accE[2] * accE[2]);
    if (n < 1e-9)
      return;
    accE[0] /= n;
    accE[1] /= n;
    accE[2] /= n;

    double corr[4];
    double qw = std::sqrt((accE[2] + 1.0) / 2.0);
    if (qw > 1e-6) {
      corr[0] = qw;
      corr[1] = 0.5 * accE[1] / qw;
      corr[2] = -0.5 * accE[0] / qw;
      corr[3] = 0.0;
    } else { // correction ≈ 180°: pick an axis to avoid dividing by ~0
      corr[0] = 0.0;
      corr[1] = 1.0;
      corr[2] = 0.0;
      corr[3] = 0.0;
    }
    quatMul(corr, _accQuat, _accQuat);
    quatNorm(_accQuat);
  }

  static double gainFromTau(double tau, double ts) {
    if (tau < 0.0)
      return 0.0; // disabled
    if (tau == 0.0)
      return 1.0; // no filtering
    return 1.0 - std::exp(-ts / tau);
  }

  static double wrap(double a) {
    if (a > M_PI)
      return a - 2 * M_PI;
    if (a < -M_PI)
      return a + 2 * M_PI;
    return a;
  }

  // Second-order Butterworth low-pass, as in the reference implementation
  static void filterCoeffs(double tau, double ts, double b[3], double a[2]) {
    double fc = (M_SQRT2 / (2.0 * M_PI)) / tau;
    double c = std::tan(M_PI * fc * ts);
    double d = c * c + M_SQRT2 * c + 1.0;
    b[0] = c * c / d;
    b[1] = 2.0 * b[0];
    b[2] = b[0];
    a[0] = 2.0 * (c * c - 1.0) / d;
    a[1] = (1.0 - M_SQRT2 * c + c * c) / d;
  }

  void filterVec(const double x[3], double out[3]) {
    double *st = _accLpState;
    if (std::isnan(st[0])) {
      // Average the first tauAcc seconds, then start the filter in steady
      // state at that mean so one noisy first sample can't skew it.
      if (std::isnan(st[1])) {
        st[1] = 0.0;
        st[2] = st[3] = st[4] = 0.0;
      }
      st[1] += 1.0;
      for (int i = 0; i < 3; i++) {
        st[2 + i] += x[i];
        out[i] = st[2 + i] / st[1];
      }
      if (st[1] * _ts >= _tauAcc) {
        for (int i = 0; i < 3; i++) {
          st[2 * i] = out[i] * (1.0 - _accLpB[0]);
          st[2 * i + 1] = out[i] * (_accLpB[2] - _accLpA[1]);
        }
      }
      return;
    }
    for (int i = 0; i < 3; i++) {
      double *s = st + 2 * i;
      double y = _accLpB[0] * x[i] + s[0];
      s[0] = _accLpB[1] * x[i] - _accLpA[0] * y + s[1];
      s[1] = _accLpB[2] * x[i] - _accLpA[1] * y;
      out[i] = y;
    }
  }

  static void quatRotate(const double q[4], const double v[3], double out[3]) {
    double x = (1 - 2 * q[2] * q[2] - 2 * q[3] * q[3]) * v[0] +
               2 * v[1] * (q[2] * q[1] - q[0] * q[3]) +
               2 * v[2] * (q[0] * q[2] + q[3] * q[1]);
    double y = 2 * v[0] * (q[0] * q[3] + q[2] * q[1]) +
               v[1] * (1 - 2 * q[1] * q[1] - 2 * q[3] * q[3]) +
               2 * v[2] * (q[2] * q[3] - q[1] * q[0]);
    double z = 2 * v[0] * (q[3] * q[1] - q[0] * q[2]) +
               2 * v[1] * (q[0] * q[1] + q[3] * q[2]) +
               v[2] * (1 - 2 * q[1] * q[1] - 2 * q[2] * q[2]);
    out[0] = x;
    out[1] = y;
    out[2] = z;
  }

  static void quatMul(const double a[4], const double b[4], double out[4]) {
    double tmp[4] = {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
                     a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
                     a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
                     a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
    std::memcpy(out, tmp, sizeof(tmp));
  }

  static void quatNorm(double q[4]) {
    double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-10) {
      q[0] /= n;
      q[1] /= n;
      q[2] /= n;
      q[3] /= n;
    }
  }
};
