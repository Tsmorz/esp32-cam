# firebeetle-camera

**Wifi-Camera** — firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8). Streams MJPEG video, runs OV5640 autofocus, and gives orientation with a compass from an optional Adafruit LSM6DSOX + LIS3MDL board — behind a web dashboard served over the camera's own WiFi and/or your home WiFi, made to be used like a webcam by someone who isn't an engineer.

The camera sensor is auto-detected. This unit uses a **DC5640-AF (OV5640, voice-coil autofocus)**; the stock OV2640 also works (without focus control).

AprilTag detection was removed on 2026-10-26 (the last version with it is on the local branch `apriltag-archive`); the IMU came back the same day, now with the magnetometer.

## Quick start

1. Create your credentials file (git-ignored):
   ```bash
   cp src/secrets.example.h src/secrets.h
   ```
   Everything in it is optional and commented out: uncomment `WIFI_SSID`/`WIFI_PASS` and an `API_TOKEN` (8–63 printable characters, checked at compile time; `openssl rand -hex 12`) if you want them built in. Without them (this is what CI and the release binaries are) the camera starts with its hotspot only, and **no token** until you claim one — see below.

2. Flash and monitor:
   ```bash
   task flash        # or: pio run -t upload && pio device monitor
   ```
   After that, update over WiFi from the dashboard's **System** page (or `curl`, below).

3. Join the camera's WiFi, **Wifi-Camera** (password `wificamera` until a camera password is set — see below). Phones and laptops open the setup page by themselves ("Sign in to network" — a captive portal); otherwise open `http://192.168.4.1`. Setup walks through three steps: choose a password (on a release binary, or without `secrets.h`; it's stored on the camera and the first visitor wins, so do it right after flashing), connect to your home WiFi, then where to find the camera from now on (`http://wifi-camera.local`, or its IP). The password is the API token: `API_TOKEN` from `secrets.h` sets it at build time instead. The token and network from `secrets.h` are also saved in flash on boot, so updating later with a release binary keeps both.

   **The hotspot is for setup only**: settings work over it, but the video, snapshots and motion readings are only served on the home network, and only with the password. Once a password is set, the hotspot's own WiFi password becomes the camera password from its next start (unless `AP_PASS` in `secrets.h` fixes it).

   **Forgot the password?** While the camera is running, hold its **BOOT** button for 5 seconds until the LED blinks fast, then let go: it restarts without a password (and the hotspot goes back to `wificamera`), and the next visitor chooses a new one. Also: Settings → Reset → Password (needs the current one), `POST /settings/reset?scope=token`, or `T` on the serial monitor. A password compiled in from `secrets.h` can't be forgotten this way.

## Dashboard

Everything on screen is in plain words — "password", "software update", "motion sensor", signal bars — with the technical details under Settings → *Advanced details*.

| Page | What's there |
|------|--------------|
| **Setup** | Opens by itself on a new camera, or one reached over its own WiFi that isn't on a home network yet: password → home WiFi → the address to use from now on. Settings → *Start setup* runs it again |
| **Live** | The video, as large as the window allows, with its size, focus state, the direction it faces, focus, photo (to this device), save and record (to the memory card), full screen, and what the animal model sees (names, or marks where). With the motion sensor: a compass, side and front tilt, and compass calibration (turn the camera in every direction; it finishes by itself) |
| **Picture** | Size and quality (presets), focus, turn (90° steps), mirror, straighten a wall seen at an angle, light, color, look. Applied instantly and **kept across restarts** |
| **Photos** | What's on the memory card: a gallery (clips play in the page), download, delete; take a photo or record; automatic photos and clips — timelapse, when the camera is moved, when an animal is seen |
| **WiFi** | Home WiFi with signal and the addresses to open, the camera's own WiFi (on/off), find and connect to a network, forget it |
| **Settings** | Version, temperature, battery, motion sensor; animal detection (model upload, on/off, how sure, which animals); sleep; software update; password; set up again, restart, reset (picture, WiFi, compass, add-on settings, password, everything). *Advanced details* has the add-ons' wiring (which GPIO each wire goes to) |

