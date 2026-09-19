#include "calibration_store.h"
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>

static Calibration _cal = CALIBRATION;
static bool _stored = false;
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

static const char *K[5] = {"k1", "k2", "p1", "p2", "k3"};

static void apply(const Calibration &c, bool stored) {
  portENTER_CRITICAL(&_mux);
  _cal = c;
  _stored = stored;
  portEXIT_CRITICAL(&_mux);
}

void calibration_load() {
  Preferences prefs;
  if (!prefs.begin(settings::CALIB, true))
    return;
  if (prefs.isKey("fx")) {
    Calibration c = {};
    c.width = prefs.getInt("w", 0);
    c.height = prefs.getInt("h", 0);
    c.fx = prefs.getDouble("fx", 0);
    c.fy = prefs.getDouble("fy", 0);
    c.cx = prefs.getDouble("cx", 0);
    c.cy = prefs.getDouble("cy", 0);
    for (int i = 0; i < 5; i++)
      c.dist[i] = prefs.getDouble(K[i], 0);
    if (calibration_valid(c))
      apply(c, true);
    else
      Serial.println("[calib] stored calibration invalid — ignored");
  }
  prefs.end();
  Serial.printf("[calib] %s\n", calibration_source());
}

Calibration calibration_get() {
  portENTER_CRITICAL(&_mux);
  const Calibration c = _cal;
  portEXIT_CRITICAL(&_mux);
  return c;
}

const char *calibration_source() {
  portENTER_CRITICAL(&_mux);
  const bool stored = _stored;
  portEXIT_CRITICAL(&_mux);
  if (stored)
    return "stored";
  return camera_calibrated(CALIBRATION) ? "built-in" : "estimate";
}

bool calibration_set(const Calibration &c) {
  if (!calibration_valid(c))
    return false;
  Preferences prefs;
  if (!prefs.begin(settings::CALIB, false))
    return false;
  prefs.putInt("w", c.width);
  prefs.putInt("h", c.height);
  prefs.putDouble("fx", c.fx);
  prefs.putDouble("fy", c.fy);
  prefs.putDouble("cx", c.cx);
  prefs.putDouble("cy", c.cy);
  for (int i = 0; i < 5; i++)
    prefs.putDouble(K[i], c.dist[i]);
  prefs.end();
  apply(c, true);
  return true;
}

void calibration_clear() {
  Preferences prefs;
  if (prefs.begin(settings::CALIB, false)) {
    prefs.clear();
    prefs.end();
  }
  apply(CALIBRATION, false);
}

size_t calibration_json(char *buf, size_t size) {
  const Calibration c = calibration_get();
  const int n = snprintf(
      buf, size,
      "{\"source\":\"%s\",\"calibrated\":%s,\"width\":%d,\"height\":%d,"
      "\"fx\":%.4f,\"fy\":%.4f,\"cx\":%.4f,\"cy\":%.4f,"
      "\"dist\":[%.8g,%.8g,%.8g,%.8g,%.8g]}",
      calibration_source(), camera_calibrated(c) ? "true" : "false", c.width,
      c.height, c.fx, c.fy, c.cx, c.cy, c.dist[0], c.dist[1], c.dist[2],
      c.dist[3], c.dist[4]);
  return n < 0 ? 0 : ((size_t)n < size ? (size_t)n : size - 1);
}
