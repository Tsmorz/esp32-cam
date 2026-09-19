#include "DFRobot_AXP313A.h"
#include "camera.h"
#include "config.h"
#include "detector.h"
#include "focus.h"
#include "generated/web_index.h"
#include "imu.h"
#include "led.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cstdarg>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static DFRobot_AXP313A axp;
static httpd_handle_t server = nullptr;
static httpd_handle_t stream_server = nullptr;
static String _saved_ssid; // network picked in the dashboard, if any

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

// snprintf that appends at pos and truncates instead of running off the end
static void appendf(char *buf, size_t size, size_t &pos, const char *fmt, ...) {
  if (pos >= size - 1)
    return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + pos, size - pos, fmt, ap);
  va_end(ap);
  if (n > 0)
    pos = (pos + n < size) ? pos + n : size - 1;
}

// State-changing routes require X-Token == API_TOKEN. Being a custom header it
// also forces a CORS preflight, which is never answered, so other websites
// can't fire these routes from a visitor's browser.
static bool authorized(httpd_req_t *req) {
  char tok[65] = {};
  if (API_TOKEN[0] &&
      httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof(tok)) == ESP_OK &&
      strcmp(tok, API_TOKEN) == 0)
    return true;
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED,
                      API_TOKEN[0] ? "Missing or wrong X-Token"
                                   : "API_TOKEN not set in secrets.h");
  return false;
}

// ── /stream (control server) → stream server
// ──────────────────────────────────────────────────────────────────
static esp_err_t stream_redirect_handler(httpd_req_t *req) {
  char host[64] = "192.168.4.1";
  httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
  char *colon = strchr(host, ':');
  if (colon)
    *colon = '\0';
  char loc[96];
  snprintf(loc, sizeof(loc), "http://%s:%d/stream", host, STREAM_PORT);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// ── /detections
// ─────────────────────────────────────────────────────────────────
static esp_err_t detections_handler(httpd_req_t *req) {
  const DetectionFrame f = detector_latest();
  const long age = f.ms ? (long)(millis() - f.ms) : -1;
  char buf[3072]; // 8 tags × ~250 B
  size_t pos = 0;
  appendf(buf, sizeof(buf), pos,
          "{\"age_ms\":%ld,\"w\":%d,\"h\":%d,\"f\":%.1f,\"tag_m\":%.4f,"
          "\"decode_ms\":%u,\"detect_ms\":%u,\"count\":%d,\"tags\":[",
          age, f.width, f.height, f.f, TAG_SIZE_M, f.decode_ms, f.detect_ms,
          f.count);
  for (int i = 0; i < f.count; i++) {
    const TagDetection &t = f.tags[i];
    appendf(buf, sizeof(buf), pos,
            "%s{\"id\":%d,"
            "\"corners\":[[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f]],"
            "\"pose\":{\"tx\":%.4f,\"ty\":%.4f,\"tz\":%.4f,"
            "\"q\":[%.4f,%.4f,%.4f,%.4f]}}",
            i > 0 ? "," : "", t.id, t.corners[0][0], t.corners[0][1],
            t.corners[1][0], t.corners[1][1], t.corners[2][0], t.corners[2][1],
            t.corners[3][0], t.corners[3][1], t.tx, t.ty, t.tz, t.q[0], t.q[1],
            t.q[2], t.q[3]);
  }
  appendf(buf, sizeof(buf), pos, "]}");
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
  uint32_t n, err;
  const char *state = imu_state(&n, &err);
  char buf[160];
  int len = snprintf(buf, sizeof(buf),
                     "{\"q\":[%.6f,%.6f,%.6f,%.6f],\"imu\":\"%s\",\"n\":%lu,"
                     "\"err\":%lu}",
                     q[0], q[1], q[2], q[3], state, (unsigned long)n,
                     (unsigned long)err);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /snapshot, /debug/gray.bmp
// ─────────────────────────────────────────────────────────────────
static esp_err_t snapshot_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t gray_handler(httpd_req_t *req) {
  uint8_t *bmp = nullptr;
  size_t len = detector_gray_bmp(&bmp);
  if (!len) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No frame processed yet");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/bmp");
  esp_err_t res = httpd_resp_send(req, (const char *)bmp, len);
  free(bmp);
  return res;
}

// ── /tag?id=N — print page for a tag in the family the detector reads.
// Centred with zero page margins, so A4 and Letter both leave more than the
// one-cell white border tag36h11 needs around the black square.
static esp_err_t tag_handler(httpd_req_t *req) {
  char query[32], val[8];
  int id = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK)
    id = atoi(val);
  char svg[1536];
  const size_t len = detector_tag_svg(id, svg, sizeof(svg));
  if (!len) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tag36h11 ids are 0-586");
    return ESP_FAIL;
  }
  char head[512], tail[160];
  snprintf(head, sizeof(head),
           "<!doctype html><meta charset=utf-8><title>tag36h11 #%d</title>"
           "<style>@page{margin:0}html,body{margin:0;background:#fff;"
           "color:#666;font:12px system-ui,sans-serif}body{height:100vh;"
           "display:flex;align-items:center;justify-content:center}"
           "p{position:fixed;bottom:12mm;margin:0}</style>",
           id);
  snprintf(tail, sizeof(tail),
           "<p>AprilTag 36h11 &middot; ID %d &middot; black square %.0f mm "
           "&middot; print at 100%% scale</p>",
           id, TAG_SIZE_M * 1000);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, svg, len);
  httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// ── / (dashboard: web/index.html, gzipped into the firmware at build time)
