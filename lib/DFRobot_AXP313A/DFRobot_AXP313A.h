#pragma once
// Minimal AXP313A driver for FireBeetle 2 ESP32-S3 V1.0
// Full library: https://github.com/DFRobot/DFRobot_AXP313A
// This shim covers only camera power — enough for the web server.

#include <Arduino.h>
#include <Wire.h>

#define AXP313A_ADDR        0x36
#define AXP313A_PWR_CTL     0x10   // output enable register (bit1=DC1, bit3=ALDO1, bit4=ALDO2, bit5=ALDO3)
#define AXP313A_DC1_VOL     0x82   // DCDC1 voltage (500mV + n×10mV)
#define AXP313A_ALDO1_CTL   0x90   // ALDO1: bit7=enable, bits4:0=voltage (500mV + n×100mV)
#define AXP313A_ALDO2_CTL   0x91   // ALDO2: bit7=enable, bits4:0=voltage
#define AXP313A_ALDO3_CTL   0x92   // ALDO3: bit7=enable, bits4:0=voltage

class DFRobot_AXP313A {
public:
    enum eCameraType_t { eOV2640 = 0, eOV7725 };

    int begin(TwoWire &wire = Wire) {
        _wire = &wire;
        _wire->beginTransmission(AXP313A_ADDR);
        return (_wire->endTransmission() == 0) ? 0 : -1;
    }

    void enableCameraPower(eCameraType_t type = eOV2640) {
        // DVDD 1.2V via DCDC1 — enable bit1 of PWR_CTL, voltage = (1200-500)/10 = 70
        writeReg(AXP313A_DC1_VOL, 70);
        writeReg(AXP313A_PWR_CTL, readReg(AXP313A_PWR_CTL) | 0x02);

        // DOVDD 1.8V via ALDO1 — bit7=enable, n=(1800-500)/100=13
        writeReg(AXP313A_ALDO1_CTL, 0x80 | 13);

        // AVDD 2.8V via ALDO2 — bit7=enable, n=(2800-500)/100=23
        writeReg(AXP313A_ALDO2_CTL, 0x80 | 23);

        delay(10);
    }

private:
    TwoWire *_wire = &Wire;

    void writeReg(uint8_t reg, uint8_t val) {
        _wire->beginTransmission(AXP313A_ADDR);
        _wire->write(reg);
        _wire->write(val);
        _wire->endTransmission();
    }

    uint8_t readReg(uint8_t reg) {
        _wire->beginTransmission(AXP313A_ADDR);
        _wire->write(reg);
        _wire->endTransmission(false);
        _wire->requestFrom(AXP313A_ADDR, (uint8_t)1);
        return _wire->available() ? _wire->read() : 0;
    }
};
