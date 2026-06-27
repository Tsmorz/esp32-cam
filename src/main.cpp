//  FireBeetle 2 ESP32-S3 V1.0 — OV2640 MJPEG stream over WiFi
//  Open http://<board-ip> in a browser to view the live feed.
//
//  Flash settings (platformio.ini):
//    board_build.arduino.memory_type = qio_opi   (OPI PSRAM)
//    board_build.partitions          = huge_app.csv

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include "DFRobot_AXP313A.h"

// ── WiFi credentials ────────────────────────────────────────────────
// Set these via build_flags or environment variables for production;
// hard-coded here for simplicity.
#ifndef WIFI_SSID
#  define WIFI_SSID "TonyS24"
#endif
#ifndef WIFI_PASS
#  define WIFI_PASS "Abc123ts"
#endif

// ── FireBeetle 2 ESP32-S3 DVP camera pins ───────────────────────────
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    45
#define CAM_PIN_SIOD     1   // SDA
#define CAM_PIN_SIOC     2   // SCL
#define CAM_PIN_D7      48   // Y9
#define CAM_PIN_D6      46   // Y8
#define CAM_PIN_D5       8   // Y7
#define CAM_PIN_D4       7   // Y6
#define CAM_PIN_D3       4   // Y5
#define CAM_PIN_D2      41   // Y4
#define CAM_PIN_D1      40   // Y3
#define CAM_PIN_D0      39   // Y2
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF    42
#define CAM_PIN_PCLK     5

// ── Stream boundary ─────────────────────────────────────────────────
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY =
    "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ── AXP313A camera power ─────────────────────────────────────────────
DFRobot_AXP313A axp;

// ── Forward declarations ─────────────────────────────────────────────
static esp_err_t stream_handler(httpd_req_t *req);
static esp_err_t index_handler(httpd_req_t *req);
void start_camera_server();

// ── Minimal HTML page ────────────────────────────────────────────────
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>GripLink Camera</title>
  <style>
    body { margin:0; background:#0d0f12; display:flex;
           flex-direction:column; align-items:center;
           font-family:'IBM Plex Mono',monospace; color:#c8d0e0; }
    h2   { font-size:13px; letter-spacing:.2em; text-transform:uppercase;
           color:#00e5ff; margin:16px 0 8px; }
    img  { max-width:100%; border:1px solid #1e2330; }
    .info{ font-size:10px; color:#3a4155; margin:8px; }
    select,button {
      background:#13161b; color:#c8d0e0; border:1px solid #1e2330;
      padding:6px 12px; margin:4px; font-family:inherit; font-size:11px;
      cursor:pointer; border-radius:2px;
    }
    button:hover { border-color:#00e5ff; color:#00e5ff; }
  </style>
</head>
<body>
  <h2>GripLink — Wrist Camera</h2>
  <img id="stream" src="/stream">
  <div>
    <select id="res" onchange="setRes(this.value)">
      <option value="10">UXGA 1600×1200</option>
      <option value="8" selected>SVGA 800×600</option>
      <option value="6">VGA 640×480</option>
      <option value="5">CIF 400×296</option>
      <option value="4">QVGA 320×240</option>
    </select>
    <button onclick="document.getElementById('stream').src='/stream?t='+Date.now()">
      ↺ Restart
    </button>
  </div>
  <div class="info">OV2640 · 2MP · 68° FOV · MJPEG</div>
  <script>
    function setRes(val) {
      fetch('/control?var=framesize&val=' + val);
      // Restart stream after a short delay
      setTimeout(() => {
        const img = document.getElementById('stream');
        img.src = '/stream?t=' + Date.now();
      }, 500);
    }
  </script>
</body>
</html>
)rawliteral";

// ─────────────────────────────────────────────────────────────────────
//  HTTP handlers
// ─────────────────────────────────────────────────────────────────────

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t  *fb     = nullptr;
    esp_err_t     res    = ESP_OK;
    char          part_buf[64];

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[cam] frame capture failed");
            res = ESP_FAIL;
            break;
        }

        // -- boundary
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY,
                                    strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) break;

        // -- part header
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
                               STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) break;

        // -- JPEG payload
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        fb = nullptr;
        if (res != ESP_OK) break;
    }

    if (fb) esp_camera_fb_return(fb);
    return res;
}