// ─────────────────────────────────────────────────────────────────────
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

// ── Battery
// ─────────────────────────────────────────────────────────────────────
// Single-cell LiPo resting voltage → charge, interpolated
static int lipo_percent(float v) {
  static const float curve[][2] = {
      {3.27f, 0},  {3.61f, 5},  {3.69f, 10}, {3.71f, 15}, {3.73f, 20},
      {3.75f, 25}, {3.77f, 30}, {3.79f, 35}, {3.80f, 40}, {3.82f, 45},
      {3.84f, 50}, {3.85f, 55}, {3.87f, 60}, {3.91f, 65}, {3.95f, 70},
      {3.98f, 75}, {4.02f, 80}, {4.08f, 85}, {4.11f, 90}, {4.15f, 95},
      {4.20f, 100}};
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (v <= curve[0][0])
    return 0;
  for (int i = 1; i < n; i++) {
    if (v <= curve[i][0]) {
      float t = (v - curve[i - 1][0]) / (curve[i][0] - curve[i - 1][0]);
      return (int)(curve[i - 1][1] + t * (curve[i][1] - curve[i - 1][1]));
    }
  }
  return 100;
}

// false when no sense pin is configured (see BAT_PIN in config.h)
static bool battery_read(float *volts, int *percent) {
#if BAT_PIN >= 0
  static_assert(BAT_PIN >= 1 && BAT_PIN <= 10,
                "BAT_PIN must be an ADC1 pin (GPIO 1-10): ADC2 reads nothing "
                "while WiFi is on");
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) // average out ADC noise
    mv += analogReadMilliVolts(BAT_PIN);
  *volts = mv / 16.0f * BAT_DIVIDER / 1000.0f;
  *percent = lipo_percent(*volts);
  return true;
#else
  (void)volts;
  (void)percent;
  return false;
#endif
}

