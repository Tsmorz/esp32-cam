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

Credentials live in `src/secrets.h` (git-ignored; template `src/secrets.example.h`): `WIFI_SSID`, `WIFI_PASS`, `API_TOKEN`, optionally `AP_PASS` (default hotspot password `esp32cam1`). `config.h` includes it via `__has_include` and falls back to empty values, so the firmware (and CI) builds without it. **Never put credentials in `config.h` or any tracked file.**

## Workflow (run after every code edit)

```bash
task format   # clang-format (.clang-format = LLVM) on src/, lib/ and test/
task check    # cppcheck static analysis — fails only on HIGH/MEDIUM
task test     # unit tests on the Mac (pio test -e native) — no board needed
task build    # pio compile (also regenerates the embedded dashboard + version)
```
LOW/style cppcheck findings and all clangd diagnostics are false positives — trust the PlatformIO build output. `task ci` = format-check → check → test → build, the same commands the CI jobs run.

**CI** (`.github/workflows/ci.yml`): jobs Format, Static analysis, Unit tests, Build run in parallel, each calling one `task` command; the shared setup (Python, PlatformIO 6.1.19, Task, `~/.platformio` cache) is the composite action `.github/actions/setup-platformio`. Build uploads `firmware.bin` + `firmware.factory.bin` (pioarduino makes the merged image automatically). Release runs only for a pushed `v*` tag, after all four pass: renames the build artifact to `<repo>-<tag>[-factory].bin`, adds SHA256SUMS, `gh release create --generate-notes` (hyphenated tags → pre-release). Validate workflow edits with `actionlint`.

**Local toolchain gotcha:** the pioarduino platform and PlatformIO core pin different `tool-scons` versions, so alternating `task test` and `task build` swaps SCons each time (a few seconds). If a run dies with `No platform named 'darwin'` or a `.sconsign` error, it collided with a swap — just rerun.

For dashboard changes: edit `web/index.html`, build, OTA, then screenshot the live page (headless Chrome via `playwright-core` works on the dev machine) — check desktop dark and 390 px light.

## Architecture

Multi-file Arduino firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8). The camera sensor is auto-detected by esp32-camera; **this unit has an OV5640 with voice-coil autofocus (DC5640-AF module)**, not the stock OV2640. Optional LSM6DSOX IMU (currently not attached — firmware must keep working without it).

**Source files:**
- `src/config.h` — ports, **task core/priority layout**, I²C/LED pins, camera DVP pin map
- `src/camera.h/cpp` — `camera_init()`, `camera_set()` (validated + persisted), `camera_restore()`, `camera_status_json()`, `stream_handler`
- `src/camera_profile.h/cpp` — **which size the sensor is at, and the sensor lock.** `framesize` set through `camera_set` is the *stream* size (what the dashboard shows); the sensor is at it while `viewers > 0` (stream connections + `camera_fb_get_at_stream_size()` for `/snapshot`), else at the *detection* size if set, the detector is ready and enabled, after a 3 s hold. `camera_profile_update()` switches; `loop()` calls it every 250 ms. Default detection size 0 (off). **Every sensor register write takes `sensor_lock()`** (recursive) — `set_framesize()` rewrites registers the mirror/flip setters read-modify-write
- `src/focus.h/cpp` — OV5640 AF: firmware upload + mode control. `focus_set_mode()` only queues; `focus_tick()` (from `loop()`) runs the MCU command sequence, so the httpd task never waits for the lens
- `src/detector.h/cpp` — AprilTag task (JPEG decode → detect → pose), thread-safe result frame, `detector_gray_bmp()`, `detector_tag_svg()`
- `src/calibration_store.h/cpp` — **the camera calibration in use**: stored in NVS (`settings::CALIB`), set over HTTP (`POST /calibration`, `scripts/push_calibration.py`), thread-safe copy via `calibration_get()`. Falls back to `src/calibration.h` (compiled-in default: `CALIB_*`, `CALIB_FX 0` = none), then to the estimate. `calibration_json.h` parses/validates the body (host-tested). Calibration does *not* record Mirror/Flip
- `src/settings.h/cpp` — the one place naming the NVS namespaces (`cam`, `det`, `wifi`, `calib`, `sys`), the schema version + migration hook, `settings_reset(scope)`. New persisted data goes through it
- `src/detection_types.h`, `detections_json.h`, `orientation_history.h`, `strbuf.h` — header-only, no Arduino, host-tested. `DetectionFrame.t_us` = capture time (`fb->timestamp`, esp_timer µs; falls back to grab time if implausible — unverified on hardware), `imu_q` = IMU orientation at `t_us` from `imu_quat_at()` (256-sample history, ~2.4 s)
- `src/push.h/cpp` — UDP push: `sub <API_TOKEN>` to :5005 subscribes, `bye <API_TOKEN>` leaves (10 s TTL, 4 max; parsing in host-tested `push_message.h`; disabled if `API_TOKEN` is empty); the detector task sends the compact `detections_json` to each subscriber
- `src/board.h/cpp` (I²C + AXP313A), `wifi_mgr.h/cpp` (AP+STA, saved network, mDNS `HOSTNAME.local` = `esp32cam.local`, async scan), `http_server.h/cpp` (both servers + every route), `http_util.h/cpp` (`authorized`, `read_body`, `send_json`), `ota_health.h/cpp`, `system.h/cpp` (`system_restart_in(ms)` — reply first, restart from `loop()`; never `delay()` in a handler)
- `src/camera_model.h` — header-only, no Arduino: `Calibration` → `CameraModel` for any w×h (`camera_model()`), `undistort_point()`
- `src/tag_pose.h` — header-only, no Arduino: `configure_detector()` (1-bit table), `detector_self_test()`, `undistort_detection()`, `mat_to_quat()`, `tag_pose()`. Firmware and unit tests share it — keep Arduino out of it
- `test/test_camera_model`, `test/test_tag_pose`, `test/test_push_message` — Unity tests, `[env:native]`; synthetic tags rendered with `apriltag_to_image()`
- `src/imu.h/cpp` — LSM6DSOX driver, gyro bias calibration, VQF sampling task, `imu_state()`
- `src/led.h/cpp` — status LED task
- `src/main.cpp` — `setup()`, `loop()` (serial CLI, `camera_profile_update()`, `ota_health_tick()`)
- `web/index.html` — dashboard source; `scripts/embed_web.py` (pre-build) gzips it into `src/generated/web_index.h` (git-ignored)
- `scripts/version.py` (pre-build) — `git describe --tags --always --dirty` → `src/generated/version.h` (`FW_VERSION`, only main.cpp includes it) → `/status.version`, System page

