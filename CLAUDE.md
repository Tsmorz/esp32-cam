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

Credentials live in `src/secrets.h` (git-ignored; template `src/secrets.example.h`, all commented out): `WIFI_SSID`, `WIFI_PASS`, `API_TOKEN` (8–63 printable, `static_assert`ed), optionally `AP_PASS` (fixes the hotspot password; otherwise `AP_PASS_DEFAULT` "wificamera" until a token exists, then the token). `config.h` includes it via `__has_include` and falls back to empty values, so the firmware (and CI) builds without it. **Never put credentials in `config.h` or any tracked file.**

## Workflow (run after every code edit)

```bash
task format   # clang-format (.clang-format = LLVM) on src/, lib/ and test/
task check    # cppcheck static analysis — fails only on HIGH/MEDIUM
task test     # unit tests on the Mac (pio test -e native) — no board needed
task build    # pio compile (also regenerates the embedded dashboard + version)
```
LOW/style cppcheck findings and all clangd diagnostics are false positives — trust the PlatformIO build output. `task ci` = format-check → check → test → build, the same commands the CI jobs run.

**CI** (`.github/workflows/ci.yml`): jobs Format, Static analysis, Unit tests, Build run in parallel, each calling one `task` command; the shared setup (Python, PlatformIO 6.1.19, Task, `~/.platformio` cache) is the composite action `.github/actions/setup-platformio`. Build uploads `firmware.bin` + `firmware.factory.bin` (pioarduino makes the merged image automatically). **Automatic releases:** job Version runs `scripts/next_version.sh` → tag for this run: on a push to `main` with an untagged commit, the next version after the highest `vX.Y.Z` (patch; `#minor`/`#major` in a commit message or PR title since then); on a pushed `v*` tag, that tag; else none. Build compiles with `FW_VERSION` = that tag (`scripts/version.py` honours the env var, else `git describe`). After all four checks pass, Tag pushes the tag (main only; `contents: write`), and Release renames the build artifact to `<repo>-<tag>[-factory].bin`, adds SHA256SUMS, `gh release create --generate-notes --verify-tag` (hyphenated tags → pre-release). Both happen in one run because a tag pushed with `GITHUB_TOKEN` doesn't trigger another workflow. Pushes to `development` publish the rolling pre-release `dev` instead (the user's choice, 2026-10-26): Tag force-moves tag `dev`, Release updates that release in place (`gh release upload --clobber` + `edit`, created once with `--latest=false`), assets `<repo>-dev[-factory].bin`; `FW_VERSION` = `<next>-dev.<commits since last vX.Y.Z>+<sha7>`. Real releases pass `--notes-start-tag <previous vX.Y.Z>` so notes don't start at `dev`. Runs on `main` and `development` are never cancelled (a cancelled run could leave a tag without its release). Validate workflow edits with `actionlint` (Homebrew).

**Against a running board:** `task smoke` (scripts/smoke_test.py) after every OTA — routes, token, POST-only, no CORS, Host check, video needs the password (cookie works, 403 over the hotspot), `/snapshot`, `/camera/status`. Options after `--` (`task smoke -- -H <ip>`); shared helpers in `scripts/boardtool.py`.

**Local toolchain gotcha:** the pioarduino platform and PlatformIO core pin different `tool-scons` versions, so alternating `task test` and `task build` swaps SCons each time (a few seconds). If a run dies with `No platform named 'darwin'` or a `.sconsign` error, it collided with a swap — just rerun.

For dashboard changes: edit `web/index.html`, build, OTA, then screenshot the live page (headless Chrome via `playwright-core` works on the dev machine) — check desktop dark and 390 px light.

## Architecture

Multi-file Arduino firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8): MJPEG video, camera settings, OV5640 autofocus, optional orientation + compass, optional memory card (photos, clips, timelapse), animal detection with an uploaded TFLite model, and deep sleep woken by the IMU, behind a web dashboard. The camera sensor is auto-detected by esp32-camera; **this unit has an OV5640 with voice-coil autofocus (DC5640-AF module)**, not the stock OV2640.

