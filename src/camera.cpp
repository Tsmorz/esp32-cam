#include "camera.h"
#include "config.h"
#include <Arduino.h>
#include <esp_camera.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GripLink Camera</title>
<style>
body{margin:0;background:#0d0f12;display:flex;flex-direction:column;align-items:center;
font-family:'IBM Plex Mono',monospace;color:#c8d0e0;}
h2{font-size:13px;letter-spacing:.2em;text-transform:uppercase;color:#00e5ff;margin:16px 0 8px;}
#wrap{position:relative;display:inline-block;}
#cam{display:block;max-width:100%;border:1px solid #1e2330;}
#ov{position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;}
</style></head><body>
<h2>GripLink — Wrist Camera</h2>
<div id="wrap">
  <img id="cam" src="/stream">
  <canvas id="ov" width="800" height="600"></canvas>
</div>
<script>
const ctx  = document.getElementById('ov').getContext('2d');
let tags   = [];
let quat   = [1, 0, 0, 0];  // [w, x, y, z] identity

// ── Data polling ──────────────────────────────────────────────────────────────
async function pollDetections() {
  try { const d = await (await fetch('/detections')).json(); tags = d.tags; } catch(_) {}
  setTimeout(pollDetections, 200);
}
async function pollPose() {
  try { const d = await (await fetch('/pose')).json(); quat = d.q; } catch(_) {}
  setTimeout(pollPose, 50);
}

// ── Quaternion → rotation matrix ──────────────────────────────────────────────
function quatToMat(w, x, y, z) {
  return [
    [1-2*(y*y+z*z),   2*(x*y-w*z),   2*(x*z+w*y)],
    [  2*(x*y+w*z), 1-2*(x*x+z*z),   2*(y*z-w*x)],
    [  2*(x*z-w*y),   2*(y*z+w*x), 1-2*(x*x+y*y)]
  ];
}

// ── Perspective project a rotated unit vector onto the canvas ─────────────────
function project(R, v, ox, oy, scale) {
  const xr = R[0][0]*v[0] + R[0][1]*v[1] + R[0][2]*v[2];
  const yr = R[1][0]*v[0] + R[1][1]*v[1] + R[1][2]*v[2];
  const zr = R[2][0]*v[0] + R[2][1]*v[1] + R[2][2]*v[2];
  const d  = 2.5 + zr;  // perspective depth
  return [ox + scale * xr / d, oy - scale * yr / d];
}

// ── Draw one axis arrow ───────────────────────────────────────────────────────
function drawAxis(R, v, ox, oy, scale, color, label) {
  const [tx, ty] = project(R, v, ox, oy, scale);
  ctx.strokeStyle = color;
  ctx.fillStyle   = color;
  ctx.lineWidth   = 3;
  ctx.beginPath(); ctx.moveTo(ox, oy); ctx.lineTo(tx, ty); ctx.stroke();
  // Arrowhead
  const angle = Math.atan2(ty - oy, tx - ox);
  const h = 10;
  ctx.beginPath();
  ctx.moveTo(tx, ty);
  ctx.lineTo(tx - h*Math.cos(angle-0.4), ty - h*Math.sin(angle-0.4));
  ctx.lineTo(tx - h*Math.cos(angle+0.4), ty - h*Math.sin(angle+0.4));
  ctx.closePath(); ctx.fill();
  ctx.font = 'bold 12px monospace';
  ctx.fillText(label, tx + 5, ty + 4);
}

// ── Render loop ───────────────────────────────────────────────────────────────
function render() {
  ctx.clearRect(0, 0, 800, 600);

  // Tag bounding boxes + labels
  ctx.strokeStyle = '#00e5ff';
  ctx.fillStyle   = '#00e5ff';
  ctx.lineWidth   = 2;
  ctx.font        = 'bold 13px monospace';
  for (const t of tags) {
    const c = t.corners;
    ctx.beginPath();
    ctx.moveTo(c[0][0], c[0][1]); ctx.lineTo(c[1][0], c[1][1]);
    ctx.lineTo(c[2][0], c[2][1]); ctx.lineTo(c[3][0], c[3][1]);
    ctx.closePath(); ctx.stroke();
    ctx.fillText('ID:' + t.id + '  z:' + t.pose.tz.toFixed(2) + 'm',
                 c[0][0] + 4, c[0][1] - 6);
  }

  // IMU orientation axes (bottom-left corner)
  const ox = 80, oy = 530, scale = 65;
  // Semi-transparent background circle
  ctx.fillStyle = 'rgba(13,15,18,0.6)';
  ctx.beginPath(); ctx.arc(ox, oy, scale + 12, 0, 2*Math.PI); ctx.fill();

  const R = quatToMat(...quat);
  drawAxis(R, [1,0,0], ox, oy, scale, '#ff4444', 'X');
  drawAxis(R, [0,1,0], ox, oy, scale, '#44ff88', 'Y');
  drawAxis(R, [0,0,1], ox, oy, scale, '#4488ff', 'Z');

  requestAnimationFrame(render);
}

pollDetections();
pollPose();
requestAnimationFrame(render);
</script>
</body></html>)rawliteral";

bool camera_init() {
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
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
  }

  Serial.println("[cam] OV2640 ready");
  return true;
}

esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
  return ESP_OK;
}

esp_err_t stream_handler(httpd_req_t *req) {
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
