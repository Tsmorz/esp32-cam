#pragma once
#include <Arduino.h>

void wifi_start();             // AP + STA (saved network, then secrets.h), mDNS
const char *wifi_saved_ssid(); // network picked in the dashboard, or ""
// Remember the network and connect to it now
void wifi_save_and_connect(const char *ssid, const char *pass);
// Non-blocking scan. WIFI_SCAN_RUNNING (-1) while one is in progress —
// call again later; otherwise the number of networks found, readable through
// WiFi.SSID(i) etc. refresh = discard the last results and scan again.
int wifi_scan(bool refresh);
// Call every ~250 ms: drops the hotspot once STA is stable and no one is on
// it, brings it back when STA is lost.
void wifi_tick();