**Who it's for (the user's rule, 2026-10-26): someone who isn't an engineer must be able to use it like a personal webcam, at every step** — setup, WiFi, updates, calibration. Plain words on screen ("password", not "API token"; "software update", not "OTA"; "motion sensor", not "IMU"; signal bars, not dBm), guided steps, no step that needs a terminal; engineering details only under Settings → *Advanced details*. Bluetooth setup was considered and rejected: Safari has no Web Bluetooth, and a native app is more friction than the captive portal.

**Security model (the user's rules, 2026-10-26):** the **hotspot is for configuration only** — `/stream`, `/snapshot`, `/pose` answer 403 over it (`via_hotspot()`), and on the home network they need the password (`X-Token` or the `wc_video` cookie from `GET /auth`/claim; `video_allowed()`). **Passwords in NVS are encrypted** (`secret_store`). A forgotten password is reset with the **BOOT button held 5 s** (no terminal). Every route checks `Host` (`host_ok()` → captive redirect), which also blocks DNS rebinding.

**Removed 2026-10-26 at the user's request:** AprilTag detection (detector, pose, calibration, printable tags, detection size, UDP push, IMU-fused track). The last version with it (including uncommitted tracking work) is the local branch `apriltag-archive`. Don't reintroduce it unless asked. The IMU was removed and re-added the same day, now with the magnetometer.

**Source files:**
- `src/config.h` — ports, **task core/priority layout**, I²C/LED pins, camera DVP pin map
- `src/camera.h/cpp` — `camera_init()`, `camera_set()` (validated + persisted), `camera_restore()`, `camera_status_json()`, `stream_handler`
- `src/camera_profile.h/cpp` — the stream size (`framesize` through `camera_set`, applied immediately), the stream viewer count (`/camera/status.viewers`, so the dashboard can tell a second viewer the stream is taken) and the sensor lock. **Every sensor register write takes `sensor_lock()`** (recursive) — `set_framesize()` rewrites registers the mirror/flip setters read-modify-write
- `src/focus.h/cpp` — OV5640 AF: firmware upload + mode control. `focus_set_mode()` only queues; `focus_tick()` (from `loop()`) runs the MCU command sequence, so the httpd task never waits for the lens
- `src/auth.h/cpp` — the API token: `API_TOKEN` from secrets.h if set, else one **claimed at runtime** (`POST /auth/claim`, first visitor wins, kept encrypted in NVS `sys/token_x`; a secrets.h token and WiFi network are copied to NVS on boot, so a later secrets-less build inherits them; CI/release binaries have no secrets.h so start unclaimed). Use `auth_token()`/`auth_configured()`, never `API_TOKEN` directly. `auth_video_cookie()` = hex SHA-256 of a label + the token. `token_rules.h` (host-tested, constexpr): 8–63 printable, no spaces — 63 because the token doubles as the hotspot's WPA2 passphrase. Serial `T` or BOOT held 5 s (`reset_button_tick()` in main.cpp, acts on release: GPIO0 low at reset = ROM downloader) forgets a claimed token
- `src/secret_store.h/cpp` — passwords at rest: AES-256-GCM blobs (version | 12-byte nonce = NVS counter `sys/nonce` + random | tag | ciphertext, AAD = `ns/key`). Key = HMAC of a label with an **eFuse key block (purpose HMAC_UP), burned by the firmware on first boot from `bootloader_random` entropy — permanent, read-protected**. No free block → key from the MAC (`secret_store_protection()` = "firmware", only obscures). IDF NVS encryption isn't usable: Arduino's `initArduino()` mounts NVS plain (and erases it on errors), and the partition table can't change over OTA. `secret_adopt()` encrypts a legacy plain value
- `src/settings.h/cpp` — the one place naming the NVS namespaces (`cam`, `wifi`, `imu`, `sys`) and keys, the schema version + migrations (`migrate()`; schema 2 clears the removed detector's `det` and `calib`; schema 3 = passwords encrypted), `settings_reset(scope)`. `settings_init()` adopts plain-text `sys/token`, `wifi/pass` on every boot, then `settings_scrub()` (copy all entries, erase the partition, write back, minus `nvs.net80211`) because NVS leaves deleted values in flash. New persisted data goes through it; secrets through `secret_store`
- `src/strbuf.h` — `appendf` / `json_esc` / `json_str` / `url_decode` (httpd query values arrive still %-encoded), header-only, host-tested (`test/test_strbuf`)
- `src/options.h/cpp` — everything the dashboard switches (NVS `opt`): wiring (`sd_sck/miso/mosi/cs`, `imu_int`; −1 = not connected; restart to apply), `timelapse_min`, `motion_action`/`detect_action` (0 none, 1 photo, 2 clip), `motion_sens` 1–3, `clip_s`, `rec_max_min`, `sleep_min`, `detect_on`, `detect_thr` %, `detect_only` (comma list). `SPECS` table validates; pins only from `FREE_PINS` (9–18, 38, 47), no two wires on one GPIO, wake pin ≤ 21 (RTC). **Pins are unverified against the FireBeetle's headers**
- `src/storage.h/cpp` — SD over SPI (`FSPI`, 16 MHz, `SD.begin(..., format_if_empty)`), `/WCAM/IMG_nnnnn.JPG` / `VID_nnnnn.MOV`, one sequence, index kept in RAM (no directory walk per gallery request). State none/missing/ok/full (< 2 MB free). Not encrypted (user's choice)
- `src/recorder.h/cpp` — task `rec` (core 1, prio 3): photos, clips (`mov_writer.h`, frame table in PSRAM, moov written at stop, mdat size patched), timelapse (`RTC_DATA_ATTR` last time, survives sleep), triggers from `imu_motion_take()` / `detect_take_trigger()` with a 30 s gap. Takes frames like the stream, so recording halves the stream's frame rate
- `src/mov_writer.h` — header-only QuickTime Motion-JPEG ('jpeg', timescale ms, stts run-length) — verified with ffprobe and AVFoundation (`avconvert`); host-tested (`test/test_mov`)
- `src/detect.h/cpp` — model in partition `spiffs` ([header 4 KB][model][labels], header written last, CRC), copied to PSRAM, TFLM `MicroMutableOpResolver<32>`, arena tried 256 KB → 3 MB. Task `detect` (core 1, **prio 1 = loop()'s, they time-slice**), ≥ 700 ms apart, only while `detect_on`; unloaded when off. Frame → `esp_jpeg_decode` RGB888 at 1/2–1/8 → `detect_post.h`. Upload holds `_mx` (waits for an inference). Measured: person_detect 96×96 int8 = 133 ms/look
- `src/detect_post.h` — header-only, host-tested (`test/test_detect`): centre crop, quantize (`InputRange` 0..1 / −1..1 / 0..255), classifier vs FOMO grid decode (flood-fill centroids), `label_negative()` ("background", "none", "no …" are never results or triggers)
- `src/power.h/cpp` — deep sleep: only with IMU + `imu_int`; blocked while viewers, recording, hotspot clients, OTA probation; idle = no request except `/status?bg=1` (background tab). Sleep: `esp_camera_deinit`, AXP313A ALDO1/DLDO1 off, `imu_prepare_wake()` (LSM6DSOX LP 26 Hz, WU threshold → INT1 latched, gyro + LIS3MDL off), ext0 on `imu_int` (pull-down), timer for the next timelapse. A timer wake (`power_woke_for_timelapse()`) takes the photo in `setup()` without WiFi or `imu_init()` (the IMU keeps its wake setup) and sleeps again
- `src/imu.h/cpp` — **Adafruit LSM6DSOX + LIS3MDL board (4517)**, optional: both I²C addresses probed (0x6A/0x6B, 0x1C/0x1E). Accel/gyro 52 Hz (±4 g, 500 dps), mag 40 Hz UHP ±4 gauss — under 100 Hz is fine per the user. Gyro bias from the first second. VQF 9D once the compass is calibrated, 6D before (uncalibrated hard iron would make heading worse than gyro drift). Compass calibration: `imu_mag_cal_start/finish/cancel/clear`, collected in the task, stored as a blob in NVS `imu/mag`; the dashboard finishes it automatically at progress 1. Quaternion is sensor → East-North-Up. **Never verified with the real board** (none attached yet): check axis alignment of the LIS3MDL vs the LSM6DSOX and the heading sign
- `src/mag_cal.h` — header-only, host-tested: min/max per axis → hard-iron offset + diagonal scale; progress = worst axis span / max(60 µT, 0.8 × widest), ≥ 200 samples; rejects fields outside 15–150 µT or axes > 2× apart
- `lib/VQF/basicvqf.hpp` — BasicVQF port: gyro strapdown, accel low-passed (2nd-order Butterworth, `tauAcc` 3 s) in the gyro-integrated frame, inclination correction; **magnetometer heading** (`updateMag`, `getQuat9D`, `delta` filtered with `tauMag` 9 s, `kMagInit` fast start — as the reference `basicvqf.cpp`), plus `restartHeading()` (not in the reference). No bias estimation. `test/test_vqf` checks heading at several yaws, with tilt, 6D unaffected, and the calibration maths
- `src/board.h/cpp` (I²C + AXP313A), `wifi_mgr.h/cpp` (AP+STA, saved network, mDNS `HOSTNAME.local` = `wifi-camera.local`, async scan, `wifi_tick()` turns the hotspot off while STA is up and unused; `/wifi/connect` only queues (`wifi_request_join`) — `wifi_tick()` joins, saves the network only once it got an IP, reverts to the saved one after 30 s; while the hotspot has clients and STA is down it disables auto-reconnect and retries once a minute (each attempt scans all channels and drops AP clients); the AP password is set in `ap_start()`, so a newly claimed token applies from the next AP start; **captive-portal DNS** while the hotspot is on — a heap `DNSServer` created/deleted with it, because `DNSServer::stop()` leaves AsyncUDP's socket bound and it kept answering on the home network), `http_server.h/cpp` (both servers + every route, all through `guarded()` = Host check), `http_util.h/cpp` (`authorized`/`token_ok`, `video_allowed`, `via_hotspot`, `host_ok`/`redirect_home`, `read_body`, `send_json`), `ota_health.h/cpp`, `system.h/cpp` (`system_restart_in(ms)` — reply first, restart from `loop()`; never `delay()` in a handler)
- `test/test_token_rules`, `test/test_strbuf`, `test/test_vqf`, `test/test_mov`, `test/test_detect` — Unity tests, `[env:native]` (`-Ilib/VQF`)
- `src/led.h/cpp` — status LED task
- `src/main.cpp` — `setup()`, `loop()` (serial CLI, BOOT-button reset, `focus_tick`, `system_tick`, `wifi_tick`, `ota_health_tick(healthy)`)
- `web/index.html` — dashboard source; `scripts/embed_web.py` (pre-build) gzips it into `src/generated/web_index.h` (git-ignored)
- `scripts/version.py` (pre-build) — `git describe --tags --always --dirty` → `src/generated/version.h` (`FW_VERSION`, written on change) and the build time → `src/generated/build_time.h` (`FW_BUILT`, every build); only `src/build_info.cpp` includes them → `/status.version` and `/status.build` (the dashboard and the rollback test tell images apart by `build`)

**Task layout** (constants in `config.h`):
- **Core 0 (radio):** SDK-pinned WiFi (23), esp_timer (22), lwIP (18), camera DMA task; both httpd tasks — control (6), stream (5); `files` (5, sends gallery files for async httpd requests).
- **Core 1:** `imu` (10, polls every 10 ms, acts on the data-ready bits; only with the IMU board), `rec` (3, only with a card wired), `led` (1), `detect` (1), Arduino `loop()` (1: serial, BOOT button, `focus_tick`, `system_tick`, 250 ms housekeeping incl. `power_tick`). AsyncUDP's task serves the captive DNS.
- The camera needs no dedicated core: JPEG is encoded in the sensor and capture is DMA.

**Boot sequence** (`setup()`):
1. Waits ≤ 3 s for a USB serial host, `ota_health_init()` (on probation? rolled back?), `settings_init()` (chip key, migrations, scrub), `auth_init()`, LED task starts (fast blink)
2. `board_init()`: `Wire.begin(1, 2, 400 kHz)` — one I²C bus (I2C0) shared by the AXP313A, the IMU board and the camera's SCCB
3. AXP313A (`0x36`) enables DOVDD 1.8 V / AVDD 2.8 V — V1.0 only; 3 attempts, then assumes V1.1+ and continues
4. `imu_init()` — WHO_AM_I probes; absent → skipped. Then `camera_init()` joins Wire's bus (`pin_sccb_sda = -1`, `sccb_i2c_port = 0`), inits at UXGA (sizes JPEG buffers for every offered resolution), drops to SVGA. Failure → boot continues, LED double-blinks
5. `focus_init()` (OV5640 only) → `camera_restore()` (saved settings)
6. WiFi AP+STA — STA tries the dashboard-saved network (Preferences `wifi`), then `WIFI_SSID`, 10 s each
7. HTTP control :80 and stream :81, mDNS started with WiFi; `imu_start()`; BOOT button input; LED → idle (steady on); `ota_health_start()` (probation clock)

**HTTP — control server (:80), single httpd task, 8 KB stack:**
- `/` — gzipped dashboard. Hash-routed pages live/camera/photos/network/system, shown as Live / Picture / Photos / WiFi / Settings (Photos: gallery with thumbnails loaded two at a time, viewer that plays MOV clips by parsing their moov, automation options; Settings: Animal detection card with model upload, Sleep card, add-on wiring under Advanced details — all `[data-opt]` controls → `/options/set`; the gallery and detection details reload when `[storage state, canWatch, hotspot]` changes), plus `#setup` (not in the menu): password (claim, or enter one for this browser) → home WiFi (scan + join, waits for `sta_ip`) → done (addresses, incl. the IP for Android). It opens by itself on the first status if `auth` is false, or if the page came over the hotspot and STA isn't up (unless skipped, `localStorage.setupSkipped`). Live is the viewfinder, sized to the window, with a Motion sensor card beside it when `/status.imu.state != none` (compass rose, tilt, calibration; polls `/pose` 100 ms). Video and motion only when `canWatch()`: not over the hotspot (`/status.hotspot`; Live explains where to watch) and with the video cookie (`ensureVideo()` calls `/auth` with the stored token; otherwise "Enter the password to watch"). *Forget* calls `/auth/forget` (the cookie is HttpOnly). Polls `/status` 2 s (1 s while watching a restart, 10 s with the tab hidden). Stream `<img>` only while Live is shown **and the tab is visible**; a stalled stream with `/camera/status.viewers > 0` = someone else has it. Token prompt via `<dialog>`, stored in `localStorage`, sent as `X-Token`. One banner (`renderBanner()`) for restart in progress > lost connection > dismissable result > `ota_pending` > unclaimed board. Reboot/reset/OTA call `watchRestart()`: a reboot shows as uptime restarting; on trial = `ota_pending` (countdown `ota_confirm_s`); the result comes from `rolled_back` / a changed `build`. `pollStatus()` runs one request at a time (`polling`). The OTA file is checked client-side (0xE9, chip id 9, `esp_app_desc_t` magic at byte 32 — a `-factory` image has the bootloader there). Look: "viewfinder" — graphite / paper, one signal colour (focus-peak magenta), crop marks, monospace readouts, system fonts only (no internet on the hotspot); deliberately unlike the personal site and the nrf-controller dashboard
- `/status` — everything the header/Settings page show (version, `build`, `ota_pending`, `ota_confirm_s`, `rolled_back`, `hotspot`, `secrets`, sensor, AF, `temp_c`, memory, `stream_port`, `schema`, `imu` {state, samples, errors, mag}, `storage` {state, total, used, recording, rec_s, why}, `power` {wake, blocker, idle_s}, `detect` {state}, WiFi incl. `ap_on`, `ap_key`, `joining`). `?bg=1` (hidden tab) doesn't count as activity for sleep
- `/camera/status`, `/wifi/scan`, `/stream` (302 → :81), `/auth/forget` (clears the cookie)
- 🎥 `/pose`, `/snapshot`, `:81/stream`, `/files`, `/files/get`, `/detect` — `video_allowed()`: 403 over the hotspot, else X-Token or cookie (401). `/files/get` uses `httpd_req_async_handler_begin` → queue → `files` task, so downloads don't hold the control task
- `/options` (public: config), 🔒 `/options/set?key=&val=`, `/files/delete?name=|all`, `/capture/photo`, `/capture/record?do=start|stop`, `/model?name=&model_len=&labels_len=&range=` (body model+labels, streamed to flash like `/ota`), `/model/forget`, `/system/sleep`. `max_uri_handlers` 32
- 🔒 `/auth` (token check, GET, sets the `wc_video` cookie; unauthenticated `POST /auth/claim` only while no token is set, also sets it), `/camera/set` (POST), `/imu/compass?do=start|finish|cancel|clear`, `/wifi/connect`, `/ota`, `/camera/reset`, `/settings/reset?scope=camera|wifi|imu|token|all`, `/system/reboot`
- Any other path: `httpd_register_err_handler(404)` → 404 if the Host is the camera (its IP on that interface via `getsockname`, `wifi-camera.local`), else 302 to `http://<that IP>/` — the captive portal (phones' connectivity checks). Verified with curl and `dig` against the board. Known routes do the same Host check (`guarded()`)

🔒 = requires `X-Token` = the API token (401 otherwise; with no token set yet — a release build before `POST /auth/claim` — every 🔒 route is refused). State changes are POST only, and **no route sends CORS headers**, so other origins can't read responses; the custom header on 🔒 routes forces a preflight that is never answered.

**Handlers must not block:** every route runs on the one control httpd task. No `delay()`, no waiting on hardware — queue the work and let `loop()` do it (`focus_tick`, `system_restart_in`, async WiFi scan).

**OTA rollback:** the bootloader has app rollback on, and Arduino would mark a new image valid before `setup()`. `ota_health.cpp` overrides `verifyRollbackLater()` to hold that back. `ota_pending` is true only when the running image is `ESP_OTA_IMG_PENDING_VERIFY`; it's confirmed `OTA_CONFIRM_MS` (20 s) after `setup()` ends **and** healthy (HTTP running, STA or AP up); never healthy → `esp_ota_mark_app_invalid_rollback_and_reboot()` after `OTA_GIVE_UP_MS` (2 min). A reset before that boots the previous image. `rolled_back` = `esp_ota_get_last_invalid_partition()` exists and isn't the running one. `/ota` is refused while pending (IDF won't write the slot a rollback needs). `task ota-rollback-test` (scripts/test_ota_rollback.py) builds an image with `-DTEST_CRASH_AFTER_BOOT` in a separate build dir, OTAs it and checks the board returns to the old image — **not yet run on the board**.

**`/wifi/scan` never blocks** (it would stall the single httpd task): the first call starts an async scan and returns `{"scanning":true}`; poll until it doesn't. `?refresh=1` rescans.

**Why two servers:** an ESP-IDF httpd instance runs every handler on one task; the endless MJPEG handler would block every other route. Stream: one viewer at a time.

**Camera:**
- PSRAM must be OPI — `board_build.arduino.memory_type = qio_opi`; **do not change this**
- `fb_count = 2` in PSRAM. `CAM_GRAB_LATEST` (config.h, default 1) makes the driver hand out the newest frame and drop older ones (lag ≈ one frame; was `CAMERA_GRAB_WHEN_EMPTY`, up to fb_count−1 frames extra) — **unverified on the board**: if the stream stutters, set it to 0
- Init UXGA → run SVGA, quality 12, XCLK 20 MHz (10/16 MHz measured *more* artifacts)
- Resolution values sent by the dashboard are `framesize_t` numbers (1,5,6,10,11,12,13,15) — this esp32-camera renumbered the enum vs. older versions; `camera_valid_framesize()` whitelists them. The OV5640 delivers 240×240 at only 9 fps (1:1 mode); 320×240 at ~20 fps
- On this OV5640, `set_hmirror`/`set_vflip` **work** (verified); the dashboard uses them, not CSS
- `set_reg`/`get_reg` exist for OV5640 in this esp32-camera — always null-check them anyway
- Known artifact: dotted green/black columns (one dash per 8 rows at fixed x) in the OV5640 JPEG output. Independent of load and init resolution; worse at lower XCLK; roughly halves at quality 20. Root cause not found yet (suspect OV5640 JPEG output timing / PLL)

**Autofocus (`focus.cpp`):** firmware blob from lib_dep `ESP32-OV5640-AF` (MIT, pinned commit); only its `ESP32_OV5640_cfg.h` is used. Upload: MCU reset (0x3000=0x20) → 124-byte Wire bursts to 0x8000 (auto-increment) → spot-check readback → byte-wise fallback → release → wait for FW_STATUS 0x70. Commands via CMD_MAIN 0x3022 / CMD_ACK 0x3023: 0x08 release (infinity), 0x03 single, 0x04 continuous. FW_STATUS 0x3029: 0x70 idle, 0x10 single focused, **0x20 continuous settled** (measured), 0x00–0x0F/0x80–0x8F focusing. The esp32-camera built-in `esp_camera_af_*` API is compiled empty in this build — don't use it. Each command waits for status to settle (≤ 2 s), then for the ack (≤ 3 s); a mode that isn't acked is rolled back.

**`lib/DFRobot_AXP313A/`** — header-only PMIC shim. Only enables ALDO1 (DOVDD 1.8 V) and DLDO1 (AVDD 2.8 V). **Never write to register 0x13 (DCDC1)** — it powers an unknown critical rail at ~1.55 V and writing to it while running crashes the board.

**External libraries:** `ESP32-OV5640-AF` — AF firmware blob only. `DNSServer` (Arduino core, AsyncUDP) for the captive portal.

## Board-specific notes

- Platform pinned to **pioarduino 55.03.311** (Arduino 3.3.11 / IDF 5.5) by URL. Plain `espressif32` resolves to PlatformIO's Arduino 2.x platform, which lacks the new-driver SCCB bus sharing
- Build uses `-O2` (`build_unflags = -Os`)
- Flash is 16 MB but the board JSON says 4 MB: `board_upload.flash_size = 16MB` is required for `default_16MB.csv` (two 6.25 MB OTA slots). Verified booting and OTA-updating
- N16R8 uses OPI PSRAM — changing `board_build.arduino.memory_type` breaks camera init
- On first flash, power-cycle (not just reset) — AXP313A sometimes needs a hard reset
- If no COM port visible: hold BOOT, tap RST, release BOOT, then flash
- `pio device monitor` needs a real TTY — tell the user to run it in their own terminal
- `/dev/cu.usbmodem1101` is often held by a stale monitor; `pkill -f "pio device monitor"; pkill -f "miniterm"` before USB uploads (or use OTA)
- **Other boards may be plugged in** — e.g. the XIAO nRF54 (USB `2886:0068`, `/dev/cu.usbmodemE6A07ED63`). Check `pio device list` before a USB upload and never flash a port that isn't the FireBeetle; on 2026-10-26 the FireBeetle didn't enumerate on USB at all, so OTA (`wifi-camera.local` / its IP) is the default
- `-DARDUINO_USB_CDC_ON_BOOT=1` is mandatory — without it `Serial` maps to UART0 (GPIO 43/44)