static esp_err_t battery_handler(httpd_req_t *req) {
  char buf[64];
  float v;
  int pct;
  int len = battery_read(&v, &pct)
                ? snprintf(buf, sizeof(buf),
                           "{\"voltage\":%.3f,\"percent\":%d}", v, pct)
                : snprintf(buf, sizeof(buf), "{\"voltage\":-1,\"percent\":-1}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /status — everything the dashboard's header and System page show
// ─────────────────────────────────────────────────────────────────────
static esp_err_t status_handler(httpd_req_t *req) {
  char buf[1024];
  size_t pos = 0;
  const DetectionFrame f = detector_latest();
  uint32_t imu_n, imu_err;
  const char *imu = imu_state(&imu_n, &imu_err);
  const bool sta = WiFi.status() == WL_CONNECTED;
  char ap_ssid[68], sta_ssid[68], saved[68];
  json_esc(AP_SSID, ap_ssid, sizeof(ap_ssid));
  json_esc(sta ? WiFi.SSID().c_str() : "", sta_ssid, sizeof(sta_ssid));
  json_esc(_saved_ssid.c_str(), saved, sizeof(saved));

  appendf(buf, sizeof(buf), pos,
          "{\"uptime\":%lu,\"build\":\"%s %s\",\"heap\":%u,\"psram\":%u,"
          "\"psram_total\":%u,\"temp_c\":%.1f,\"stream_port\":%d,\"auth\":%s,"
          "\"sensor\":\"%s\",\"af\":\"%s\",",
          (unsigned long)(millis() / 1000), __DATE__, __TIME__,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
          (unsigned)ESP.getPsramSize(), temperatureRead(), STREAM_PORT,
          API_TOKEN[0] ? "true" : "false", camera_sensor_name(), focus_state());
  appendf(buf, sizeof(buf), pos,
          "\"imu\":{\"state\":\"%s\",\"samples\":%lu,\"errors\":%lu},", imu,
          (unsigned long)imu_n, (unsigned long)imu_err);
  appendf(buf, sizeof(buf), pos,
          "\"detect\":{\"ready\":%s,\"enabled\":%s,\"tag_mm\":%.0f,"
          "\"decode_ms\":%u,\"detect_ms\":%u,\"age_ms\":%ld,\"count\":%d},",
          detector_ready() ? "true" : "false",
          detector_enabled() ? "true" : "false", TAG_SIZE_M * 1000, f.decode_ms,
          f.detect_ms, f.ms ? (long)(millis() - f.ms) : -1L, f.count);
  appendf(
      buf, sizeof(buf), pos,
      "\"wifi\":{\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ap_clients\":%d,"
      "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"rssi\":%d,\"saved\":\"%s\"},",
      ap_ssid, WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(),
      sta_ssid, sta ? WiFi.localIP().toString().c_str() : "",
      sta ? WiFi.RSSI() : 0, saved);
  float v;
  int pct;
  if (battery_read(&v, &pct))
    appendf(buf, sizeof(buf), pos,
            "\"battery\":{\"wired\":true,\"voltage\":%.3f,\"percent\":%d}}", v,
            pct);
  else
    appendf(buf, sizeof(buf), pos, "\"battery\":{\"wired\":false}}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, pos);
  return ESP_OK;
}

// ── /camera/status — current sensor settings, to populate the Camera page
// ─────────────────────────────────────────────────────────────────────
static esp_err_t camera_status_handler(httpd_req_t *req) {
  char buf[512];
  size_t len = camera_status_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /system/reboot, /camera/reset
// ─────────────────────────────────────────────────────────────────────
static esp_err_t reboot_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  httpd_resp_sendstr(req, "OK");
  delay(300);
  esp_restart();
  return ESP_OK;
}

static esp_err_t camera_reset_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  camera_reset_settings(); // defaults apply on the next boot
  httpd_resp_sendstr(req, "OK");
  delay(300);
  esp_restart();
  return ESP_OK;
}

// ── /wifi/scan
// ───────────────────────────────────────────────────────────────────
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  static constexpr size_t BUF = 4096;
  char *buf = (char *)malloc(BUF); // too big for the httpd task stack
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  int n = WiFi.scanNetworks(false, false); // blocking, no hidden
  if (n < 0)
    n = 0;
  char esc[68];
  size_t pos = 0;
  json_esc(WiFi.SSID().c_str(), esc, sizeof(esc));
  appendf(buf, BUF, pos, "{\"sta\":\"%s\",\"networks\":[", esc);
  for (int i = 0; i < n && i < 24; i++) {
    json_esc(WiFi.SSID(i).c_str(), esc, sizeof(esc));
    appendf(buf, BUF, pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}",
            i > 0 ? "," : "", esc, WiFi.RSSI(i),
            (int)(WiFi.encryptionType(i) != WIFI_AUTH_OPEN));
  }
  appendf(buf, BUF, pos, "]}");
  WiFi.scanDelete();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, pos);
  free(buf);
  return ESP_OK;
}

