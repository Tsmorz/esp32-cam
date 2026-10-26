# firebeetle-camera

**Wifi-Camera** — firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8). Streams MJPEG video, runs OV5640 autofocus, and gives orientation with a compass from an optional Adafruit LSM6DSOX + LIS3MDL board — behind a web dashboard served over the camera's own WiFi and/or your home WiFi, made to be used like a webcam by someone who isn't an engineer.

The camera sensor is auto-detected. This unit uses a **DC5640-AF (OV5640, voice-coil autofocus)**; the stock OV2640 also works (without focus control).

AprilTag detection was removed on 2026-10-26 (the last version with it is on the local branch `apriltag-archive`); the IMU came back the same day, now with the magnetometer.

## Quick start

1. Create your credentials file (git-ignored):
   ```bash
   cp src/secrets.example.h src/secrets.h
   ```
   Fill in `WIFI_SSID`, `WIFI_PASS` and an `API_TOKEN` (`openssl rand -hex 12`). Without `secrets.h` the firmware still builds (this is what CI and the release binaries are): hotspot only, and the board has **no token** until you claim one — see below.

2. Flash and monitor:
   ```bash
   task flash        # or: pio run -t upload && pio device monitor
   ```
   After that, update over WiFi from the dashboard's **System** page (or `curl`, below).

3. Join the camera's WiFi, **Wifi-Camera** (password `wificamera`, override with `AP_PASS` in `secrets.h`). Phones and laptops open the setup page by themselves ("Sign in to network" — a captive portal); otherwise open `http://192.168.4.1`. Setup walks through three steps: choose a password (on a release binary, or without `secrets.h`; it's stored on the camera and the first visitor wins, so do it right after flashing), connect to your home WiFi, then where to find the camera from now on (`http://wifi-camera.local`, or its IP). The password is the API token: `API_TOKEN` from `secrets.h` sets it at build time instead. The token and network from `secrets.h` are also saved in flash on boot, so updating later with a release binary keeps both. Forget the password with Settings → Reset → Password, `POST /settings/reset?scope=token`, or `T` on the serial monitor.

## Dashboard

Everything on screen is in plain words — "password", "software update", "motion sensor", signal bars — with the technical details under Settings → *Advanced details*.

| Page | What's there |
|------|--------------|
| **Setup** | Opens by itself on a new camera, or one reached over its own WiFi that isn't on a home network yet: password → home WiFi → the address to use from now on. Settings → *Start setup* runs it again |
| **Live** | The video, as large as the window allows, with its size, focus state, the direction it faces, focus, photo and full screen. With the motion sensor: a compass, side and front tilt, and compass calibration (turn the camera in every direction; it finishes by itself) |
| **Picture** | Size and quality (presets), focus, mirror / upside down, light, color, look. Applied instantly and **kept across restarts** |
| **WiFi** | Home WiFi with signal and the addresses to open, the camera's own WiFi (on/off), find and connect to a network, forget it |
| **Settings** | Version, temperature, motion sensor; software update; password; set up again, restart, reset (picture, WiFi, compass, password, everything) |

The page lives in `web/index.html`; `scripts/embed_web.py` gzips it into the firmware at build time (~25 KB). No external assets or web fonts, so it works on the hotspot without internet. Light and dark follow the OS until the header toggle picks one (remembered in the browser).

What it does for you:
- **The stream is let go when the page isn't watching it** — another page, or the tab hidden — so a forgotten tab doesn't hold the one stream slot. A second viewer is told someone else is watching instead of waiting on a blank frame.
- **A lost connection says so**: a banner with the time of the last values, which stay on screen faded. On the hotspot it reminds you to rejoin it.
- **Firmware updates are followed to the end**: the file is checked first (an ESP32-S3 app image, not the `-factory` one), then upload → install → the 20 s probation (*don't unplug*) → *Updated* (with a reload button for the new dashboard), or *failed to start, back on the previous version*. Reboots and settings resets reconnect by themselves the same way.
- **Setup over the camera's own WiFi**: the captive portal opens the setup page; after joining a home network it gives the new address (`wifi-camera.local`, and the IP for phones that can't resolve `.local`).

