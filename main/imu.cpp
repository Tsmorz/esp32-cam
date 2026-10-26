#include "imu.h"
#include "basicvqf.hpp"
#include "mag_cal.h"
#include "options.h"
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <atomic>
#include <esp_attr.h>
#include <esp_system.h>

// ── LSM6DSOX register map
// ─────────────────────────────────────────────────────
// 0x6A with SA0/SDO low (Adafruit's default), 0x6B with it high
static const uint8_t LSM_ADDRS[] = {0x6A, 0x6B};
#define LSM_WHO 0x0F    // WHO_AM_I → must return 0x6C
#define LSM_CTRL1 0x10  // accel control
#define LSM_CTRL2 0x11  // gyro  control
#define LSM_CTRL3 0x12  // general control (BDU, IF_INC)
#define LSM_CTRL6 0x15  // XL_HM_MODE (bit4): 1 = accel low-power modes
#define LSM_WU_SRC 0x1B // wake-up source; reading it clears a latched event
#define LSM_STATUS 0x1E // bit1 GDA: new gyro sample available
#define LSM_OUT 0x22    // first output register: OUTX_L_G
#define LSM_TAP0 0x56   // LIR (bit0): latch interrupts
#define LSM_TAP2 0x58   // INTERRUPTS_ENABLE (bit7)
#define LSM_WU_THS 0x5B // wake-up threshold, FS/64 per LSB
#define LSM_WU_DUR 0x5C // wake-up duration
#define LSM_MD1 0x5E    // INT1 routing: INT1_WU (bit5)

// ── LIS3MDL register map
// ──────────────────────────────────────────────────────
// 0x1C with SDO/SA1 low (Adafruit's default), 0x1E with it high
static const uint8_t MAG_ADDRS[] = {0x1C, 0x1E};
#define MAG_WHO 0x0F    // WHO_AM_I → must return 0x3D
#define MAG_CTRL1 0x20  // XY mode, output rate
#define MAG_CTRL2 0x21  // full scale
#define MAG_CTRL3 0x22  // continuous / single / off
#define MAG_CTRL4 0x23  // Z mode
#define MAG_CTRL5 0x24  // BDU
#define MAG_STATUS 0x27 // bit3 ZYXDA: new sample on all axes
#define MAG_OUT 0x28    // OUT_X_L; multi-byte reads need bit 7 set
#define MAG_AUTO_INC 0x80

// ── Rates and scales
// ──────────────────────────────────────────────────────────
// Below 100 Hz on purpose: orientation for a camera doesn't need more,
// and the I²C bus is shared with the camera.
static constexpr double ODR_HZ = 52.0;     // accel + gyro
static constexpr double MAG_ODR_HZ = 40.0; // magnetometer
// Accel ±4 g @ 0.122 mg/LSB → m/s²
static constexpr double ACCEL_SCALE = 0.000122 * 9.80665;
// Gyro 500 dps @ 17.5 mdps/LSB → rad/s (a camera turned by hand exceeds 250
// dps)
static constexpr double GYRO_SCALE = 0.0175 * (M_PI / 180.0);
// Magnetometer ±4 gauss @ 6842 LSB/gauss → µT (1 gauss = 100 µT)
static constexpr float MAG_SCALE = 100.0f / 6842.0f;

// Poll at about twice the accel/gyro rate so every sample is caught once
static constexpr uint32_t POLL_MS = 10;
// Gyro bias calibration: the first second of samples, if the board is still
static constexpr int CAL_SAMPLES = (int)ODR_HZ;
static constexpr double CAL_MAX_RATE = 0.05; // rad/s (~3 °/s) → "moving"

static uint8_t _lsm = 0, _mag = 0; // I²C addresses found; 0 = absent
static BasicVQF *_vqf = nullptr;
static double _quat[4] = {1, 0, 0, 0};
static float _field[3] = {0, 0, 0};
static bool _have_field = false, _heading_mag = false;
static std::atomic<uint32_t> _samples{0}, _errors{0};
static std::atomic<bool> _calibrated{false};
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
// Compass calibration in use, and the one being collected (under _mux)
static MagCal _cal;
static MagCalRun _run;
static bool _cal_running = false;
static std::atomic<bool> _cal_changed{false}; // → VQF converges again
static std::atomic<bool> _moved{false};       // imu_motion_take()
static TaskHandle_t _task = nullptr;

