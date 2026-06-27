#include "DFRobot_AXP313A.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#ifndef WIFI_SSID
#define WIFI_SSID "TonyS24"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "Abc123ts"
#endif

// FireBeetle 2 ESP32-S3 DVP camera pins
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 45
#define CAM_PIN_SIOD 1
#define CAM_PIN_SIOC 2
#define CAM_PIN_D7 48
#define CAM_PIN_D6 46
#define CAM_PIN_D5 8
#define CAM_PIN_D4 7
#define CAM_PIN_D3 4
#define CAM_PIN_D2 41
#define CAM_PIN_D1 40
#define CAM_PIN_D0 39
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 42
#define CAM_PIN_PCLK 5

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

DFRobot_AXP313A axp;

static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GripLink Camera</title>
<style>
body{margin:0;background:#0d0f12;display:flex;flex-direction:column;align-items:center;
font-family:'IBM Plex Mono',monospace;color:#c8d0e0;}
h2{font-size:13px;letter-spacing:.2em;text-transform:uppercase;color:#00e5ff;margin:16px 0 8px;}
img{max-width:100%;border:1px solid #1e2330;}
</style></head><body>
<h2>GripLink — Wrist Camera</h2>
<img src="/stream">
</body></html>)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
  return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = nullptr;
  char part_buf[64];
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) break;
    httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    httpd_resp_send_chunk(req, part_buf, hlen);
    esp_err_t res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    fb = nullptr;
    if (res != ESP_OK) break;
  }
  if (fb) esp_camera_fb_return(fb);
  return ESP_OK;
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\n[boot] FireBeetle 2 ESP32-S3");

  Wire.begin();
  while (axp.begin() != 0) {
    Serial.println("[axp] not found, retrying...");
    delay(1000);
  }
  axp.enableCameraPower(axp.eOV2640);
  Serial.println("[axp] camera power enabled");
  delay(200);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return;
  }
  Serial.println("[cam] OV2640 ready");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[wifi] connecting to %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[wifi] http://%s\n", WiFi.localIP().toString().c_str());

  httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &hcfg) == ESP_OK) {
    httpd_uri_t idx = {"/", HTTP_GET, index_handler, nullptr};
    httpd_uri_t stm = {"/stream", HTTP_GET, stream_handler, nullptr};
    httpd_register_uri_handler(server, &idx);
    httpd_register_uri_handler(server, &stm);
    Serial.println("[http] server started — open the URL above");
  }
}

void loop() {
  delay(10000);
}
