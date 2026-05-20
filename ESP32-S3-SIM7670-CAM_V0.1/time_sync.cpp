#include "time_sync.h"
#include "config.h"
#include <Arduino.h>
#include <sys/time.h>

// timeSyncInit() (WiFi NTP) 는 제거됨.
// 시간 동기화는 simSyncTime() → AT+CNTP/AT+CCLK? → applyKSTTime() 로 수행됨.

// RTC_DATA_ATTR: Deep Sleep 중에도 RTC SRAM 에 보존됨.
// 최초 부팅 시에만 초기화(false/0), 이후 Deep Sleep 복귀 시에는 이전 값 유지.
RTC_DATA_ATTR bool   ntpSynced          = false;
RTC_DATA_ATTR time_t nextCaptureTime    = 0;
RTC_DATA_ATTR int    g_captureIntervalMin = 10;   // set intv <n> — capture every n minutes

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


// Wake-up 후 RTC 드리프트 보정 전용.
// applyKSTTime() 과 달리 nextCaptureTime 을 변경하지 않음.
// nextCaptureTime 을 덮어쓰면 다음 루프에서 캡처 스케줄이 한 주기 밀리는 버그가 발생함.
// simConnect() → HTTP 성공 후 호출 (LTE 등록 = 모뎀 RTC 정확 보장).
bool correctRtcFromModem(const char *kstTimeStr)
{
  int year, mon, day, hh, mm, ss;
  if (sscanf(kstTimeStr, "%d-%d-%d %d:%d:%d",
             &year, &mon, &day, &hh, &mm, &ss) != 6)
  {
    Serial.println("[TIME] correctRtcFromModem: parse error");
    return false;
  }

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

  time_t epoch = mktime(&t);
  if (epoch == (time_t)-1)
  {
    Serial.println("[TIME] correctRtcFromModem: mktime error");
    return false;
  }

  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, NULL);

  Serial.printf("[TIME] RTC drift corrected — KST: %04d/%02d/%02d %02d:%02d:%02d\n",
                year, mon, day, hh, mm, ss);
  // nextCaptureTime 은 변경하지 않음 (RTC 메모리 값 또는 captureAndSave 후 값 유지)
  return true;
}
