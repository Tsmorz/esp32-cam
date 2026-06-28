#include "DFRobot_AXP313A.h"
#include "camera.h"
#include "config.h"
#include "detector.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>

static DFRobot_AXP313A axp;
static httpd_handle_t server = nullptr;

// ── /detections — JSON snapshot of the most recent AprilTag frame
// ─────────────
static esp_err_t detections_handler(httpd_req_t *req) {
  const DetectionFrame &f = detector_latest();
  char buf[2048];
  int pos = snprintf(buf, sizeof(buf), "{\"count\":%d,\"tags\":[", f.count);
  for (int i = 0; i < f.count; i++) {
    const TagDetection &t = f.tags[i];
    if (i > 0)
      pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
    pos += snprintf(
        buf + pos, sizeof(buf) - pos,
        "{\"id\":%d,"
        "\"corners\":[[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f]],"
        "\"pose\":{\"tx\":%.4f,\"ty\":%.4f,\"tz\":%.4f}}",
        t.id, t.corners[0][0], t.corners[0][1], t.corners[1][0],
        t.corners[1][1], t.corners[2][0], t.corners[2][1], t.corners[3][0],
        t.corners[3][1], t.tx, t.ty, t.tz);
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, pos);
  return ESP_OK;
}

static void server_start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  if (httpd_start(&server, &cfg) != ESP_OK)
    return;
  httpd_uri_t idx = {"/", HTTP_GET, index_handler, nullptr};
  httpd_uri_t stm = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_uri_t det = {"/detections", HTTP_GET, detections_handler, nullptr};
  httpd_register_uri_handler(server, &idx);
  httpd_register_uri_handler(server, &stm);
  httpd_register_uri_handler(server, &det);
  Serial.println("[http] server started");
}

static void print_stats() {
  Serial.println("=== stats ===");
  Serial.printf("  ap   : %s  http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  sta  : %s  http://%s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    Serial.printf("  rssi : %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("  sta  : not connected");
  }
  Serial.printf("  heap : %u free\n", ESP.getFreeHeap());
  Serial.printf("  psram: %u free\n", ESP.getFreePsram());
  Serial.println("=============");
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\n[boot] FireBeetle 2 ESP32-S3");

  // AXP313A power IC — enables DOVDD (1.8V) and AVDD (2.8V) for the camera
  Wire.begin();
  while (axp.begin() != 0) {
    Serial.println("[axp] not found, retrying...");
    delay(1000);
  }
  axp.enableCameraPower(axp.eOV2640);
  Serial.println("[axp] camera power enabled");
  delay(200);

  if (!camera_init())
    return;
  detector_init();

  // AP+STA: always broadcast own hotspot, also try joining existing WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[ap]  SSID: %s  http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("[sta] connecting to %s", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\n[sta] http://%s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("\n[sta] not connected — AP only");

  server_start();
  Serial.println("[ready] type ? for stats");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '?')
      print_stats();
    if (c == 't')
      detector_toggle();
  }

  // Run AprilTag detection every 500 ms
  static uint32_t last_detect = 0;
  if (millis() - last_detect >= 500) {
    detector_grab();
    last_detect = millis();
  }

  delay(10);
}
