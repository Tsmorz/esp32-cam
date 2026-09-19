# firebeetle-camera

GripLink wrist camera firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** (N16R8). Streams MJPEG video, detects AprilTags, runs OV5640 autofocus, and estimates orientation from an optional LSM6DSOX IMU — all behind a web dashboard served over the camera's own hotspot and/or your WiFi.

The camera sensor is auto-detected. This unit uses a **DC5640-AF (OV5640, voice-coil autofocus)**; the stock OV2640 also works (without focus control).

## Quick start

1. Create your credentials file (git-ignored):
   ```bash
   cp src/secrets.example.h src/secrets.h
   ```
   Fill in `WIFI_SSID`, `WIFI_PASS` and an `API_TOKEN` (`openssl rand -hex 12`). Without `secrets.h` the firmware still builds: hotspot only, with settings/OTA/WiFi changes disabled.

2. Flash and monitor:
   ```bash
   task flash        # or: pio run -t upload && pio device monitor
   ```
   After that, update over WiFi from the dashboard's **System** page (or `curl`, below). Keep the board still for ~1 s after boot if an IMU is attached (gyro calibration).

3. Join **GripLink-Cam** (password `griplink1`, override with `AP_PASS` in `secrets.h`) and open `http://192.168.4.1`, or the STA IP printed to serial. The dashboard asks for the API token the first time you change something.

## Dashboard

| Page | What's there |
|------|--------------|
| **Live** | Video with an oriented box, pose axes, ID, distance, position and quaternion on each detected tag (ID and distance only on narrow screens), detector view (the grayscale image AprilTag sees), snapshot, focus button; tag list, detection rate/timing, IMU orientation (hidden until an IMU is found) |
| **Camera** | Grouped settings — Image, Focus, Orientation, Exposure, Color, Tone. Applied instantly and **kept across reboots**; *Reset to defaults* clears them |
| **Network** | Hotspot and WiFi status with signal, scan and join (remembered) |
| **System** | Battery, sensor/AF/IMU state, memory, firmware upload, API token, diagnostics, reboot |

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

## Battery

The FireBeetle 2 ESP32-S3 doesn't route the battery to any GPIO, so the gauge needs two resistors:

```
BAT+ ── 100 kΩ ──┬── 100 kΩ ── GND
                 ├── 100 nF ── GND
               GPIO 10
```

Then set `#define BAT_PIN 10` in `src/config.h`. The dashboard shows percent (LiPo discharge curve) and voltage; until then it shows "Not wired". The pin must be ADC1 (GPIO 1–10) — ADC2 pins read nothing while WiFi is on.

## WiFi

The board runs **AP+STA simultaneously**:
- Hotspot always on at `http://192.168.4.1`
- STA tries the network last chosen in the dashboard, then the one in `secrets.h` (10 s each), then stays hotspot-only
- Switching STA networks can move the hotspot to another channel, briefly dropping its clients

## HTTP endpoints

Control server, port **80**:

| Path | Description |
|------|-------------|
| `GET /` | Dashboard |
| `GET /status` | Uptime, sensor, AF state, IMU, detector state/timing, WiFi, battery |
| `GET /detections` | `{"age_ms","w","h","f","tag_m","decode_ms","detect_ms","count","tags":[{"id","corners","pose":{"tx","ty","tz","q":[w,x,y,z]}}]}` — corners in `w`×`h` pixels, `f` = focal length in those pixels (principal point at the centre); pose frames below |
| `GET /pose` | `{"q":[w,x,y,z],"imu":"none\|calibrating\|ok","n","err"}` |
| `GET /battery` | `{"voltage","percent"}`; `-1` when no battery pin is configured |
| `GET /camera/status` | Current camera settings + `af`, `af_state` |
| `GET /snapshot` | One JPEG frame |
| `GET /debug/gray.bmp` | The last grayscale frame the tag detector processed |
| `GET /tag?id=N` | Printable tag36h11 page (black square = `TAG_SIZE_M`) |
| `GET /wifi/scan` | Nearby networks (blocking, ~2 s) |
| `GET /stream` | 302 redirect to `:81/stream` |
| `GET /camera/set?var=&val=` 🔒 | `framesize`, `quality` (4–63), `brightness`/`contrast`/`saturation`/`sharpness`/`ae_level` (−2…2), `awb`, `aec`, `agc`, `hmirror`, `vflip` (0/1), `wb_mode` (0–4), `af` (0 infinity, 1 focus once, 2 continuous) |
| `GET /detector/set?enabled=0\|1` 🔒 | Tag detection on/off |
| `POST /wifi/connect` 🔒 | `{"ssid","pass"}` — joins and remembers the network |
| `POST /ota` 🔒 | Raw `firmware.bin`; reboots on success |
| `POST /camera/reset` 🔒 | Forget saved camera settings and reboot |
| `POST /system/reboot` 🔒 | Reboot |

Stream server, port **81**: `GET /stream` — MJPEG, one viewer at a time.

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

