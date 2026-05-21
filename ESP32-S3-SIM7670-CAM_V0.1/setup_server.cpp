#include "setup_server.h"
#include "config.h"
#include "time_sync.h"      // g_captureIntervalMin
#include "image_capture.h"  // g_captureTarget
#include "sd_storage.h"     // saveConfig
#include "led.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "img_converters.h" // frame2jpg
#include <WiFi.h>
#include <Arduino.h>
#include <math.h>           // sinf()
#include "esp_timer.h"      // esp_timer_get_time() — μs 정밀 하드웨어 타이머

// ─────────────────────────────────────────────────────────────────────────────
// 전역 상태
// ─────────────────────────────────────────────────────────────────────────────
// g_setupMode: false 로 초기화.
// Deep Sleep 은 완전 재부팅이므로 .data 세그먼트가 Flash 초기값으로 복원됨.
// true 로 초기화하면 Deep Sleep Wake-up 후 loop() 에서 setupServerLoop() 로 빠져
// 최대 5분간 캡처가 발생하지 않는 문제 발생.
// enterSetupMode() 호출 시에만 true 로 설정하고, 최초 부팅 여부는
// setup() 의 esp_sleep_get_wakeup_cause() 로 판별함.
bool     g_setupMode    = false;
uint32_t g_setupStartMs = 0;

static httpd_handle_t  s_httpd          = NULL;
static volatile bool   s_startRequested = false;   // /start POST 수신 시 true

// ─────────────────────────────────────────────────────────────────────────────
// HTML 페이지 템플릿
//   snprintf 인자 순서: %d=intv, %d=cnt, %d=remaining_ms
//   CSS/JS 의 리터럴 % 는 모두 %% 로 이스케이프
// ─────────────────────────────────────────────────────────────────────────────
static const char HTML_TEMPLATE[] =
  "<!DOCTYPE html><html lang=\"ko\"><head>"
  "<meta charset=\"UTF-8\">"
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>카메라 설정</title>"
  "<style>"
  "*{box-sizing:border-box}"
  "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:12px;"
       "background:#0d1117;color:#c9d1d9;max-width:500px;margin:0 auto}"
  "h2{text-align:center;color:#58a6ff;margin:8px 0 4px;font-size:20px}"
  "#timer{text-align:center;color:#8b949e;font-size:13px;margin-bottom:12px}"
  "#preview{width:100%%;display:block;border-radius:6px;background:#161b22;"
            "margin-bottom:16px;min-height:160px;object-fit:contain}"
  ".card{background:#161b22;border:1px solid #30363d;border-radius:6px;"
         "padding:14px;margin-bottom:14px}"
  ".card h3{margin:0 0 12px;font-size:12px;color:#8b949e;"
             "text-transform:uppercase;letter-spacing:1px}"
  ".row{display:flex;align-items:center;margin-bottom:10px}"
  ".row label{flex:1;font-size:14px}"
  ".row input{width:80px;padding:7px;background:#0d1117;border:1px solid #30363d;"
              "border-radius:4px;color:#c9d1d9;font-size:15px;text-align:center}"
  ".row input:focus{outline:none;border-color:#58a6ff}"
  ".unit{margin-left:8px;font-size:13px;color:#8b949e;width:20px}"
  ".btn{display:block;width:100%%;padding:12px;border:none;border-radius:6px;"
        "font-size:15px;font-weight:600;cursor:pointer;margin-top:6px}"
  ".btn-save{background:#21262d;color:#c9d1d9;border:1px solid #30363d}"
  ".btn-start{background:#238636;color:#fff;font-size:16px;padding:14px;margin-top:4px}"
  "#msg{text-align:center;font-size:13px;color:#8b949e;margin-top:10px;min-height:18px}"
  "</style></head><body>"
  "<h2>카메라 설정</h2>"
  "<div id=\"timer\"></div>"
  "<img id=\"preview\" src=\"/capture\" alt=\"카메라 미리보기\">"
  "<div class=\"card\">"
  "<h3>촬영 설정</h3>"
  "<div class=\"row\">"
    "<label>촬영 간격</label>"
    "<input type=\"number\" id=\"intv\" min=\"1\" max=\"1440\" value=\"%d\">"
    "<span class=\"unit\">분</span>"
  "</div>"
  "<div class=\"row\">"
    "<label>전송 묶음</label>"
    "<input type=\"number\" id=\"cnt\" min=\"1\" max=\"100\" value=\"%d\">"
    "<span class=\"unit\">회</span>"
  "</div>"
  "<button class=\"btn btn-save\" onclick=\"saveCfg()\">설정 저장</button>"
  "</div>"
  "<button class=\"btn btn-start\" onclick=\"startOp()\">운영 시작</button>"
  "<div id=\"msg\"></div>"
  // ── JavaScript: 전역 스코프에 선언해야 onclick 속성에서 호출 가능 ──
  "<script>"
  "var end=Date.now()+%d;"
  "function tick(){"
    "var r=Math.max(0,end-Date.now());"
    "var m=Math.floor(r/60000),s=Math.floor((r%%60000)/1000);"
    "document.getElementById('timer').textContent="
      "'남은 시간: '+('0'+m).slice(-2)+':'+('0'+s).slice(-2);"
    "if(r>0)setTimeout(tick,500);"
    "else document.getElementById('timer').textContent='시간 초과 — 운영 모드로 전환';"
  "}"
  "tick();"
  "var busy=false;"
  "function refreshImg(){"
    "if(busy)return;busy=true;"
    "var i=new Image();"
    "i.onload=function(){document.getElementById('preview').src=this.src;busy=false;};"
    "i.onerror=function(){busy=false;};"
    "i.src='/capture?t='+Date.now();"
  "}"
  "setInterval(refreshImg,1000);"
  "function saveCfg(){"
    "var intv=document.getElementById('intv').value;"
    "var cnt=document.getElementById('cnt').value;"
    "var msg=document.getElementById('msg');"
    "msg.textContent='저장 중...';"
    "fetch('/set',{method:'POST',"
      "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:'intv='+intv+'&cnt='+cnt})"
    ".then(function(r){return r.text();})"
    ".then(function(t){msg.textContent='저장됨: '+t;})"
    ".catch(function(){msg.textContent='저장 실패';});"
  "}"
  "function startOp(){"
    "document.getElementById('msg').textContent='운영 모드로 전환 중...';"
    "fetch('/start',{method:'POST'})"
    ".then(function(){"
      "document.getElementById('msg').textContent='전환 완료. WiFi가 종료됩니다.';"
    "})"
    ".catch(function(){document.getElementById('msg').textContent='전환 실패';});"
  "}"
  "</script>"
  "</body></html>";