## Status LED (GPIO 21)

| Pattern | Meaning |
|---------|---------|
| Short blip every 2 s | Running |
| Fast blink | Booting |
| Slow blink | Joining WiFi |
| Double blink | Camera failed to start (dashboard and OTA still work) |
| Flicker | Firmware update in progress |

## WiFi

The board runs **AP+STA simultaneously**:
- Hotspot at `http://192.168.4.1` — on at boot; once STA has been up 30 s with no device on the hotspot it turns off, and it comes back when STA has been down 15 s (`/status.wifi.ap_on`)
- STA tries the network last chosen in the dashboard, then the one in `secrets.h` (10 s each), then stays hotspot-only
- Switching STA networks can move the hotspot to another channel, briefly dropping its clients
- On the router's network the board also answers to **`http://wifi-camera.local`** (mDNS, `HOSTNAME` in `config.h`) — no need to look up its IP
- **Captive portal** while the hotspot is on: a DNS server answers every name with the camera's address, and requests for other hosts (the phones' connectivity checks) are redirected to the dashboard, so joining the hotspot opens the setup page. The DNS server only exists while the hotspot is on

## HTTP endpoints

Control server, port **80**:

| Path | Description |
|------|-------------|
| `GET /` | Dashboard |
| `POST /auth/claim` | While the board has no token (release builds), sets it: body = the token (8–64 printable characters), header `X-Claim: 1`; 400 once one is set |
| `GET /auth` 🔒 | `OK` if the token is right — the dashboard calls it before an upload |
| `GET /status` | Firmware version, uptime, `ota_pending`, sensor, AF state, temperature, memory, `imu` (`state`, `samples`, `errors`, `mag`), WiFi (incl. `ap_on`) |
| `GET /pose` | `{"q":[w,x,y,z],"imu":"none\|calibrating\|ok","n","err","mag":"none\|uncalibrated\|calibrating\|ok","heading":"mag\|gyro","cal","field_ut"}` — sensor → East-North-Up; `heading` = where yaw comes from; `cal` = compass calibration progress 0–1 |
| `GET /camera/status` | Current camera settings + `af`, `af_state`, `viewers` (open stream connections) |
| `GET /snapshot` | One JPEG frame at the stream's size |
| `GET /wifi/scan[?refresh=1]` | Nearby networks. Never blocks: answers `{"scanning":true}` while a scan runs — ask again until it doesn't (`refresh=1` starts a new one) |
| `GET /stream` | 302 redirect to `:81/stream` |
| `POST /camera/set?var=&val=` 🔒 | `framesize`, `quality` (4–63), `brightness`/`contrast`/`saturation`/`sharpness`/`ae_level` (−2…2), `awb`, `aec`, `agc`, `hmirror`, `vflip` (0/1), `wb_mode` (0–4), `af` (0 infinity, 1 focus once, 2 continuous) |
| `POST /wifi/connect` 🔒 | `{"ssid","pass"}` — joins and remembers the network |
| `POST /ota` 🔒 | Raw `firmware.bin`; reboots on success |
| `POST /camera/reset` 🔒 | Forget saved camera settings and reboot |
| `POST /imu/compass?do=` 🔒 | `start` a compass calibration, `finish` it (400 until enough rotation was seen), `cancel`, or `clear` the stored one |
| `POST /settings/reset?scope=` 🔒 | `all`, `camera`, `wifi`, `imu` (compass calibration) or `token`: forget those saved settings and reboot |
| `POST /system/reboot` 🔒 | Reboot |

