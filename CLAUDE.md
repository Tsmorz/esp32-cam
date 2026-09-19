# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

```bash
# Build
pio run -e firebeetle2_esp32s3

# Build and flash over USB (kill monitor first to free the port)
pkill -f "pio device monitor"; pio run -e firebeetle2_esp32s3 -t upload --upload-port /dev/cu.usbmodem1101

# Flash over WiFi (no serial port needed) — board IP from /status or the router
TOK=$(sed -n 's/#define API_TOKEN "\(.*\)"/\1/p' src/secrets.h)
curl -H "X-Token: $TOK" -H "Content-Type: application/octet-stream" \
     --data-binary @.pio/build/firebeetle2_esp32s3/firmware.bin http://<ip>/ota

# Serial monitor (run in a separate terminal — do NOT run from Claude Code shell)
pio device monitor

# Clean build artifacts
pio run -e firebeetle2_esp32s3 -t clean
```

Credentials live in `src/secrets.h` (git-ignored; template `src/secrets.example.h`): `WIFI_SSID`, `WIFI_PASS`, `API_TOKEN`, optionally `AP_PASS`. `config.h` includes it via `__has_include` and falls back to empty values, so the firmware (and CI) builds without it. **Never put credentials in `config.h` or any tracked file.**

## Workflow (run after every code edit)

```bash
task format   # clang-format (.clang-format = LLVM) on all src/ and lib/ sources
task check    # cppcheck static analysis — fails only on HIGH/MEDIUM
task build    # pio compile (also regenerates the embedded dashboard)
```
LOW/style cppcheck findings and all clangd diagnostics are false positives — trust the PlatformIO build output. `task ci` runs the same checks as `.github/workflows/ci.yml`.

For dashboard changes: edit `web/index.html`, build, OTA, then screenshot the live page (headless Chrome via `playwright-core` works on the dev machine) — check desktop dark and 390 px light.

## Architecture

Multi-file Arduino firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8). The camera sensor is auto-detected by esp32-camera; **this unit has an OV5640 with voice-coil autofocus (DC5640-AF module)**, not the stock OV2640. Optional LSM6DSOX IMU (currently not attached — firmware must keep working without it).

**Source files:**
- `src/config.h` — ports, **task core/priority layout**, I²C/LED/battery pins, camera DVP pin map
- `src/camera.h/cpp` — `camera_init()`, `camera_set()` (validated + persisted to Preferences `cam`), `camera_restore()`, `camera_status_json()`, `stream_handler`
- `src/focus.h/cpp` — OV5640 AF: firmware upload + mode control
- `src/detector.h/cpp` — AprilTag task, pose, thread-safe result frame, `detector_gray_bmp()`
- `src/imu.h/cpp` — LSM6DSOX driver, gyro bias calibration, VQF sampling task, `imu_state()`
- `src/led.h/cpp` — status LED task
- `src/main.cpp` — AXP313A, WiFi AP+STA, both HTTP servers, auth, battery, `/status`, serial CLI
- `web/index.html` — dashboard source; `scripts/embed_web.py` (pre-build) gzips it into `src/generated/web_index.h` (git-ignored)

**Task layout** (constants in `config.h`):
- **Core 0 (radio):** SDK-pinned WiFi (23), esp_timer (22), lwIP (18), camera DMA task; both httpd tasks — control (6), stream (5).
- **Core 1 (sensing):** `imu` (10, polls every 5 ms, acts on the data-ready bit), `detect` (2, CPU-bound, every `DETECT_PERIOD_MS`=200 ms at most), `led` (1), Arduino `loop()` (1, serial only).
- The camera needs no dedicated core: JPEG is encoded in the sensor and capture is DMA.

**Boot sequence** (`setup()`):
1. LED task starts (fast blink)
2. `Wire.begin(1, 2, 400 kHz)` — one I²C bus (I2C0) shared by AXP313A, LSM6DSOX and the camera's SCCB
3. AXP313A (`0x36`) enables DOVDD 1.8 V / AVDD 2.8 V — V1.0 only; 3 attempts, then assumes V1.1+ and continues
4. `imu_init()` — WHO_AM_I; absent → skipped
5. `camera_init()` joins Wire's bus (`pin_sccb_sda = -1`, `sccb_i2c_port = 0`), inits at UXGA (sizes JPEG buffers for every offered resolution), drops to SVGA. Failure → boot continues, LED double-blinks
6. `focus_init()` (OV5640 only) → `camera_restore()` (saved settings) → `detector_init()`
7. WiFi AP+STA — STA tries the dashboard-saved network (Preferences `wifi`), then `WIFI_SSID`, 10 s each
8. HTTP control :80 and stream :81
9. `imu_start()` / `detector_start()`, LED → idle heartbeat