// ─────────────────────────────────────────────────────────────────────────────
// GET /  — HTML 설정 페이지
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t index_handler(httpd_req_t *req)
{
  uint32_t elapsed_ms = millis() - g_setupStartMs;
  int remaining_ms = (elapsed_ms < SETUP_AP_TIMEOUT_MS)
                     ? (int)(SETUP_AP_TIMEOUT_MS - elapsed_ms)
                     : 0;

  char *page = (char *)malloc(4096);
  if (!page) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  int len = snprintf(page, 4096, HTML_TEMPLATE,
                     g_captureIntervalMin,
                     g_captureTarget,
                     remaining_ms);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  esp_err_t ret = httpd_resp_send(req, page, len);
  free(page);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /capture  — 카메라 스냅샷 반환 (미리보기용, SD 저장 없음)
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t capture_handler(httpd_req_t *req)
{
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=preview.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store");

  esp_err_t ret;
  if (fb->format == PIXFORMAT_JPEG) {
    ret = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
    // JPEG 이외 포맷이면 변환
    uint8_t *jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
    esp_camera_fb_return(fb);
    fb = NULL;
    if (!ok) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    ret = httpd_resp_send(req, (const char *)jpg_buf, jpg_len);
    free(jpg_buf);
    return ret;
  }
  if (fb) esp_camera_fb_return(fb);
  return ret;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST body 에서 key=value 를 찾아 정수로 반환하는 헬퍼.
// body: null-terminated string, e.g. "intv=10&cnt=2"
// key: 찾을 키 이름 (e.g. "intv")
// 반환: 파싱 성공 시 정수값, 실패 시 def(기본값)
// ─────────────────────────────────────────────────────────────────────────────
static int parseBodyInt(const char *body, const char *key, int def)
{
  // "key=" 패턴 검색 — 반드시 문자열 첫 위치이거나 '&' 바로 뒤에 있어야 함
  size_t klen = strlen(key);
  const char *p = body;
  while ((p = strstr(p, key)) != NULL) {
    if ((p == body || *(p - 1) == '&') && *(p + klen) == '=') {
      return atoi(p + klen + 1);   // '=' 다음부터 atoi (비숫자에서 자동 종료)
    }
    p++;
  }
  return def;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /set  — 촬영 간격·전송 묶음 저장
//   body: intv=<n>&cnt=<n>  (application/x-www-form-urlencoded)
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t set_handler(httpd_req_t *req)
{
  // content_len 기반으로 정확한 바이트 수 수신
  int total = (int)req->content_len;
  if (total <= 0 || total > 62) {
    Serial.printf("[SETUP] /set bad content_len=%d\n", total);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  char body[64] = {0};
  int  offset   = 0;
  while (offset < total) {
    int n = httpd_req_recv(req, body + offset, total - offset);
    if (n <= 0) {
      Serial.printf("[SETUP] /set recv error n=%d\n", n);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    offset += n;
  }
  body[offset] = '\0';

  Serial.printf("[SETUP] /set body: \"%s\"\n", body);

  // strstr 기반 파싱 (httpd_query_key_value 는 POST body 파싱에 불안정)
  int new_intv = parseBodyInt(body, "intv", g_captureIntervalMin);
  int new_cnt  = parseBodyInt(body, "cnt",  g_captureTarget);

  // 범위 검증 후 적용
  if (new_intv >= 1 && new_intv <= 1440) g_captureIntervalMin = new_intv;
  if (new_cnt  >= 1 && new_cnt  <= 100)  g_captureTarget      = new_cnt;

  bool saved = saveConfig();

  Serial.printf("[SETUP] Applied — intv=%d min  cnt=%d  (saved=%s)\n",
                g_captureIntervalMin, g_captureTarget, saved ? "OK" : "FAIL");

  char resp[64];
  int  rlen;
  if (saved)
  {
    rlen = snprintf(resp, sizeof(resp),
                    "intv=%d min, cnt=%d", g_captureIntervalMin, g_captureTarget);
  }
  else
  {
    // 설정값은 RAM 에 적용됐지만 SD 기록 실패 → 브라우저에 경고 표시
    rlen = snprintf(resp, sizeof(resp), "[오류] SD 저장 실패 (설정은 적용됨)");
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, rlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /start  — "운영 시작" 버튼
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t start_handler(httpd_req_t *req)
{
  s_startRequested = true;
  Serial.println("[SETUP] '운영 시작' received via HTTP");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// URI 등록 헬퍼 매크로
// ─────────────────────────────────────────────────────────────────────────────
#define REG_URI(srv, uri_str, meth, hndlr)                    \
  do {                                                          \
    httpd_uri_t _u = {                                          \
      .uri     = (uri_str),                                     \
      .method  = (meth),                                        \
      .handler = (hndlr),                                       \
      .user_ctx = NULL                                          \
    };                                                          \
    httpd_register_uri_handler((srv), &_u);                     \
  } while (0)

// ─────────────────────────────────────────────────────────────────────────────
// setup HTTP 서버 내부 시작
// ─────────────────────────────────────────────────────────────────────────────
static void startSetupHttpd()
{
  if (s_httpd) {
    httpd_stop(s_httpd);
    s_httpd = NULL;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 8;
  cfg.server_port      = 80;

  if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
    Serial.println("[SETUP] httpd_start failed");
    return;
  }

  REG_URI(s_httpd, "/",        HTTP_GET,  index_handler);
  REG_URI(s_httpd, "/capture", HTTP_GET,  capture_handler);
  REG_URI(s_httpd, "/set",     HTTP_POST, set_handler);
  REG_URI(s_httpd, "/start",   HTTP_POST, start_handler);

  Serial.println("[SETUP] HTTP server started on port 80");
}

// ─────────────────────────────────────────────────────────────────────────────
// 호흡 무드등 — setup mode 동안 플래시 LED(GPIO1) 를 White 로 천천히 점멸
//
//   [타이밍] esp_timer_get_time() (하드웨어 μs 타이머) 사용
//            loop() delay 나 WiFi 처리 지연에 무관하게 항상 정확한 위상 계산
//
//   [파형]   sin² 감마 보정 적용
//            linear = 0.5 + 0.5·sin(2π·t/T − π/2)   ← 0→1→0 정규화
//            brightness = linear² × 255               ← 감마 보정 (저밝기 부드럽게)
//
//            linear sin:  0→20→127→240→255  (저밝기에서 급격히 올라가 플리커 느낌)
//            sin² 보정:   0→ 2→ 64→225→255  (저밝기를 매우 서서히 시작)
//
//   [주기]   2000ms (변경하려면 BREATH_PERIOD_MS 수정)
//   [속도]   16ms 마다 갱신 (~62Hz) — 50ms loop 내에서도 정밀 타이밍 유지
// ─────────────────────────────────────────────────────────────────────────────
#define BREATH_PERIOD_MS  2000U

static void breathingLedUpdate()
{
  static uint32_t s_lastUpdate = 0;

  // esp_timer_get_time(): 부팅 이후 μs 단위 하드웨어 카운터
  // → millis() 보다 WiFi/루프 지연의 영향을 받지 않아 위상 계산이 정확함
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

  if (now_ms - s_lastUpdate < 16) return;   // ~62Hz 상한
  s_lastUpdate = now_ms;

  // ── 정규화된 선형값 (0.0 ~ 1.0) ─────────────────────────────────
  float phase  = (float)(now_ms % BREATH_PERIOD_MS)
                 / (float)BREATH_PERIOD_MS
                 * 2.0f * (float)M_PI;
  float linear = 0.5f + 0.5f * sinf(phase - (float)M_PI / 2.0f);

  // ── sin² 감마 보정: 저밝기 구간을 부드럽게 ─────────────────────
  uint8_t brightness = (uint8_t)(linear * linear * 255.0f);

  flashLedSet(brightness, brightness, brightness);
}

// ─────────────────────────────────────────────────────────────────────────────
// setup 모드 종료 (내부용)
// ─────────────────────────────────────────────────────────────────────────────
static void exitSetupMode()
{
  Serial.println("[SETUP] Exiting setup mode → operation mode");

  // HTTP 서버 종료
  if (s_httpd) {
    httpd_stop(s_httpd);
    s_httpd = NULL;
  }

  // 호흡 무드등 끄기
  flashLedSet(0, 0, 0);

  // AP 종료 후 WiFi 완전 종료.
  // WIFI_STA 로 전환하면 STA netif 가 먼저 생성되고,
  // 이후 WiFi.begin() 이 wifi_init_default() 에서 콜백을 중복 등록해
  // "netstack cb reg failed" 에러가 발생함.
  // WIFI_OFF 로 완전히 내린 뒤 WiFi.begin() 이 깨끗하게 초기화하도록 함.
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  // ── 다음 캡처 시각 재계산 ──────────────────────────────────────────────
  // set_handler() 에서 g_captureIntervalMin 이 변경되었을 수 있음.
  // simInit() 시점의 nextCaptureTime 은 변경 전 interval 로 계산된 값이므로
  // 여기서 현재 interval 기준으로 다시 계산해야 올바른 경계에서 Sleep / Wake-up 됨.
  // (버튼 누름·5분 타임아웃 모두 이 함수를 거치므로 한 곳에서 처리.)
  if (ntpSynced)
  {
    nextCaptureTime = calcNextBoundary();
    struct tm nextTm;
    localtime_r(&nextCaptureTime, &nextTm);
    Serial.printf("[SETUP] nextCaptureTime recalculated — intv=%d min  next: %02d:%02d:00 KST\n",
                  g_captureIntervalMin, nextTm.tm_hour, nextTm.tm_min);
  }

  g_setupMode = false;
  ledSet(0, 40, 0);   // green: standby
}

// ─────────────────────────────────────────────────────────────────────────────
// 공개 API
// ─────────────────────────────────────────────────────────────────────────────

void enterSetupMode()
{
  Serial.println("[SETUP] Entering setup mode");
  s_startRequested = false;
  g_setupMode      = true;
  g_setupStartMs   = millis();

  // WiFi AP 시작
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_SSID);   // 오픈 네트워크 (비밀번호 없음)

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[SETUP] AP SSID: %s  IP: %s\n", SETUP_AP_SSID, ip.toString().c_str());

  startSetupHttpd();

  ledSet(255, 165, 0);   // orange: setup mode
}

void setupServerLoop()
{
  if (!g_setupMode) return;

  // ── 호흡 무드등 ──
  breathingLedUpdate();

  // ── 5분 타임아웃 ──
  if (millis() - g_setupStartMs >= SETUP_AP_TIMEOUT_MS) {
    Serial.println("[SETUP] Timeout (5 min) — auto-exiting setup mode");
    exitSetupMode();
    return;
  }

  // ── "운영 시작" 버튼 처리 ──
  if (s_startRequested) {
    s_startRequested = false;
    exitSetupMode();
    return;
  }
}
