#include "focus.h"
#include "camera_profile.h"
#include <Arduino.h>
#include <Wire.h>
#include <cstring>
#include <esp_camera.h>

// AF firmware blob + command registers: ESP32-OV5640-AF (MIT, Eric Nam)
#include <ESP32_OV5640_cfg.h>

static sensor_t *_s = nullptr;
static int _mode = -1;

static bool wr(int reg, uint8_t val) {
  return _s->set_reg(_s, reg, 0xff, val) >= 0;
}
static int rd(int reg) { return _s->get_reg(_s, reg, 0xff); }

// The AF MCU takes seconds to settle and acknowledge, and the caller is often
// the single httpd task, so a mode change is a small state machine that
// focus_tick() advances from loop() — it never waits.
//
// Each mode is a sequence of MCU commands. The MCU ignores a command while it
// is still moving the lens (status 0x00–0x0F), so each one first waits for the
// status to settle (≤ 2 s), then is acknowledged by the MCU clearing CMD_ACK
// (≤ 3 s).
static constexpr uint32_t SETTLE_MS = 2000, ACK_MS = 3000;
enum class Step { Idle, Settle, Ack };
static Step _step = Step::Idle;
static uint8_t _cmds[2];
static int _ncmds = 0, _icmd = 0;
static int _prev_mode = -1;
static uint32_t _step_ms = 0;

static bool settled(int st) {
  return st == FW_STATUS_S_IDLE || st == FW_STATUS_S_FOCUSED || st == 0x20;
}

void focus_tick() {
  if (!_s || _step == Step::Idle)
    return;
  sensor_lock();
  const uint32_t now = millis();
  if (_step == Step::Settle) {
    if (settled(rd(OV5640_CMD_FW_STATUS)) || now - _step_ms > SETTLE_MS) {
      wr(OV5640_CMD_ACK, 0x01);
      wr(OV5640_CMD_MAIN, _cmds[_icmd]);
      _step = Step::Ack;
      _step_ms = now;
    }
  } else if (rd(OV5640_CMD_ACK) == 0x00) {
    if (++_icmd < _ncmds) {
      _step = Step::Settle;
      _step_ms = now;
    } else {
      _step = Step::Idle;
    }
  } else if (now - _step_ms > ACK_MS) {
    Serial.printf("[af] cmd 0x%02x not acked (ack=0x%02x status=0x%02x)\n",
                  _cmds[_icmd], rd(OV5640_CMD_ACK), rd(OV5640_CMD_FW_STATUS));
    _mode = _prev_mode; // the mode never took effect
    _step = Step::Idle;
  }
  sensor_unlock();
}

// Burst-write the firmware to 0x8000.. over the shared bus (the OV5640
// auto-increments the register address), 124 bytes per transaction.
static bool upload_burst() {
  const size_t CHUNK = 124; // Wire buffer is 128 incl. the 2 address bytes
  for (size_t off = 0; off < sizeof(OV5640_AF_Config); off += CHUNK) {
    size_t n = sizeof(OV5640_AF_Config) - off;
    if (n > CHUNK)
      n = CHUNK;
    const uint16_t reg = 0x8000 + off;
    Wire.beginTransmission(_s->slv_addr);
    Wire.write(reg >> 8);
    Wire.write(reg & 0xff);
    Wire.write(OV5640_AF_Config + off, n);
    if (Wire.endTransmission() != 0)
      return false;
  }
  // Spot-check that auto-increment really happened
  for (size_t i = 0; i < sizeof(OV5640_AF_Config); i += 257)
    if (rd(0x8000 + i) != OV5640_AF_Config[i])
      return false;
  return true;
}

// ~10× slower fallback: one SCCB write (and read-modify) per byte
static bool upload_bytewise() {
  for (size_t i = 0; i < sizeof(OV5640_AF_Config); i++)
    if (!wr(0x8000 + i, OV5640_AF_Config[i]))
      return false;
  return true;
}

bool focus_init() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || s->id.PID != OV5640_PID || !s->set_reg || !s->get_reg)
    return false;
  _s = s;

  const uint32_t t0 = millis();
  wr(0x3000, 0x20); // hold the AF MCU in reset while loading
  bool ok = upload_burst();
  if (!ok) {
    Serial.println("[af] burst upload failed, falling back to byte writes");
    ok = upload_bytewise();
  }
  if (!ok) {
    Serial.println("[af] firmware upload failed");
    _s = nullptr;
    return false;
  }
  for (int reg = OV5640_CMD_MAIN; reg <= OV5640_CMD_PARA4; reg++)
    wr(reg, 0x00);
  wr(OV5640_CMD_FW_STATUS, 0x7f);
  wr(0x3000, 0x00); // release the MCU

  for (int i = 0; rd(OV5640_CMD_FW_STATUS) != FW_STATUS_S_IDLE; i++) {
    if (i > 100) { // 1 s
      Serial.println("[af] firmware did not start");
      _s = nullptr;
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  Serial.printf("[af] OV5640 focus firmware running (%lu ms)\n",
                (unsigned long)(millis() - t0));
  // Boot has nothing else to do: run the first mode change to its end
  if (!focus_set_mode(FOCUS_CONTINUOUS))
    return false;
  while (_step != Step::Idle) {
    vTaskDelay(pdMS_TO_TICKS(10));
    focus_tick();
  }
  return _mode == FOCUS_CONTINUOUS;
}

bool focus_available() { return _s != nullptr; }

bool focus_set_mode(int mode) {
  if (!_s)
    return false;
  uint8_t cmds[2];
  int n = 0;
  switch (mode) {
  case FOCUS_INFINITY:
    cmds[n++] = 0x08; // release focus
    break;
  case FOCUS_SINGLE:
    cmds[n++] = 0x08;
    cmds[n++] = AF_TRIG_SINGLE_AUTO_FOCUS;
    break;
  case FOCUS_CONTINUOUS:
    cmds[n++] = AF_CONTINUE_AUTO_FOCUS;
    break;
  default:
    return false;
  }
  sensor_lock();
  if (_step == Step::Idle)
    _prev_mode = _mode; // a request that replaces a pending one keeps the
                        // last mode that actually took effect
  memcpy(_cmds, cmds, n);
  _ncmds = n;
  _icmd = 0;
  _step = Step::Settle;
  _step_ms = millis();
  _mode = mode;
  sensor_unlock();
  return true;
}

int focus_mode() { return _s ? _mode : -1; }

int focus_raw_status() { return _s ? rd(OV5640_CMD_FW_STATUS) : -1; }

const char *focus_state() {
  if (!_s)
    return "none";
  int st = rd(OV5640_CMD_FW_STATUS);
  if (st < 0)
    return "error";
  // 0x10: single-shot focused; 0x20: continuous AF settled (measured)
  if (st == FW_STATUS_S_FOCUSED || st == 0x20)
    return "focused";
  if (st == FW_STATUS_S_IDLE)
    return "idle";
  if (st <= 0x0f || (st >= 0x80 && st <= 0x8f))
    return "focusing";
  return "error";
}
