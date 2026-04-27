#include "esp_camera.h"
#include <WiFi.h>
#include <FastLED.h>
#include "SD_MMC.h"

#include "board_config.h"

// ===========================
// Enter your WiFi credentials
// ===========================
// const char *ssid = "TWOSOME F2";
// const char *password = "twosome123";
// const char *ssid = "AndroidHotspot4216";
// const char *password = "sognatore";
const char *ssid = "KT_GiGA_9748";
const char *password = "9cf0bkd529";

// ===========================
// NTP 시간 동기화 설정 (내부 RTC)
// ===========================
#define NTP_SERVER     "pool.ntp.org"
#define NTP_GMT_OFFSET (9L * 3600L)   // KST = UTC+9
#define NTP_DST_OFFSET 0

void startCameraServer();
void setupLedFlash();

framesize_t current_cam_framesize;
int current_cam_quality;
sensor_t *camera_sensor2 = NULL;

// ─── WS2812B ────────────────────────────────────────────────────────────────
#define WS2812_PIN  38
#define WS2812_NUM  1   // 보드에 장착된 LED 개수

CRGB leds[WS2812_NUM];

void ledSet(uint8_t r, uint8_t g, uint8_t b)
{
  fill_solid(leds, WS2812_NUM, CRGB(r, g, b));
  FastLED.show();
}

void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs)
{
  for (int i = 0; i < n; i++) {
    ledSet(r, g, b);
    delay(periodMs / 2);
    ledSet(0, 0, 0);
    delay(periodMs / 2);
  }
}

// ─── SD 카드 (SDMMC 1-bit) ──────────────────────────────────────────────────
#define SD_CLK_PIN  5   // CLK
#define SD_CMD_PIN  4   // CMD
#define SD_D0_PIN   6   // D0

static bool     sdReady      = false;
static uint32_t captureCount = 0;   // NTP 실패 시 파일명 폴백용

bool sdSetup()
{
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] Mount failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] No card detected");
    return false;
  }
  Serial.printf("[SD] Ready  %llu MB\n",
                SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}

// SD 카드 디렉터리 재귀 생성 (/Data/Image/2026/04/27 각 레벨 개별 생성)
void sdMkdirRecursive(const char *dirPath)
{
  char tmp[64];
  strncpy(tmp, dirPath, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (!SD_MMC.exists(tmp)) SD_MMC.mkdir(tmp);
      *p = '/';
    }
  }
  if (!SD_MMC.exists(tmp)) SD_MMC.mkdir(tmp);
}

// ─── 10분 경계 타이머 ────────────────────────────────────────────────────────
// KST 오프셋(9h = 32400s)은 600의 배수이므로 UTC epoch 기준 계산이 KST 10분과 동일하게 정렬됨
static time_t nextCaptureTime = 0;
static bool   ntpSynced       = false;

// 스트리밍 루프 일시정지 플래그 — app_httpd.cpp 에서 extern으로 참조
volatile bool capturePending = false;

time_t calcNextBoundary()
{
  time_t now;
  time(&now);
  return ((now / 600) + 1) * 600;  // 다음 10분 경계 epoch
}

// app_httpd.cpp 에서 선언된 스트리밍 상태 플래그
extern bool isStreaming;