// ── /wifi/connect
// ────────────────────────────────────────────────────────────────
static esp_err_t wifi_connect_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char body[256] = {};
  int ret = httpd_req_recv(req, body, sizeof(body) - 1);
  if (ret <= 0)
    return ESP_FAIL;
  char ssid[33] = {}, pass[65] = {};
  if (!json_str(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
    return ESP_FAIL;
  }
  json_str(body, "pass", pass, sizeof(pass));

  // Remembered across reboots; setup() tries it before the compiled-in network
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  _saved_ssid = ssid;

  httpd_resp_sendstr(req, "OK");
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  Serial.printf("[sta] connecting to %s\n", ssid);
  return ESP_OK;
}

// ── /ota
// ────────────────────────────────────────────────────────────────────────
// Uses ESP-IDF esp_ota_ops directly — avoids Arduino Update.h global
// constructor (UpdateClass Update) which crashes before setup() on OPI PSRAM.
static esp_err_t ota_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL; // closes the socket instead of draining the upload
  led_set(LED_OTA);
  const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
  if (!dst || dst == esp_ota_get_running_partition()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "No OTA partition — check partition table");
    return ESP_FAIL;
  }
  size_t total = req->content_len;
  esp_ota_handle_t ota;
  if (esp_ota_begin(dst, total > 0 ? total : OTA_SIZE_UNKNOWN, &ota) !=
      ESP_OK) {
    led_set(LED_IDLE);
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
  if (!ok)
    esp_ota_abort(ota);
  if (!ok || esp_ota_end(ota) != ESP_OK ||
      esp_ota_set_boot_partition(dst) != ESP_OK) {
    led_set(LED_IDLE);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA write failed");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  delay(500);
  esp_restart();
  return ESP_OK;
}

// ── /camera/set
// ─────────────────────────────────────────────────────────────────
static esp_err_t camera_set_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[128] = {}, var[32] = {}, val[16] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(qs, "val", val, sizeof(val)) != ESP_OK ||
      !camera_set(var, atoi(val))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad var or val");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── /detector/set?enabled=0|1
// ─────────────────────────────────────────────────────────────────
static esp_err_t detector_set_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[32] = {}, val[4] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "enabled", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled");
    return ESP_FAIL;
  }
  detector_set_enabled(atoi(val) != 0);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static void server_start() {
  // Control server: dashboard, JSON polling, settings, OTA
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = HTTP_PORT;
  cfg.core_id = NET_CORE;
  cfg.task_priority = PRIO_HTTP_CTRL;
  cfg.stack_size = 8192; // default 4096 is too small for OTA + JSON buffers
  cfg.max_uri_handlers = 20;
  cfg.max_open_sockets = 7;
  cfg.lru_purge_enable = true; // drop oldest idle socket if limit is reached
  cfg.recv_wait_timeout = 30;  // seconds — OTA uploads can be large
  cfg.send_wait_timeout = 10;

  // Stream server: its handler never returns while a client watches, so it
  // gets its own task. One viewer at a time; the next waits for a free slot.
  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port = STREAM_PORT;
  scfg.ctrl_port = cfg.ctrl_port + 1;
  scfg.core_id = NET_CORE;
  scfg.task_priority = PRIO_HTTP_STREAM;
  scfg.max_uri_handlers = 1;
  scfg.max_open_sockets = 3; // 7 + 3 + 2×(listen + ctrl) ≤ LWIP_MAX_SOCKETS
  scfg.lru_purge_enable = true;
  scfg.send_wait_timeout = 5; // drop a vanished viewer quickly

  if (httpd_start(&server, &cfg) != ESP_OK) {
    Serial.println("[http] control server failed to start");
    return;
  }
  const httpd_uri_t routes[] = {
      {"/", HTTP_GET, index_handler, nullptr},
      {"/stream", HTTP_GET, stream_redirect_handler, nullptr},
      {"/detections", HTTP_GET, detections_handler, nullptr},
      {"/pose", HTTP_GET, pose_handler, nullptr},
      {"/battery", HTTP_GET, battery_handler, nullptr},
      {"/wifi/scan", HTTP_GET, wifi_scan_handler, nullptr},
      {"/wifi/connect", HTTP_POST, wifi_connect_handler, nullptr},
      {"/ota", HTTP_POST, ota_handler, nullptr},
      {"/camera/set", HTTP_GET, camera_set_handler, nullptr},
      {"/snapshot", HTTP_GET, snapshot_handler, nullptr},
      {"/debug/gray.bmp", HTTP_GET, gray_handler, nullptr},
      {"/tag", HTTP_GET, tag_handler, nullptr},
      {"/detector/set", HTTP_GET, detector_set_handler, nullptr},
      {"/status", HTTP_GET, status_handler, nullptr},
      {"/camera/status", HTTP_GET, camera_status_handler, nullptr},
      {"/camera/reset", HTTP_POST, camera_reset_handler, nullptr},
      {"/system/reboot", HTTP_POST, reboot_handler, nullptr},
  };
  for (const httpd_uri_t &r : routes)
    httpd_register_uri_handler(server, &r);

  if (httpd_start(&stream_server, &scfg) != ESP_OK) {
    Serial.println("[http] stream server failed to start");
    return;
  }
  const httpd_uri_t stream = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(stream_server, &stream);
  Serial.printf("[http] control :%d, stream :%d\n", HTTP_PORT, STREAM_PORT);
}

