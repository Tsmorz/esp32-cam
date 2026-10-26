#pragma once

// Everything the board remembers across reboots lives in NVS (Preferences),
// one namespace per module. This is the one place that names them, versions
// the layout, and resets them.

namespace settings {
static constexpr const char *SYS = "sys";    // schema version, API token
static constexpr const char *CAMERA = "cam"; // sensor settings + stream size
static constexpr const char *WIFI = "wifi";  // network picked in the dashboard
static constexpr const char *IMU = "imu";    // compass calibration
// Passwords, encrypted (secret_store.h)
static constexpr const char *TOKEN_KEY = "token_x";    // in SYS: claimed token
static constexpr const char *WIFI_PASS_KEY = "pass_x"; // in WIFI
static constexpr const char *WIFI_SSID_KEY = "ssid";   // in WIFI, plain
} // namespace settings

// Bump when a namespace's keys change meaning, and add the step to
// settings_init(). 1 = the layout the firmware has always used; 2 = the
// AprilTag detector's namespaces (det, calib) are gone; 3 = passwords are
// encrypted, and the flash is scrubbed of the plain-text ones.
static constexpr int SETTINGS_SCHEMA = 3;

void settings_init();  // first thing in setup(): migrate old layouts
int settings_schema(); // what is stored (0 = fresh flash)
bool settings_reset(const char *scope); // camera|wifi|imu|token|all
