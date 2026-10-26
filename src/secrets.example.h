#pragma once
// Copy to secrets.h (git-ignored), then uncomment and fill in what you want.
// Anything left out falls back to config.h: no home network (set it up in
// the dashboard), and a password chosen in the dashboard on first use.
// Whatever is set here is also saved to the camera (encrypted), so a later
// build without secrets.h keeps it.

// Home network the camera joins.
// #define WIFI_SSID "MyNetwork"
// #define WIFI_PASS "MyPassword"

// The camera's password (the API token): 8-63 printable characters, no
// spaces — checked at compile time. Generate one with: openssl rand -hex 12
// #define API_TOKEN "..."

// Optional: a fixed password for the camera's own WiFi (8-63 characters).
// Without it the hotspot uses "wificamera" until a password is set, then
// the camera's password.
// #define AP_PASS "..."
