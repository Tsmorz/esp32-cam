#include "DFRobot_AXP313A.h"
#include "camera.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>

static DFRobot_AXP313A axp;
static httpd_handle_t server = nullptr;

static void server_start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  if (httpd_start(&server, &cfg) != ESP_OK) return;
  httpd_uri_t idx = {"/", HTTP_GET, index_handler, nullptr};
  httpd_uri_t stm = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(server, &idx);
  httpd_register_uri_handler(server, &stm);
  Serial.println("[http] server started");
}

static void print_stats() {
  Serial.println("=== stats ===");
  Serial.printf("  ap   : %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  sta  : %s  http://%s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
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

  if (!camera_init()) return;

  // AP+STA: always broadcast own hotspot, also try joining existing WiFi
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[ap]  SSID: %s  http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

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
    if (c == '?') print_stats();
  }
  delay(10);
}
