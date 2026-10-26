#pragma once
// Minimal AXP313A driver for FireBeetle 2 ESP32-S3 V1.0
// Full library: https://github.com/DFRobot/DFRobot_AXP313A
// This shim covers only camera power — enough for the web server.

#include <Arduino.h>
#include <Wire.h>

#define AXP313A_ADDR 0x36
#define AXP313A_PWR_CTL                                                        \
  0x10 // enable: bit0=DCDC1, bit1=DCDC2, bit2=DCDC3, bit3=ALDO1, bit4=DLDO1
#define AXP313A_DC1_VOL                                                        \
  0x13 // DCDC1 voltage: 500mV + n×10mV  (7-bit, mask 0x7F)
#define AXP313A_ALDO1_CTL                                                      \
  0x16 // ALDO1 voltage: 500mV + n×100mV (5-bit, mask 0x1F)
#define AXP313A_DLDO1_CTL                                                      \
  0x17 // DLDO1 voltage: 500mV + n×100mV (5-bit, mask 0x1F)

class DFRobot_AXP313A {
public:
  enum eCameraType_t { eOV2640 = 0, eOV7725 };

  int begin(TwoWire &wire = Wire) {
    _wire = &wire;
    _wire->beginTransmission(AXP313A_ADDR);
    return (_wire->endTransmission() == 0) ? 0 : -1;
  }

  // false if the enable register couldn't be read: then nothing is switched,
  // because writing a guessed value would also turn DCDC1–3 off
  bool enableCameraPower(eCameraType_t type = eOV2640) {
    // DCDC1 (0x10 bit0) is already ON at boot powering a critical rail — do NOT
    // change its voltage or it will crash the board. Camera DVDD (1.2V) is
    // provided by DCDC2 (bit1), which is also on at boot (reg 0x14 = 0x46 = 70
    // → 500+70×10 = 1200mV). We only need to enable ALDO1 (DOVDD 1.8V) and
    // DLDO1 (AVDD 2.8V).
    writeReg(AXP313A_ALDO1_CTL, 13); // DOVDD 1.8V: 500 + 13×100 = 1800mV
    writeReg(AXP313A_DLDO1_CTL, 23); // AVDD  2.8V: 500 + 23×100 = 2800mV

    // Enable ALDO1 (bit3) and DLDO1 (bit4) — leave DCDC1/2/3 unchanged
    const int ctl = readReg(AXP313A_PWR_CTL);
    if (ctl < 0)
      return false;
    writeReg(AXP313A_PWR_CTL, (uint8_t)ctl | 0x08 | 0x10);

    delay(10);
    return true;
  }

private:
  TwoWire *_wire = &Wire;

  void writeReg(uint8_t reg, uint8_t val) {
    _wire->beginTransmission(AXP313A_ADDR);
    _wire->write(reg);
    _wire->write(val);
    _wire->endTransmission();
  }

  int readReg(uint8_t reg) { // -1 on a bus error
    _wire->beginTransmission(AXP313A_ADDR);
    _wire->write(reg);
    if (_wire->endTransmission(false) != 0)
      return -1;
    if (_wire->requestFrom(AXP313A_ADDR, (uint8_t)1) != 1)
      return -1;
    return _wire->available() ? _wire->read() : -1;
  }
};
