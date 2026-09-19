# firebeetle-camera

GripLink wrist camera firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8). Streams MJPEG video, detects AprilTags, runs OV5640 autofocus, and estimates orientation from an optional LSM6DSOX IMU — all behind a web dashboard served over the camera's own hotspot and/or your WiFi.

The camera sensor is auto-detected. This unit uses a **DC5640-AF (OV5640, voice-coil autofocus)**; the stock OV2640 also works (without focus control).

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
   After that, update over WiFi from the dashboard's **System** page (or `curl`, below). Keep the board still for ~1 s after boot if an IMU is attached (gyro calibration).

3. Join **GripLink-Cam** (password `esp32cam1`, override with `AP_PASS` in `secrets.h`) and open `http://192.168.4.1`, or the STA IP printed to serial. The dashboard asks for the API token the first time you change something. **On a board with no token** (a release binary, or no `secrets.h`) it asks you to *choose* one instead; it is stored in flash and the first visitor wins, so do it right after flashing. Then use **Network** to join your WiFi. Forget a claimed token with `POST /settings/reset?scope=token`, or `T` on the serial monitor.

## Dashboard

| Page | What's there |
|------|--------------|
| **Live** | Video with an oriented box, pose axes, ID, distance, position and quaternion on each detected tag (ID and distance only on narrow screens), detector view (the grayscale image AprilTag sees), snapshot, focus button; tag list, detection rate/timing, IMU orientation (hidden until an IMU is found) |
| **Camera** | Grouped settings — Image, Focus, Orientation, Exposure, Color, Tone. Applied instantly and **kept across reboots**; *Reset to defaults* clears them |
| **Network** | Hotspot and WiFi status with signal, scan and join (remembered) |
| **System** | Sensor/AF/IMU state, memory, firmware upload, API token, diagnostics, reboot |

The page lives in `web/index.html`; `scripts/embed_web.py` gzips it into the firmware at build time (~16 KB). No external assets, so it works on the hotspot without internet.

## Status LED (GPIO 21)

| Pattern | Meaning |
|---------|---------|
| Solid on | A tag is in view |
| Short blip every 2 s | Running, nothing detected |
| Fast blink | Booting |
| Slow blink | Joining WiFi |
| Double blink | Camera failed to start (dashboard, IMU and OTA still work) |
| Flicker | Firmware update in progress |

## WiFi

The board runs **AP+STA simultaneously**:
- Hotspot always on at `http://192.168.4.1`
- STA tries the network last chosen in the dashboard, then the one in `secrets.h` (10 s each), then stays hotspot-only
- Switching STA networks can move the hotspot to another channel, briefly dropping its clients
- On the router's network the board also answers to **`http://esp32cam.local`** (mDNS, `HOSTNAME` in `config.h`) — no need to look up its IP

## HTTP endpoints

Control server, port **80**:

