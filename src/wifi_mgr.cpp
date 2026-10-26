#include "wifi_mgr.h"
#include "auth.h"
#include "config.h"
#include "secret_store.h"
#include "settings.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#ifdef AP_PASS
static_assert(sizeof(AP_PASS) - 1 >= 8 && sizeof(AP_PASS) - 1 <= 63,
              "AP_PASS in secrets.h: 8-63 characters (WPA2)");
#endif

static const uint32_t AP_OFF_AFTER_MS = 30000; // STA up this long → drop AP
static const uint32_t AP_ON_AFTER_MS = 15000;  // STA down this long → AP back
static const uint32_t JOIN_TIMEOUT_MS = 30000; // a new network, then give up
static const uint32_t QUIET_RETRY_MS = 60000;  // see wifi_tick()

// The network that worked last (picked in the dashboard, or secrets.h).
// Only touched from loop() — the httpd task queues joins, see below.
static String _saved_ssid, _saved_pass;
static const char *_ap_key = "default"; // which password the running AP has

// A join asked for by /wifi/connect: carried out by wifi_tick(), and saved
// only once it got an address, so a mistyped password doesn't replace the
// network that works
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
static char _req_ssid[33], _req_pass[65]; // under _mux
static char _saved_name[33];              // under _mux: _saved_ssid for readers
static bool _req = false;                 // under _mux
static char _join_ssid[33], _join_pass[65];
static bool _joining = false;
static uint32_t _join_ms = 0;

// Captive portal: while the hotspot is on, every name resolves to the camera,
// so a phone that joins it opens the dashboard by itself ("Sign in to
// network"); http_server.cpp redirects the phone's connectivity checks.
// Created and deleted with the hotspot: DNSServer::stop() leaves its UDP
// socket bound (AsyncUDP::close() only disconnects), so it would keep
// answering on the home network; deleting it removes the socket.
static DNSServer *_dns = nullptr;

static void portal(bool on) {
  if (on && !_dns) {
    _dns = new DNSServer();
    if (_dns->start(53, "*", WiFi.softAPIP()))
      Serial.println("[ap]  captive portal on");
  } else if (!on && _dns) {
    delete _dns;
    _dns = nullptr;
  }
}

// The hotspot's password: AP_PASS from secrets.h, else the camera's password
// once one is claimed (so the hotspot isn't open to anyone who knows the
// default), else the default. Takes effect when the hotspot (re)starts, so a
// phone doing setup over it isn't thrown off when it claims the password.
static void ap_start() {
#ifdef AP_PASS
  const char *pass = AP_PASS;
  _ap_key = "custom";
#else
  const char *pass = auth_token();
  _ap_key = "camera";
  if (!pass[0]) {
    pass = AP_PASS_DEFAULT;
    _ap_key = "default";
  }
#endif
  WiFi.softAP(AP_SSID, pass);
  portal(true);
}

void wifi_saved_ssid(char *out, size_t n) {
  portENTER_CRITICAL(&_mux);
  strlcpy(out, _saved_name, n);
  portEXIT_CRITICAL(&_mux);
}

static void set_saved(const String &ssid, const String &pass) {
  _saved_ssid = ssid;
  _saved_pass = pass;
  portENTER_CRITICAL(&_mux);
  strlcpy(_saved_name, ssid.c_str(), sizeof(_saved_name));
  portEXIT_CRITICAL(&_mux);
}
const char *wifi_ap_key() { return _ap_key; }

void wifi_joining(char *out, size_t n) {
  portENTER_CRITICAL(&_mux);
  strlcpy(out, _req ? _req_ssid : _joining ? _join_ssid : "", n);
  portEXIT_CRITICAL(&_mux);
}

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

// Remembered across reboots and across firmware builds; the password
// encrypted
static void save_network(const char *ssid, const char *pass) {
  Preferences prefs;
  prefs.begin(settings::WIFI, false);
  prefs.putString(settings::WIFI_SSID_KEY, ssid);
  prefs.end();
  secret_put(settings::WIFI, settings::WIFI_PASS_KEY, pass);
  set_saved(ssid, pass);
}

