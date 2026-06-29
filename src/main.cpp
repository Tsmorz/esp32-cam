#include "DFRobot_AXP313A.h"
#include "camera.h"
#include "config.h"
#include "detector.h"
#include "imu.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static DFRobot_AXP313A axp;
static httpd_handle_t server = nullptr;

// ── helpers
// ──────────────────────────────────────────────────────────────────
static void json_esc(const char *in, char *out, size_t outlen) {
  size_t o = 0;
  for (const char *p = in; *p && o < outlen - 2; p++) {
    if (*p == '"' || *p == '\\')
      out[o++] = '\\';
    if ((unsigned char)*p >= 0x20)
      out[o++] = *p;
  }
  out[o] = '\0';
}

static bool json_str(const char *json, const char *key, char *out,
                     size_t outlen) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  const char *p = strstr(json, needle);
  if (!p)
    return false;
  p += strlen(needle);
  size_t i = 0;
  while (*p && *p != '"' && i < outlen - 1) {
    if (*p == '\\' && *(p + 1))
      p++;
    out[i++] = *p++;
  }
  out[i] = '\0';
  return true;
}

// ── /detections
// ─────────────────────────────────────────────────────────────────
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

// ── /pose
// ───────────────────────────────────────────────────────────────────────
static esp_err_t pose_handler(httpd_req_t *req) {
  double q[4];
  imu_get_quat(q);
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "{\"q\":[%.6f,%.6f,%.6f,%.6f]}", q[0],
                     q[1], q[2], q[3]);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /battery
// ─────────────────────────────────────────────────────────────────────
static esp_err_t battery_handler(httpd_req_t *req) {
  // GPIO14 = ADC2_CH3 on ESP32-S3. ADC2 is blocked when WiFi is active;
  // analogReadMilliVolts returns 0 in that case. Return -1 sentinel so
  // the dashboard can show "N/A" instead of a bogus 0V reading.
  uint32_t mv = analogReadMilliVolts(BAT_PIN);
  char buf[64];
  int len;
  if (mv < 100) {
    len = snprintf(buf, sizeof(buf), "{\"voltage\":-1,\"percent\":-1}");
  } else {
    float voltage = (float)mv * 2.0f / 1000.0f;
    int percent = (int)((voltage - 3.0f) / 1.2f * 100.0f);
    if (percent < 0)
      percent = 0;
    if (percent > 100)
      percent = 100;
    len = snprintf(buf, sizeof(buf), "{\"voltage\":%.3f,\"percent\":%d}",
                   voltage, percent);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /wifi/scan
// ───────────────────────────────────────────────────────────────────
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  int n = WiFi.scanNetworks(false, false); // blocking, no hidden
  if (n < 0)
    n = 0;
  char buf[4096];
  char esc[68];
  json_esc(WiFi.SSID().c_str(), esc, sizeof(esc));
  int pos = snprintf(buf, sizeof(buf), "{\"sta\":\"%s\",\"networks\":[", esc);
  for (int i = 0; i < n && i < 24; i++) {
    json_esc(WiFi.SSID(i).c_str(), esc, sizeof(esc));
    if (i > 0)
      pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
    pos +=
        snprintf(buf + pos, sizeof(buf) - pos,
                 "{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}", esc, WiFi.RSSI(i),
                 (int)(WiFi.encryptionType(i) != WIFI_AUTH_OPEN));
  }
  pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
  WiFi.scanDelete();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, pos);
  return ESP_OK;
}

// ── /wifi/connect
// ────────────────────────────────────────────────────────────────
static esp_err_t wifi_connect_handler(httpd_req_t *req) {
  char body[256] = {};
  int ret = httpd_req_recv(req, body, sizeof(body) - 1);
  if (ret <= 0)
    return ESP_FAIL;
  char ssid[64] = {}, pass[128] = {};
  json_str(body, "ssid", ssid, sizeof(ssid));
  json_str(body, "pass", pass, sizeof(pass));
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "OK");
  if (strlen(ssid) > 0) {
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    Serial.printf("[sta] connecting to %s\n", ssid);
  }
  return ESP_OK;
}