| Path | Description |
|------|-------------|
| `GET /` | Dashboard |
| `POST /auth/claim` | While the board has no token (release builds), sets it: body = the token (8–64 printable characters), header `X-Claim: 1`; 400 once one is set |
| `GET /auth` 🔒 | `OK` if the token is right — the dashboard calls it before an upload |
| `GET /status` | Firmware version, uptime, sensor, AF state, IMU, detector state/timing, calibration, WiFi |
| `GET /detections` | `{"seq","t_us","now_us","age_ms","w","h","tag_m","imu_q","cam":{"calibrated","fx","fy","cx","cy","dist":[k1,k2,p1,p2,k3]},"decode_ms","detect_ms","count","tags":[{"id","corners","pose":{"tx","ty","tz","q":[w,x,y,z]}}]}` — corners as seen in the frame, in `w`×`h` pixels; `cam` = the intrinsics used, in the same pixels; `seq` = processed-frame counter (the dashboard derives the detection rate from it); `t_us` = when the camera captured the frame and `now_us` = the device clock at this reply (both µs since boot, so `now_us − t_us` is the true latency without any clock sync); `imu_q` = the IMU orientation at `t_us`, or `null` (no IMU / still calibrating); pose frames below |
| `GET /pose` | `{"q":[w,x,y,z],"imu":"none\|calibrating\|ok","n","err"}` |
| `GET /camera/status` | Current camera settings + `af`, `af_state` |
| `GET /snapshot` | One JPEG frame, always at the stream's size |
| `GET /calibration` | The camera calibration in use: `{"source":"stored\|built-in\|estimate","calibrated","width","height","fx","fy","cx","cy","dist":[k1,k2,p1,p2,k3]}` |
| `GET /debug/gray.bmp` | The last grayscale frame the tag detector processed |
| `GET /tag?id=N` | Printable tag36h11 page (black square = `TAG_SIZE_M`) |
| `GET /wifi/scan[?refresh=1]` | Nearby networks. Never blocks: answers `{"scanning":true}` while a scan runs — ask again until it doesn't (`refresh=1` starts a new one) |
| `GET /stream` | 302 redirect to `:81/stream` |
| `POST /camera/set?var=&val=` 🔒 | `framesize`, `quality` (4–63), `brightness`/`contrast`/`saturation`/`sharpness`/`ae_level` (−2…2), `awb`, `aec`, `agc`, `hmirror`, `vflip` (0/1), `wb_mode` (0–4), `af` (0 infinity, 1 focus once, 2 continuous) |
| `POST /detector/set?enabled=0\|1&res=N` 🔒 | Tag detection on/off; `res` = the `framesize` the camera drops to while nobody watches the stream (0 = same as the stream) |
| `POST /calibration` 🔒 | JSON body as above (`dist` optional); stored on the board. `DELETE /calibration` 🔒 forgets it |
| `POST /wifi/connect` 🔒 | `{"ssid","pass"}` — joins and remembers the network |
| `POST /ota` 🔒 | Raw `firmware.bin`; reboots on success |
| `POST /camera/reset` 🔒 | Forget saved camera settings and reboot |
| `POST /settings/reset?scope=` 🔒 | `all`, `camera`, `detector`, `wifi` or `calibration`: forget those saved settings and reboot |
| `POST /system/reboot` 🔒 | Reboot |

Stream server, port **81**: `GET /stream` — MJPEG, one viewer at a time.

### Pushed detections (UDP)

For a robot, polling `/detections` is the wrong shape. Send `sub <API_TOKEN>` to the board's UDP port **5005** to subscribe (repeat at least every 10 s to stay subscribed; `bye <API_TOKEN>` unsubscribes, up to 4 subscribers). Datagrams without the token are ignored, and until a token is set nothing can subscribe — so a forged source address can't aim the stream at someone else. Every processed frame then arrives at your address as one JSON datagram — the `/detections` fields without `cam`, `corners` and timings, so 8 tags fit in one packet:

```python
import socket, json
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
board = ("esp32cam.local", 5005)
hello = b"sub " + TOKEN.encode()   # API_TOKEN from secrets.h
s.sendto(hello, board)
while True:
    try:
        d = json.loads(s.recvfrom(2048)[0])
    except socket.timeout:
        s.sendto(hello, board); continue
    latency_ms = (d["now_us"] - d["t_us"]) / 1000   # capture → send
    for t in d["tags"]:
        print(t["id"], t["pose"]["tz"], latency_ms)
```

Pose and `imu_q` refer to the same instant, `t_us`: detection finishes 0.2–2 s after the capture, so fuse with `imu_q`, not with the IMU's latest orientation.

### Detection size

The stream and the detector share one sensor size, and detection speed depends on it (see the table). So there are two: the **stream size** (Camera page) applies while anyone watches the stream or takes a `/snapshot`; the **detection size** (Detection card, `res`) applies when nobody does — a headless robot can detect at 320×240 and still get an 800×600 view when you open the dashboard. Default: same as the stream. The sensor goes back to the detection size 3 s after the last viewer leaves.

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
| 1 | `imu` | 10 | 104 Hz sampling + VQF; preempts detection |
| 1 | `detect` | 2 | AprilTag, CPU-bound |
| 1 | `led` | 1 | Status LED |
| 1 | Arduino `loop()` | 1 | Serial commands |

