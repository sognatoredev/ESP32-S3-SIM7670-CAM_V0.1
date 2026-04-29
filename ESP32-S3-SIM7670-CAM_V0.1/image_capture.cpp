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

static uint32_t captureCount = 0;   // fallback counter when NTP not synced

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

  // Pause streaming
  capturePending = true;
  if (isStreaming)
  {
    delay(300);   // let current frame finish
    Serial.println("[CAP] Streaming paused");
  }

  // Switch to FHD + fixed quality for the still capture.
  // AT+HTTPDATA hard limit is 319488 bytes; JPEG quality 12 keeps FHD under ~200 KB
  // in typical outdoor scenes.  Lower number = higher quality / larger file.
  framesize_t prevFramesize = current_cam_framesize;
  sensor_t   *s             = esp_camera_sensor_get();
  int         prevQuality   = s->status.quality;
  s->set_framesize(s, FRAMESIZE_FHD);
  s->set_quality(s, 12);
  delay(300);

  // Discard stale frame from previous resolution
  {
    camera_fb_t *fl = esp_camera_fb_get();
    if (fl) esp_camera_fb_return(fl);
  }

  // Flash on -> capture -> flash off
  ledSet(255, 255, 255);  delay(200);
  camera_fb_t *fb = esp_camera_fb_get();
  ledSet(0, 40, 0);

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

  if (simReady)
  {
    sendWithRetry(String(filePath));
    retryPendingFiles();
  }
  else
  {
    Serial.println("[SIM] Not ready — file retained in RTU");
  }

  ledSet(0, 40, 0);
}