Stream server, port **81**: `GET /stream` — MJPEG, one viewer at a time (the next one waits until it's free).

Any other path: 404 when addressed to the camera, a redirect to the dashboard otherwise (captive portal).

🔒 Requires header `X-Token: <API_TOKEN>`, e.g.
```bash
curl -H "X-Token: $TOKEN" -H "Content-Type: application/octet-stream" \
     --data-binary @.pio/build/firebeetle2_esp32s3/firmware.bin http://192.168.4.1/ota
```

## Tasks and cores

| Core | Task | Priority | Role |
|------|------|----------|------|
| 0 | WiFi / lwIP / esp_timer / camera DMA | 23 / 18 / 22 / SDK | SDK-pinned radio + capture |
| 0 | httpd control (:80) | 6 | Dashboard, JSON, settings, OTA |
| 0 | httpd stream (:81) | 5 | MJPEG |
| 1 | `imu` | 10 | 52 Hz sampling + VQF (only with the IMU board) |
| 1 | `led` | 1 | Status LED |
| 1 | Arduino `loop()` | 1 | Serial commands, autofocus, housekeeping |

Core 0 is reserved for radio and network I/O. The camera needs no core of its own — the sensor encodes JPEG in hardware and frames arrive by DMA. Tune in `src/config.h`.

## Saved settings and firmware updates

Everything the board remembers (camera settings, WiFi network, compass calibration, API token) lives in flash under one versioned layout (`src/settings.h`); `POST /settings/reset` clears any part of it. Schema 2 dropped the AprilTag detector's settings: a board updated from an older firmware clears them on first boot.

A firmware update over WiFi boots **on probation**: if the new image resets before it has been up 20 s with the HTTP servers running (a crash in `setup()`, a boot loop), the bootloader goes back to the previous image on its own. A power cycle inside those 20 s counts as a failure too, so let a fresh update run for half a minute. `/status` shows `ota_pending` while it is still on probation.

## Motion sensor (optional)

- **Adafruit LSM6DSOX + LIS3MDL 9-DoF board** (product 4517) on the shared I²C bus (GPIO 1/2, STEMMA QT): LSM6DSOX at `0x6A` (`0x6B`), LIS3MDL at `0x1C` (`0x1E`) — both addresses are probed. Without it the firmware runs as before and the dashboard hides the motion card
- Rates kept low on purpose (the bus is shared with the camera): accel + gyro 52 Hz (±4 g, 500 dps — a camera turned by hand exceeds 250 dps), magnetometer 40 Hz in ultra-high-performance mode (±4 gauss)
- Filter: **VQF** (`lib/VQF/basicvqf.hpp`, a port of the reference BasicVQF) — gyro + accel for tilt, the magnetometer for heading (`tauMag` 9 s, fast initial convergence). Gyro bias is measured in the first second after boot: keep the camera still then
- **Compass calibration**: the ESP32, the camera and any battery bend the field near the board. Live → Motion sensor → *Calibrate*, then turn the camera slowly in every direction; it finishes by itself once every axis has swept far enough, and the hard-iron offset and per-axis scale are stored in flash (`src/mag_cal.h`, host-tested). **Until calibrated, heading comes from the gyro and drifts** — uncalibrated, the magnetometer would be worse than the drift
- *Facing* in the dashboard is where the board's **x axis** points (clockwise from north), assuming the IMU's and the magnetometer's axes are parallel, as on the Adafruit board. Not yet verified with the real board: check that the compass turns the right way and that tilting doesn't move it

## Autofocus (OV5640)

The OV5640's focus motor is run by an MCU inside the sensor whose firmware is uploaded over I²C at every boot (~4.3 KB burst write, verified, byte-wise fallback). The blob comes from [ESP32-OV5640-AF](https://github.com/0015/ESP32-OV5640-AF) (MIT); control lives in `src/focus.cpp`. Default mode is continuous; *Single* focuses once and holds; *Infinity* releases the lens.

## Source layout

```
src/
  config.h            Ports, task layout, pins; includes secrets.h
  secrets.example.h   Template for the git-ignored secrets.h
  auth.h/cpp          The API token: compiled in, or claimed at runtime and kept in flash
  token_rules.h       What a valid token looks like (host-tested)
  imu.h/cpp           LSM6DSOX + LIS3MDL driver, VQF sampling task, compass calibration
  mag_cal.h           Compass calibration maths (host-tested)
  strbuf.h            appendf / json_esc
  settings.h/cpp      NVS namespaces, schema version, reset
  camera.h/cpp        Camera init, settings + persistence, MJPEG handler
  camera_profile.h/cpp  Stream size, viewers, the sensor lock
  focus.h/cpp         OV5640 autofocus firmware upload and control
  led.h/cpp           Status LED task
  system.h/cpp        Deferred restart
  board.h/cpp         I²C bus + AXP313A power
  wifi_mgr.h/cpp      AP+STA, saved network, mDNS, async scan, captive-portal DNS
  http_server.h/cpp   Both HTTP servers: every route
  http_util.h/cpp     Auth check, body reading, JSON reply
  ota_health.h/cpp    Confirms a new OTA image once it has run 20 s (else rollback)
  main.cpp            setup(), loop(), serial CLI
  generated/          web_index.h, version.h (build output, git-ignored)
test/                 Unit tests (pio test -e native)
web/index.html        Dashboard source
scripts/              Pre-build hooks (dashboard embedding, version), smoke and rollback tests
.github/              CI workflow and its shared PlatformIO setup
lib/
  DFRobot_AXP313A/    AXP313A PMIC shim (V1.0 boards only)
  VQF/                BasicVQF with magnetometer, header-only (host-tested)
```

## Development, CI and releases

| Command | What it does | CI job |
|---------|--------------|--------|
| `task init` | First-time setup: check tools, create `src/secrets.h`, fetch toolchains and libraries | — |
| `task format` | clang-format everything | — |
| `task format-check` | Fail on unformatted sources | Format |
| `task check` | cppcheck; fails on high/medium findings | Static analysis |
| `task test` | Unit tests on your computer (token rules, VQF with magnetometer, compass calibration) — no board | Unit tests |
| `task build` | Compile the firmware | Build firmware |
| `task ci` | All four, in that order | — |
| `task smoke` | Check the running board over the network: routes, token, POST-only, no CORS, snapshot (`-- -H <ip> --token <t>` for options) | — |
| `task ota-rollback-test` | OTA a deliberately crashing image and check the board rolls back to the old one | — |

`.github/workflows/ci.yml` runs Format, Static analysis, Unit tests and Build in parallel on every push and pull request, each by calling the same `task` command. Build uploads `firmware.bin` and `firmware.factory.bin` as a workflow artifact and writes the RAM/flash usage to the run summary.

**Releasing:** push a version tag.

```bash
git tag v1.1.0 && git push origin v1.1.0
```

When all four jobs pass, the Release job publishes a GitHub release with notes generated from the commits and three assets:

- `<repo>-v1.1.0.bin` — for the dashboard's firmware upload or `curl … /ota`
- `<repo>-v1.1.0-factory.bin` — bootloader, partition table and app in one, for a blank board: `esptool.py --chip esp32s3 write_flash 0x0 <file>`
- `SHA256SUMS`

Tags with a hyphen (`v1.1.0-rc1`) become pre-releases. The firmware reports its version (`git describe`, e.g. `v1.1.0` or `v1.1.0-3-gabc1234-dirty`) in `/status` and on the System page.

## V1.0 vs V1.1

| Version | AXP313A |
|---------|---------|
| **V1.0** (this board) | Required — shim in `lib/DFRobot_AXP313A/` handles it |
| V1.1+ | Absent — detected after 3 probes and skipped; boot continues |

## Troubleshooting

- **Green/black dotted columns in the video** — an OV5640 JPEG artifact (fixed columns, one dot every 8 rows). Not caused by load; worse at a lower camera clock (10/16 MHz than 20 MHz); higher JPEG quality numbers reduce it (≈50 % fewer at 20). Root cause not found yet
- **Black screen / camera init error** — power-cycle after flashing; AXP313A needs a hard reset on first boot. The LED double-blinks; dashboard, IMU and OTA keep working
- **Compass direction is wrong or wanders** — calibrate it again (Live → Motion sensor), away from metal and magnets; it's only used once calibrated
- **Dashboard says "Wrong token"** — the stored token was cleared; enter the token again (System → Access) — `API_TOKEN` from `secrets.h`, or the one you chose on a release build
- **Stuck on a wrong dashboard-chosen network** — it falls back to `secrets.h` after 10 s; `task factory` erases all saved settings
- **PSRAM error** — `board_build.arduino.memory_type = qio_opi` must not be changed; N16R8 uses OPI PSRAM
- **No COM port** — hold BOOT, tap RST, release BOOT, then flash
