#include "board.h"
#include "DFRobot_AXP313A.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

static DFRobot_AXP313A axp;

void board_init() {
  // One I²C bus: AXP313A, the IMU board and — once camera_init() joins it —
  // the camera's SCCB
  Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
  Wire.setTimeOut(50); // prevent I2C hangs if bus is stuck

  // V1.0 boards power the camera through the AXP313A; V1.1+ have no PMIC
  bool axp_found = false;
  for (int i = 0; i < 3 && !axp_found; i++) {
    axp_found = axp.begin() == 0;
    if (!axp_found)
      delay(200);
  }
  if (axp_found) {
    if (axp.enableCameraPower(axp.eOV2640))
      Serial.println("[axp] camera power enabled");
    else
      Serial.println(
          "[axp] couldn't read the power register — rails untouched");
    delay(200);
  } else {
    Serial.println("[axp] not found — assuming V1.1+ (no PMIC)");
  }
}