The page lives in `web/index.html`; `scripts/embed_web.py` gzips it into the firmware at build time (~25 KB). No external assets or web fonts, so it works on the hotspot without internet. Light and dark follow the OS until the header toggle picks one (remembered in the browser).

What it does for you:
- **The stream is let go when the page isn't watching it** — another page, or the tab hidden — so a forgotten tab doesn't hold the one stream slot. A second viewer is told someone else is watching instead of waiting on a blank frame.
- **A lost connection says so**: a banner with the time of the last values, which stay on screen faded. On the hotspot it reminds you to rejoin it.
- **Firmware updates are followed to the end**: the file is checked first (an ESP32-S3 app image, not the `-factory` one), then upload → install → the 20 s probation (*don't unplug*) → *Updated* (with a reload button for the new dashboard), *failed to start, back on the previous version* (from `/status.rolled_back`), or *still on version X* (the same file again). Reboots and settings resets reconnect by themselves the same way.
- **Setup over the camera's own WiFi**: the captive portal opens the setup page; after joining a home network it gives the new address (`wifi-camera.local`, and the IP for phones that can't resolve `.local`).

## Status LED (GPIO 21)

| Pattern | Meaning |
|---------|---------|
| On | Running |
| Fast blink | Booting |
| Slow blink | Joining WiFi |
| Double blink | Camera failed to start (dashboard and OTA still work) |
| Flicker | Firmware update in progress |

## WiFi

The board runs **AP+STA simultaneously**:
- Hotspot at `http://192.168.4.1` — on at boot; once STA has been up 30 s with no device on the hotspot it turns off, and it comes back when STA has been down 15 s (`/status.wifi.ap_on`)
- STA tries the network last chosen in the dashboard, then the one in `secrets.h` (10 s each), then stays hotspot-only
- Joining a network from the dashboard only replaces the saved one once it worked: a wrong password or a network out of reach sends the camera back to the network it had after 30 s (`/status.wifi.joining` while it tries)
- Switching STA networks can move the hotspot to another channel, briefly dropping its clients. While a device is on the hotspot and the home network is gone, STA retries once a minute instead of continuously, since each attempt scans every channel and knocks hotspot clients off
- On the router's network the board also answers to **`http://wifi-camera.local`** (mDNS, `HOSTNAME` in `config.h`) — no need to look up its IP
- **Captive portal** while the hotspot is on: a DNS server answers every name with the camera's address, and requests for other hosts (the phones' connectivity checks) are redirected to the dashboard, so joining the hotspot opens the setup page. The DNS server only exists while the hotspot is on
- **Host check**: every route answers only when the `Host` header is the camera (one of its addresses, `wifi-camera.local`, or none); anything else gets the same redirect. That covers the captive portal and shuts out DNS rebinding (a web page whose own name is pointed at the camera)

## HTTP endpoints

Control server, port **80**:

