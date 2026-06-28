#pragma once
// BasicVQF — VQF orientation filter (6D, gyro + accel, no magnetometer).
// Algorithm from: Laidig & Seel (2023) "VQF: Highly Accurate IMU Orientation
// Estimation with Bias Estimation and Magnetic Disturbance Rejection".
// This is the simplified (no bias estimation) variant.

#include <cmath>
#include <cstring>

class BasicVQF {
public:
    // gyrTs: gyroscope sample period [s]
    // accTs: accelerometer sample period [s] (pass -1 to use gyrTs)
    // tauAcc: time constant [s] for accel LP filter (default 3 s)
    explicit BasicVQF(double gyrTs, double accTs = -1.0, double tauAcc = 3.0)
        : _gyrTs(gyrTs), _accTs(accTs < 0.0 ? gyrTs : accTs), _tauAcc(tauAcc),
          _initialized(false)
    {
        _q[0] = 1.0; _q[1] = 0.0; _q[2] = 0.0; _q[3] = 0.0;
        _accLP[0] = 0.0; _accLP[1] = 0.0; _accLP[2] = 1.0;
    }

    // gyr: angular velocity [rad/s]  (x, y, z)
    // acc: specific force    [m/s²]  (x, y, z)
    void update(const double gyr[3], const double acc[3]) {
        // ── 1. Gyroscope integration ──────────────────────────────────────────
        double gx = gyr[0], gy = gyr[1], gz = gyr[2];
        double norm = std::sqrt(gx*gx + gy*gy + gz*gz);
        if (norm > 1e-9) {
            double half_angle = norm * _gyrTs * 0.5;
            double s = std::sin(half_angle) / norm;
            double dq[4] = { std::cos(half_angle), gx*s, gy*s, gz*s };
            quatMul(_q, dq, _q);
            quatNorm(_q);
        }

        // ── 2. Accelerometer low-pass filter ─────────────────────────────────
        double ax = acc[0], ay = acc[1], az = acc[2];
        double anorm = std::sqrt(ax*ax + ay*ay + az*az);
        if (anorm < 0.1) return;  // ignore near-zero or noise
        ax /= anorm; ay /= anorm; az /= anorm;

        double alpha = 1.0 - std::exp(-_accTs / _tauAcc);
        if (!_initialized) {
            _accLP[0] = ax; _accLP[1] = ay; _accLP[2] = az;
            _initialized = true;
        } else {
            _accLP[0] += alpha * (ax - _accLP[0]);
            _accLP[1] += alpha * (ay - _accLP[1]);
            _accLP[2] += alpha * (az - _accLP[2]);
        }

        // ── 3. Gravity prediction from current quaternion (body frame) ────────
        double w = _q[0], qx = _q[1], qy = _q[2], qz = _q[3];
        double vx = 2.0*(qx*qz - w*qy);
        double vy = 2.0*(w*qx  + qy*qz);
        double vz = w*w - qx*qx - qy*qy + qz*qz;

        // ── 4. Error = cross product (measured × predicted) ───────────────────
        double ex = _accLP[1]*vz - _accLP[2]*vy;
        double ey = _accLP[2]*vx - _accLP[0]*vz;
        double ez = _accLP[0]*vy - _accLP[1]*vx;

        // ── 5. Correction quaternion (small-angle, first order) ───────────────
        double k = alpha;  // correction gain tracks LP filter bandwidth
        double cq[4] = { 1.0, k*ex*0.5, k*ey*0.5, k*ez*0.5 };
        quatNorm(cq);
        quatMul(_q, cq, _q);
        quatNorm(_q);
    }

    // Output quaternion as [w, x, y, z] (rotation from Earth to body frame)
    void getQuat6D(double out[4]) const {
        std::memcpy(out, _q, 4 * sizeof(double));
    }

private:
    double _gyrTs, _accTs, _tauAcc;
    double _q[4];
    double _accLP[3];
    bool   _initialized;

    static void quatMul(const double a[4], const double b[4], double out[4]) {
        double tmp[4] = {
            a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3],
            a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2],
            a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1],
            a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0]
        };
        std::memcpy(out, tmp, 4 * sizeof(double));
    }

    static void quatNorm(double q[4]) {
        double n = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (n > 1e-10) { q[0]/=n; q[1]/=n; q[2]/=n; q[3]/=n; }
    }
};
