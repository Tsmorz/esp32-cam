#pragma once
#include <stdint.h>

bool imu_init();
void imu_update();
void imu_get_quat(double quat[4]);  // [w, x, y, z]