void wifi_start() {
  WiFi.persistent(false); // the dashboard's choice lives in Preferences
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_AP_STA);
  ap_start();
  Serial.printf("[ap]  SSID: %s  http://%s  (password: %s)\n", AP_SSID,
                WiFi.softAPIP().toString().c_str(), _ap_key);

  // A network picked in the dashboard wins; secrets.h is the fallback
  Preferences prefs;
  prefs.begin(settings::WIFI, true);
  String ssid = prefs.getString(settings::WIFI_SSID_KEY, "");
  prefs.end();
  char pass[65];
  secret_get(settings::WIFI, settings::WIFI_PASS_KEY, pass, sizeof(pass));
  set_saved(ssid, pass);
  // The network in secrets.h is kept in flash too, so a later build without
  // secrets.h (CI, releases) still knows it: stored when nothing is saved yet,
  // and again if it is the one that worked while the saved one did not
  if (WIFI_SSID[0] && !ssid.length()) {
    save_network(WIFI_SSID, WIFI_PASS);
    Serial.println("[sta] network from secrets.h saved to flash");
  }
  bool up = sta_try(_saved_ssid.c_str(), _saved_pass.c_str());
  if (!up && WIFI_SSID[0] && _saved_ssid != WIFI_SSID &&
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

void wifi_request_join(const char *ssid, const char *pass) {
  portENTER_CRITICAL(&_mux);
  strlcpy(_req_ssid, ssid, sizeof(_req_ssid));
  strlcpy(_req_pass, pass, sizeof(_req_pass));
  _req = true;
  portEXIT_CRITICAL(&_mux);
}

static void join_tick(bool sta_up) {
  bool start = false;
  portENTER_CRITICAL(&_mux);
  if (_req) {
    memcpy(_join_ssid, _req_ssid, sizeof(_join_ssid));
    memcpy(_join_pass, _req_pass, sizeof(_join_pass));
    memset(_req_pass, 0, sizeof(_req_pass));
    _req = false;
    _joining = start = true;
  }
  portEXIT_CRITICAL(&_mux);
  if (start) {
    WiFi.setAutoReconnect(true);
    WiFi.disconnect();
    WiFi.begin(_join_ssid, _join_pass);
    _join_ms = millis();
    Serial.printf("[sta] joining %s\n", _join_ssid);
    return;
  }
  if (!_joining)
    return;
  if (sta_up && WiFi.SSID() == _join_ssid && (uint32_t)WiFi.localIP() != 0) {
    save_network(_join_ssid, _join_pass);
    Serial.printf("[sta] joined %s, saved\n", _join_ssid);
  } else if (millis() - _join_ms >= JOIN_TIMEOUT_MS) {
    // Wrong password or out of reach: back to the one that worked
    Serial.printf("[sta] couldn't join %s, back to %s\n", _join_ssid,
                  _saved_ssid.length() ? _saved_ssid.c_str() : "none");
    WiFi.disconnect();
    if (_saved_ssid.length())
      WiFi.begin(_saved_ssid.c_str(), _saved_pass.c_str());
  } else {
    return;
  }
  portENTER_CRITICAL(&_mux);
  _joining = false;
  portEXIT_CRITICAL(&_mux);
  memset(_join_pass, 0, sizeof(_join_pass));
}

// The hotspot is the way in when the router isn't reachable, so it only goes
// away once STA has been up for a while and comes back when STA has been
// down for a while. Saves the radio's second interface (beacons) the rest of
// the time.
void wifi_tick() {
  static uint32_t since_ms = 0; // when the current STA state began
  static bool sta_was_up = false;
  static bool quiet = false;
  static uint32_t quiet_ms = 0;
  const bool sta_up = WiFi.status() == WL_CONNECTED;
  const bool ap_on = (WiFi.getMode() & WIFI_AP) != 0;
  join_tick(sta_up);

  // While someone is on the hotspot and the home network is gone, STA's
  // reconnect attempts scan every channel and knock them off — just when they
  // may be fixing the WiFi. Retry once a minute instead until they leave.
  const bool want_quiet = !sta_up && !_joining && ap_on &&
                          WiFi.softAPgetStationNum() > 0 &&
                          _saved_ssid.length() && millis() - since_ms > 15000;
  if (want_quiet != quiet) {
    quiet = want_quiet;
    quiet_ms = millis();
    WiFi.setAutoReconnect(!quiet);
    if (quiet) {
      WiFi.disconnect();
    } else if (!sta_up && !_joining && _saved_ssid.length()) {
      WiFi.begin(_saved_ssid.c_str(), _saved_pass.c_str());
    }
  } else if (quiet && millis() - quiet_ms >= QUIET_RETRY_MS) {
    quiet_ms = millis();
    WiFi.begin(_saved_ssid.c_str(), _saved_pass.c_str()); // one attempt
  }

  if (sta_up != sta_was_up) {
    sta_was_up = sta_up;
    since_ms = millis();
    return;
  }
  const uint32_t held = millis() - since_ms;
  if (sta_up && ap_on && WiFi.softAPgetStationNum() == 0 &&
      held >= AP_OFF_AFTER_MS) {
    portal(false);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println("[ap]  off — STA is up");
  } else if (!sta_up && !ap_on && held >= AP_ON_AFTER_MS) {
    WiFi.mode(WIFI_AP_STA);
    ap_start();
    Serial.println("[ap]  on — STA is down");
  }
}

int wifi_scan(bool refresh) {
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || (n >= 0 && !refresh))
    return n;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false); // async, no hidden
  return WIFI_SCAN_RUNNING;
}
