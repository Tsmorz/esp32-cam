#pragma once

// OV5640 autofocus. The sensor's focus motor is driven by an internal MCU
// whose firmware must be uploaded over SCCB at every boot. Fixed-focus sensors
// (OV2640, OV3660, ...) report "none" and ignore mode changes.

enum FocusMode {
  FOCUS_INFINITY = 0,   // release the lens to its rest (far) position
  FOCUS_SINGLE = 1,     // focus once now, then hold
  FOCUS_CONTINUOUS = 2, // refocus whenever the scene changes
};

bool focus_init(); // after camera_init(); false if unsupported or it failed
bool focus_available();
bool focus_set_mode(int mode); // FocusMode; SINGLE re-triggers each call
int focus_mode();              // -1 when unavailable
// "none", "idle", "focusing", "focused" or "error"
const char *focus_state();
int focus_raw_status(); // FW_STATUS register (0x3029), -1 when unavailable
