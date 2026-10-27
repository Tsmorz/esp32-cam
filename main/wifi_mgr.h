#pragma once
#include <Arduino.h>

void wifi_start(); // AP + STA (saved network, then secrets.h), mDNS
void wifi_saved_ssid(char *out, size_t n); // network that worked last, or ""
// "default", "camera" (the camera's password) or "custom" (AP_PASS): what the
// running hotspot's WiFi password is
const char *wifi_ap_key();
// Join a network (from the httpd task: only queued, wifi_tick() does it). It
// is remembered once it works; after 30 s without, the camera goes back to
// the network it had.
void wifi_request_join(const char *ssid, const char *pass);
void wifi_joining(char *out, size_t n); // network being joined, or ""
// Non-blocking scan. WIFI_SCAN_RUNNING (-1) while one is in progress —
// call again later; otherwise the number of networks found, readable through
// WiFi.SSID(i) etc. refresh = discard the last results and scan again.
int wifi_scan(bool refresh);
// The camera's name: http://<name>.local. Starts as config.h's HOSTNAME;
// wifi_set_hostname() checks it (hostname_rules.h), remembers it and has
// wifi_tick() announce it — from the httpd task, so it only queues that.
void wifi_hostname(char *out, size_t n);
bool wifi_set_hostname(const char *name);
// Call every ~250 ms: carries out joins, renames, drops the hotspot once STA is
// stable and no one is on it, brings it back when STA is lost.
void wifi_tick();
