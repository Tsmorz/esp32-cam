#pragma once

// Everything the board remembers across reboots lives in NVS (Preferences),
// one namespace per module. This is the one place that names them, versions
// the layout, and resets them.

namespace settings {
static constexpr const char *SYS = "sys";     // schema version
static constexpr const char *CAMERA = "cam";  // sensor settings + stream size
static constexpr const char *DETECT = "det";  // detection size
static constexpr const char *WIFI = "wifi";   // network picked in the dashboard
static constexpr const char *CALIB = "calib"; // camera calibration
} // namespace settings

// Bump when a namespace's keys change meaning, and add the step to
// settings_init(). 1 = the layout the firmware has always used.
static constexpr int SETTINGS_SCHEMA = 1;

void settings_init();  // first thing in setup(): migrate old layouts
int settings_schema(); // what is stored (0 = fresh flash)
bool settings_reset(const char *scope); // camera|detector|wifi|calibration|all