- Family **tag36h11**. Each detection takes one frame away from the stream
- **Rate depends on the camera resolution** (Camera → Resolution), because every frame is a JPEG that has to be decoded first, and decode time grows with the JPEG's size even when it's decoded at reduced scale. Measured on this board (240 MHz, one core, window scene):

  | Resolution | Detector sees | Decode | Detect | Rate |
  |------------|---------------|--------|--------|------|
  | 160×120 | 160×120 | 19 ms | 24 ms | 5 Hz (capped) |
  | 240×240 | 240×240 | 54 ms | 51 ms | 5 Hz (capped) |
  | 320×240 | 320×240 | 72 ms | 65 ms | 5 Hz (capped) |
  | 640×480 | 320×240 | 252 ms | 80 ms | 3.1 Hz |
  | 800×600 | 400×300 | 398 ms | 231 ms | 1.6 Hz |
  | 1280×720 | 320×180 | 649 ms | 80 ms | 1.5 Hz |
  | 1600×1200 | 400×300 | 1514 ms | 509 ms | 0.4 Hz |

  The 5 Hz cap is `DETECT_PERIOD_MS` in `src/config.h`. Detect time varies with how busy the scene is. Smaller images detect tags at shorter range; range scales with the width the detector sees. In a clean synthetic test the smallest readable tag was 12 px across — about 4.4 m for a 165 mm tag at 320×240 and 5.5 m at 400×300 — real blur, noise and JPEG artefacts will cut that
- Frames are decoded at the smallest JPEG scale that fits 400×300 (SVGA → 400×300); AprilTag finds quads on a 2× decimated copy and reads the bits at 400×300
- Tags only decode in a non-mirrored image — if text in the feed reads backwards, turn on **Mirror**
- Corrects up to **1 bit error** per tag. The library default (2) needs a 36 MB decode table, more than the 8 MB PSRAM; the allocation used to fail silently and nothing was ever detected. 1 bit needs 1 MB
- A self-test at boot detects a synthetic tag; `/status` reports `detect.ready`, and the dashboard says so if it failed
- **Pose:** position `tx, ty, tz` (metres) and quaternion `q = [w, x, y, z]` of the tag in the camera frame. Camera: x right, y down, z forward. Tag: origin at its centre, x right, y down, z *into* the tag. A tag facing the camera upright is `q = [1, 0, 0, 0]`. The Live view draws the tag's X (red), Y (green) and Z (blue) axes, half a tag long
- Pose uses approximate intrinsics: fx ≈ the width of the full 4:3 view (800 px at SVGA). Square and 5:4 sizes are crops of that view, so 240×240 uses f = 320. Set `TAG_SIZE_M` in `src/detector.h` to your tag's black-square edge (default 165 mm, what `/tag` prints) and calibrate `FOCAL_PER_WIDTH` in `src/detector.cpp` for accurate distances

## Autofocus (OV5640)

The OV5640's focus motor is run by an MCU inside the sensor whose firmware is uploaded over I²C at every boot (~4.3 KB burst write, verified, byte-wise fallback). The blob comes from [ESP32-OV5640-AF](https://github.com/0015/ESP32-OV5640-AF) (MIT); control lives in `src/focus.cpp`. Default mode is continuous; *Single* focuses once and holds; *Infinity* releases the lens.

## IMU / orientation (optional)

- **LSM6DSOX** at I²C `0x6A` (`0x6B` in `src/imu.cpp` if SA0 is high), 104 Hz, sharing the camera's I²C bus
- Filter: **BasicVQF** (6D, no magnetometer) in `lib/VQF/basicvqf.hpp`; gyro bias is measured in the first second after boot
- Not detected at boot → the firmware runs without it and the dashboard says so

## Source layout

```
src/
  config.h            Ports, task layout, pins, battery; includes secrets.h
  secrets.example.h   Template for the git-ignored secrets.h
  camera.h/cpp        Camera init, settings + persistence, MJPEG handler
  focus.h/cpp         OV5640 autofocus firmware upload and control
  detector.h/cpp      AprilTag detection task and pose estimation
  imu.h/cpp           LSM6DSOX driver + VQF sampling task
  led.h/cpp           Status LED task
  main.cpp            PMIC, WiFi, HTTP servers, auth, serial CLI
  generated/          web_index.h (build output, git-ignored)
web/index.html        Dashboard source
scripts/              Pre-build hooks: dashboard embedding, apriltag manifest
lib/
  DFRobot_AXP313A/    AXP313A PMIC shim (V1.0 boards only)
  VQF/                BasicVQF header-only implementation
```

## V1.0 vs V1.1

| Version | AXP313A |
|---------|---------|
| **V1.0** (this board) | Required — shim in `lib/DFRobot_AXP313A/` handles it |
| V1.1+ | Absent — detected after 3 probes and skipped; boot continues |

## Troubleshooting

- **No tags detected** — check the tag is AprilTag **36h11** (print one from the dashboard), not mirrored in the feed (see Mirror), in focus, and at least ~5 % of the frame wide. The Live page's *Detector view* shows exactly what the detector sees
- **Green/black dotted columns in the video** — an OV5640 JPEG artifact (fixed columns, one dot every 8 rows). Not caused by load or the camera clock; higher JPEG quality numbers reduce it (≈50 % fewer at 20)
- **Black screen / camera init error** — power-cycle after flashing; AXP313A needs a hard reset on first boot. The LED double-blinks; dashboard, IMU and OTA keep working
- **Dashboard says "Wrong token"** — the stored token was cleared; enter `API_TOKEN` from `secrets.h` (System → Access)
- **Stuck on a wrong dashboard-chosen network** — it falls back to `secrets.h` after 10 s; `task factory` erases all saved settings
- **PSRAM error** — `board_build.arduino.memory_type = qio_opi` must not be changed; N16R8 uses OPI PSRAM
- **No COM port** — hold BOOT, tap RST, release BOOT, then flash
