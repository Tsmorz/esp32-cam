#include "imu.h"
#include "basicvqf.hpp"
#include "orientation_history.h"
#include <Arduino.h>
#include <Wire.h>
#include <atomic>
#include <esp_timer.h>

// ── LSM6DSOX register map
// ─────────────────────────────────────────────────────
#define LSM_ADDR 0x6A   // SA0/SDO pin LOW; use 0x6B if pulled HIGH
#define LSM_WHO 0x0F    // WHO_AM_I → must return 0x6C
#define LSM_CTRL1 0x10  // accel control
#define LSM_CTRL2 0x11  // gyro  control
#define LSM_CTRL3 0x12  // general control (BDU, IF_INC)
#define LSM_STATUS 0x1E // bit1 GDA: new gyro sample available
#define LSM_OUT 0x22    // first output register: OUTX_L_G

// ── Sensitivity constants
// ──────────────────────────────────────────────────────
// Accel ±2 g  @ 0.061 mg/LSB → m/s²
static constexpr double ACCEL_SCALE = 0.000061 * 9.80665;
// Gyro  250 dps @ 8.75 mdps/LSB → rad/s
static constexpr double GYRO_SCALE = 0.00875 * (M_PI / 180.0);

static constexpr double ODR_HZ = 104.0;
// Poll at twice the ODR so every sample is caught exactly once
static constexpr uint32_t POLL_MS = 5;
// Gyro bias calibration: the first second of samples, if the board is still
static constexpr int CAL_SAMPLES = 104;
static constexpr double CAL_MAX_RATE = 0.05; // rad/s (~3 °/s) → "moving"

static BasicVQF *_vqf = nullptr;
static double _quat[4] = {1, 0, 0, 0};
static std::atomic<uint32_t> _samples{0}, _errors{0};
static std::atomic<bool> _calibrated{false};
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
// One entry per 104 Hz sample: ~2.4 s, longer than the slowest detection
static OrientationHistory<256> _hist;

// ── I2C helpers
// ───────────────────────────────────────────────────────────────
static bool rd(uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(LSM_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom((uint8_t)LSM_ADDR, n) != n)
    return false;
  for (uint8_t i = 0; i < n; i++)
    out[i] = Wire.read();
  return true;
}

static void wr(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(LSM_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void imu_task(void *) {
  double bias[3] = {0, 0, 0};
  double sum[3] = {0, 0, 0};
  int cal = 0;
  bool moving = false;

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    xTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

    uint8_t status, b[12];
    if (!rd(LSM_STATUS, &status, 1)) {
      _errors++;
      continue;
    }
    if (!(status & 0x02))
      continue; // no new sample since the last read
    const int64_t t_us = esp_timer_get_time();
    if (!rd(LSM_OUT, b, 12)) { // 6 gyro + 6 accel bytes
      _errors++;
      continue;
    }
    _samples++;

    double gyr[3], acc[3];
    for (int i = 0; i < 3; i++) {
      gyr[i] = (int16_t)((b[2 * i + 1] << 8) | b[2 * i]) * GYRO_SCALE;
      acc[i] = (int16_t)((b[2 * i + 7] << 8) | b[2 * i + 6]) * ACCEL_SCALE;
    }

    if (cal < CAL_SAMPLES) {
      for (int i = 0; i < 3; i++) {
        sum[i] += gyr[i];
        moving |= fabs(gyr[i]) > CAL_MAX_RATE;
      }
      if (++cal == CAL_SAMPLES) {
        if (moving) {
          Serial.println("[imu] moved during calibration — gyro bias not "
                         "removed, expect yaw drift");
        } else {
          for (int i = 0; i < 3; i++)
            bias[i] = sum[i] / CAL_SAMPLES;
          Serial.printf("[imu] gyro bias [%+.2f %+.2f %+.2f] °/s\n",
                        bias[0] * 180 / M_PI, bias[1] * 180 / M_PI,
                        bias[2] * 180 / M_PI);
        }
        _calibrated = true;
      }
      continue;
    }

    for (int i = 0; i < 3; i++)
      gyr[i] -= bias[i];
    _vqf->update(gyr, acc);

    double q[4];
    _vqf->getQuat6D(q);
    const float qf[4] = {(float)q[0], (float)q[1], (float)q[2], (float)q[3]};
    portENTER_CRITICAL(&_mux);
    memcpy(_quat, q, sizeof(q));
    _hist.push(t_us, qf);
    portEXIT_CRITICAL(&_mux);
  }
}

// ── Public API
// ────────────────────────────────────────────────────────────────
bool imu_init() {
  uint8_t who = 0;
  if (!rd(LSM_WHO, &who, 1) || who != 0x6C) {
    Serial.println("[imu] LSM6DSOX not found (WHO_AM_I != 0x6C)");
    return false;
  }
  wr(LSM_CTRL3, 0x44); // BDU=1, IF_INC=1 (block-data-update + auto-increment)
  wr(LSM_CTRL2, 0x40); // gyro:  104 Hz, 250 dps
  wr(LSM_CTRL1, 0x40); // accel: 104 Hz, ±2 g

  _vqf = new BasicVQF(1.0 / ODR_HZ);
  Serial.println("[imu] LSM6DSOX ready — keep still ~1 s for gyro calibration");
  return true;
}

void imu_start(BaseType_t core, UBaseType_t prio) {
  if (!_vqf)
    return;
  xTaskCreatePinnedToCore(imu_task, "imu", 4096, nullptr, prio, nullptr, core);
}

void imu_get_quat(double quat[4]) {
  portENTER_CRITICAL(&_mux);
  memcpy(quat, _quat, sizeof(_quat));
  portEXIT_CRITICAL(&_mux);
}

bool imu_quat_at(int64_t t_us, float q[4]) {
  OrientationSample a, b;
  portENTER_CRITICAL(&_mux);
  const bool ok = _hist.bracket(t_us, a, b);
  portEXIT_CRITICAL(&_mux);
  if (!ok)
    return false;
  orientation_interpolate(a, b, t_us, q);
  return true;
}

const char *imu_state(uint32_t *samples, uint32_t *errors) {
  *samples = _samples;
  *errors = _errors;
  if (!_vqf)
    return "none";
  return _calibrated ? "ok" : "calibrating";
}
