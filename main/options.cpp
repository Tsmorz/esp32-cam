#include "options.h"
#include "settings.h"
#include "strbuf.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <strings.h>

static Options _o;
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED; // detect_only

// GPIOs offered for wiring: not used by the camera, I²C, LED, BOOT, USB,
// flash/PSRAM or UART0. Check which ones your board brings out to a header.
static const int FREE_PINS[] = {9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 38, 47};

bool options_pin_free(int g) {
  for (int p : FREE_PINS)
    if (p == g)
      return true;
  return false;
}

bool options_wake_pin(int g) {
  return g >= 0 && g <= 21 && options_pin_free(g);
}

static bool pin_ok(int v) { return v == -1 || options_pin_free(v); }
static bool wake_ok(int v) { return v == -1 || options_wake_pin(v); }
static bool action_ok(int v) { return v >= ACT_NONE && v <= ACT_CLIP; }
static bool sens_ok(int v) { return v >= 1 && v <= 3; }
static bool flag_ok(int v) { return v == 0 || v == 1; }
static bool timelapse_ok(int v) {
  return v == 0 || v == 1 || v == 5 || v == 15 || v == 30 || v == 60;
}
static bool clip_ok(int v) { return v >= 5 && v <= 60; }
static bool rec_ok(int v) { return v >= 1 && v <= 30; }
static bool sleep_ok(int v) {
  return v == 0 || v == 5 || v == 15 || v == 30 || v == 60;
}
static bool thr_ok(int v) { return v >= 10 && v <= 99; }

struct Spec {
  const char *key;
  int Options::*field;
  bool (*valid)(int);
  bool restart; // wiring: set up at boot
  bool pin;     // must not clash with another pin
};

static const Spec SPECS[] = {
    {"sd_sck", &Options::sd_sck, pin_ok, true, true},
    {"sd_miso", &Options::sd_miso, pin_ok, true, true},
    {"sd_mosi", &Options::sd_mosi, pin_ok, true, true},
    {"sd_cs", &Options::sd_cs, pin_ok, true, true},
    {"imu_int", &Options::imu_int, wake_ok, true, true},
    {"timelapse_min", &Options::timelapse_min, timelapse_ok, false, false},
    {"motion_action", &Options::motion_action, action_ok, false, false},
    {"motion_sens", &Options::motion_sens, sens_ok, false, false},
    {"clip_s", &Options::clip_s, clip_ok, false, false},
    {"rec_max_min", &Options::rec_max_min, rec_ok, false, false},
    {"sleep_min", &Options::sleep_min, sleep_ok, false, false},
    {"detect_on", &Options::detect_on, flag_ok, false, false},
    {"detect_action", &Options::detect_action, action_ok, false, false},
    {"detect_thr", &Options::detect_thr, thr_ok, false, false},
};
static constexpr const char *ONLY_KEY = "detect_only";

// NVS keys are at most 15 characters: the spec keys fit
void options_init() {
  Preferences prefs;
  if (!prefs.begin(settings::OPTIONS, true))
    return;
  for (const Spec &s : SPECS) {
    const int v = prefs.getInt(s.key, _o.*s.field);
    if (s.valid(v))
      _o.*s.field = v;
  }
  prefs.getString(ONLY_KEY, _o.detect_only, sizeof(_o.detect_only));
  prefs.end();
}

const Options &options() { return _o; }

bool options_watching(const char *label) {
  char only[sizeof(_o.detect_only)];
  portENTER_CRITICAL(&_mux);
  memcpy(only, _o.detect_only, sizeof(only));
  portEXIT_CRITICAL(&_mux);
  if (!only[0])
    return true;
  const size_t n = strlen(label);
  for (const char *p = only; *p;) {
    while (*p == ',' || *p == ' ')
      p++;
    const char *e = p;
    while (*e && *e != ',')
      e++;
    const char *t = e; // trim trailing spaces
    while (t > p && t[-1] == ' ')
      t--;
    if ((size_t)(t - p) == n && !strncasecmp(p, label, n))
      return true;
    p = e;
  }
  return false;
}

bool options_set(const char *key, const char *val, bool *restart,
                 const char **why) {
  *restart = false;
  *why = "Unknown setting";
  Preferences prefs;
  if (!strcmp(key, ONLY_KEY)) {
    if (strlen(val) >= sizeof(_o.detect_only)) {
      *why = "Too long";
      return false;
    }
    portENTER_CRITICAL(&_mux);
    strlcpy(_o.detect_only, val, sizeof(_o.detect_only));
    portEXIT_CRITICAL(&_mux);
    if (prefs.begin(settings::OPTIONS, false)) {
      prefs.putString(ONLY_KEY, val);
      prefs.end();
    }
    return true;
  }
  for (const Spec &s : SPECS) {
    if (strcmp(key, s.key))
      continue;
    char *end;
    const long v = strtol(val, &end, 10);
    if (!val[0] || *end || !s.valid((int)v)) {
      *why = "Value not allowed";
      return false;
    }
    if (s.pin && v >= 0)
      for (const Spec &o : SPECS)
        if (o.pin && &o != &s && _o.*o.field == v) {
          *why = "That GPIO is already used for another wire";
          return false;
        }
    _o.*s.field = (int)v;
    if (prefs.begin(settings::OPTIONS, false)) {
      prefs.putInt(s.key, (int)v);
      prefs.end();
    }
    *restart = s.restart;
    return true;
  }
  return false;
}

size_t options_json(char *buf, size_t size) {
  size_t pos = 0;
  appendf(buf, size, pos, "{");
  for (const Spec &s : SPECS)
    appendf(buf, size, pos, "\"%s\":%d,", s.key, _o.*s.field);
  char only[sizeof(_o.detect_only)], esc[2 * sizeof(only)];
  portENTER_CRITICAL(&_mux);
  memcpy(only, _o.detect_only, sizeof(only));
  portEXIT_CRITICAL(&_mux);
  json_esc(only, esc, sizeof(esc));
  appendf(buf, size, pos, "\"%s\":\"%s\",\"pins\":[", ONLY_KEY, esc);
  for (size_t i = 0; i < sizeof(FREE_PINS) / sizeof(FREE_PINS[0]); i++)
    appendf(buf, size, pos, "%s%d", i ? "," : "", FREE_PINS[i]);
  appendf(buf, size, pos, "]}");
  return pos;
}
