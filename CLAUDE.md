# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Build
pio run

# Build and flash to connected board
pio run -t upload

# Watch serial output (finds the board IP address after boot)
pio device monitor

# Build + flash + monitor in one step
pio run -t upload && pio device monitor

# Clean build artifacts
pio run -t clean
```

WiFi credentials can be set via `platformio.ini` build flags to avoid hardcoding them in source:
```ini
build_flags =
    -DWIFI_SSID=\"MyNetwork\"
    -DWIFI_PASS=\"MyPassword\"
```

## Architecture

Single-file Arduino firmware (`src/main.cpp`) targeting the **DFRobot FireBeetle 2 ESP32-S3 V1.0** with an OV2640 camera. No RTOS tasks or threads — the ESP-IDF HTTP server runs its own internal tasks; `loop()` is just an LED heartbeat.

**Boot sequence** (`setup()`):
1. AXP313A power IC (I2C at `0x36`) enables camera rail voltages — this is a V1.0-only requirement
2. `esp_camera_init()` configures the OV2640 over DVP using the pin map defined at the top of `main.cpp`
3. WiFi connects and the IP is printed to serial
4. `esp_http_server` starts and registers three URI handlers

**HTTP handlers:**
- `index_handler` — serves the inline HTML viewer (defined as a raw string literal `INDEX_HTML`)
- `stream_handler` — infinite loop that calls `esp_camera_fb_get()` and sends MJPEG multipart chunks; the connection stays open until the client disconnects
- `control_handler` — parses `?var=framesize&val=<n>` and calls `sensor_t::set_framesize()` on the live sensor

**Camera config:**
- PSRAM must be OPI (octal) — set by `board_build.arduino.memory_type = qio_opi` in `platformio.ini`; **do not change this**
- Double-buffered (`fb_count = 2`) in PSRAM, `CAMERA_GRAB_WHEN_EMPTY` mode
- Default resolution: SVGA 800×600, JPEG quality 12 (lower = better, range 0–63)

**`lib/DFRobot_AXP313A/`** — a minimal local shim (header-only) for the AXP313A PMIC. It only enables camera power rails and is not the full upstream library. For V1.1+ boards, the AXP is absent; `axp.begin()` will return `-1` but the firmware continues anyway.

## Board-specific notes

- The N16R8 module uses OPI PSRAM — any change to `board_build.arduino.memory_type` will cause camera init to fail at runtime
- `huge_app.csv` partition scheme is required; the camera + HTTP stack exceeds the default app partition
- On first flash, power-cycle (not just reset) the board — the AXP313A sometimes requires a hard reset to bring rails up correctly
- If no COM port is visible: hold BOOT, tap RST, release BOOT, then flash
- The LED on GPIO 21 blinks every 2 s as a heartbeat; it blinks rapidly on camera init failure