**Task layout** (constants in `config.h`):
- **Core 0 (radio):** SDK-pinned WiFi (23), esp_timer (22), lwIP (18), camera DMA task; both httpd tasks — control (6), stream (5).
- **Core 1 (sensing):** `imu` (10, polls every 5 ms, acts on the data-ready bit), `detect` (2, CPU-bound, every `DETECT_PERIOD_MS`=33 ms at most; its `DETECT_THREADS`=2 apriltag worker pthreads are unpinned, 8 KB stacks, so core 0's idle time helps), `led` (1), Arduino `loop()` (1: serial, `focus_tick`, `system_tick`, 250 ms housekeeping).
- The camera needs no dedicated core: JPEG is encoded in the sensor and capture is DMA.

**Boot sequence** (`setup()`):
1. `settings_init()`, LED task starts (fast blink)
2. `board_init()`: `Wire.begin(1, 2, 400 kHz)` — one I²C bus (I2C0) shared by AXP313A, LSM6DSOX and the camera's SCCB
3. AXP313A (`0x36`) enables DOVDD 1.8 V / AVDD 2.8 V — V1.0 only; 3 attempts, then assumes V1.1+ and continues
4. `imu_init()` — WHO_AM_I; absent → skipped
5. `camera_init()` joins Wire's bus (`pin_sccb_sda = -1`, `sccb_i2c_port = 0`), inits at UXGA (sizes JPEG buffers for every offered resolution), drops to SVGA. Failure → boot continues, LED double-blinks
6. `focus_init()` (OV5640 only) → `camera_restore()` (saved settings + detection size) → `calibration_load()` → `detector_init()`
7. WiFi AP+STA — STA tries the dashboard-saved network (Preferences `wifi`), then `WIFI_SSID`, 10 s each
8. HTTP control :80 and stream :81, mDNS started with WiFi
9. `push_start()` (UDP :5005), `imu_start()` / `detector_start()`, LED → idle heartbeat

**HTTP — control server (:80), single httpd task, 8 KB stack:**
- `/` — gzipped dashboard. Four hash-routed pages (live/camera/network/system). Polls `/status` 2 s; on Live also `/detections` 200 ms and `/pose` 100 ms (only if IMU present). Stream `<img>` only while Live is shown. Token prompt via `<dialog>`, stored in `localStorage`, sent as `X-Token`
- `/status` — everything the header/System page show (sensor, AF, IMU, detector timing, WiFi, `temp_c` chip temperature, `stream_port`)
- `/calibration` (GET; 🔒 POST/DELETE), 🔒 `/settings/reset?scope=`, `/detections`, `/pose`, `/camera/status`, `/snapshot`, `/debug/gray.bmp`, `/tag?id=N` (printable tag page), `/wifi/scan`, `/stream` (302 → :81)
- 🔒 `/camera/set`, `/detector/set` (`enabled`, `res`; this and `/camera/set` are POST), `/wifi/connect`, `/ota`, `/camera/reset`, `/system/reboot`

🔒 = requires `X-Token: API_TOKEN` (401 otherwise; disabled if `API_TOKEN` is empty). State changes are POST/DELETE only, and **no route sends CORS headers**, so other origins can't read responses; the custom header on 🔒 routes forces a preflight that is never answered.

**Handlers must not block:** every route runs on the one control httpd task. No `delay()`, no waiting on hardware — queue the work and let `loop()` do it (`focus_tick`, `system_restart_in`, async WiFi scan).

**OTA rollback:** the bootloader has app rollback on, and Arduino would mark a new image valid before `setup()`. `ota_health.cpp` overrides `verifyRollbackLater()` to hold that back; `loop()` confirms after `OTA_CONFIRM_MS` (20 s). A reset before that boots the previous image. `/status.ota_pending`. Untested on the board.

**`/wifi/scan` never blocks** (it would stall the single httpd task): the first call starts an async scan and returns `{"scanning":true}`; poll until it doesn't. `?refresh=1` rescans.

**Why two servers:** an ESP-IDF httpd instance runs every handler on one task; the endless MJPEG handler would block every other route. Stream: one viewer at a time.

**Camera:**
- PSRAM must be OPI — `board_build.arduino.memory_type = qio_opi`; **do not change this**
- `fb_count = 2` in PSRAM, `CAMERA_GRAB_WHEN_EMPTY`; stream and detector each take whole frames
- Init UXGA → run SVGA, quality 12, XCLK 20 MHz (10/16 MHz measured *more* artifacts)
- Resolution values sent by the dashboard are `framesize_t` numbers (1,5,6,10,11,12,13,15) — this esp32-camera renumbered the enum vs. older versions; `valid_framesize()` whitelists them
- On this OV5640, `set_hmirror`/`set_vflip` **work** (verified); the dashboard uses them, not CSS. Tags don't decode in a mirrored image
- `set_reg`/`get_reg` exist for OV5640 in this esp32-camera — always null-check them anyway
- Known artifact: dotted green/black columns (one dash per 8 rows at fixed x) in the OV5640 JPEG output. Independent of detector load and init resolution; worse at lower XCLK; roughly halves at quality 20. Root cause not found yet (suspect OV5640 JPEG output timing / PLL)

**Autofocus (`focus.cpp`):** firmware blob from lib_dep `ESP32-OV5640-AF` (MIT, pinned commit); only its `ESP32_OV5640_cfg.h` is used. Upload: MCU reset (0x3000=0x20) → 124-byte Wire bursts to 0x8000 (auto-increment) → spot-check readback → byte-wise fallback → release → wait for FW_STATUS 0x70. Commands via CMD_MAIN 0x3022 / CMD_ACK 0x3023: 0x08 release (infinity), 0x03 single, 0x04 continuous. FW_STATUS 0x3029: 0x70 idle, 0x10 single focused, **0x20 continuous settled** (measured), 0x00–0x0F/0x80–0x8F focusing. The esp32-camera built-in `esp_camera_af_*` API is compiled empty in this build — don't use it. Each command waits for status to settle (≤ 2 s), then for the ack (≤ 3 s); a mode that isn't acked is rolled back.

**AprilTag detection:**
- Family tag36h11 with **`apriltag_detector_add_family_bits(…, 1)`** — never the default `apriltag_detector_add_family()` (2 bits): its decode table is ~36 MB, `calloc` fails on 8 MB PSRAM, and apriltag then silently matches nothing (this is why detection never worked before). 1 bit = 1 MB PSRAM. `detector_init()` checks `family->impl` and runs a self-test on a synthetic tag; `/status` → `detect.ready`
- `/tag?id=N` renders the tag with `apriltag_to_image()`: black square = `TAG_SIZE_M`, centred with `@page{margin:0}` so A4/Letter keep ≥ 1 cell of white border. Verified: prints 165 mm, and host-compiled apriltag decodes it (hamming 0)
- Frames: while the MJPEG stream runs, `stream_handler` hands **every** frame to `detector_offer()` (via `camera_on_stream_frame`) — the detector must not `esp_camera_fb_get()` then, or it steals frames from the 2-buffer queue. Triple-buffered (`_stream_spare` → `_stream_ready` ↔ `_jpeg_copy`, `JPEG_BUF` = UXGA/5 each), so the detector never waits for the next frame. No stream frame for 500 ms → it grabs directly
- Decode: `esp_new_jpeg` (Espressif SIMD decoder, lib_dep) in block mode to CbYCrY; only Y is kept, box-averaged to the smallest 1/2ⁿ fitting 400×300 — no RGB. Cost scales with the *source* JPEG
- `quad_decimate = 2` (quads at 200×150, bits at 400×300); `min_white_black_diff = 20` (default 5 let JPEG noise and the dotted-column artefact make hundreds of candidate blobs; host sweep: no tag lost up to 20, lowest-contrast tags lost at 25)
- The S3's FPU is single precision: apriltag's double maths is soft-float. `scripts/apriltag_manifest.py` rewrites quad fitting (`apriltag_quad_thresh.c`) and `refine_edges()` to float and makes `polyval()` Horner (was `pow()` per term); `tag_pose.h` replaces apriltag's orthogonal iteration (matd_op string parsing + malloc per step, 2 × 50 steps) with fixed-size float arrays — pose 313 → 12.6 ms per tag at 50 steps, unit-tested against apriltag's iteration at the same step count. `POSE_STEPS = 200` (not apriltag's 50): simulated with 0.1 px corner noise, 50 steps leaves 5 % of poses ≥ 2.3° short of converged (p99 error 13° vs 6° at 200); median unchanged, 500 adds nothing. Measured on the board: 17.5 ms per tag at 200 (12.7 ms frontal), +5 ms over 50
- Timings and camera fps per resolution: README → AprilTag detection. **Live the camera is the limit** (OV5640 delivers 240×240 at only 9 fps; 320×240 is the fastest for detection)
- Intrinsics: `camera_model(w, h)` in **source** pixels (from `calibration.h`, or the estimate fx = fy = `FOCAL_PER_WIDTH × max(w, 4h/3)`, centre principal point); divided by `div` for the decoded image. Non-4:3 sizes are centred sensor crops (esp32-camera OV5640 `ratio_table`: 1:1/5:4 keep full height, 16:9/3:2 full width — checked against upstream source), so one 4:3 calibration rescales to all. `/detections` publishes the model as `cam`
- Pose: `tag_pose()` — if the model has distortion, `undistort_detection()` undistorts `det->p` and rebuilds `det->H` with `homography_compute()` (apriltag's `homography_compute2` is static), then apriltag's `estimate_tag_pose()` algorithm with `orthogonal_iteration4()` (float arrays) as the iteration, then `mat_to_quat()` → `q = [w,x,y,z]` (w ≥ 0), tag→camera, AprilTag frames (tag z into the tag, identity = facing the camera). Published corners stay distorted (they must match the image); the dashboard projects axes with forward distortion
- Verified on host: reprojection from (q, t) within 0.13 px of detected corners; with a strong barrel lens, undistortion cut a corner tag's error from 111 mm / 10.9° to 8 mm / 0.4°. Unit tests cover this and fail if `TAG_BITS_CORRECTED` goes back to 2 (table 36,544,272 B) or undistortion is skipped
- Results published under a spinlock; `detector_latest()` returns a copy; disabling clears the frame

**VQF / IMU:**
- `lib/VQF/basicvqf.hpp` — port of reference BasicVQF: gyro strapdown, accel low-passed (2nd-order Butterworth, `tauAcc = 3 s`) in the gyro-integrated frame, inclination correction. No bias estimation, no magnetometer
- IMU task averages the first 104 samples as gyro bias if still (< 0.05 rad/s); otherwise warns
- LSM6DSOX at `0x6A`; `0x6B` if SA0 is high

**`lib/DFRobot_AXP313A/`** — header-only PMIC shim. Only enables ALDO1 (DOVDD 1.8 V) and DLDO1 (AVDD 2.8 V). **Never write to register 0x13 (DCDC1)** — it powers an unknown critical rail at ~1.55 V and writing to it while running crashes the board.

**External libraries:**
- `apriltag` v3.3.0 — no PlatformIO manifest; `scripts/apriltag_manifest.py` (pre-build) patches the auto-generated `library.json` (src filter excluding `apriltag_pywrap.c`, `-Wno-error=incompatible-pointer-types` for GCC 14) and the sources (float / Horner, above; marker line at the top of each patched file — delete `.pio/libdeps/*/apriltag` to re-fetch pristine sources)
- `esp_new_jpeg` 1.0.2 — Espressif component zip (headers + prebuilt `libesp_new_jpeg.a`, Espressif MIT licence: Espressif chips only); `scripts/esp_new_jpeg_manifest.py` writes its `library.json` (no sources, link the esp32s3 archive)
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
