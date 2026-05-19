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

static uint32_t captureCount    = 0;   // fallback counter when NTP not synced
int             g_captureTarget = 1;   // set cnt <n> — send after every n captures
static int      s_captureAccum  = 0;   // captures accumulated since last TX
int             g_lastCaptureWidth  = 0;   // resolution of most recent capture
int             g_lastCaptureHeight = 0;

void captureAndSave()
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
    return;
  }

  capturePending = true;

  // Switch to FHD for the still capture.
  // OV5640 natively supports FHD (1920x1080).
  // AT+HTTPDATA hard limit is 319488 bytes; JPEG quality 12 keeps FHD under ~200 KB.
  framesize_t prevFramesize = current_cam_framesize;
  sensor_t   *s             = esp_camera_sensor_get();
  int         prevQuality   = s->status.quality;
  // s->set_framesize(s, FRAMESIZE_FHD); // default.
  // s->set_framesize(s, FRAMESIZE_HD); // 2026.05.04 feat.CSH : 해상도 변경 테스트 FRAMESIZE_HD 1280x720
  s->set_framesize(s, FRAMESIZE_XGA); // 2026.05.04 feat.CSH : 해상도 변경 테스트 FRAMESIZE_XGA 1024x768    현재 캡처는 되지만 서버로 전송 중 계속 Error 발생
  // s->set_framesize(s, FRAMESIZE_SVGA); // 2026.05.04 feat.CSH : 해상도 변경 테스트 FRAMESIZE_SVGA 800x600    현재 캡처는 되지만 서버로 전송 중 계속 Error 발생
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

  // Flash on -> capture -> flash off
  // GPIO38 상태 LED + GPIO1 플래시 8개 동시 점등
  ledSet(255, 255, 255);
  flashLedSet(255, 255, 255);
  // delay(200);
  delay(150); // OV5640 AE 안정화: 플래시 점등 후 3~4프레임(~100ms) 수렴 대기
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
    return;
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
      return;
    }
  }

  char   dirPath[56], filePath[80];

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
    return;
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
    return;
  }
  Serial.printf("[CAP] Saved: %s (%u bytes)\n", filePath, (unsigned)written);
  saveConfig();   // update Image Resolution in config.txt

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

      if (g_captureTarget == 1)
      {
        sendWithRetry(String(filePath));
        retryPendingFiles();
      }
      else
      {
        // Send all accumulated RTU files (oldest first)
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
