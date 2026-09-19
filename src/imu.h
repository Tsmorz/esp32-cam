#pragma once
#include <freertos/FreeRTOS.h>
#include <stdint.h>

bool imu_init(); // probe + configure; call after Wire.begin()
void imu_start(BaseType_t core, UBaseType_t prio); // spawn the sampling task
void imu_get_quat(double quat[4]);                 // [w, x, y, z]
// "none" (not found), "calibrating" or "ok"; samples and I²C read errors
const char *imu_state(uint32_t *samples, uint32_t *errors);