// ── /ota
// ────────────────────────────────────────────────────────────────────────
// Uses ESP-IDF esp_ota_ops directly — avoids Arduino Update.h global
// constructor (UpdateClass Update) which crashes before setup() on OPI PSRAM.
static esp_err_t ota_handler(httpd_req_t *req) {
  const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
  if (!dst) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "No OTA partition — check partition table");
    return ESP_FAIL;
  }
  size_t total = req->content_len;
  esp_ota_handle_t ota;
  if (esp_ota_begin(dst, total > 0 ? total : OTA_SIZE_UNKNOWN, &ota) !=
      ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA begin failed");
    return ESP_FAIL;
  }
  uint8_t buf[2048];
  size_t remaining = total;
  bool ok = true;
  while (remaining > 0 || total == 0) {
    size_t to_read =
        (remaining > 0 && remaining < sizeof(buf)) ? remaining : sizeof(buf);
    int recv = httpd_req_recv(req, (char *)buf, to_read);
    if (recv == 0)
      break;
    if (recv < 0 || esp_ota_write(ota, buf, recv) != ESP_OK) {
      ok = false;
      break;
    }
    if (total > 0)
      remaining -= recv;
  }
  if (!ok || esp_ota_end(ota) != ESP_OK ||
      esp_ota_set_boot_partition(dst) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA write failed");
    return ESP_FAIL;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "OK");
  delay(500);
  esp_restart();
  return ESP_OK;
}

// ── /camera/set
// ─────────────────────────────────────────────────────────────────
static esp_err_t camera_set_handler(httpd_req_t *req) {
  char qs[128] = {};
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < sizeof(qs)) {
    httpd_req_get_url_query_str(req, qs, qlen + 1);
    char varname[32] = {}, valstr[16] = {};
    httpd_query_key_value(qs, "var", varname, sizeof(varname));
    httpd_query_key_value(qs, "val", valstr, sizeof(valstr));
    camera_set(varname, atoi(valstr));
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static void server_start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 12;
  cfg.max_open_sockets = 10;   // default 7 is too few with /stream + polling
  cfg.lru_purge_enable = true; // drop oldest idle socket if limit is reached
  cfg.recv_wait_timeout = 30;  // seconds — OTA uploads can be large
  cfg.send_wait_timeout = 10;
  if (httpd_start(&server, &cfg) != ESP_OK)
    return;

  httpd_uri_t idx = {"/", HTTP_GET, index_handler, nullptr};
  httpd_uri_t stm = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_uri_t det = {"/detections", HTTP_GET, detections_handler, nullptr};
  httpd_uri_t pose = {"/pose", HTTP_GET, pose_handler, nullptr};
  httpd_uri_t bat = {"/battery", HTTP_GET, battery_handler, nullptr};
  httpd_uri_t wscan = {"/wifi/scan", HTTP_GET, wifi_scan_handler, nullptr};
  httpd_uri_t wconn = {"/wifi/connect", HTTP_POST, wifi_connect_handler,
                       nullptr};
  httpd_uri_t ota = {"/ota", HTTP_POST, ota_handler, nullptr};
  httpd_uri_t camset = {"/camera/set", HTTP_GET, camera_set_handler, nullptr};

  httpd_register_uri_handler(server, &idx);
  httpd_register_uri_handler(server, &stm);
  httpd_register_uri_handler(server, &det);
  httpd_register_uri_handler(server, &pose);
  httpd_register_uri_handler(server, &bat);
  httpd_register_uri_handler(server, &wscan);
  httpd_register_uri_handler(server, &wconn);
  httpd_register_uri_handler(server, &ota);
  httpd_register_uri_handler(server, &camset);
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
  delay(3000); // wait for USB CDC to enumerate before printing
  Serial.setDebugOutput(true);
  Serial.println("\n[boot] FireBeetle 2 ESP32-S3");

  Wire.begin();
  Wire.setTimeOut(50); // prevent I2C hangs if bus is stuck
  while (axp.begin() != 0) {
    Serial.println("[axp] not found, retrying...");
    delay(1000);
  }
  axp.enableCameraPower(axp.eOV2640);
  Serial.println("[axp] camera power enabled");
  delay(200);

  imu_init(); // before camera_init — camera driver reinstalls I2C0 for SCCB
  if (!camera_init())
    return;
  detector_init();

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
  Serial.println("[ready]  ?=stats  t=toggle-detector");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '?')
      print_stats();
    if (c == 't')
      detector_toggle();
  }

  static uint32_t last_imu = 0;
  if (millis() - last_imu >= 10) {
    imu_update();
    last_imu = millis();
  }

  static uint32_t last_detect = 0;
  if (millis() - last_detect >= 500) {
    detector_grab();
    last_detect = millis();
  }

  delay(1);
}
