#pragma once
#include <freertos/FreeRTOS.h>
#include <stdint.h>

// Orientation from an LSM6DSOX (accel + gyro) and, if present, an LIS3MDL
// magnetometer — Adafruit's 9-DoF board (product 4517) has both — fused with
// VQF. Everything works without the board: imu_state() says "none".

bool imu_init(); // probe + configure; call after Wire.begin()
void imu_start(BaseType_t core, UBaseType_t prio); // spawn the sampling task
// Sensor → Earth (East-North-Up) [w, x, y, z]. Heading follows magnetic north
// once the compass is calibrated; before that it comes from the gyro alone
// and drifts.
void imu_get_quat(double quat[4]);
// "none" (not found), "calibrating" (gyro bias, first second) or "ok";
// samples and I²C read errors
const char *imu_state(uint32_t *samples, uint32_t *errors);

// ── Compass (LIS3MDL) ────────────────────────────────────────────────────────
// "none" (no magnetometer), "uncalibrated", "calibrating" or "ok"
const char *imu_mag_state();
bool imu_heading_from_mag(); // the orientation's heading uses the compass
// Latest field in µT (calibrated once a calibration exists); false if none
bool imu_mag_field(float ut[3]);
// Calibration: start, turn the board through every direction, then finish.
// finish() stores it in flash; false if not enough rotation was seen (see
// imu_mag_cal_progress()). clear() forgets the stored one.
bool imu_mag_cal_start();
float imu_mag_cal_progress(); // 0…1 while calibrating
bool imu_mag_cal_finish();
void imu_mag_cal_cancel();
void imu_mag_cal_clear();

// ── Motion and wake ──────────────────────────────────────────────────────────
bool imu_present();
// The camera was moved or bumped since the last call (options().motion_sens)
bool imu_motion_take();
// Before deep sleep: stops the task, gyro and compass off, accelerometer in
// low-power mode raising INT1 on movement (sens 1–3). The wire from INT1 to
// options().imu_int is what wakes the board.
bool imu_prepare_wake(int sens);