// Gyro bias survives deep sleep: after a wake by motion the board is moving,
// so the first second can't be used to measure it again
RTC_DATA_ATTR static double _rtc_bias[3];
RTC_DATA_ATTR static bool _rtc_bias_ok = false;

// Software motion detection while awake, by options().motion_sens (1–3):
// turning faster than this, or a jolt this far from 1 g
static constexpr double MOVE_GYR[] = {0, 1.0, 0.5, 0.25}; // rad/s
static constexpr double MOVE_ACC[] = {0, 3.0, 1.5, 0.8};  // m/s²
// The same for waking from sleep, in WU_THS steps of 62.5 mg (±4 g / 64)
static constexpr uint8_t WAKE_THS[] = {0, 8, 4, 2};

// ── I2C helpers
// ───────────────────────────────────────────────────────────────
static bool rd(uint8_t addr, uint8_t reg, uint8_t *out, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0)
    return false;
  if (Wire.requestFrom(addr, n) != n)
    return false;
  for (uint8_t i = 0; i < n; i++)
    out[i] = Wire.read();
  return true;
}

static void wr(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t probe(const uint8_t *addrs, size_t n, uint8_t who_reg,
                     uint8_t who) {
  for (size_t i = 0; i < n; i++) {
    uint8_t v = 0;
    if (rd(addrs[i], who_reg, &v, 1) && v == who)
      return addrs[i];
  }
  return 0;
}

static int16_t le16(const uint8_t *b) { return (int16_t)((b[1] << 8) | b[0]); }

// A new magnetometer sample, in µT; false if none since the last call
static bool read_mag(float ut[3]) {
  uint8_t status, b[6];
  if (!rd(_mag, MAG_STATUS, &status, 1)) {
    _errors++;
    return false;
  }
  if (!(status & 0x08))
    return false;
  if (!rd(_mag, MAG_OUT | MAG_AUTO_INC, b, 6)) {
    _errors++;
    return false;
  }
  for (int i = 0; i < 3; i++)
    ut[i] = le16(b + 2 * i) * MAG_SCALE;
  return true;
}

static void imu_task(void *) {
  double bias[3] = {0, 0, 0};
  double sum[3] = {0, 0, 0};
  int cal = 0;
  bool moving = false;
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && _rtc_bias_ok) {
    memcpy(bias, _rtc_bias, sizeof(bias));
    cal = CAL_SAMPLES;
    _calibrated = true;
    Serial.println("[imu] woke from sleep: gyro bias from before");
  }

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    xTaskDelayUntil(&wake, pdMS_TO_TICKS(POLL_MS));

    uint8_t status, b[12];
    if (!rd(_lsm, LSM_STATUS, &status, 1)) {
      _errors++;
      continue;
    }
    if (status & 0x02) {               // a new gyro (and accel) sample
      if (!rd(_lsm, LSM_OUT, b, 12)) { // 6 gyro + 6 accel bytes
        _errors++;
        continue;
      }
      _samples++;
      double gyr[3], acc[3];
      for (int i = 0; i < 3; i++) {
        gyr[i] = le16(b + 2 * i) * GYRO_SCALE;
        acc[i] = le16(b + 2 * i + 6) * ACCEL_SCALE;
      }
      if (cal < CAL_SAMPLES) {
        for (int i = 0; i < 3; i++) {
          sum[i] += gyr[i];
          moving |= fabs(gyr[i]) > CAL_MAX_RATE;
        }
        if (++cal == CAL_SAMPLES) {
          if (moving) {
            Serial.println("[imu] moved during calibration — gyro bias not "
                           "removed, expect drift");
          } else {
            for (int i = 0; i < 3; i++)
              _rtc_bias[i] = bias[i] = sum[i] / CAL_SAMPLES;
            _rtc_bias_ok = true;
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
      const int s = options().motion_sens;
      const double g =
          sqrt(gyr[0] * gyr[0] + gyr[1] * gyr[1] + gyr[2] * gyr[2]);
      const double a =
          sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
      if (s >= 1 && s <= 3 &&
          (g > MOVE_GYR[s] || fabs(a - 9.80665) > MOVE_ACC[s]))
        _moved = true;
    }
    if (!_calibrated)
      continue;

    // The magnetometer runs slower; fuse each of its samples as it comes
    float raw[3];
    if (_mag && read_mag(raw)) {
      MagCal c;
      portENTER_CRITICAL(&_mux);
      if (_cal_running)
        _run.add(raw);
      c = _cal;
      portEXIT_CRITICAL(&_mux);
      double m[3];
      mag_apply(c, raw, m);
      if (_cal_changed.exchange(false))
        _vqf->restartHeading();
      // Uncalibrated, the field is mostly the board's own iron: heading would
      // be worse than the gyro's drift, so it isn't used until calibrated
      if (c.valid)
        _vqf->updateMag(m);
      portENTER_CRITICAL(&_mux);
      for (int i = 0; i < 3; i++)
        _field[i] = m[i];
      _have_field = true;
      _heading_mag = c.valid;
      portEXIT_CRITICAL(&_mux);
    }

    double q[4];
    if (_heading_mag)
      _vqf->getQuat9D(q);
    else
      _vqf->getQuat6D(q);
    portENTER_CRITICAL(&_mux);
    memcpy(_quat, q, sizeof(q));
    portEXIT_CRITICAL(&_mux);
  }
}

static void cal_load() {
  Preferences prefs;
  MagCal c;
  if (prefs.begin(settings::IMU, true)) {
    if (prefs.getBytesLength("mag") == sizeof(c))
      prefs.getBytes("mag", &c, sizeof(c));
    prefs.end();
  }
  _cal = c;
  if (c.valid)
    Serial.printf("[imu] compass calibration: offset [%.1f %.1f %.1f] µT, "
                  "scale [%.2f %.2f %.2f]\n",
                  c.offset[0], c.offset[1], c.offset[2], c.scale[0], c.scale[1],
                  c.scale[2]);
}

// ── Public API
// ────────────────────────────────────────────────────────────────
bool imu_init() {
  _lsm = probe(LSM_ADDRS, sizeof(LSM_ADDRS), LSM_WHO, 0x6C);
  if (!_lsm) {
    Serial.println("[imu] LSM6DSOX not found at 0x6A/0x6B");
    return false;
  }
  wr(_lsm, LSM_CTRL3, 0x44); // BDU=1, IF_INC=1 (block-data-update + auto-inc)
  // Undo imu_prepare_wake(): high-performance mode, no wake-up interrupt
  wr(_lsm, LSM_CTRL6, 0x00);
  wr(_lsm, LSM_MD1, 0x00);
  wr(_lsm, LSM_TAP2, 0x00);
  uint8_t src;
  rd(_lsm, LSM_WU_SRC, &src, 1); // clears the latched event that woke us
  wr(_lsm, LSM_CTRL2, 0x34);     // gyro:  52 Hz, 500 dps
  wr(_lsm, LSM_CTRL1, 0x38);     // accel: 52 Hz, ±4 g

  _mag = probe(MAG_ADDRS, sizeof(MAG_ADDRS), MAG_WHO, 0x3D);
  if (_mag) {
    wr(_mag, MAG_CTRL1, 0x78); // XY ultra-high performance, 40 Hz
    wr(_mag, MAG_CTRL2, 0x00); // ±4 gauss
    wr(_mag, MAG_CTRL4, 0x0C); // Z ultra-high performance
    wr(_mag, MAG_CTRL5, 0x40); // BDU
    wr(_mag, MAG_CTRL3, 0x00); // continuous conversion
    cal_load();
  }

  _vqf = new BasicVQF(1.0 / ODR_HZ, 3.0, 1.0 / MAG_ODR_HZ);
  Serial.printf("[imu] LSM6DSOX at 0x%02X%s — keep still ~1 s for gyro "
                "calibration\n",
                _lsm, _mag ? ", LIS3MDL compass" : ", no compass");
  return true;
}

void imu_start(BaseType_t core, UBaseType_t prio) {
  if (!_vqf)
    return;
  xTaskCreatePinnedToCore(imu_task, "imu", 4096, nullptr, prio, &_task, core);
}

bool imu_motion_take() { return _moved.exchange(false); }

bool imu_present() { return _lsm != 0; }

bool imu_prepare_wake(int sens) {
  if (!_lsm || sens < 1 || sens > 3)
    return false;
  if (_task)
    vTaskSuspend(_task);     // the bus is ours now
  wr(_lsm, LSM_CTRL2, 0x00); // gyro off
  wr(_lsm, LSM_CTRL6, 0x10); // accel low-power mode
  wr(_lsm, LSM_CTRL1, 0x28); // accel 26 Hz, ±4 g
  wr(_lsm, LSM_WU_DUR, 0x00);
  wr(_lsm, LSM_WU_THS, WAKE_THS[sens]);
  wr(_lsm, LSM_TAP0, 0x01); // latch: INT1 stays high until WU_SRC is read
  wr(_lsm, LSM_TAP2, 0x80); // interrupts on
  wr(_lsm, LSM_MD1, 0x20);  // wake-up → INT1 (push-pull, active high)
  if (_mag)
    wr(_mag, MAG_CTRL3, 0x03); // compass off (~0.3 mA in UHP)
  delay(100);                  // filters settle; drop what that triggered
  uint8_t src;
  rd(_lsm, LSM_WU_SRC, &src, 1);
  return true;
}

void imu_get_quat(double quat[4]) {
  portENTER_CRITICAL(&_mux);
  memcpy(quat, _quat, sizeof(_quat));
  portEXIT_CRITICAL(&_mux);
}

const char *imu_state(uint32_t *samples, uint32_t *errors) {
  *samples = _samples;
  *errors = _errors;
  if (!_vqf)
    return "none";
  return _calibrated ? "ok" : "calibrating";
}

const char *imu_mag_state() {
  if (!_mag)
    return "none";
  portENTER_CRITICAL(&_mux);
  const bool running = _cal_running, valid = _cal.valid;
  portEXIT_CRITICAL(&_mux);
  return running ? "calibrating" : valid ? "ok" : "uncalibrated";
}

bool imu_heading_from_mag() {
  portENTER_CRITICAL(&_mux);
  const bool h = _heading_mag;
  portEXIT_CRITICAL(&_mux);
  return h;
}

bool imu_mag_field(float ut[3]) {
  portENTER_CRITICAL(&_mux);
  const bool ok = _have_field;
  memcpy(ut, _field, sizeof(_field));
  portEXIT_CRITICAL(&_mux);
  return ok;
}

bool imu_mag_cal_start() {
  if (!_mag)
    return false;
  portENTER_CRITICAL(&_mux);
  _run.reset();
  _cal_running = true;
  portEXIT_CRITICAL(&_mux);
  Serial.println("[imu] compass calibration: turn the board in every "
                 "direction");
  return true;
}

float imu_mag_cal_progress() {
  portENTER_CRITICAL(&_mux);
  const float p = _cal_running ? _run.progress() : 0;
  portEXIT_CRITICAL(&_mux);
  return p;
}

bool imu_mag_cal_finish() {
  MagCal c;
  portENTER_CRITICAL(&_mux);
  const bool ok = _cal_running && _run.result(c);
  if (ok)
    _cal_running = false;
  portEXIT_CRITICAL(&_mux);
  if (!ok)
    return false;
  Preferences prefs;
  if (prefs.begin(settings::IMU, false)) {
    prefs.putBytes("mag", &c, sizeof(c));
    prefs.end();
  }
  portENTER_CRITICAL(&_mux);
  _cal = c;
  portEXIT_CRITICAL(&_mux);
  _cal_changed = true;
  Serial.printf("[imu] compass calibrated: offset [%.1f %.1f %.1f] µT, "
                "scale [%.2f %.2f %.2f]\n",
                c.offset[0], c.offset[1], c.offset[2], c.scale[0], c.scale[1],
                c.scale[2]);
  return true;
}

void imu_mag_cal_cancel() {
  portENTER_CRITICAL(&_mux);
  _cal_running = false;
  portEXIT_CRITICAL(&_mux);
}

void imu_mag_cal_clear() {
  Preferences prefs;
  if (prefs.begin(settings::IMU, false)) {
    prefs.remove("mag");
    prefs.end();
  }
  portENTER_CRITICAL(&_mux);
  _cal = MagCal();
  _cal_running = false;
  portEXIT_CRITICAL(&_mux);
}
