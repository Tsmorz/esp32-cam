#include "camera_light.h"
#include "camera_profile.h"
#include "ov5640_timing.h"
#include <Arduino.h>
#include <esp_camera.h>

static int _flicker = FLICKER_AUTO;
// On: the AEC only lengthens frames once the longest single-frame exposure
// at full gain isn't enough, so in good light nothing changes
static int _lowlight = 1;
// Last values written, for /camera/status
static uint32_t _sysclk = 0, _hts = 0, _vts = 0;
static Ov5640Band _band{};

static sensor_t *ov5640() {
  sensor_t *s = esp_camera_sensor_get();
  return s && s->id.PID == OV5640_PID && s->get_reg && s->set_reg ? s : nullptr;
}

static int reg8(sensor_t *s, int reg) { return s->get_reg(s, reg, 0xff); }
static int reg16(sensor_t *s, int reg) { // -1 on a failed read
  const int hi = reg8(s, reg), lo = reg8(s, reg + 1);
  return hi < 0 || lo < 0 ? -1 : hi << 8 | lo;
}
static void put8(sensor_t *s, int reg, int val) {
  s->set_reg(s, reg, 0xff, val);
}
static void bits(sensor_t *s, int reg, int mask, bool on) {
  s->set_reg(s, reg, mask, on ? mask : 0);
}

void camera_light_apply() {
  sensor_t *s = ov5640();
  if (!s)
    return;
  sensor_lock();
  int r[5];
  const int at[5] = {0x3034, 0x3035, 0x3036, 0x3037, 0x3108};
  bool read_ok = true;
  for (int i = 0; i < 5; i++)
    read_ok &= (r[i] = reg8(s, at[i])) >= 0;
  const int hts = reg16(s, 0x380c), vts = reg16(s, 0x380e);
  read_ok &= hts > 0 && vts > 0;
  _band = {}; // a failed read leaves the driver's values alone
  if (read_ok) {
    const Ov5640Pll pll = {(uint8_t)r[0], (uint8_t)r[1], (uint8_t)r[2],
                           (uint8_t)r[3], (uint8_t)r[4]};
    _sysclk = ov5640_sysclk_hz(s->xclk_freq_hz, pll);
    _hts = hts;
    _vts = vts;
    _band = ov5640_band(_sysclk, _hts, _vts, _lowlight);
  }
  if (_band.step50) {
    put8(s, 0x3a08, _band.step50 >> 8); // 50 Hz band step
    put8(s, 0x3a09, _band.step50 & 0xff);
    put8(s, 0x3a0a, _band.step60 >> 8); // 60 Hz band step
    put8(s, 0x3a0b, _band.step60 & 0xff);
    put8(s, 0x3a0d, _band.max60);         // max bands, 60 Hz
    put8(s, 0x3a0e, _band.max50);         // max bands, 50 Hz
    put8(s, 0x3a02, _band.max_expo >> 8); // max exposure (lines), 60 Hz
    put8(s, 0x3a03, _band.max_expo & 0xff);
    put8(s, 0x3a14, _band.max_expo >> 8); // max exposure (lines), 50 Hz
    put8(s, 0x3a15, _band.max_expo & 0xff);
  }
  s->set_aec2(s, _lowlight); // night mode: exposure may exceed one frame
  bits(s, 0x3a00, 0x20, _flicker != FLICKER_OFF);  // band filter on
  bits(s, 0x3c01, 0x80, _flicker != FLICKER_AUTO); // manual 50/60
  bits(s, 0x3c00, 0x04, _flicker == FLICKER_50HZ); // manual: 50 Hz
  sensor_unlock();
  Serial.printf("[cam] light: sysclk %lu Hz, HTS %lu, VTS %lu, band "
                "%u/%u lines, max exposure %u lines\n",
                (unsigned long)_sysclk, (unsigned long)_hts,
                (unsigned long)_vts, _band.step50, _band.step60,
                _band.max_expo);
}

bool camera_light_set_flicker(int mode) {
  if (!ov5640() || mode < FLICKER_OFF || mode > FLICKER_60HZ)
    return false;
  _flicker = mode;
  camera_light_apply();
  return true;
}

bool camera_light_set_lowlight(int on) {
  if (!ov5640() || (on != 0 && on != 1))
    return false;
  _lowlight = on;
  camera_light_apply();
  return true;
}

int camera_light_flicker() { return _flicker; }
int camera_light_lowlight() { return _lowlight; }

size_t camera_light_json(char *buf, size_t size) {
  sensor_t *s = ov5640();
  if (!s || !size) {
    if (size)
      buf[0] = 0;
    return 0;
  }
  int hz = 0;
  sensor_lock();
  if (_flicker == FLICKER_50HZ)
    hz = 50;
  else if (_flicker == FLICKER_60HZ)
    hz = 60;
  else if (_flicker == FLICKER_AUTO)
    hz = reg8(s, 0x3c0c) & 0x01 ? 50 : 60; // the sensor's detection result
  // What the AEC chose: exposure in 1/16 lines (0x3500–02), gain in 1/16
  // (0x350A–B; 0xF8 = the ceiling)
  const int expo =
      (reg8(s, 0x3500) & 0x0f) << 16 | reg8(s, 0x3501) << 8 | reg8(s, 0x3502);
  const int gain = reg16(s, 0x350a) & 0x3ff;
  sensor_unlock();
  const int n = snprintf(
      buf, size,
      ",\"flicker\":%d,\"lowlight\":%d,\"flicker_hz\":%d,\"timing\":{"
      "\"sysclk\":%lu,\"hts\":%lu,\"vts\":%lu,\"step50\":%u,\"step60\":%u,"
      "\"max_expo\":%u,\"expo\":%d,\"gain\":%d}",
      _flicker, _lowlight, hz, (unsigned long)_sysclk, (unsigned long)_hts,
      (unsigned long)_vts, _band.step50, _band.step60, _band.max_expo,
      expo >> 4, gain);
  if (n < 0) {
    buf[0] = 0;
    return 0;
  }
  return (size_t)n < size ? n : size - 1;
}