// ── WiFi
// ────────────────────────────────────────────────────────────────────────
static bool sta_try(const char *ssid, const char *pass) {
  if (!ssid[0])
    return false;
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  Serial.printf("[sta] connecting to %s", ssid);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(" failed");
    return false;
  }
  Serial.printf("\n[sta] http://%s\n", WiFi.localIP().toString().c_str());
  return true;
}

static void wifi_start() {
  WiFi.persistent(false); // the dashboard's choice lives in Preferences
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[ap]  SSID: %s  http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());

  // A network picked in the dashboard wins; secrets.h is the fallback
  Preferences prefs;
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();
  _saved_ssid = ssid;
  if (sta_try(ssid.c_str(), pass.c_str()))
    return;
  if (ssid != WIFI_SSID && sta_try(WIFI_SSID, WIFI_PASS))
    return;
  Serial.println("[sta] not connected — AP only");
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
  Serial.printf("  temp : %.1f °C (chip)\n", temperatureRead());
  Serial.println("=============");
}

void setup() {
  Serial.begin(115200);
  delay(3000); // wait for USB CDC to enumerate before printing
  Serial.setDebugOutput(true);
  Serial.println("\n[boot] FireBeetle 2 ESP32-S3");
  led_start(APP_CORE, PRIO_LED); // fast blink until ready

  // One I²C bus: AXP313A, LSM6DSOX and — once camera_init() joins it — the
  // camera's SCCB
  Wire.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
  Wire.setTimeOut(50); // prevent I2C hangs if bus is stuck

  // V1.0 boards power the camera through the AXP313A; V1.1+ have no PMIC
  bool axp_found = false;
  for (int i = 0; i < 3 && !axp_found; i++) {
    axp_found = axp.begin() == 0;
    if (!axp_found)
      delay(200);
  }
  if (axp_found) {
    axp.enableCameraPower(axp.eOV2640);
    Serial.println("[axp] camera power enabled");
    delay(200);
  } else {
    Serial.println("[axp] not found — assuming V1.1+ (no PMIC)");
  }

  imu_init();
  const bool cam_ok = camera_init();
  if (cam_ok) {
    focus_init();     // OV5640 only; ~0.2 s to upload the AF firmware
    camera_restore(); // settings saved from the dashboard
    detector_init();
  } else {
    Serial.println("[boot] no camera — dashboard, IMU and OTA still run");
  }

  led_set(LED_WIFI);
  wifi_start();
  server_start();

  // Tasks start last so bring-up runs uncontended (see Task layout in config.h)
  imu_start(APP_CORE, PRIO_IMU);
  if (cam_ok)
    detector_start(APP_CORE, PRIO_DETECT, DETECT_PERIOD_MS);
  led_set(cam_ok ? LED_IDLE : LED_ERROR);
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
  delay(10);
}
