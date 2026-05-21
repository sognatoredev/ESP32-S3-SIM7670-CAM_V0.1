#include "image_capture.h"
#include "config.h"
#include "camera_mgr.h"
#include "sd_storage.h"
#include "sim_modem.h"
#include "image_tx.h"
#include "led.h"
#include "app_httpd.h"
#include "time_sync.h"
#include "esp_camera.h"
#include "SD_MMC.h"
#include <Arduino.h>
#include <time.h>

// RTC_DATA_ATTR: Deep Sleep 복귀 시에도 값 보존.
// captureCount  — 파일명 fallback 카운터 (NTP 미동기 시 사용)
// g_captureTarget — TX 경계 주기 설정값 (txPeriod = interval × cnt 분)
RTC_DATA_ATTR static uint32_t captureCount    = 0;
RTC_DATA_ATTR        int      g_captureTarget = 1;
int             g_lastCaptureWidth  = 0;
int             g_lastCaptureHeight = 0;

// ─────────────────────────────────────────────────────────────────────────────
// TX 경계 판별 헬퍼
//
// TX 주기(txPeriodMin) = g_captureIntervalMin × g_captureTarget 분.
// 자정 기준 누적 분(hour×60+min)이 txPeriodMin 의 배수인 시각을 TX 경계로 정의.
//
//   예) interval=2, cnt=15 → txPeriod=30 분
//       :00, :30 마다 TX.
//
//   예) interval=5, cnt=6 → txPeriod=30 분
//       :00, :30 마다 TX.
//
// isTxBoundary(t)         : t 가 TX 경계이면 true.
// calcNextTxBoundary(t)   : t 이후(포함) 다음 TX 경계 time_t (로그용).
// ─────────────────────────────────────────────────────────────────────────────
static bool isTxBoundary(time_t t)
{
  int txPeriodMin = g_captureIntervalMin * g_captureTarget;
  if (txPeriodMin <= 0) return false;
  struct tm tm_s;
  localtime_r(&t, &tm_s);
  int minOfDay = tm_s.tm_hour * 60 + tm_s.tm_min;
  return (minOfDay % txPeriodMin == 0);
}

static time_t calcNextTxBoundary(time_t afterTime)
{
  int txPeriodMin = g_captureIntervalMin * g_captureTarget;
  if (txPeriodMin <= 0) return afterTime;
  struct tm tm_s;
  localtime_r(&afterTime, &tm_s);
  int minOfDay  = tm_s.tm_hour * 60 + tm_s.tm_min;
  int remainder = minOfDay % txPeriodMin;
  int minsToNext = (remainder == 0) ? txPeriodMin : (txPeriodMin - remainder);
  // 현재 분의 초(sec) 를 빼서 정확히 분 경계로 맞춤
  time_t base = afterTime - (time_t)tm_s.tm_sec;
  return base + (time_t)(minsToNext * 60);
}

