#include "time_sync.h"
#include "config.h"
#include "led.h"
#include <Arduino.h>
#include <sys/time.h>

bool   ntpSynced          = false;
time_t nextCaptureTime    = 0;
int    g_captureIntervalMin = 10;   // set intv <n> — capture every n minutes

bool applyKSTTime(const char *kstTimeStr)
{
  int year, mon, day, hh, mm, ss;
  if (sscanf(kstTimeStr, "%d-%d-%d %d:%d:%d",
             &year, &mon, &day, &hh, &mm, &ss) != 6)
  {
    Serial.println("[TIME] applyKSTTime: parse error");
    return false;
  }

  // mktime treats struct tm as local time.
  // Set TZ to KST so mktime converts KST → UTC epoch correctly.
  setenv("TZ", "KST-9", 1);
  tzset();

  struct tm t = {};
  t.tm_year  = year - 1900;
  t.tm_mon   = mon - 1;
  t.tm_mday  = day;
  t.tm_hour  = hh;
  t.tm_min   = mm;
  t.tm_sec   = ss;
  t.tm_isdst = 0;

  time_t epoch = mktime(&t);   // KST → UTC epoch
  if (epoch == (time_t)-1)
  {
    Serial.println("[TIME] applyKSTTime: mktime error");
    return false;
  }

  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  ntpSynced       = true;
  nextCaptureTime = calcNextBoundary();

  struct tm disp;
  localtime_r(&epoch, &disp);
  Serial.printf("[TIME] System time set — KST: %04d/%02d/%02d %02d:%02d:%02d\n",
                disp.tm_year + 1900, disp.tm_mon + 1, disp.tm_mday,
                disp.tm_hour, disp.tm_min, disp.tm_sec);

  struct tm nextTm;
  localtime_r(&nextCaptureTime, &nextTm);
  Serial.printf("[CAP] Next capture: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);
  return true;
}

void timeSyncInit()
{
  // If modem already synced the clock, skip WiFi NTP
  if (ntpSynced)
  {
    Serial.println("[NTP] Already synced via modem — skipping WiFi NTP");
    return;
  }

  // Set KST timezone first (needed even when NTP is pending)
  setenv("TZ", "KST-9", 1);
  tzset();

  configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
  Serial.print("[NTP] Syncing via WiFi");

  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 20)
  {
    delay(500);
    Serial.print(".");
    retries++;
  }

  if (retries < 20)
  {
    ntpSynced = true;
    Serial.printf("\n[NTP] %04d/%02d/%02d %02d:%02d:%02d KST\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    nextCaptureTime = calcNextBoundary();
    struct tm nextTm;
    localtime_r(&nextCaptureTime, &nextTm);
    Serial.printf("[CAP] Next capture: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);
  }
  else
  {
    Serial.println("\n[NTP] Sync failed");
    ledBlink(255, 80, 0, 5, 200);
  }
}

time_t calcNextBoundary()
{
  time_t now;
  time(&now);
  long intervalSec = (long)g_captureIntervalMin * 60L;
  return ((now / intervalSec) + 1) * intervalSec;
}
