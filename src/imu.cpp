#include "imu.h"
#include "basicvqf.hpp"
#include <Arduino.h>
#include <Wire.h>

// ── LSM6DSOX register map ─────────────────────────────────────────────────────
#define LSM_ADDR  0x6A   // SA0/SDO pin LOW; use 0x6B if pulled HIGH
#define LSM_WHO   0x0F   // WHO_AM_I → must return 0x6C
#define LSM_CTRL1 0x10   // accel control
#define LSM_CTRL2 0x11   // gyro  control
#define LSM_CTRL3 0x12   // general control (BDU, IF_INC)
#define LSM_OUT   0x22   // first output register: OUTX_L_G

// ── Sensitivity constants ──────────────────────────────────────────────────────
// Accel ±2 g  @ 0.061 mg/LSB → m/s²
static constexpr double ACCEL_SCALE = 0.000061 * 9.80665;
// Gyro  250 dps @ 8.75 mdps/LSB → rad/s
static constexpr double GYRO_SCALE  = 0.00875 * (M_PI / 180.0);

static BasicVQF *_vqf    = nullptr;
static double    _quat[4] = {1, 0, 0, 0};

// ── I2C helpers ───────────────────────────────────────────────────────────────
static uint8_t rd(uint8_t reg) {
    Wire.beginTransmission(LSM_ADDR);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(LSM_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

static void wr(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(LSM_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// ── Public API ────────────────────────────────────────────────────────────────
bool imu_init() {
    if (rd(LSM_WHO) != 0x6C) {
        Serial.println("[imu] LSM6DSOX not found (WHO_AM_I != 0x6C)");
        return false;
    }
    wr(LSM_CTRL3, 0x44);  // BDU=1, IF_INC=1 (block-data-update + auto-increment)
    wr(LSM_CTRL2, 0x40);  // gyro:  104 Hz, 250 dps
    wr(LSM_CTRL1, 0x40);  // accel: 104 Hz, ±2 g

    _vqf = new BasicVQF(1.0 / 104.0);  // 104 Hz sample period
    Serial.println("[imu] LSM6DSOX ready, VQF initialized at 104 Hz");
    return true;
}

void imu_update() {
    if (!_vqf) return;

    // Burst-read 12 bytes starting at OUTX_L_G: 6 gyro + 6 accel
    Wire.beginTransmission(LSM_ADDR);
    Wire.write(LSM_OUT);
    Wire.endTransmission(false);
    Wire.requestFrom(LSM_ADDR, (uint8_t)12);

    uint8_t buf[12] = {};
    for (int i = 0; Wire.available() && i < 12; i++) buf[i] = Wire.read();

    int16_t gx_raw = (int16_t)((buf[1]  << 8) | buf[0]);
    int16_t gy_raw = (int16_t)((buf[3]  << 8) | buf[2]);
    int16_t gz_raw = (int16_t)((buf[5]  << 8) | buf[4]);
    int16_t ax_raw = (int16_t)((buf[7]  << 8) | buf[6]);
    int16_t ay_raw = (int16_t)((buf[9]  << 8) | buf[8]);
    int16_t az_raw = (int16_t)((buf[11] << 8) | buf[10]);

    double gyr[3] = { gx_raw * GYRO_SCALE,  gy_raw * GYRO_SCALE,  gz_raw * GYRO_SCALE  };
    double acc[3] = { ax_raw * ACCEL_SCALE, ay_raw * ACCEL_SCALE, az_raw * ACCEL_SCALE };

    _vqf->update(gyr, acc);
    _vqf->getQuat6D(_quat);
}

void imu_get_quat(double quat[4]) {
    for (int i = 0; i < 4; i++) quat[i] = _quat[i];
}