// ─────────────────────────────────────────────────────────────────────────────
// performCapture()  —  static helper
//
// 카메라 해상도 전환 → 노출 안정화 → 플래시 점등 → 캡처 → JPEG 검증 → SD 저장.
// 성공: 저장된 파일 경로(String) 반환.
// 실패: "" 반환 (캡처 불가 / JPEG 오류 / SD 쓰기 오류).
// ─────────────────────────────────────────────────────────────────────────────
static String performCapture()
{
  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo);

  if (hasTime)
  {
    Serial.printf("\n[CAP] %04d/%02d/%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
  else
  {
    Serial.println("\n[CAP] No time sync");
  }

  if (!sdReady)
  {
    Serial.println("[CAP] SD not ready, skipping");
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return "";
  }

  capturePending = true;

  // Switch to XGA for the still capture (1024×768).
  framesize_t prevFramesize = current_cam_framesize;
  sensor_t   *s             = esp_camera_sensor_get();
  int         prevQuality   = s->status.quality;
  // s->set_framesize(s, FRAMESIZE_FHD);  // 1920×1080
  // s->set_framesize(s, FRAMESIZE_HD);   // 1280×720
  s->set_framesize(s, FRAMESIZE_XGA);     // 1024×768  (현재 선택)
  // s->set_framesize(s, FRAMESIZE_SVGA); // 800×600
  s->set_quality(s, 12);
  delay(300);

  // Discard 3 stale frames after resolution change.
  for (int i = 0; i < 3; i++)
  {
    camera_fb_t *fl = esp_camera_fb_get();
    if (fl) esp_camera_fb_return(fl);
    delay(50);
  }

  // Flash on → capture → flash off
  ledSet(255, 255, 255);
  flashLedSet(255, 255, 255);
  delay(200);
  camera_fb_t *fb = esp_camera_fb_get();
  flashLedSet(0, 0, 0);
  ledSet(0, 40, 0);

  s->set_framesize(s, prevFramesize);
  s->set_quality(s, prevQuality);
  current_cam_framesize = prevFramesize;
  capturePending = false;

  if (!fb)
  {
    Serial.println("[CAP] Capture failed");
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return "";
  }

  size_t imgLen = fb->len;

  // Validate JPEG
  {
    bool jpegStart = (imgLen >= 2 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8);
    bool sofFound  = false;
    if (jpegStart)
    {
      for (size_t i = 0; i + 8 < imgLen; i++)
      {
        if (fb->buf[i] == 0xFF &&
            (fb->buf[i+1] == 0xC0 || fb->buf[i+1] == 0xC2))
        {
          uint16_t sofH = ((uint16_t)fb->buf[i+5] << 8) | fb->buf[i+6];
          uint16_t sofW = ((uint16_t)fb->buf[i+7] << 8) | fb->buf[i+8];
          Serial.printf("[CAP] JPEG SOF: %u x %u  (fb: %u x %u)  size=%u bytes\n",
                        sofW, sofH, fb->width, fb->height, (unsigned)imgLen);
          g_lastCaptureWidth  = sofW;
          g_lastCaptureHeight = sofH;
          sofFound = true;
          break;
        }
      }
    }
    if (!jpegStart || !sofFound)
    {
      Serial.printf("[CAP] Invalid JPEG (start=%d sofFound=%d size=%u) — discarding\n",
                    jpegStart, sofFound, (unsigned)imgLen);
      esp_camera_fb_return(fb);
      ledBlink(255, 80, 0, 3, 400);
      ledSet(0, 40, 0);
      return "";
    }
  }

  char dirPath[56], filePath[80];

  if (hasTime)
  {
    snprintf(dirPath, sizeof(dirPath), "%s/%04d/%02d/%02d",
             SD_RTU_ROOT,
             timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday);
    snprintf(filePath, sizeof(filePath), "%s/%04d%02d%02d_%02d%02d%02d.jpg",
             dirPath,
             timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    sdMkdirRecursive(dirPath);
  }
  else
  {
    snprintf(filePath, sizeof(filePath), "/RTU/IMG_%06lu.jpg", (unsigned long)captureCount);
  }

  File file = SD_MMC.open(filePath, FILE_WRITE);
  if (!file)
  {
    Serial.printf("[CAP] Cannot open: %s\n", filePath);
    esp_camera_fb_return(fb);
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return "";
  }

  size_t written = file.write(fb->buf, imgLen);
  file.close();
  esp_camera_fb_return(fb);
  captureCount++;

  if (written != imgLen)
  {
    Serial.printf("[CAP] Write error %u/%u bytes\n", (unsigned)written, (unsigned)imgLen);
    ledBlink(255, 80, 0, 3, 400);
    ledSet(0, 40, 0);
    return "";
  }

  Serial.printf("[CAP] Saved: %s (%u bytes)\n", filePath, (unsigned)written);
  saveConfig();   // update Image Resolution in config.txt

  return String(filePath);
}

// ─────────────────────────────────────────────────────────────────────────────
// captureAndSaveToSD()
//
// Deep Sleep 복귀 시 1순위 호출 (SIM 초기화 전).
// 캡처 + SD 저장만 수행하고 TX 는 하지 않는다.
// TX 경계 판별은 txAfterWake() 에서 scheduledCaptureTime 기준으로 수행.
// 성공: 파일 경로 반환.  실패: "" 반환.
// ─────────────────────────────────────────────────────────────────────────────
String captureAndSaveToSD()
{
  return performCapture();
}

// ─────────────────────────────────────────────────────────────────────────────
// txAfterWake()
//
// Deep Sleep 복귀 후 SIM 초기화(simInit) 완료 시 호출.
// capturedPath         : captureAndSaveToSD() 반환값 ("" = 캡처 실패).
// scheduledCaptureTime : 이번 Wake-up 의 예약 시각.
//                        setup() 에서 nextCaptureTime 을 RTC 보정 전에 저장한 값.
//                        simInit 이후에는 30-90 s 경과하여 time(NULL) 로는
//                        TX 경계를 판별할 수 없으므로 이 값을 사용해야 함.
//
// TX 경계 판별: (hour×60+min) % (interval×cnt) == 0
// simInit() → simConnect() 내에서 simPostDeviceStatus() 이미 호출됨 — 중복 없음.
// fail-fast: sendWithRetry() 실패 시 retryPendingFiles() 건너뜀.
// ─────────────────────────────────────────────────────────────────────────────
void txAfterWake(const String &capturedPath, time_t scheduledCaptureTime)
{
  if (!simReady)
  {
    Serial.println("[TX] SIM not ready — file retained in RTU");
    return;
  }

  int txPeriodMin = g_captureIntervalMin * g_captureTarget;

  if (!isTxBoundary(scheduledCaptureTime))
  {
    // 다음 TX 경계 시각 계산하여 로그 출력
    time_t nextTx = calcNextTxBoundary(scheduledCaptureTime);
    struct tm nextTm;
    localtime_r(&nextTx, &nextTm);
    Serial.printf("[TX] Not a TX boundary (period=%d min) — next TX: %02d:%02d:00 KST\n",
                  txPeriodMin, nextTm.tm_hour, nextTm.tm_min);
    return;
  }

  Serial.printf("[TX] TX boundary reached (period=%d min) — sending files\n", txPeriodMin);

  // HTTP(simConnect) → TCP 전환 안정화 대기
  delay(1500);
  Serial.println("[TX] HTTP→TCP settling delay done");

  if (capturedPath.length() > 0)
  {
    // 방금 찍은 파일 우선 전송 후 나머지 pending 파일 정리 (fail-fast)
    bool ok = sendWithRetry(capturedPath);
    if (ok)
      retryPendingFiles();
    else
      Serial.println("[TX] Send failed — entering sleep, file retained in RTU");
  }
  else
  {
    // 캡처 실패 시에도 TX 경계이면 이전 누적 파일 전송 시도
    retryPendingFiles();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// captureAndSave()  —  loop() 에서 스케줄 도달 시 호출 (운영 모드 정기 캡처)
//
// TX 경계 판별: nextCaptureTime (현재 캡처의 예약 시각, loop() 에서 아직 갱신 전)
//   기준으로 isTxBoundary() 를 호출.
// TX 경계이면: simPostDeviceStatus → delay(1500) → sendWithRetry → retryPendingFiles.
// fail-fast: sendWithRetry 실패 시 retryPendingFiles 건너뜀.
// ─────────────────────────────────────────────────────────────────────────────
void captureAndSave()
{
  // nextCaptureTime 은 loop() 에서 captureAndSave() 반환 후에 갱신되므로
  // 여기서는 현재 캡처의 예약 시각을 가리킨다.
  time_t thisCaptureTime = nextCaptureTime;

  String filePath = performCapture();
  if (filePath.isEmpty())
  {
    ledSet(0, 40, 0);
    return;
  }

  if (!simReady)
  {
    Serial.println("[SIM] Not ready — file retained in RTU");
    ledSet(0, 40, 0);
    return;
  }

  int txPeriodMin = g_captureIntervalMin * g_captureTarget;

  if (isTxBoundary(thisCaptureTime))
  {
    Serial.printf("[TX] TX boundary reached (period=%d min) — sending files\n", txPeriodMin);

    // 이미지 전송 전 디바이스 상태 POST (모뎀 신호·배터리·시간 포함)
    SimInfo info = simGetInfo();
    simPostDeviceStatus(info);

    // HTTP → TCP 전환 안정화 대기
    delay(1500);
    Serial.println("[TX] HTTP→TCP settling delay done");

    // fail-fast: sendWithRetry 실패 시 retryPendingFiles 건너뜀
    bool ok = sendWithRetry(filePath);
    if (ok)
      retryPendingFiles();
    else
      Serial.println("[TX] Send failed — file retained in RTU");
  }
  else
  {
    // TX 경계가 아님 — 파일만 저장, 다음 TX 경계 시각 로그 출력
    time_t nextTx = calcNextTxBoundary(thisCaptureTime);
    struct tm nextTm;
    localtime_r(&nextTx, &nextTm);
    Serial.printf("[CAP] File saved — TX at %02d:%02d:00 KST (period=%d min)\n",
                  nextTm.tm_hour, nextTm.tm_min, txPeriodMin);
  }

  ledSet(0, 40, 0);
}
