# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Build
pio run -e firebeetle2_esp32s3

# Build and flash (kill monitor first to free the port)
pkill -f "pio device monitor"; pio run -e firebeetle2_esp32s3 -t upload --upload-port /dev/cu.usbmodem1101

# Serial monitor (run in a separate terminal — do NOT run from Claude Code shell)
pio device monitor

# Clean build artifacts
pio run -e firebeetle2_esp32s3 -t clean
```

WiFi credentials can be set via `platformio.ini` build flags to avoid hardcoding:
```ini
build_flags =
    -DWIFI_SSID=\"MyNetwork\"
    -DWIFI_PASS=\"MyPassword\"
```

## Architecture

Multi-file Arduino firmware targeting the **DFRobot FireBeetle 2 ESP32-S3 V1.0** with OV2640 camera and LSM6DSOX IMU. No RTOS tasks — the ESP-IDF HTTP server runs its own internal tasks; `loop()` handles IMU polling, AprilTag detection, and serial commands.

**Source files:**
- `src/config.h` — all compile-time constants: WiFi credentials, AP SSID/pass, camera DVP pin map
- `src/camera.h/cpp` — `camera_init()`, `stream_handler`, `index_handler`, `INDEX_HTML`
- `src/detector.h/cpp` — AprilTag detection, pose estimation, frame buffering
- `src/imu.h/cpp` — LSM6DSOX I²C driver, VQF filter update
- `src/main.cpp` — AXP313A init, WiFi AP+STA, HTTP server start, serial CLI, `loop()`

**Boot sequence** (`setup()`):
1. AXP313A (I²C `0x36`, SDA=GPIO 1, SCL=GPIO 2) enables DOVDD (1.8 V) and AVDD (2.8 V) for the camera — V1.0 only
2. `camera_init()` configures OV2640 over DVP and tunes AWB/AEC/BPC/WPC
3. `detector_init()` allocates PSRAM buffers (RGB565 + grayscale + JPEG copy) and creates the tag36h11 apriltag detector
4. `imu_init()` verifies LSM6DSOX WHO_AM_I, configures 104 Hz ODR, initialises VQF
5. WiFi starts in `WIFI_AP_STA` — AP always at 192.168.4.1, STA attempts home WiFi for 10 s
6. HTTP server registers four handlers: `/`, `/stream`, `/detections`, `/pose`

**HTTP handlers:**
- `index_handler` — serves `INDEX_HTML` (raw string literal in `camera.cpp`); includes canvas overlay JS that polls `/detections` at 200 ms and `/pose` at 50 ms, renders in a single `requestAnimationFrame` loop
- `stream_handler` — infinite multipart JPEG loop; stays open until client disconnects
- `detections_handler` — returns JSON with tag count, corner pixel coords (full-res), and translation pose
- `pose_handler` — returns `{"q":[w,x,y,z]}` from the VQF filter

**`loop()` timing:**
- Serial commands: every iteration (1 ms delay)
- IMU update: every 10 ms (~100 Hz)
- AprilTag detection: every 500 ms (2 Hz)

**Camera config:**
- PSRAM must be OPI (octal) — `board_build.arduino.memory_type = qio_opi`; **do not change this**
- Double-buffered (`fb_count = 2`) in PSRAM, `CAMERA_GRAB_WHEN_EMPTY` mode
- SVGA 800×600 JPEG quality 12; XCLK 20 MHz

**AprilTag detection:**
- `detector_grab()` calls `esp_camera_fb_get()`, copies the JPEG to a pre-allocated PSRAM buffer, returns the frame buffer immediately (so the stream isn't stalled), then calls `detector_run()`
- `detector_run()` decodes JPEG to RGB565 at `JPG_SCALE_4X` (→ 200×150), converts to grayscale, runs the apriltag detector, estimates pose via `estimate_tag_pose()`, prints to serial
- Detection can be toggled with serial `t` command; `_enabled` flag short-circuits `detector_grab()`

**VQF / IMU:**
- `lib/VQF/basicvqf.hpp` — self-contained BasicVQF implementation (no external dependency); gyro integration + accelerometer tilt correction with LP filtering, `tauAcc = 3.0 s`
- `imu_update()` burst-reads 12 bytes from LSM6DSOX (6 gyro + 6 accel), converts to SI units, calls `_vqf->update()`
- LSM6DSOX at I²C `0x6A`; change `LSM_ADDR` in `imu.cpp` to `0x6B` if SA0 is pulled high

**`lib/DFRobot_AXP313A/`** — header-only PMIC shim. Only enables ALDO1 (DOVDD 1.8 V) and DLDO1 (AVDD 2.8 V). **Never write to register 0x13 (DCDC1)** — it powers an unknown critical rail at ~1.55 V and writing to it while running crashes the board.

**External library:**
- `apriltag` pulled from `https://github.com/AprilRobotics/apriltag.git#v3.3.0`
- `apriltag_pywrap.c` excluded via `library.json` in `.pio/libdeps/firebeetle2_esp32s3/apriltag/` — if you re-fetch the library, re-apply the filter: `{"build":{"srcFilter":"+<*.c> -<apriltag_pywrap.c> +<common/*.c>"}}`

## Board-specific notes

- N16R8 module uses OPI PSRAM — changing `board_build.arduino.memory_type` causes camera init to fail at runtime
- `huge_app.csv` partition scheme required — camera + HTTP + apriltag exceeds the default partition
- On first flash, power-cycle (not just reset) — AXP313A sometimes needs a hard reset to bring rails up
- If no COM port visible: hold BOOT, tap RST, release BOOT, then flash
- `pio device monitor` requires a real TTY and will fail when run from Claude Code's non-interactive shell — tell the user to run it in their own terminal
- Serial port `/dev/cu.usbmodem1101` is frequently locked by a stale monitor process; run `pkill -f "pio device monitor"; pkill -f "miniterm"` before uploading
- `-DARDUINO_USB_CDC_ON_BOOT=1` is mandatory — without it `Serial` maps to UART0 (GPIO 43/44) not USB CDC