**HTTP — control server (:80), single httpd task, 8 KB stack:**
- `/` — gzipped dashboard. Four hash-routed pages (live/camera/network/system). Polls `/status` 2 s; on Live also `/detections` 200 ms and `/pose` 100 ms (only if IMU present). Stream `<img>` only while Live is shown. Token prompt via `<dialog>`, stored in `localStorage`, sent as `X-Token`
- `/status` — everything the header/System page show (sensor, AF, IMU, detector timing, WiFi, battery, `temp_c` chip temperature, `stream_port`)
- `/detections`, `/pose`, `/battery`, `/camera/status`, `/snapshot`, `/debug/gray.bmp`, `/tag?id=N` (printable tag page), `/wifi/scan`, `/stream` (302 → :81)
- 🔒 `/camera/set`, `/detector/set`, `/wifi/connect`, `/ota`, `/camera/reset`, `/system/reboot`

🔒 = requires `X-Token: API_TOKEN` (401 otherwise; disabled if `API_TOKEN` is empty). No CORS on these; the custom header forces a never-answered preflight, blocking cross-site requests.

**Why two servers:** an ESP-IDF httpd instance runs every handler on one task; the endless MJPEG handler would block every other route. Stream: one viewer at a time.

**Camera:**
- PSRAM must be OPI — `board_build.arduino.memory_type = qio_opi`; **do not change this**
- `fb_count = 2` in PSRAM, `CAMERA_GRAB_WHEN_EMPTY`; stream and detector each take whole frames
- Init UXGA → run SVGA, quality 12, XCLK 20 MHz (10/16 MHz measured *more* artifacts)
- Resolution values sent by the dashboard are `framesize_t` numbers (1,5,6,10,11,12,13,15) — this esp32-camera renumbered the enum vs. older versions; `valid_framesize()` whitelists them
- On this OV5640, `set_hmirror`/`set_vflip` **work** (verified); the dashboard uses them, not CSS. Tags don't decode in a mirrored image
- `set_reg`/`get_reg` exist for OV5640 in this esp32-camera — always null-check them anyway
- Known artifact: dotted green/black columns (one dash per 8 rows at fixed x) in the OV5640 JPEG output. Independent of detector load and init resolution; worse at lower XCLK; roughly halves at quality 20. Root cause not found yet (suspect OV5640 JPEG output timing / PLL)

**Autofocus (`focus.cpp`):** firmware blob from lib_dep `ESP32-OV5640-AF` (MIT, pinned commit); only its `ESP32_OV5640_cfg.h` is used. Upload: MCU reset (0x3000=0x20) → 124-byte Wire bursts to 0x8000 (auto-increment) → spot-check readback → byte-wise fallback → release → wait for FW_STATUS 0x70. Commands via CMD_MAIN 0x3022 / CMD_ACK 0x3023: 0x08 release (infinity), 0x03 single, 0x04 continuous. FW_STATUS 0x3029: 0x70 idle, 0x10 single focused, **0x20 continuous settled** (measured), 0x00–0x0F/0x80–0x8F focusing. The esp32-camera built-in `esp_camera_af_*` API is compiled empty in this build — don't use it.

