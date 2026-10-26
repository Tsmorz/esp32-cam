#pragma once
#include <stddef.h>

// OV5640 only (other sensors: setters return false, apply does nothing):
// - flicker filter: exposure in whole pulses of mains light, so no stripes
// - low light: exposure may run longer than a frame (slower video, less grain)
// Both depend on the line timing, which every size change rewrites, so
// camera_set_stream_size() calls camera_light_apply() after it.

enum CameraFlicker { FLICKER_OFF, FLICKER_AUTO, FLICKER_50HZ, FLICKER_60HZ };

// Each takes sensor_lock() for its register writes
bool camera_light_set_flicker(int mode); // CameraFlicker
bool camera_light_set_lowlight(int on);
void camera_light_apply(); // re-derive the band steps from the live timing

int camera_light_flicker();
int camera_light_lowlight();

// ,"flicker":…,"lowlight":…,"flicker_hz":…,"timing":{…} for /camera/status
// (leading comma, no braces; empty for other sensors). flicker_hz: what the
// filter uses — the sensor's own detection in auto, 0 when off. timing: the
// values written, plus the exposure (lines) and gain (1/16) the AEC chose.
size_t camera_light_json(char *buf, size_t size);
