#pragma once

// FireBeetle 2 ESP32-S3 board bring-up: the shared I²C bus and the camera's
// power rails.
void board_init();             // Wire.begin + AXP313A (V1.0 boards); call first
void board_camera_power_off(); // before deep sleep (V1.0 boards; else no-op)
