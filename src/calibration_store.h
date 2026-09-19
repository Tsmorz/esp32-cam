#pragma once
#include "camera_model.h"
#include <stddef.h>

// The camera calibration in use. It is kept in flash (Preferences "calib") so
// it survives reboots and OTA updates, and is set over HTTP — see /calibration.
// Falls back to the compiled-in one in calibration.h, then to the estimate.
void calibration_load();          // call once at boot
Calibration calibration_get();    // thread-safe copy
const char *calibration_source(); // "stored" | "built-in" | "estimate"
bool calibration_set(const Calibration &c); // false: invalid; else persisted
void calibration_clear();                   // forget the stored one
size_t calibration_json(char *buf, size_t size);