**AprilTag detection:**
- Family tag36h11 with **`apriltag_detector_add_family_bits(…, 1)`** — never the default `apriltag_detector_add_family()` (2 bits): its decode table is ~36 MB, `calloc` fails on 8 MB PSRAM, and apriltag then silently matches nothing (this is why detection never worked before). 1 bit = 1 MB PSRAM. `detector_init()` checks `family->impl` and runs a self-test on a synthetic tag; `/status` → `detect.ready`
- `/tag?id=N` renders the tag with `apriltag_to_image()`: black square = `TAG_SIZE_M`, centred with `@page{margin:0}` so A4/Letter keep ≥ 1 cell of white border. Verified: prints 165 mm, and host-compiled apriltag decodes it (hamming 0)
- `grab()` copies the JPEG (`JPEG_BUF` = UXGA/5, the camera's own buffer size) and returns the frame immediately
- Decodes at the smallest scale fitting 400×300 (SVGA → 400×300); `quad_decimate = 2` (quads at 200×150, bits at 400×300)
- Rate is set by the stream resolution: JPEG decode cost scales with the *source* JPEG, not the output scale. Measured: 320×240 ≈ 72+65 ms, 640×480 ≈ 252+80 ms (3.1 Hz), 800×600 ≈ 398+231 ms (1.6 Hz), 1600×1200 ≈ 2 s. ≤ 320×240 hits the `DETECT_PERIOD_MS` = 200 ms (5 Hz) cap. Table in README
- Intrinsics are in **decoded** pixels: `f = focal_px(dw, dh) = FOCAL_PER_WIDTH × max(w, 4h/3)` — non-4:3 sizes are sensor crops (OV5640 `ratio_table`: 1:1/5:4 keep full height, 16:9/3:2 full width); centre principal point; corners scaled back to source pixels. `/detections` publishes `f` in source pixels
- Pose: `mat_to_quat()` turns apriltag's R into `q = [w,x,y,z]` (w ≥ 0), tag→camera, AprilTag frames (tag z into the tag, identity = facing the camera). Verified on host: reprojecting the tag corners from (q, t) lands within 0.13 px of the detected corners; the dashboard's projection uses the same formula
- Results published under a spinlock; `detector_latest()` returns a copy; disabling clears the frame

**Battery:** the board has no battery-sense line. `BAT_PIN -1` = not wired (dashboard says so). With a 2:1 divider to an ADC1 pin (GPIO 10 recommended), set `BAT_PIN`; `battery_read()` averages 16 samples and maps through a LiPo curve.

**VQF / IMU:**
- `lib/VQF/basicvqf.hpp` — port of reference BasicVQF: gyro strapdown, accel low-passed (2nd-order Butterworth, `tauAcc = 3 s`) in the gyro-integrated frame, inclination correction. No bias estimation, no magnetometer
- IMU task averages the first 104 samples as gyro bias if still (< 0.05 rad/s); otherwise warns
- LSM6DSOX at `0x6A`; `0x6B` if SA0 is high

**`lib/DFRobot_AXP313A/`** — header-only PMIC shim. Only enables ALDO1 (DOVDD 1.8 V) and DLDO1 (AVDD 2.8 V). **Never write to register 0x13 (DCDC1)** — it powers an unknown critical rail at ~1.55 V and writing to it while running crashes the board.

**External libraries:**
- `apriltag` v3.3.0 — no PlatformIO manifest; `scripts/apriltag_manifest.py` (pre-build) patches the auto-generated `library.json` (src filter excluding `apriltag_pywrap.c`, `-Wno-error=incompatible-pointer-types` for GCC 14)
- `ESP32-OV5640-AF` — AF firmware blob only

## Board-specific notes

- Platform pinned to **pioarduino 55.03.311** (Arduino 3.3.11 / IDF 5.5) by URL. Plain `espressif32` resolves to PlatformIO's Arduino 2.x platform, which lacks the new-driver SCCB bus sharing
- Build uses `-O2` (`build_unflags = -Os`)
- Flash is 16 MB but the board JSON says 4 MB: `board_upload.flash_size = 16MB` is required for `default_16MB.csv` (two 6.25 MB OTA slots). Verified booting and OTA-updating
- N16R8 uses OPI PSRAM — changing `board_build.arduino.memory_type` breaks camera init
- On first flash, power-cycle (not just reset) — AXP313A sometimes needs a hard reset
- If no COM port visible: hold BOOT, tap RST, release BOOT, then flash
- `pio device monitor` needs a real TTY — tell the user to run it in their own terminal
- `/dev/cu.usbmodem1101` is often held by a stale monitor; `pkill -f "pio device monitor"; pkill -f "miniterm"` before USB uploads (or use OTA)
- `-DARDUINO_USB_CDC_ON_BOOT=1` is mandatory — without it `Serial` maps to UART0 (GPIO 43/44)