Core 0 is reserved for radio and network I/O; core 1 is the sensing core. The camera needs no core of its own — the sensor encodes JPEG in hardware and frames arrive by DMA. Tune in `src/config.h`.

## Tags

Only **AprilTag 36h11** markers are detected (IDs 0–586). QR codes, ArUco markers and other AprilTag families (16h5, 25h9, Standard41h12, …) are ignored.

- **Get one from the board:** Live → Detection → *Printable tag*, or `http://<board>/tag?id=N`. It's generated from the same code table the detector uses and works on the hotspot without internet
- **Print at 100 % scale** on plain white paper. The black square comes out 165 mm, the size distances are computed for (`TAG_SIZE_M`); if yours measures differently, set `TAG_SIZE_M` to the measured black-square edge
- Keep white paper around the black square — at least one grid cell (⅛ of the square) on every side. The print page leaves 22–25 mm on A4 and Letter
- Keep it flat and matte; glare across the tag stops it decoding
- Use a different ID for each object you want to tell apart

## AprilTag detection

- Family **tag36h11**. While the video stream runs, the detector works on the stream's own frames, so detection doesn't lower the video frame rate
- **Rate depends on the camera resolution** (Camera → Resolution). Measured on this board with fixed test frames (a real snapshot with and without a pasted tag), frame in → pose out:

  | Resolution | Detector sees | Decode | Detect | Pose (per tag) | Processing rate | Camera delivers |
  |------------|---------------|--------|--------|----------------|-----------------|-----------------|
  | 240×240 | 240×240 | 10 ms | 40–49 ms | 13 ms | 15–20 Hz | **9 fps** |
  | 320×240 | 320×240 | 13 ms | ~60 ms | 13 ms | 11–14 Hz | 20 fps |
  | 640×480 | 320×240 | 48–58 ms | 40–60 ms | 13 ms | 8–9 Hz | ~11 fps |
  | 800×600 | 400×300 | 70–97 ms | 60–140 ms | 13 ms | 4–6.5 Hz | ~9 fps |

  The live rate is the lower of the two last columns. The OV5640's 1:1 mode makes 240×240 the slowest resolution to capture (9 fps, independent of exposure), so **320×240 is the fastest setting for detection**. Over a weak WiFi link the stream itself may send fewer frames (11 fps at 320×240 at −85 dBm), and then detection runs at the stream's rate. Detect time grows with how busy the scene is; the cap is `DETECT_PERIOD_MS` (30 Hz) in `src/config.h`. Smaller images detect tags at shorter range; range scales with the width the detector sees. In a clean synthetic test the smallest readable tag was 12 px across — about 4.4 m for a 165 mm tag at 320×240 and 5.5 m at 400×300 — real blur, noise and JPEG artefacts will cut that
