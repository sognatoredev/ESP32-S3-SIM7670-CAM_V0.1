#include "setup_server.h"
#include "config.h"
#include "time_sync.h"      // g_captureIntervalMin
#include "image_capture.h"  // g_captureTarget
#include "sd_storage.h"     // saveConfig
#include "led.h"
#include "ov5640_af.h"
#include "camera_mgr.h"     // SetCameraFramesize, capturePending
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
static httpd_handle_t  s_stream_httpd   = NULL;    // port 81 — MJPEG 전용
static volatile bool   s_startRequested = false;   // /start POST 수신 시 true
static bool            s_ledSteady      = false;   // 3회 디밍 완료 후 LED 고정 플래그

// ── 세팅모드 LED 상태 ─────────────────────────────────────────────────────────
static uint8_t s_ledBrightness = 100;   // 0–100 %
static uint8_t s_ledMask       = 0xFF;  // bit0=LED0 … bit7=LED7

// s_ledSteady == true 일 때 mask + brightness 를 즉시 플래시 LED 에 반영
static void applyFlashLed() {
  if (!s_ledSteady) return;
  uint8_t v = (uint8_t)((uint32_t)s_ledBrightness * 255 / 100);
  flashLedSetMask(s_ledMask, v, v, v);
}

// ── MJPEG 스트리밍 상수 (세팅모드 전용, port 81) ─────────────────────────────
#define SETUP_MJPEG_BOUNDARY  "SetupFrameBound"
static const char *s_streamCT    = "multipart/x-mixed-replace;boundary=" SETUP_MJPEG_BOUNDARY;
static const char *s_frameBound  = "\r\n--" SETUP_MJPEG_BOUNDARY "\r\n";
static const char *s_frameHdr    = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ─────────────────────────────────────────────────────────────────────────────
// HTML 페이지 템플릿
//   snprintf 인자 순서: %d=intv, %d=cnt, %d=remaining_ms
//   CSS/JS 의 리터럴 % 는 모두 %% 로 이스케이프
// ─────────────────────────────────────────────────────────────────────────────
// snprintf 인수 순서: %d=intv, %d=cnt, %d=remaining_ms
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
  ".card h3{margin:0 0 10px;font-size:12px;color:#8b949e;"
             "text-transform:uppercase;letter-spacing:1px}"
  ".row{display:flex;align-items:center;margin-bottom:8px}"
  ".row label{flex:1;font-size:14px}"
  ".row input[type=number]{width:80px;padding:7px;background:#0d1117;"
    "border:1px solid #30363d;border-radius:4px;color:#c9d1d9;font-size:15px;"
    "text-align:center}"
  ".row input[type=number]:focus{outline:none;border-color:#58a6ff}"
  ".unit{margin-left:8px;font-size:13px;color:#8b949e;width:20px}"
  // 슬라이더
  ".row input[type=range]{flex:1;margin:0 8px;accent-color:#58a6ff}"
  // LED 체크박스 그리드
  "#ledgrid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-bottom:8px}"
  ".lchk{display:flex;flex-direction:column;align-items:center;font-size:12px;"
         "background:#0d1117;border:1px solid #30363d;border-radius:4px;padding:6px}"
  ".lchk input{width:18px;height:18px;margin-top:4px;accent-color:#58a6ff}"
  // 버튼
  ".btn{display:block;width:100%%;padding:11px;border:none;border-radius:6px;"
        "font-size:14px;font-weight:600;cursor:pointer;margin-top:5px}"
  ".btn-save{background:#21262d;color:#c9d1d9;border:1px solid #30363d}"
  ".btn-start{background:#238636;color:#fff;font-size:16px;padding:13px;margin-top:4px}"
  ".btn-row{display:flex;gap:6px}"
  ".btn-row .btn{flex:1}"
  "#msg{text-align:center;font-size:13px;color:#8b949e;margin-top:10px;min-height:18px}"
  "</style></head><body>"
  "<h2>카메라 설정</h2>"
  "<div id=\"timer\"></div>"
  "<img id=\"preview\" src=\"\" alt=\"카메라 미리보기\">"

  // ── 촬영 설정 ──
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

    // ── 포커스 설정 ──
  "<div class=\"card\">"
  "<h3>포커스 설정</h3>"
  "<div class=\"row\">"
    "<label>저장된 포커스</label>"
    "<span id=\"fpos_saved\" style=\"font-size:14px\">자동 AF</span>"
  "</div>"
  "<div class=\"row\">"
    "<label>현재 VCM 위치</label>"
    "<span id=\"fpos_cur\" style=\"font-size:14px\">-</span>"
    "<span class=\"unit\"></span>"
  "</div>"
  "<button class=\"btn btn-save\" onclick=\"doAF()\" style=\"margin-bottom:6px\">"
    "AF 실행 (자동 초점 탐색)</button>"
  "<div style=\"display:flex;gap:6px;align-items:stretch;margin-bottom:6px\">"
    "<button class=\"btn btn-save\" style=\"flex:1;font-size:22px;padding:8px\""
      " onclick=\"doVcm(-5)\">&#x2212;</button>"
    "<span style=\"flex:2;display:flex;align-items:center;justify-content:center;"
                  "font-size:12px;color:#8b949e\">렌즈 수동 조절<br>(0=무한 ~ 1023=접사)</span>"
    "<button class=\"btn btn-save\" style=\"flex:1;font-size:22px;padding:8px\""
      " onclick=\"doVcm(5)\">&#x2B;</button>"
  "</div>"
  "<div class=\"btn-row\">"
    "<button class=\"btn btn-save\" onclick=\"saveFocus()\">포커스 저장</button>"
    "<button class=\"btn btn-save\" style=\"color:#f85149\" onclick=\"clearFocus()\">"
      "초기화 (자동 AF)</button>"
  "</div>"
  "<div id=\"fmsg\" style=\"text-align:center;font-size:13px;color:#8b949e;"
                            "margin-top:8px;min-height:18px\"></div>"
  "</div>"

  // ── 플래시 LED 밝기 ──
  "<div class=\"card\">"
  "<h3>플래시 LED 밝기</h3>"
  "<div class=\"row\">"
    "<label>밝기</label>"
    "<input type=\"range\" id=\"lbright\" min=\"0\" max=\"100\" value=\"100\""
           " oninput=\"onBright(this.value)\">"
    "<span id=\"lbval\" style=\"width:38px;text-align:right\">100%%</span>"
  "</div>"
  "</div>"

  // ── 플래시 LED 선택 ──
  "<div class=\"card\">"
  "<h3>플래시 LED 선택</h3>"
  "<div id=\"ledgrid\">"
    "<div class=\"lchk\">L1<input type=\"checkbox\" id=\"l0\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L2<input type=\"checkbox\" id=\"l1\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L3<input type=\"checkbox\" id=\"l2\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L4<input type=\"checkbox\" id=\"l3\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L5<input type=\"checkbox\" id=\"l4\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L6<input type=\"checkbox\" id=\"l5\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L7<input type=\"checkbox\" id=\"l6\" checked onchange=\"sendMask()\"></div>"
    "<div class=\"lchk\">L8<input type=\"checkbox\" id=\"l7\" checked onchange=\"sendMask()\"></div>"
  "</div>"
  "<div class=\"btn-row\">"
    "<button class=\"btn btn-save\" onclick=\"allLed(1)\">모두 켜기</button>"
    "<button class=\"btn btn-save\" onclick=\"allLed(0)\">모두 끄기</button>"
  "</div>"
  "</div>"

  // ── AP 이름(Wi-Fi SSID) 변경 ──
  "<div class=\"card\">"
  "<h3>AP 이름 (Wi-Fi SSID)</h3>"
  "<div class=\"row\">"
    "<input type=\"text\" id=\"apname\" maxlength=\"32\""
           " style=\"flex:1;padding:7px;background:#0d1117;border:1px solid #30363d;"
                    "border-radius:4px;color:#c9d1d9;font-size:15px\""
           " placeholder=\"영문/숫자/-/_ 만 허용\">"
  "</div>"
  "<button class=\"btn btn-save\" onclick=\"saveApName()\">AP 이름 저장</button>"
  "<div id=\"apmsg\" style=\"text-align:center;font-size:13px;color:#8b949e;"
                            "margin-top:8px;min-height:18px\">"
    "저장 시 다음 세팅모드 진입부터 적용됩니다 (지금 접속은 유지됨)</div>"
  "</div>"

  "<button class=\"btn btn-start\" onclick=\"startOp()\">운영 시작</button>"
  "<div id=\"msg\"></div>"

  "<script>"
  // 타이머
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
  // MJPEG 스트림 연결
  "document.getElementById('preview').src='http://'+location.hostname+':81/stream';"
  // 초기 상태 로드
  "fetch('/state').then(function(r){return r.json();}).then(function(d){"
    "document.getElementById('lbright').value=d.b;"
    "document.getElementById('lbval').textContent=d.b+'%%';"
    "for(var i=0;i<8;i++)"
      "document.getElementById('l'+i).checked=!!(d.m&(1<<i));"
    "document.getElementById('fpos_saved').textContent=d.fp>=0?'VCM='+d.fp:'자동 AF';"
    "document.getElementById('fpos_cur').textContent=d.vp;"
    "document.getElementById('apname').value=d.ap;"
  "}).catch(function(){});"
  // 밝기
  "function onBright(v){"
    "document.getElementById('lbval').textContent=v+'%%';"
    "fetch('/led',{method:'POST',"
      "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:'brightness='+v}).catch(function(){});"
  "}"
  // LED 마스크
  "function getMask(){"
    "var m=0;"
    "for(var i=0;i<8;i++)if(document.getElementById('l'+i).checked)m|=(1<<i);"
    "return m;"
  "}"
  "function sendMask(){"
    "fetch('/led_select',{method:'POST',"
      "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:'mask='+getMask()}).catch(function(){});"
  "}"
  "function allLed(on){"
    "for(var i=0;i<8;i++)document.getElementById('l'+i).checked=!!on;"
    "sendMask();"
  "}"
  // 포커스: AF 실행
  "function doAF(){"
    "var msg=document.getElementById('fmsg');"
    "msg.textContent='AF 실행 중...';"
    "fetch('/af',{method:'POST'})"
    ".then(function(r){return r.text();})"
    ".then(function(t){"
      "msg.textContent=t;"
      "var m=t.match(/VCM=(\\d+)/);"
      "if(m)document.getElementById('fpos_cur').textContent=m[1];"
    "})"
    ".catch(function(){msg.textContent='AF 실패';});"
  "}"
  // 포커스: 수동 VCM 조절
  "function doVcm(step){"
    "var msg=document.getElementById('fmsg');"
    "fetch('/vcm',{method:'POST',"
      "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:'step='+step})"
    ".then(function(r){return r.text();})"
    ".then(function(t){"
      "document.getElementById('fpos_cur').textContent=t;"
      "msg.textContent='VCM: '+t;"
    "})"
    ".catch(function(){});"
  "}"
  // 포커스: 현재 VCM 위치 저장
  "function saveFocus(){"
    "var msg=document.getElementById('fmsg');"
    "msg.textContent='저장 중...';"
    "fetch('/save_focus',{method:'POST'})"
    ".then(function(r){return r.text();})"
    ".then(function(t){"
      "msg.textContent='포커스 저장됨: VCM='+t;"
      "document.getElementById('fpos_saved').textContent='VCM='+t;"
    "})"
    ".catch(function(){msg.textContent='저장 실패';});"
  "}"
  // 포커스: 저장 초기화 (자동 AF 복원)
  "function clearFocus(){"
    "var msg=document.getElementById('fmsg');"
    "fetch('/clear_focus',{method:'POST'})"
    ".then(function(r){return r.text();})"
    ".then(function(){"
      "msg.textContent='초기화됨 — 자동 AF 사용';"
      "document.getElementById('fpos_saved').textContent='자동 AF';"
    "})"
    ".catch(function(){});"
  "}"
  // 촬영 설정 저장
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
  // AP 이름 저장
  "function saveApName(){"
    "var v=document.getElementById('apname').value;"
    "var msg=document.getElementById('apmsg');"
    "msg.textContent='저장 중...';"
    "fetch('/set_ap',{method:'POST',"
      "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:'ap='+encodeURIComponent(v)})"
    ".then(function(r){return r.text();})"
    ".then(function(t){msg.textContent=t;})"
    ".catch(function(){msg.textContent='저장 실패';});"
  "}"
  // 운영 시작
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

  char *page = (char *)malloc(10240);
  if (!page) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  int len = snprintf(page, 10240, HTML_TEMPLATE,
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
// POST /set_ap  — AP 이름(Wi-Fi SSID) 저장
//   body: ap=<name>  (application/x-www-form-urlencoded, encodeURIComponent 적용됨)
//   허용 문자: 영문/숫자/-/_ 만 (isValidApName). 그 외 문자는 encodeURIComponent 가
//   %XX 로 치환하므로 '%' 자체가 charset 검증에 걸려 자동으로 거부됨 — 별도 URL
//   디코딩 불필요.
//   변경은 즉시 반영되지 않고 config.txt 에만 저장 — 다음 세팅모드 진입
//   (enterSetupMode() 의 WiFi.softAP() 호출) 시점부터 적용됨. 지금 접속 중인
//   AP 세션을 끊지 않기 위한 설계.
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t set_ap_handler(httpd_req_t *req)
{
  int total = (int)req->content_len;
  if (total <= 0 || total > 48) {
    Serial.printf("[SETUP] /set_ap bad content_len=%d\n", total);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  char body[50] = {0};
  int  offset   = 0;
  while (offset < total) {
    int n = httpd_req_recv(req, body + offset, total - offset);
    if (n <= 0) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    offset += n;
  }
  body[offset] = '\0';

  Serial.printf("[SETUP] /set_ap body: \"%s\"\n", body);

  // "ap=" 뒤부터 '&' 전까지 추출 (최대 32자)
  const char *key = "ap=";
  char       *p   = strstr(body, key);
  char        newName[33] = {0};
  int         nlen = 0;
  if (p) {
    p += strlen(key);
    while (*p && *p != '&' && nlen < 32) newName[nlen++] = *p++;
  }

  char resp[64];
  int  rlen;

  if (nlen > 0 && isValidApName(newName, nlen))
  {
    memcpy(g_apName, newName, nlen);
    g_apName[nlen] = '\0';
    bool saved = saveConfig();
    Serial.printf("[SETUP] AP name set: %s  (saved=%s)\n", g_apName, saved ? "OK" : "FAIL");
    if (saved)
      rlen = snprintf(resp, sizeof(resp), "저장됨: %s (다음 진입부터 적용)", g_apName);
    else
      rlen = snprintf(resp, sizeof(resp), "[오류] SD 저장 실패 (이름은 적용됨)");
  }
  else
  {
    rlen = snprintf(resp, sizeof(resp), "[오류] 1-32자, 영문/숫자/-/_ 만 허용");
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
// GET /state  — 현재 LED·줌 상태를 JSON 으로 반환 (페이지 초기 로드용)
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t state_handler(httpd_req_t *req)
{
  int vcmPos = cameraGetVcmPos();
  char json[140];   // g_apName(최대 32자) 포함하도록 여유 있게 확장
  snprintf(json, sizeof(json),
           "{\"b\":%d,\"m\":%d,\"fp\":%d,\"vp\":%d,\"ap\":\"%s\"}",
           (int)s_ledBrightness, (int)s_ledMask,
           g_savedFocusPos, vcmPos, g_apName);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, json, strlen(json));
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /led  — 플래시 LED 밝기 설정
//   body: brightness=<0–100>
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t led_handler(httpd_req_t *req)
{
  int total = (int)req->content_len;
  if (total <= 0 || total > 32) { httpd_resp_send_500(req); return ESP_FAIL; }
  char body[34] = {0};
  httpd_req_recv(req, body, total);
  int v = parseBodyInt(body, "brightness", (int)s_ledBrightness);
  if (v < 0)   v = 0;
  if (v > 100) v = 100;
  s_ledBrightness   = (uint8_t)v;
  g_flashBrightness = v;           // 글로벌 즉시 반영 → 운영모드 캡처에 사용
  applyFlashLed();
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /led_select  — 플래시 LED 마스크 설정
//   body: mask=<0–255>
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t led_select_handler(httpd_req_t *req)
{
  int total = (int)req->content_len;
  if (total <= 0 || total > 16) { httpd_resp_send_500(req); return ESP_FAIL; }
  char body[18] = {0};
  httpd_req_recv(req, body, total);
  int v = parseBodyInt(body, "mask", (int)s_ledMask);
  s_ledMask   = (uint8_t)(v & 0xFF);
  g_flashMask = s_ledMask;    // 글로벌 즉시 반영 → 운영모드 캡처에 사용
  applyFlashLed();
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /af  — SAF 트리거 + 완료 대기 (세팅모드 "AF 실행" 버튼)
//   응답: "AF 완료  VCM=<n>" 또는 "AF 타임아웃  VCM=<n>"
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t af_handler(httpd_req_t *req)
{
  int pos = cameraDoSingleAF(3000);
  char resp[48];
  int rlen;
  if (pos < 0)
    rlen = snprintf(resp, sizeof(resp), "AF 실패 (렌즈 범위 초과)");
  else
    rlen = snprintf(resp, sizeof(resp), "AF 완료  VCM=%d", pos);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, rlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /vcm  — VCM 위치 수동 ±조절 (렌즈 수동 이동)
//   body: step=<N>  (양수=접사 방향, 음수=무한대 방향)
//   응답: 적용 후 VCM 위치 숫자 문자열
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t vcm_handler(httpd_req_t *req)
{
  int total = (int)req->content_len;
  if (total <= 0 || total > 24) { httpd_resp_send_500(req); return ESP_FAIL; }
  char body[26] = {0};
  httpd_req_recv(req, body, total);
  int step   = parseBodyInt(body, "step", 0);
  int curPos = cameraGetVcmPos();
  if (curPos < 0) curPos = 0;
  cameraApplyFocusPos(curPos + step);
  int newPos = cameraGetVcmPos();   // 실제 기록값 읽기
  char resp[12];
  int rlen = snprintf(resp, sizeof(resp), "%d", newPos);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, rlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /save_focus  — 현재 VCM 위치를 g_savedFocusPos 에 저장
//   응답: 저장된 VCM 위치 숫자 문자열
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t save_focus_handler(httpd_req_t *req)
{
  int pos = cameraGetVcmPos();
  if (pos < 0) pos = 0;
  g_savedFocusPos = pos;
  bool saved = saveConfig();
  Serial.printf("[SETUP] Focus saved: VCM=%d  (SD=%s)\n",
                pos, saved ? "OK" : "FAIL");
  char resp[12];
  int rlen = snprintf(resp, sizeof(resp), "%d", pos);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, resp, rlen);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /clear_focus  — 저장된 포커스 초기화 (운영모드 SAF 자동 실행으로 복원)
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t clear_focus_handler(httpd_req_t *req)
{
  g_savedFocusPos = FOCUS_POS_UNSET;
  saveConfig();
  Serial.println("[SETUP] Focus cleared — will use auto SAF on capture");
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
// GET /stream  — MJPEG 연속 스트리밍 (port 81 전용)
//
//   multipart/x-mixed-replace 형식으로 JPEG 프레임을 끊김 없이 전송.
//   브라우저의 <img src="http://<host>:81/stream"> 가 수신하여 실시간 표시.
//   g_setupMode 가 false 가 되거나 클라이언트 연결이 끊기면 루프 종료.
// ─────────────────────────────────────────────────────────────────────────────
static esp_err_t setup_stream_handler(httpd_req_t *req)
{
  esp_err_t res = httpd_resp_set_type(req, s_streamCT);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control",               "no-cache");

  char frame_hdr[64];

  while (g_setupMode)
  {
    // SD 실사 캡처 중이면 프레임 버퍼 경쟁 방지를 위해 잠시 대기
    while (capturePending) delay(10);

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }

    uint8_t *jpg     = fb->buf;
    size_t   jpgLen  = fb->len;
    bool     needFree = false;

    if (fb->format != PIXFORMAT_JPEG) {
      needFree = frame2jpg(fb, 80, &jpg, &jpgLen);
      esp_camera_fb_return(fb);
      fb = NULL;
      if (!needFree) { res = ESP_FAIL; break; }
    }

    // 경계 + 헤더 + 이미지 데이터를 청크로 전송
    res = httpd_resp_send_chunk(req, s_frameBound, strlen(s_frameBound));
    if (res == ESP_OK) {
      size_t hlen = snprintf(frame_hdr, sizeof(frame_hdr), s_frameHdr, jpgLen);
      res = httpd_resp_send_chunk(req, frame_hdr, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)jpg, jpgLen);
    }

    if (fb)       { esp_camera_fb_return(fb); }
    else if (needFree) { free(jpg); }

    if (res != ESP_OK) break;   // 클라이언트 연결 끊김
  }

  httpd_resp_send_chunk(req, NULL, 0);   // 스트림 종료 신호
  return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// MJPEG 전용 httpd 시작 (port 81)
// ─────────────────────────────────────────────────────────────────────────────
static void startSetupStreamHttpd()
{
  if (s_stream_httpd) {
    httpd_stop(s_stream_httpd);
    s_stream_httpd = NULL;
  }

  httpd_config_t cfg  = HTTPD_DEFAULT_CONFIG();
  cfg.server_port      = 81;
  cfg.ctrl_port        = 32769;
  cfg.max_uri_handlers = 2;

  if (httpd_start(&s_stream_httpd, &cfg) != ESP_OK) {
    Serial.println("[SETUP] Stream httpd start failed");
    return;
  }

  httpd_uri_t u = {
    .uri      = "/stream",
    .method   = HTTP_GET,
    .handler  = setup_stream_handler,
    .user_ctx = NULL
  };
  httpd_register_uri_handler(s_stream_httpd, &u);
  Serial.println("[SETUP] Stream httpd started on port 81");
}

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
  cfg.max_uri_handlers = 13;   // /, /capture, /set, /set_ap, /start, /state, /led, /led_select, /af, /vcm, /save_focus, /clear_focus
  cfg.server_port      = 80;

  if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
    Serial.println("[SETUP] httpd_start failed");
    return;
  }

  REG_URI(s_httpd, "/",             HTTP_GET,  index_handler);
  REG_URI(s_httpd, "/capture",     HTTP_GET,  capture_handler);
  REG_URI(s_httpd, "/set",         HTTP_POST, set_handler);
  REG_URI(s_httpd, "/set_ap",      HTTP_POST, set_ap_handler);
  REG_URI(s_httpd, "/start",       HTTP_POST, start_handler);
  REG_URI(s_httpd, "/state",       HTTP_GET,  state_handler);
  REG_URI(s_httpd, "/led",         HTTP_POST, led_handler);
  REG_URI(s_httpd, "/led_select",  HTTP_POST, led_select_handler);
  REG_URI(s_httpd, "/af",          HTTP_POST, af_handler);
  REG_URI(s_httpd, "/vcm",         HTTP_POST, vcm_handler);
  REG_URI(s_httpd, "/save_focus",  HTTP_POST, save_focus_handler);
  REG_URI(s_httpd, "/clear_focus", HTTP_POST, clear_focus_handler);

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
  // 3사이클 완료 후 LED 고정 상태에서는 즉시 반환 (LED 값 변경 없음)
  if (s_ledSteady) return;

  static uint32_t s_lastUpdate = 0;

  // esp_timer_get_time(): 부팅 이후 μs 단위 하드웨어 카운터
  // → millis() 보다 WiFi/루프 지연의 영향을 받지 않아 위상 계산이 정확함
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

  if (now_ms - s_lastUpdate < 16) return;   // ~62Hz 상한
  s_lastUpdate = now_ms;

  // ── 세팅모드 진입 이후 경과 시간으로 완료된 사이클 수 산출 ──────
  uint32_t elapsed    = millis() - g_setupStartMs;
  uint32_t cyclesDone = elapsed / BREATH_PERIOD_MS;

  if (cyclesDone >= 3) {
    // 3회 디밍 완료 → 사용자 mask + brightness 로 고정
    s_ledSteady = true;
    applyFlashLed();
    Serial.println("[SETUP] Flash LED: 3x dim done → steady ON");
    return;
  }

  // ── 현재 사이클 내 위상 계산 ─────────────────────────────────────
  float phase  = (float)(elapsed % BREATH_PERIOD_MS)
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

  // g_setupMode 를 먼저 false 로 설정.
  // setup_stream_handler 의 while(g_setupMode) 루프가 다음 프레임에서
  // 자연 종료되어야 httpd_stop() 이 데드락 없이 완료됨.
  g_setupMode = false;

  // HTTP 서버 종료 (스트림 httpd 먼저 — WiFi 종료 전에 소켓을 정리)
  if (s_stream_httpd) {
    httpd_stop(s_stream_httpd);
    s_stream_httpd = NULL;
  }
  if (s_httpd) {
    httpd_stop(s_httpd);
    s_httpd = NULL;
  }

  // 플래시 LED 끄기 (고정 ON 상태였을 수 있음)
  flashLedSet(0, 0, 0);

  // 카메라 해상도 HD(1280×720) 복원
  // 세팅모드 진입 시 SVGA 로 낮췄던 것을 운영 모드 기본값으로 되돌림
  SetCameraFramesize(FRAMESIZE_HD);

  // 세팅모드에서 변경한 LED 밝기·마스크·줌 설정을 config.txt 에 저장.
  // "운영 시작" 버튼 또는 5분 타임아웃 양쪽 모두 이 함수를 거치므로 한 곳에서 처리.
  saveConfig();

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

  ledSet(0, 40, 0);   // green: standby
}

// ─────────────────────────────────────────────────────────────────────────────
// 공개 API
// ─────────────────────────────────────────────────────────────────────────────

void enterSetupMode()
{
  Serial.println("[SETUP] Entering setup mode");
  s_startRequested = false;
  s_ledSteady      = false;   // LED 3회 디밍 카운터 초기화
  s_ledBrightness  = (uint8_t)g_flashBrightness;  // 저장된 값으로 초기화
  s_ledMask        = g_flashMask;                 // 저장된 값으로 초기화
  g_setupMode      = true;
  g_setupStartMs   = millis();

  // WiFi AP 시작
  // g_apName: config.txt "ap_name=" 로 저장된 값 (없으면 SETUP_AP_SSID 기본값).
  // 웹/시리얼에서 이름을 바꿔도 이 함수가 다시 호출되기 전까지는 반영되지 않음
  // (현재 접속 중인 AP 세션을 끊지 않기 위해 "다음 진입부터 적용" 방식으로 설계됨).
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_apName);   // 오픈 네트워크 (비밀번호 없음)

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[SETUP] AP SSID: %s  IP: %s\n", g_apName, ip.toString().c_str());

  startSetupHttpd();

  // MJPEG 스트리밍 전용 httpd 시작 (port 81)
  startSetupStreamHttpd();

  // ① CAF 먼저 시작 — 해상도 변경 전에 트리거해야 8051 MCU 상태가 안정적.
  //   set_framesize() 이후 CAF 를 트리거하면 ISP 파이프라인 재구성 중에
  //   명령이 전달되어 AF 가 정상 동작하지 않을 수 있음.
  if (ov5640AfTriggerContinuous() == 0)
    Serial.println("[SETUP] AF continuous mode started");
  else
    Serial.println("[SETUP] AF continuous mode skipped (no AF module)");

  // ② 해상도를 SVGA(800×600)로 낮춰 MJPEG 프레임레이트 향상.
  //   CAF 는 해상도와 무관하게 계속 동작하므로 순서 교환 후에도 AF 유지됨.
  //   운영 모드 복귀 시 exitSetupMode() 에서 HD(1280×720) 로 복원됨.
  SetCameraFramesize(FRAMESIZE_SVGA);

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
