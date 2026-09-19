#pragma once
#include <freertos/FreeRTOS.h>
#include <stdint.h>

bool imu_init(); // probe + configure; call after Wire.begin()
void imu_start(BaseType_t core, UBaseType_t prio); // spawn the sampling task
void imu_get_quat(double quat[4]);                 // [w, x, y, z]
// Orientation at t_us (esp_timer clock, within the last ~2.4 s), interpolated
// from the sample history; false if the IMU is absent, still calibrating, or
// t_us is outside the history. q = [w, x, y, z].
bool imu_quat_at(int64_t t_us, float q[4]);
// "none" (not found), "calibrating" or "ok"; samples and I²C read errors
const char *imu_state(uint32_t *samples, uint32_t *errors);