// /control?var=framesize&val=<n>
static esp_err_t control_handler(httpd_req_t *req) {
    char buf[64];
    int  len = std::min((int)sizeof(buf) - 1, (int)req->content_len);    httpd_req_get_url_query_str(req, buf, sizeof(buf));

    // Parse var= and val=
    char var[16] = {}, val[8] = {};
    httpd_query_key_value(buf, "var", var, sizeof(var));
    httpd_query_key_value(buf, "val", val, sizeof(val));

    sensor_t *s = esp_camera_sensor_get();
    if (s && strcmp(var, "framesize") == 0) {
        s->set_framesize(s, (framesize_t)atoi(val));
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────
//  Camera + server init
// ─────────────────────────────────────────────────────────────────────

static bool init_camera() {
    camera_config_t config = {};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = CAM_PIN_D0;
    config.pin_d1       = CAM_PIN_D1;
    config.pin_d2       = CAM_PIN_D2;
    config.pin_d3       = CAM_PIN_D3;
    config.pin_d4       = CAM_PIN_D4;
    config.pin_d5       = CAM_PIN_D5;
    config.pin_d6       = CAM_PIN_D6;
    config.pin_d7       = CAM_PIN_D7;
    config.pin_xclk     = CAM_PIN_XCLK;
    config.pin_pclk     = CAM_PIN_PCLK;
    config.pin_vsync    = CAM_PIN_VSYNC;
    config.pin_href     = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn     = CAM_PIN_PWDN;
    config.pin_reset    = CAM_PIN_RESET;

    config.xclk_freq_hz = 20000000;        // 20 MHz XCLK
    config.pixel_format = PIXFORMAT_JPEG;

    // With 8MB OPI PSRAM: allocate 2 frame buffers at SVGA for smooth streaming
    config.frame_size   = FRAMESIZE_SVGA;  // 800×600 default
    config.jpeg_quality = 12;              // 0=best, 63=worst; 10–15 is sweet spot
    config.fb_count     = 2;              // double-buffer
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[cam] init failed: 0x%x\n", err);
        return false;
    }

    // Tune OV2640 sensor
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_brightness(s,  0);   // -2 to 2
        s->set_contrast(s,    0);   // -2 to 2
        s->set_saturation(s,  0);   // -2 to 2
        s->set_whitebal(s,    1);   // AWB on
        s->set_awb_gain(s,    1);
        s->set_exposure_ctrl(s, 1); // AEC on
        s->set_aec2(s,        1);   // AEC DSP
        s->set_gain_ctrl(s,   1);   // AGC on
        s->set_hmirror(s,     0);   // flip if needed
        s->set_vflip(s,       0);
    }
    return true;
}

void start_camera_server() {
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers  = 8;
    config.stack_size        = 8192;

    httpd_handle_t stream_httpd = nullptr;
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {

        httpd_uri_t index_uri = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = index_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(stream_httpd, &index_uri);

        httpd_uri_t stream_uri = {
            .uri      = "/stream",
            .method   = HTTP_GET,
            .handler  = stream_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(stream_httpd, &stream_uri);

        httpd_uri_t control_uri = {
            .uri      = "/control",
            .method   = HTTP_GET,
            .handler  = control_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(stream_httpd, &control_uri);

        Serial.println("[http] server started");
    }
}

// ─────────────────────────────────────────────────────────────────────
//  Arduino entry points
// ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(3000); // native USB CDC needs time to re-enumerate after reset
    Serial.println("\n[boot] FireBeetle 2 ESP32-S3 camera web server");

    // ── 1. Power on camera via AXP313A (V1.0 only) ──────────────────
    Wire.begin();
    int axp_ret = axp.begin();
    if (axp_ret != 0) {
        Serial.printf("[axp] init failed (%d) — check I2C wiring\n", axp_ret);
        // Don't halt — V1.1 boards don't need AXP and will return error here
    } else {
        axp.enableCameraPower(DFRobot_AXP313A::eOV2640);
        Serial.println("[axp] camera power enabled");
    }
    delay(200); // let rails settle — OV2640 needs ~100-200ms after power-on

    // ── 2. Init OV2640 ───────────────────────────────────────────────
    if (!init_camera()) {
        Serial.println("[cam] FATAL — check PSRAM setting (must be OPI)");
        // Power-cycle may be needed; blink LED to signal error
        pinMode(21, OUTPUT);
        while (true) {
            digitalWrite(21, HIGH); delay(200);
            digitalWrite(21, LOW);  delay(200);
        }
    }
    Serial.println("[cam] OV2640 ready");

    // ── 3. Connect WiFi ──────────────────────────────────────────────
    Serial.printf("[wifi] connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[wifi] connected — http://%s\n",
                  WiFi.localIP().toString().c_str());

    // ── 4. Start HTTP server ─────────────────────────────────────────
    start_camera_server();
    Serial.println("[boot] ready — open the URL above in your browser");
}

void loop() {
    // Everything is handled by the HTTP server tasks.
    // LED heartbeat so you know it's alive.
    static uint32_t last = 0;
    if (millis() - last > 2000) {
        last = millis();
        digitalWrite(21, !digitalRead(21));
    }
}
