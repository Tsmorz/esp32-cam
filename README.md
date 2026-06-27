# firebeetle-camera

MJPEG stream web server for the **DFRobot FireBeetle 2 ESP32-S3 V1.0**
with the OV2640 camera. Open `http://<board-ip>` in any browser.

## Quick start

1. Edit WiFi credentials in `src/main.cpp` (or pass via build flags):
   ```ini
   build_flags =
       -DWIFI_SSID=\"MyNetwork\"
       -DWIFI_PASS=\"MyPassword\"
   ```

2. Flash:
   ```bash
   pio run -t upload
   pio device monitor   # watch for the IP address
   ```

3. Open `http://<ip>` — click **↺ Restart** if the stream is black on first load.

## V1.0 vs V1.1

| Version | AXP313A needed? |
|---|---|
| **V1.0** (yours) | **Yes** — shim in `lib/DFRobot_AXP313A/` handles it |
| V1.1+ | No — just remove the `axp.begin()` / `axp.enableCameraPower()` calls |

## Endpoints

| Path | Description |
|---|---|
| `GET /` | HTML viewer page |
| `GET /stream` | MJPEG stream (embed as `<img src="...">`) |
| `GET /control?var=framesize&val=<n>` | Change resolution at runtime |

## Resolution values

| `val` | Size |
|---|---|
| 4 | QVGA 320×240 |
| 5 | CIF 400×296 |
| 6 | VGA 640×480 |
| 8 | SVGA 800×600 (default) |
| 10 | UXGA 1600×1200 |

## Embedding in GripLink dashboard

```html
<img src="http://<gripper-ip>/stream" width="320" height="240"
     style="border:1px solid #1e2330">
```

The camera runs its own WiFi connection independently of the ESP-NOW
telemetry link, so video doesn't affect sensor latency.

## Troubleshooting

- **Black screen / init error** — power-cycle the board after flashing; the
  AXP313A sometimes needs a hard reset on first boot.
- **PSRAM error** — confirm `board_build.arduino.memory_type = qio_opi` in
  `platformio.ini`. The N16R8 module uses OPI (octal) PSRAM, not QIO.
- **Garbled image** — lower resolution or increase `jpeg_quality` value
  (higher number = lower quality but faster).
- **Can't find COM port** — hold BOOT, tap RST, release BOOT, then flash.
