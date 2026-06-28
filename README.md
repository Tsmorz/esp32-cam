# firebeetle-camera

GripLink wrist camera firmware for the **DFRobot FireBeetle 2 ESP32-S3 V1.0** with an OV2640 camera and LSM6DSOX IMU. Streams MJPEG video, detects AprilTags, and estimates orientation — all served over a built-in WiFi AP and/or your home network.

## Quick start

1. Set WiFi credentials in `src/config.h` or via build flags:
   ```ini
   build_flags =
       -DWIFI_SSID=\"MyNetwork\"
       -DWIFI_PASS=\"MyPassword\"
   ```

2. Flash and monitor:
   ```bash
   pio run -t upload
   pio device monitor
   ```

3. Connect to **GripLink-Cam** WiFi (password: `griplink1`) and open `http://192.168.4.1`, or use the STA IP printed to serial.

## WiFi modes

The board runs **AP+STA simultaneously**:
- AP always on at `http://192.168.4.1` — direct connect, no router needed
- STA joins your home WiFi if available (10 s timeout, then AP-only)

## Serial commands

| Key | Action |
|-----|--------|
| `?` | Print IP, SSID, RSSI, heap, PSRAM |
| `t` | Toggle AprilTag detector on/off |

## HTTP endpoints

| Path | Description |
|------|-------------|
| `GET /` | HTML viewer with AprilTag overlays and IMU orientation axes |
| `GET /stream` | MJPEG stream (embed as `<img src="...">`) |
| `GET /detections` | JSON snapshot of detected AprilTags with corners and pose |
| `GET /pose` | JSON IMU quaternion `{"q":[w,x,y,z]}` |

## AprilTag detection

- Family: **tag36h11**
- Runs at 2 Hz in `loop()`, processes JPEG frames decoded to grayscale at 1/4 scale (200×150)
- Camera buffer is returned immediately; detection runs on a PSRAM copy so the stream isn't stalled
- Pose requires `TAG_SIZE_M` in `src/detector.h` to match your printed tag's side length in metres (default 16.5 cm)

## IMU / orientation

- Sensor: **LSM6DSOX** at I²C `0x6A` (change to `0x6B` in `src/imu.cpp` if SA0 is pulled high)
- Filter: **BasicVQF** (6D gyro + accel, no magnetometer) in `lib/VQF/basicvqf.hpp`
- Output rate: ~100 Hz, quaternion served at `/pose`
- The browser overlays 3D unit axes (X red, Y green, Z blue) in the bottom-left corner of the stream

## Source layout

```
src/
  config.h        WiFi credentials, AP SSID/pass, camera DVP pin map
  camera.h/cpp    Camera init, MJPEG stream handler, HTML page
  detector.h/cpp  AprilTag detection and pose estimation
  imu.h/cpp       LSM6DSOX driver + VQF filter
  main.cpp        AXP313A init, WiFi AP+STA, HTTP server, loop

lib/
  DFRobot_AXP313A/   AXP313A PMIC shim (V1.0 boards only)
  VQF/               BasicVQF header-only implementation
```

## V1.0 vs V1.1

| Version | AXP313A |
|---------|---------|
| **V1.0** (this board) | Required — shim in `lib/DFRobot_AXP313A/` handles it |
| V1.1+ | Absent — `axp.begin()` returns `-1` and firmware continues |

## Troubleshooting

- **Black screen / camera init error** — power-cycle after flashing; AXP313A needs a hard reset on first boot
- **PSRAM error** — `board_build.arduino.memory_type = qio_opi` must not be changed; N16R8 uses OPI PSRAM
- **`[imu] LSM6DSOX not found`** — check I²C address (0x6A vs 0x6B) and wiring; axes show identity quaternion until IMU is found
- **No COM port** — hold BOOT, tap RST, release BOOT, then flash
- **White lines in image** — known OV2640 rolling-shutter artifact; try `jpeg_quality = 8` or `FRAMESIZE_VGA`
