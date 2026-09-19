#include "wifi_mgr.h"
#include "config.h"
#include "settings.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

static const uint32_t AP_OFF_AFTER_MS = 30000; // STA up this long → drop AP
static const uint32_t AP_ON_AFTER_MS = 15000;  // STA down this long → AP back

static String _saved_ssid; // network picked in the dashboard, if any

const char *wifi_saved_ssid() { return _saved_ssid.c_str(); }

static bool sta_try(const char *ssid, const char *pass) {
  if (!ssid[0])
    return false;
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  Serial.printf("[sta] connecting to %s", ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" failed");
    return false;
  }
  Serial.printf("\n[sta] http://%s\n", WiFi.localIP().toString().c_str());
  return true;
}

// Remembered across reboots and across firmware builds
static void save_network(const char *ssid, const char *pass) {
  Preferences prefs;
  prefs.begin(settings::WIFI, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  _saved_ssid = ssid;
}

void wifi_start() {
  WiFi.persistent(false); // the dashboard's choice lives in Preferences
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[ap]  SSID: %s  http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());

  // A network picked in the dashboard wins; secrets.h is the fallback
  Preferences prefs;
  prefs.begin(settings::WIFI, true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  _saved_ssid = ssid;
  // The network in secrets.h is kept in flash too, so a later build without
  // secrets.h (CI, releases) still knows it: stored when nothing is saved yet,
  // and again if it is the one that worked while the saved one did not
  if (WIFI_SSID[0] && !ssid.length()) {
    save_network(WIFI_SSID, WIFI_PASS);
    Serial.println("[sta] network from secrets.h saved to flash");
    ssid = WIFI_SSID;
    pass = WIFI_PASS;
  }
  bool up = sta_try(ssid.c_str(), pass.c_str());
  if (!up && WIFI_SSID[0] && ssid != WIFI_SSID &&
      sta_try(WIFI_SSID, WIFI_PASS)) {
    up = true;
    save_network(WIFI_SSID, WIFI_PASS);
    Serial.println("[sta] network from secrets.h saved to flash");
  }
  if (!up)
    Serial.println("[sta] not connected — AP only");

  // http://<HOSTNAME>.local on the router's network; the IP-stack hooks
  // re-announce it when STA connects later
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.printf("[mdns] http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("[mdns] failed to start");
  }
}

// The hotspot is the way in when the router isn't reachable, so it only goes
// away once STA has been up for a while and comes back when STA has been
// down for a while. Saves the radio's second interface (beacons) the rest of
// the time.
void wifi_tick() {
  static uint32_t since_ms = 0; // when the current STA state began
  static bool sta_was_up = false;
  const bool sta_up = WiFi.status() == WL_CONNECTED;
  const bool ap_on = (WiFi.getMode() & WIFI_AP) != 0;
  if (sta_up != sta_was_up) {
    sta_was_up = sta_up;
    since_ms = millis();
    return;
  }
  const uint32_t held = millis() - since_ms;
  if (sta_up && ap_on && WiFi.softAPgetStationNum() == 0 &&
      held >= AP_OFF_AFTER_MS) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println("[ap]  off — STA is up");
  } else if (!sta_up && !ap_on && held >= AP_ON_AFTER_MS) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("[ap]  on — STA is down");
  }
}

void wifi_save_and_connect(const char *ssid, const char *pass) {
  save_network(ssid, pass); // wifi_start() tries it before secrets.h

  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  Serial.printf("[sta] connecting to %s\n", ssid);
}

int wifi_scan(bool refresh) {
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || (n >= 0 && !refresh))
    return n;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false); // async, no hidden
  return WIFI_SCAN_RUNNING;
}
