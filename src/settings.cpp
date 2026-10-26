#include "settings.h"
#include "auth.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

static int _stored_schema = 0;

static void clear_ns(const char *ns) {
  Preferences prefs;
  if (prefs.begin(ns, false)) {
    prefs.clear();
    prefs.end();
  }
}

// Brings flash written by an older firmware up to SETTINGS_SCHEMA, oldest step
// first. Schema 0 → 1 changes nothing: an unversioned flash already used the
// v1 layout.
static void migrate(int from) {
  if (from < 2) { // the tag detector was removed: drop its data
    clear_ns("det");
    clear_ns("calib");
  }
}

void settings_init() {
  Preferences prefs;
  if (!prefs.begin(settings::SYS, false))
    return;
  _stored_schema = prefs.getInt("schema", 0);
  if (_stored_schema > SETTINGS_SCHEMA) {
    // Flashed back to older firmware: leave the newer data alone, it may
    // still be readable
    Serial.printf("[settings] schema %d is newer than this firmware (%d)\n",
                  _stored_schema, SETTINGS_SCHEMA);
  } else if (_stored_schema < SETTINGS_SCHEMA) {
    migrate(_stored_schema);
    prefs.putInt("schema", SETTINGS_SCHEMA);
    _stored_schema = SETTINGS_SCHEMA;
  }
  prefs.end();
}

int settings_schema() { return _stored_schema; }

bool settings_reset(const char *scope) {
  const bool all = !strcmp(scope, "all");
  bool known = all;
  if (all || !strcmp(scope, "camera")) {
    clear_ns(settings::CAMERA);
    known = true;
  }
  if (all || !strcmp(scope, "token")) {
    auth_forget();
    known = true;
  }
  if (all || !strcmp(scope, "wifi")) {
    clear_ns(settings::WIFI);
    known = true;
  }
  if (all || !strcmp(scope, "imu")) {
    clear_ns(settings::IMU);
    known = true;
  }
  return known;
}