// ─── 이미지 캡처 & SD 저장 ──────────────────────────────────────────────────
void captureAndSave()
{
  // ── 현재 시각 출력 ──
  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo);

  if (hasTime) {
    Serial.printf("\n[CAP] %04d/%02d/%02d %02d:%02d:%02d  OV5640 FHD 1920x1080 Start Capture.\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    Serial.println("\n[CAP] Time info get failed. Start Capture.");
  }

  if (!sdReady) {
    Serial.println("[CAP] SD not ready, skip");
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }

  // ── 스트리밍 루프 일시정지 요청 ──
  // capturePending = true 로 설정하면 app_httpd.cpp 스트리밍 루프가
  // 현재 프레임 전송 완료 후 fb_get() 호출을 멈추고 대기함
  capturePending = true;
  if (isStreaming) {
    // 스트리밍 태스크가 진행 중인 프레임을 마치고 버퍼를 반납할 때까지 대기
    // 최대 1프레임 전송 시간(약 200ms) + 여유 시간
    delay(300);
    Serial.println("[CAP] Streaming paused for capture");
  }

  // ── 해상도 전환: 스트리밍(HD 1280×720) → FHD (1920×1080) ──
  framesize_t prevFramesize = current_cam_framesize;
  sensor_t   *s             = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_FHD);
  delay(300);   // 센서 안정화 대기

  // 전환 직후 이전 해상도로 버퍼링된 잔류 프레임 버리기
  {
    camera_fb_t *flush = esp_camera_fb_get();
    if (flush) esp_camera_fb_return(flush);
  }

  // ── White Flash ON → 카메라 노출 → 캡처 ──
  ledSet(255, 255, 255);
  delay(200);
  camera_fb_t *fb = esp_camera_fb_get();
  ledSet(0, 40, 0);   // Flash OFF

  // ── 해상도 복원 후 스트리밍 재개 ──
  s->set_framesize(s, prevFramesize);
  current_cam_framesize = prevFramesize;
  capturePending = false;   // 스트리밍 루프 재개

  if (!fb) {
    Serial.println("[CAP] fb_get failed");
    capturePending = false;   // 실패 시에도 스트리밍 재개 보장
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }

  // ── 저장 경로 생성 ──
  size_t imgLen = fb->len;
  char   dirPath[48];
  char   filePath[72];

  if (hasTime) {
    snprintf(dirPath, sizeof(dirPath), "/Data/Image/%04d/%02d/%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
    snprintf(filePath, sizeof(filePath), "%s/%04d%02d%02d_%02d%02d%02d.jpg",
             dirPath,
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    sdMkdirRecursive(dirPath);
  } else {
    snprintf(filePath, sizeof(filePath), "/IMG_%06lu.jpg", (unsigned long)captureCount);
  }

  // ── SD 저장 ──
  File file = SD_MMC.open(filePath, FILE_WRITE);
  if (!file) {
    Serial.printf("[CAP] File open failed: %s\n", filePath);
    esp_camera_fb_return(fb);
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }

  size_t written = file.write(fb->buf, imgLen);
  file.close();
  esp_camera_fb_return(fb);

  if (written == imgLen) {
    captureCount++;
    Serial.printf("[CAP] Saved: %s  (%u bytes)\n", filePath, written);
  } else {
    Serial.printf("[CAP] Write error %u/%u bytes\n", written, imgLen);
    ledBlink(255, 80, 0, 3, 400);   // 주황 = 부분 기록 오류
  }

  ledSet(0, 40, 0);
}

// ─── Camera helpers ─────────────────────────────────────────────────────────
void SetCameraFramesize(int size)
{
  if (size < 0 || size > 21) {
    Serial.println("Not supported framesize!");
    return;
  }
  current_cam_framesize = (framesize_t)size;
  camera_sensor2->set_framesize(camera_sensor2, current_cam_framesize);
  Serial.println("Change the framesize!");
}

void SetCameraQuality(int quality)
{
  if (quality < 10 || quality > 63) {
    Serial.println("Not supported quality!");
    return;
  }
  current_cam_quality = quality;
  camera_sensor2->set_quality(camera_sensor2, current_cam_quality);
}

// ─── setup() ────────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // LED 초기화 — 파란색: 부팅 중
  FastLED.addLeds<WS2812B, WS2812_PIN, RGB>(leds, WS2812_NUM);
  FastLED.setBrightness(255);
  ledSet(0, 0, 50);

  // ── 카메라 설정 ──
  camera_config_t config;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      // PSRAM 버퍼를 QSXGA(5MP)로 초기화 → 런타임 5MP 전환 시 OOM 방지
      config.frame_size   = FRAMESIZE_QSXGA;
      config.jpeg_quality = 10;
      config.fb_count     = 1;
      config.grab_mode    = CAMERA_GRAB_LATEST;
    } else {
      config.frame_size  = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x\n", err);
    ledBlink(255, 0, 0, 10, 200);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("[CAM] Sensor PID: 0x%04X\n", s->id.PID);

  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  camera_sensor2 = esp_camera_sensor_get();
  SetCameraFramesize(11);   // HD 1280×720 (스트리밍 기본 해상도)

  // ── SD 카드 초기화 ──
  if (sdSetup()) {
    sdReady = true;
    ledBlink(0, 255, 0, 2, 200);   // 초록 2회 = SD OK
  } else {
    ledBlink(255, 0, 0, 5, 300);   // 빨간 5회 = SD 없음/실패
  }

  // ── WiFi 연결 — 노란색 점멸 ──
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    ledSet(50, 50, 0);
    delay(250);
    ledSet(0, 0, 0);
    delay(250);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  // ── NTP 동기화 → 내부 RTC에 시각 저장 ──
  configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
  Serial.print("NTP syncing");
  struct tm timeinfo;
  int ntpRetry = 0;
  while (!getLocalTime(&timeinfo) && ntpRetry < 20) {
    delay(500);
    Serial.print(".");
    ntpRetry++;
  }

  if (ntpRetry < 20) {
    ntpSynced = true;
    Serial.printf("\n[RTC] Internel RTC Set Complete : %04d/%02d/%02d %02d:%02d:%02d KST\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    // 다음 10분 경계 계산 및 출력
    nextCaptureTime = calcNextBoundary();
    struct tm nextTm;
    localtime_r(&nextCaptureTime, &nextTm);
    Serial.printf("[CAP] First Image Capture : %02d:%02d:00 KST\n",
                  nextTm.tm_hour, nextTm.tm_min);
  } else {
    Serial.println("\n[RTC] NTP sync failed.");
    ledBlink(255, 80, 0, 5, 200);   // 주황 5회: NTP 실패 경고
  }

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  ledSet(0, 40, 0);   // 초록: 대기 중
}

// ─── loop() ─────────────────────────────────────────────────────────────────
void loop()
{
  if (ntpSynced && nextCaptureTime > 0) {
    time_t now;
    time(&now);

    if (now >= nextCaptureTime) {
      // 캡처 실행 전에 다음 경계를 미리 계산 (captureAndSave 소요 시간 무관하게 정확한 다음 주기 보장)
      nextCaptureTime = ((now / 600) + 1) * 600;

      struct tm nextTm;
      localtime_r(&nextCaptureTime, &nextTm);
      Serial.printf("[CAP] Next Image Capture : %02d:%02d:00 KST\n",
                    nextTm.tm_hour, nextTm.tm_min);

      captureAndSave();
    }
  }

  delay(500);   // 500ms 간격 체크 → 경계 도달 후 최대 ±0.5초 오차
}
