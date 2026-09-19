#include "focus.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_camera.h>

// AF firmware blob + command registers: ESP32-OV5640-AF (MIT, Eric Nam)
#include <ESP32_OV5640_cfg.h>

static sensor_t *_s = nullptr;
static int _mode = -1;

static bool wr(int reg, uint8_t val) {
  return _s->set_reg(_s, reg, 0xff, val) >= 0;
}
static int rd(int reg) { return _s->get_reg(_s, reg, 0xff); }

// The AF MCU acknowledges a command by clearing CMD_ACK
static bool command(uint8_t cmd) {
  // The MCU ignores commands while it is still moving the lens (status
  // 0x00–0x0F): wait for it to settle first (≤ 2 s)
  for (int i = 0; i < 200; i++) {
    const int st = rd(OV5640_CMD_FW_STATUS);
    if (st == FW_STATUS_S_IDLE || st == FW_STATUS_S_FOCUSED || st == 0x20)
      break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  wr(OV5640_CMD_ACK, 0x01);
  wr(OV5640_CMD_MAIN, cmd);
  for (int i = 0; i < 300; i++) { // ≤ 3 s
    if (rd(OV5640_CMD_ACK) == 0x00)
      return true;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  Serial.printf("[af] cmd 0x%02x not acked (ack=0x%02x status=0x%02x)\n", cmd,
                rd(OV5640_CMD_ACK), rd(OV5640_CMD_FW_STATUS));
  return false;
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
  return focus_set_mode(FOCUS_CONTINUOUS);
}

bool focus_available() { return _s != nullptr; }

bool focus_set_mode(int mode) {
  if (!_s)
    return false;
  bool ok;
  switch (mode) {
  case FOCUS_INFINITY:
    ok = command(0x08); // release focus
    break;
  case FOCUS_SINGLE:
    ok = command(0x08) && command(AF_TRIG_SINGLE_AUTO_FOCUS);
    break;
  case FOCUS_CONTINUOUS:
    ok = command(AF_CONTINUE_AUTO_FOCUS);
    break;
  default:
    return false;
  }
  if (ok)
    _mode = mode;
  return ok;
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
