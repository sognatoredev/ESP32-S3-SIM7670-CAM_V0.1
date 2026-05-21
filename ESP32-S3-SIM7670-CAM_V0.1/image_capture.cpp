#include "image_capture.h"
#include "config.h"
#include "camera_mgr.h"
#include "sd_storage.h"
#include "sim_modem.h"
#include "image_tx.h"
#include "led.h"
#include "app_httpd.h"
#include "esp_camera.h"
#include "SD_MMC.h"
#include <Arduino.h>

// RTC_DATA_ATTR: Deep Sleep 복귀 시에도 값 보존.
// captureCount  — 파일명 fallback 카운터 (NTP 미동기 시 사용)
// g_captureTarget — 전송 전 누적 캡처 수 설정값 (set cnt <n> 으로 변경 가능)
// s_captureAccum  — 마지막 TX 이후 누적 캡처 수 (g_captureTarget > 1 시 의미 있음)
RTC_DATA_ATTR static uint32_t captureCount    = 0;
RTC_DATA_ATTR        int      g_captureTarget = 1;
RTC_DATA_ATTR static int      s_captureAccum  = 0;
int             g_lastCaptureWidth  = 0;   // resolution of most recent capture
int             g_lastCaptureHeight = 0;

// ─────────────────────────────────────────────────────────────────────────────
// performCapture()  —  static helper
//
// 카메라 해상도 전환 → 노출 안정화 → 플래시 점등 → 캡처 → JPEG 검증 → SD 저장.
// 성공: 저장된 파일 경로(String) 반환.
// 실패: "" 반환 (캡처 불가 / JPEG 오류 / SD 쓰기 오류 포함).
//
// NOTE: s_captureAccum 는 갱신하지 않는다 — 호출자(captureAndSave / captureAndSaveToSD)
//       가 성공 여부를 확인한 후 직접 증가시킨다.
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
  // AT+HTTPDATA hard limit is 319488 bytes; JPEG quality 12 keeps XGA well under that.
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
  // OV5640 needs 2-3 frames to stabilise after a timing/resolution switch.
  for (int i = 0; i < 3; i++)
  {
    camera_fb_t *fl = esp_camera_fb_get();
    if (fl) esp_camera_fb_return(fl);
    delay(50);
  }

  // Flash on → capture → flash off
  ledSet(255, 255, 255);
  flashLedSet(255, 255, 255);
  delay(200);   // OV5640 AE 안정화: 플래시 점등 후 3~4프레임(~100ms) 수렴 대기
  camera_fb_t *fb = esp_camera_fb_get();
  flashLedSet(0, 0, 0);   // 플래시 먼저 끄기
  ledSet(0, 40, 0);        // 상태 LED 복구

  // Restore streaming resolution and quality
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

  // Validate JPEG: must start with FF D8 and contain a SOF0/SOF2 marker.
  // A missing SOF means the camera output is corrupt (unsupported resolution,
  // DMA underrun, etc.) — discard and abort rather than save a broken file.
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
// 성공: 파일 경로 반환.  실패: "" 반환.
// ─────────────────────────────────────────────────────────────────────────────
String captureAndSaveToSD()
{
  String path = performCapture();
  if (path.length() > 0)
  {
    s_captureAccum++;
    Serial.printf("[CAP] Accumulated %d/%d (no TX yet — SIM not init)\n",
                  s_captureAccum, g_captureTarget);
  }
  return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// txAfterWake()
//
// Deep Sleep 복귀 후 SIM 초기화(simInit) 완료 시 호출.
// capturedPath: captureAndSaveToSD() 반환값 ("" = 캡처 실패).
//
// ■ simInit() → simConnect() 내에서 simPostDeviceStatus() 가 이미 호출됨.
//   여기서는 중복 호출하지 않는다.
//
// ■ fail-fast: sendWithRetry() 실패 시 retryPendingFiles() 를 건너뛰고 즉시 반환.
//   실패한 파일은 /RTU 에 보존 → 다음 Wake-up 시 retryPendingFiles() 로 재전송.
//   loop() 는 nextCaptureTime 까지 Deep Sleep 에 진입한다.
// ─────────────────────────────────────────────────────────────────────────────
void txAfterWake(const String &capturedPath)
{
  if (!simReady)
  {
    Serial.println("[TX] SIM not ready — file retained in RTU");
    return;
  }

  if (s_captureAccum < g_captureTarget)
  {
    Serial.printf("[TX] Need %d more capture(s) before TX (%d/%d) — skipping TX\n",
                  g_captureTarget - s_captureAccum, s_captureAccum, g_captureTarget);
    return;
  }

  s_captureAccum = 0;

  // AT+HTTPTERM 이후 내부 PDP(IP bearer) 컨텍스트가 비동기 해제됨.
  // 해제 완료 전에 AT+NETOPEN 을 호출하면 +NETOPEN: 1 (컨텍스트 충돌) 이 발생해
  // 첫 번째 전송이 거의 항상 실패함. 1.5초 대기로 HTTP → TCP 전환 안정화.
  delay(1500);
  Serial.println("[TX] HTTP→TCP settling delay done");

  if (g_captureTarget == 1 && capturedPath.length() > 0)
  {
    // cnt=1: 방금 찍은 파일을 우선 전송 (fail-fast)
    bool ok = sendWithRetry(capturedPath);
    if (ok)
      retryPendingFiles();   // 이전 미전송 파일 추가 정리
    else
      Serial.println("[TX] Send failed — entering sleep, file retained in RTU");
  }
  else
  {
    // cnt>1: 누적된 모든 RTU 파일 전송 (oldest-first)
    retryPendingFiles();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// captureAndSave()  —  loop() 에서 스케줄 도달 시 호출 (운영 모드 정기 캡처)
//
// SIM 초기화는 이미 setup() 에서 완료됨.
// simPostDeviceStatus() 를 TX 직전에 호출하여 최신 신호/배터리 정보를 서버에 전달.
//
// fail-fast TX: sendWithRetry() 실패 시 retryPendingFiles() 건너뜀.
//   실패 파일은 /RTU 보존 → 다음 캡처 후 retryPendingFiles() 로 재전송.
// ─────────────────────────────────────────────────────────────────────────────
void captureAndSave()
{
  String filePath = performCapture();
  if (filePath.isEmpty())
  {
    ledSet(0, 40, 0);
    return;
  }

  s_captureAccum++;
  Serial.printf("[CAP] Accumulated %d/%d\n", s_captureAccum, g_captureTarget);

  if (simReady)
  {
    if (s_captureAccum >= g_captureTarget)
    {
      s_captureAccum = 0;

      // 이미지 전송 전 디바이스 상태 POST (모뎀 신호·배터리·시간 포함)
      SimInfo info = simGetInfo();
      simPostDeviceStatus(info);

      // AT+HTTPTERM 이후 내부 PDP(IP bearer) 컨텍스트가 비동기 해제됨.
      // 해제 완료 전에 AT+NETOPEN 을 호출하면 +NETOPEN: 1 (컨텍스트 충돌) 이 발생해
      // 첫 번째 전송이 거의 항상 실패함. 1.5초 대기로 HTTP → TCP 전환 안정화.
      delay(1500);
      Serial.println("[TX] HTTP→TCP settling delay done");

      if (g_captureTarget == 1)
      {
        // fail-fast: 전송 실패 시 retryPendingFiles 건너뜀
        bool ok = sendWithRetry(filePath);
        if (ok)
          retryPendingFiles();
        else
          Serial.println("[TX] Send failed — file retained in RTU");
      }
      else
      {
        // cnt>1: 누적된 모든 RTU 파일 전송 (oldest-first)
        retryPendingFiles();
      }
    }
    else
    {
      Serial.printf("[CAP] Waiting for %d more capture(s) before TX\n",
                    g_captureTarget - s_captureAccum);
    }
  }
  else
  {
    Serial.println("[SIM] Not ready — file retained in RTU");
  }

  ledSet(0, 40, 0);
}
