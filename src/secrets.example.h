#pragma once
// Copy to secrets.h (git-ignored) and fill in. Anything left undefined falls
// back to the defaults in config.h.

// Home network the camera joins in STA mode (leave empty for AP-only).
#define WIFI_SSID "MyNetwork"
#define WIFI_PASS "MyPassword"

// Shared secret for the endpoints that change state (/ota, /wifi/connect,
// /camera/set). The dashboard asks for it once and keeps it in localStorage.
// Generate one with: openssl rand -hex 12
#define API_TOKEN "change-me"

// Optional: override the always-on access point's password (min 8 chars).
// #define AP_PASS "esp32cam1"
