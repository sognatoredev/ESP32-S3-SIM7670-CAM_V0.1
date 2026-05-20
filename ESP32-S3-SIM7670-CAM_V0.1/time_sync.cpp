#include "time_sync.h"
#include "config.h"
#include <Arduino.h>
#include <sys/time.h>

// timeSyncInit() (WiFi NTP) 는 제거됨.
// 시간 동기화는 simSyncTime() → AT+CNTP/AT+CCLK? → applyKSTTime() 로 수행됨.

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


time_t calcNextBoundary()
{
  time_t now;
  time(&now);
  long intervalSec = (long)g_captureIntervalMin * 60L;
  return ((now / intervalSec) + 1) * intervalSec;
}