- Frames are decoded straight to grayscale (Espressif's SIMD `esp_new_jpeg`, luma only) at the smallest 1/2ⁿ scale that fits 400×300 (SVGA → 400×300); AprilTag finds quads on a 2× decimated copy and reads the bits at 400×300
- apriltag is patched at build time for the ESP32-S3, whose FPU has no double precision: quad fitting, edge refinement and pose run in float (`scripts/apriltag_manifest.py`, `src/tag_pose.h`). Corners and poses match the unpatched library (unit test)
- Tags only decode in a non-mirrored image — if text in the feed reads backwards, turn on **Mirror**
- Corrects up to **1 bit error** per tag. The library default (2) needs a 36 MB decode table, more than the 8 MB PSRAM; the allocation used to fail silently and nothing was ever detected. 1 bit needs 1 MB
- A self-test at boot detects a synthetic tag; `/status` reports `detect.ready`, and the dashboard says so if it failed
- **Pose:** position `tx, ty, tz` (metres) and quaternion `q = [w, x, y, z]` of the tag in the camera frame. Camera: x right, y down, z forward. Tag: origin at its centre, x right, y down, z *into* the tag. A tag facing the camera upright is `q = [1, 0, 0, 0]`. The Live view draws the tag's X (red), Y (green) and Z (blue) axes, half a tag long
- Set `TAG_SIZE_M` in `src/detector.h` to your tag's black-square edge (default 165 mm, what `/tag` prints)

## Camera calibration

The calibration belongs to one physical camera, so it is stored **on the board** (flash, survives reboots and OTA updates) and sent over HTTP — no reflash. Until one is set, poses use an estimate (fx = fy = the width of the full 4:3 view, principal point at the centre, no distortion) and the Live page's Detection card says *Estimate*.

```bash
# calib.json: {"width":800,"height":600,"fx":..,"fy":..,"cx":..,"cy":..,"dist":[k1,k2,p1,p2,k3]}
scripts/push_calibration.py -H esp32cam.local calib.json   # token from src/secrets.h
scripts/push_calibration.py -H esp32cam.local              # show what the board has
scripts/push_calibration.py -H esp32cam.local --clear      # back to the estimate
```

- `width`×`height` is the resolution you calibrated at; the numbers are OpenCV's `K[0][0]`, `K[1][1]`, `K[0][2]`, `K[1][2]` and the first five `dist_coeffs`. The board rejects an implausible calibration (fx and fy more than 2× apart, principal point outside the image, …)
- The firmware rescales it to whatever resolution the camera runs at. 240×240 and 1280×720 are centred crops of the sensor's 4:3 view (esp32-camera's OV5640 `ratio_table`), so a calibration at a 4:3 size such as 800×600 covers every resolution
- Each tag's corners are undistorted before its pose is computed; the Live view projects the axes back through the same distortion so they sit on the image
- To calibrate: print a checkerboard, save 15–20 `/snapshot` frames at 800×600 from different angles and distances (with the Mirror/Flip settings you run with — the calibration doesn't record them), run `cv2.calibrateCamera`
- `src/calibration.h` is only a compiled-in default, used when nothing is stored (e.g. a fleet sharing one lens); leave `CALIB_FX 0` to use the estimate
- Measured with a strong wide-angle lens model on a test scene: ignoring distortion put a tag near the corner 111 mm and 10.9° off; with the calibration applied, under 8 mm and 0.5°

## Saved settings and firmware updates

Everything the board remembers (camera, detection size, WiFi, calibration) lives in flash under one versioned layout (`src/settings.h`); `POST /settings/reset` clears any part of it.

A firmware update over WiFi boots **on probation**: if the new image resets before it has been up 20 s with the HTTP servers running (a crash in `setup()`, a boot loop), the bootloader goes back to the previous image on its own. A power cycle inside those 20 s counts as a failure too, so let a fresh update run for half a minute. `/status` shows `ota_pending` while it is still on probation.

## Autofocus (OV5640)

The OV5640's focus motor is run by an MCU inside the sensor whose firmware is uploaded over I²C at every boot (~4.3 KB burst write, verified, byte-wise fallback). The blob comes from [ESP32-OV5640-AF](https://github.com/0015/ESP32-OV5640-AF) (MIT); control lives in `src/focus.cpp`. Default mode is continuous; *Single* focuses once and holds; *Infinity* releases the lens.

## IMU / orientation (optional)

- **LSM6DSOX** at I²C `0x6A` (`0x6B` in `src/imu.cpp` if SA0 is high), 104 Hz, sharing the camera's I²C bus
- Filter: **BasicVQF** (6D, no magnetometer) in `lib/VQF/basicvqf.hpp`; gyro bias is measured in the first second after boot
- Not detected at boot → the firmware runs without it and the dashboard says so

## Source layout

```
src/
  config.h            Ports, task layout, pins; includes secrets.h
  secrets.example.h   Template for the git-ignored secrets.h
  auth.h/cpp          The API token: compiled in, or claimed at runtime and kept in flash
  token_rules.h       What a valid token looks like (host-tested)
  calibration.h       Compiled-in default calibration (the board's stored one wins)
  calibration_store.h/cpp  The calibration in use, persisted in flash
  calibration_json.h  Calibration ⇄ JSON (host-tested)
  camera_model.h      Calibration → intrinsics for any resolution; undistortion
  tag_pose.h          Detector settings, self-test, pose + quaternion maths
  detection_types.h   TagDetection / DetectionFrame (capture time, IMU orientation)
  detections_json.h   Detections → JSON, full for HTTP and compact for UDP (host-tested)
  orientation_history.h  IMU orientation ring buffer + lookup by time (host-tested)
  strbuf.h            appendf / json_esc
  settings.h/cpp      NVS namespaces, schema version, reset
  camera.h/cpp        Camera init, settings + persistence, MJPEG handler
  camera_profile.h/cpp  Stream vs. detection size, viewers, the sensor lock
  focus.h/cpp         OV5640 autofocus firmware upload and control
  detector.h/cpp      AprilTag detection task and pose estimation
  imu.h/cpp           LSM6DSOX driver + VQF sampling task
  led.h/cpp           Status LED task
  push.h/cpp          UDP push of detections to subscribers
  push_message.h      Subscription datagram parsing (host-tested)
  system.h/cpp        Deferred restart
  board.h/cpp         I²C bus + AXP313A power
  wifi_mgr.h/cpp      AP+STA, saved network, mDNS, async scan
  http_server.h/cpp   Both HTTP servers: every route
  http_util.h/cpp     Auth check, body reading, JSON reply
  ota_health.h/cpp    Confirms a new OTA image once it has run 20 s (else rollback)
  main.cpp            setup(), loop(), serial CLI
  generated/          web_index.h, version.h (build output, git-ignored)
test/                 Unit tests (pio test -e native)
web/index.html        Dashboard source
scripts/              Pre-build hooks: dashboard embedding, version, apriltag manifest
.github/              CI workflow and its shared PlatformIO setup
lib/
  DFRobot_AXP313A/    AXP313A PMIC shim (V1.0 boards only)
  VQF/                BasicVQF header-only implementation
```

## Development, CI and releases

| Command | What it does | CI job |
|---------|--------------|--------|
| `task init` | First-time setup: check tools, create `src/secrets.h`, fetch toolchains and libraries | — |
| `task format` | clang-format everything | — |
| `task format-check` | Fail on unformatted sources | Format |
| `task check` | cppcheck; fails on high/medium findings | Static analysis |
| `task test` | Unit tests on your computer, with the real apriltag library — no board | Unit tests |
| `task build` | Compile the firmware | Build firmware |
| `task ci` | All four, in that order | — |

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

- **No tags detected** — check the tag is AprilTag **36h11** (print one from the dashboard), not mirrored in the feed (see Mirror), in focus, and at least ~5 % of the frame wide. The Live page's *Detector view* shows exactly what the detector sees
- **Green/black dotted columns in the video** — an OV5640 JPEG artifact (fixed columns, one dot every 8 rows). Not caused by load or the camera clock; higher JPEG quality numbers reduce it (≈50 % fewer at 20)
- **Black screen / camera init error** — power-cycle after flashing; AXP313A needs a hard reset on first boot. The LED double-blinks; dashboard, IMU and OTA keep working
- **Dashboard says "Wrong token"** — the stored token was cleared; enter the token again (System → Access) — `API_TOKEN` from `secrets.h`, or the one you chose on a release build
- **Stuck on a wrong dashboard-chosen network** — it falls back to `secrets.h` after 10 s; `task factory` erases all saved settings
- **PSRAM error** — `board_build.arduino.memory_type = qio_opi` must not be changed; N16R8 uses OPI PSRAM
- **No COM port** — hold BOOT, tap RST, release BOOT, then flash