| Path | Description |
|------|-------------|
| `GET /` | Dashboard |
| `POST /auth/claim` | While the board has no token (release builds), sets it: body = the token (8–63 printable characters, no spaces), header `X-Claim: 1`; 400 once one is set. Also sets the video cookie |
| `GET /auth` 🔒 | `OK` if the token is right, and sets the **video cookie** (`wc_video`, HttpOnly, a hash of the token) that `/stream`, `/snapshot` and `/pose` accept in place of `X-Token` — an `<img>` can't send headers |
| `GET /auth/forget` | Drops the video cookie (the dashboard's *Forget*) |
| `GET /status` | Firmware version, `build` (build time — differs between any two builds), uptime, `ota_pending` + `ota_confirm_s`, `rolled_back` (the last update failed to start), `hotspot` (this request came over it), `secrets` (`chip`/`firmware`, see below), sensor, AF state, temperature, memory, `imu` (`state`, `samples`, `errors`, `mag`), WiFi (incl. `ap_on`, `ap_key` = `default`/`camera`/`custom`, `joining`) |
| `GET /pose` 🎥 | `{"q":[w,x,y,z],"imu":"none\|calibrating\|ok","n","err","mag":"none\|uncalibrated\|calibrating\|ok","heading":"mag\|gyro","cal","field_ut"}` — sensor → East-North-Up; `heading` = where yaw comes from; `cal` = compass calibration progress 0–1 |
| `GET /camera/status` | Current camera settings + `af`, `af_state`, `viewers` (open stream connections) |
| `GET /snapshot` 🎥 | One JPEG frame at the stream's size |
| `GET /wifi/scan[?refresh=1]` | Nearby networks. Never blocks: answers `{"scanning":true}` while a scan runs — ask again until it doesn't (`refresh=1` starts a new one) |
| `GET /stream` | 302 redirect to `:81/stream` |
| `POST /camera/set?var=&val=` 🔒 | `framesize`, `quality` (4–63), `brightness`/`contrast`/`saturation`/`sharpness`/`ae_level` (−2…2), `awb`, `aec`, `agc`, `hmirror` (0/1), `rotate` (quarter turns clockwise, 0–3), `quad` (straightening: `x0,y0,…,x3,y3` in thousandths of the picture as shown, clockwise from top left; empty = off), `wb_mode` (0–4), `af` (0 infinity, 1 focus once, 2 continuous) |
| `POST /wifi/connect` 🔒 | `{"ssid","pass"}` — joins the network, and remembers it once it worked |
| `POST /wifi/hostname?name=` 🔒 | Rename the camera: `http://<name>.local` (1–32 lowercase letters, numbers and dashes) |
| `POST /ota` 🔒 | Raw `firmware.bin`; reboots on success. Refused while the last update is still on probation (`ota_pending`) |
| `POST /camera/reset` 🔒 | Forget saved camera settings and reboot |
| `POST /imu/compass?do=` 🔒 | `start` a compass calibration, `finish` it (400 until enough rotation was seen), `cancel`, or `clear` the stored one |
| `POST /settings/reset?scope=` 🔒 | `all`, `camera`, `wifi`, `imu` (compass calibration), `options` (add-ons, automation, sleep, detection) or `token`: forget those saved settings and reboot |
| `POST /system/reboot` 🔒 | Reboot |
| `POST /system/sleep` 🔒 | Deep sleep now (only with the motion sensor's wake wire set) |
| `GET /options` | Wiring, automation, sleep and detection settings, plus the GPIOs offered for wiring |
| `POST /options/set?key=&val=` 🔒 | Change one (`val` URL-encoded); `{"restart":true}` for wiring, which applies after a restart |
| `GET /files[?offset=&limit=]` 🎥 | The memory card's photos and clips, newest first: `{"total","files":[{"name","size","time"}]}` |
| `GET /files/get?name=[&dl=1]` 🎥 | One file (JPEG or QuickTime); `dl=1` = as a download. Sent by a worker task, so other routes keep answering |
| `POST /files/delete?name=` 🔒 | Delete one, or `name=all` |
| `POST /capture/photo` 🔒 | Save a photo on the card |
| `POST /capture/record?do=start\|stop` 🔒 | Record a clip (stops by itself after `rec_max_min`) |
| `GET /detect` 🎥 | Model, input, speed, labels and the latest results (`label`, `score`, `x`/`y` 0–1 or −1 for the whole picture) |
| `POST /model?name=&model_len=&labels_len=&range=` 🔒 | Upload a model: body = the `.tflite`, then the labels text. `range`: 0 = pixels 0..1, 1 = −1..1, 2 = 0..255 |
| `POST /model/forget` 🔒 | Remove the model |

Stream server, port **81**: `GET /stream` 🎥 — MJPEG, one viewer at a time (the next one waits until it's free).

Any other path: 404 when addressed to the camera, a redirect to the dashboard otherwise (captive portal).

🎥 Home network only (403 over the hotspot), and the password: `X-Token` or the video cookie (401 otherwise).

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
| 0 | `files` | 5 | Sends gallery files (async httpd requests) |
| 1 | `imu` | 10 | 52 Hz sampling + VQF, motion detection (only with the IMU board) |
| 1 | `rec` | 3 | Memory card: photos, clips, timelapse, triggers (only with a card wired) |
| 1 | `detect` | 1 | Animal detection, ~once a second (takes turns with `loop()`) |
| 1 | `led` | 1 | Status LED |
| 1 | Arduino `loop()` | 1 | Serial commands, autofocus, housekeeping |

Core 0 is reserved for radio and network I/O. The camera needs no core of its own — the sensor encodes JPEG in hardware and frames arrive by DMA. Tune in `src/config.h`.

## Saved settings and firmware updates

Everything the board remembers (camera settings, WiFi network, compass calibration, API token) lives in flash under one versioned layout (`src/settings.h`); `POST /settings/reset` clears any part of it. Schema 2 dropped the AprilTag detector's settings; schema 3 encrypts the passwords (below). A board updated from an older firmware migrates on first boot.

**Passwords are encrypted at rest** (`src/secret_store.cpp`): the API token and the home WiFi password are AES-256-GCM blobs in NVS. The key comes from the chip's HMAC peripheral: on first boot the firmware burns a random 256-bit key into a free **eFuse key block** (purpose `HMAC_UP`, read-protected — software can use it but never read it back). This is **permanent** and uses one of the chip's six key blocks; a dump of the flash alone no longer reveals the passwords. If no block is free, the key is derived from the MAC address instead, which only obscures them (`/status.secrets` = `firmware`, Settings → Advanced details). IDF's built-in NVS encryption isn't used: Arduino mounts NVS unencrypted before `setup()`, and a new partition table can't be installed over the air. On the first boot of schema 3 the plain-text passwords are encrypted and the NVS partition is rewritten (erase + copy back), because NVS leaves deleted values in flash; the WiFi driver's own store (`nvs.net80211`) is dropped too. A power cut during that one-time step loses the saved settings. Passwords compiled in from `secrets.h` are still in the firmware image itself.

A firmware update over WiFi boots **on probation**: it is confirmed 20 s after `setup()` finished, once the HTTP servers run and the camera is on a network (home WiFi or its hotspot). If it resets before that (a crash, a boot loop), the bootloader goes back to the previous image on its own; if it never gets healthy, it rolls itself back after 2 minutes. A power cycle during probation counts as a failure too, so let a fresh update run for half a minute. `/status` shows `ota_pending` (and `ota_confirm_s`) while on probation, and `rolled_back` after a failed one.

## Memory card (optional)

Photos and clips go to a memory card on SPI — e.g. **Adafruit's SPI Flash SD Card (XTSD 512 MB)**, SD-NAND that behaves like a microSD card. Wire SCK, MISO, MOSI and CS to free GPIOs (9–18, 38, 47; check which your board brings out) plus 3V and GND, then pick those pins under Settings → *Advanced details* and restart. A new card is formatted (FAT) by itself.

- Files: `/WCAM/IMG_00001.JPG` and `/WCAM/VID_00002.MOV`, one numbering. Times come from the internet clock once on the home network (SNTP, UTC)
- Clips are **QuickTime Motion-JPEG** (`src/mov_writer.h`, host-tested): they play in QuickTime on a Mac or iPhone and in VLC, and the dashboard plays them by itself. 512 MB holds roughly 10,000 photos at 800×600, or ~50 min of 320×240 video
- Automatic: a **timelapse** (every 1–60 min), a photo or clip **when the camera is moved**, or **when an animal is seen**. After an automatic one, triggers wait 30 s
- While recording, the stream gets about half the frames (both take them from the camera). Not encrypted (the user's choice); photos are private through the web server only

## Animal detection (optional)

Upload a **TensorFlow Lite** model (`.tflite`) and its labels (`.txt`, one per line) under Settings → *Animal detection* — for example one trained and exported in Edge Impulse. It runs on the camera with TensorFlow Lite Micro and Espressif's esp-nn kernels; nothing leaves the board.

- Two kinds: a classifier (output `[N]`: what the whole picture shows) or a **FOMO** grid (output `[gh][gw][N]`, class 0 = background: also where — marks on the live video). int8, uint8 or float; gray or RGB input, any size
- Stored in the flash partition the firmware doesn't otherwise use (`spiffs`, up to 3.3 MB; header written last, so a cut-off upload leaves no model), copied to PSRAM, working memory found by trial (256 KB – 3 MB)
- Each look: the newest frame decoded at 1/2–1/8 size, centre-cropped to the model's shape, quantized (`src/detect_post.h`, host-tested). *Picture values* must match training: 0..1 (Edge Impulse), −1..1 (MobileNet) or 0..255
- Labels such as `background`, `none`, `unknown` or `no …` mean "nothing seen": never shown, never a trigger. *Only these animals* limits triggers to the names listed
- Measured on this board with TensorFlow's 96×96 person-detection model (a stand-in of the same size class): 133 ms per look including the JPEG decode

## Battery level (optional)

The board charges a Li-ion cell on its BAT connector but can't measure it (the pin some FireBeetles use for that is the camera's VSYNC here). Two equal resistors (e.g. 100 kΩ) from the battery's + to GPIO 9 or 10 and from there to GND, chosen under Settings → *Advanced details* → *Battery sense*, show the charge at the top of every page and in `/status.battery` — accurate to about 10 %; it can't tell when it's charging.

## Sleep (optional)

With the motion sensor and a wire from its **INT1** pin to an RTC-capable GPIO (9–18, set under *Advanced details*), the camera can **deep sleep** after 5–60 idle minutes (Settings → *Sleep*), or at once (*Sleep now*). Moving it wakes it: the LSM6DSOX stays on in low-power mode (26 Hz, gyro and compass off) and raises INT1 above the chosen movement threshold. Waking is a full start (~5 s to video). Before sleeping, the camera's power rails are switched off through the AXP313A.

- It stays awake while someone watches, records, is on the hotspot, or during an update's probation. A dashboard tab in the background doesn't keep it awake
- With a timelapse on, a timer wakes it for each photo, which it takes without WiFi before sleeping again
- The gyro bias is kept across sleep (RTC memory): after a wake by motion the board isn't still
- Not possible without the wake wire: nothing else could wake it (BOOT/RST and a power cycle always work)

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
  secret_store.h/cpp  Passwords encrypted at rest (eFuse HMAC key, AES-GCM)
  build_info.h/cpp    Version + build time, the one file rebuilt every build
  imu.h/cpp           LSM6DSOX + LIS3MDL driver, VQF sampling task, compass calibration, motion + wake-up
  options.h/cpp       Wiring, automation, sleep and detection settings (dashboard-editable)
  storage.h/cpp       Memory card on SPI: files, gallery index
  recorder.h/cpp      Photos, clips, timelapse, triggers (own task)
  mov_writer.h        QuickTime Motion-JPEG writer (host-tested)
  detect.h/cpp        Model storage + TensorFlow Lite Micro task
  detect_post.h       Picture → model input, output → detections (host-tested)
  power.h/cpp         Deep sleep and wake
  mag_cal.h           Compass calibration maths (host-tested)
  strbuf.h            appendf / json_esc / json_str / url_decode (host-tested)
  settings.h/cpp      NVS namespaces, schema version, reset
  camera.h/cpp        Camera init, settings + persistence, MJPEG handler
  camera_profile.h/cpp  Stream size, viewers, the sensor lock
  focus.h/cpp         OV5640 autofocus firmware upload and control
  led.h/cpp           Status LED task
  system.h/cpp        Deferred restart or sleep
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
| `task test` | Unit tests on your computer (token rules, JSON helpers, VQF with magnetometer, compass calibration, clip writer, detection maths) — no board | Unit tests |
| `task build` | Compile the firmware | Build firmware |
| `task ci` | All four, in that order | — |
| `task smoke` | Check the running board over the network: routes, token, POST-only, no CORS, Host check, video access rules, snapshot (`-- -H <ip> --token <t>` for options) | — |
| `task ota-rollback-test` | OTA a deliberately crashing image and check the board rolls back to the old one | — |

`.github/workflows/ci.yml` runs Format, Static analysis, Unit tests and Build in parallel on every push and pull request, each by calling the same `task` command. Build uploads `firmware.bin` and `firmware.factory.bin` as a workflow artifact and writes the RAM/flash usage to the run summary.

**Releasing is automatic:** every merge (or push) to `main` gets the next version, once all four checks pass — the Tag job creates the tag and the Release job publishes it in the same run. By default the patch number goes up (`v1.4.0` → `v1.4.1`); put **`#minor`** or **`#major`** in the pull request's title (it lands in the merge commit) or in any commit message since the last release to bump that instead (`v1.5.0`, `v2.0.0`). `scripts/next_version.sh` decides; try it locally:

```bash
GITHUB_REF=refs/heads/main GITHUB_EVENT_NAME=push scripts/next_version.sh   # tag=v1.4.1
```

The firmware is built with that version already (`FW_VERSION`), so the released binary reports `v1.4.1`. A commit on `main` that already carries a `v*` tag isn't tagged again.

**Development builds:** every push to `development` (after the same checks) updates one rolling pre-release, **`dev`**: its tag moves to the new commit and its files are replaced, so the links stay the same — handy to share a test build:

- `…/releases/download/dev/<repo>-dev.bin` (update file) and `<repo>-dev-factory.bin`, plus `SHA256SUMS`
- the firmware reports the version it leads up to, e.g. `v1.4.1-dev.3+3da6968` (3 commits since `v1.4.0`, at commit `3da6968`)
- it is never marked as the latest release, and release notes of real releases start at the previous real one (`--notes-start-tag`), not at `dev` To release by hand — a pre-release, or from another branch — push a tag as before:

```bash
git tag v1.5.0-rc1 && git push origin v1.5.0-rc1
```

The Release job publishes a GitHub release with notes generated from the commits and three assets:

- `<repo>-v1.1.0.bin` — for the dashboard's firmware upload or `curl … /ota`
- `<repo>-v1.1.0-factory.bin` — bootloader, partition table and app in one, for a blank board: `esptool.py --chip esp32s3 write_flash 0x0 <file>`
- `SHA256SUMS`

Tags with a hyphen (`v1.1.0-rc1`) become pre-releases. The firmware reports its version (`git describe`, e.g. `v1.1.0` or `v1.1.0-3-gabc1234-dirty`, or the one CI set) in `/status` and on the Settings page.

## V1.0 vs V1.1

| Version | AXP313A |
|---------|---------|
| **V1.0** (this board) | Required — shim in `lib/DFRobot_AXP313A/` handles it |
| V1.1+ | Absent — detected after 3 probes and skipped; boot continues |

## Troubleshooting

- **Green/black dotted columns in the video** — an OV5640 JPEG artifact (fixed columns, one dot every 8 rows). Not caused by load; worse at a lower camera clock (10/16 MHz than 20 MHz); higher JPEG quality numbers reduce it (≈50 % fewer at 20). Root cause not found yet
- **Black screen / camera init error** — power-cycle after flashing; AXP313A needs a hard reset on first boot. The LED double-blinks; dashboard, IMU and OTA keep working
- **Compass direction is wrong or wanders** — calibrate it again (Live → Motion sensor), away from metal and magnets; it's only used once calibrated
- **Dashboard asks for the password** — enter it again (Settings → Password) — `API_TOKEN` from `secrets.h`, or the one you chose. Forgotten: hold BOOT 5 s (see setup)
- **No video on the camera's own WiFi** — by design: the hotspot is for setup. Watch on the home network
- **Joined a wrong network** — the camera goes back to the one it had after 30 s; `task factory` erases all saved settings
- **PSRAM error** — `board_build.arduino.memory_type = qio_opi` must not be changed; N16R8 uses OPI PSRAM
- **No COM port** — hold BOOT, tap RST, release BOOT, then flash
