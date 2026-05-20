#pragma once
#include <time.h>

extern bool   ntpSynced;
extern time_t nextCaptureTime;
extern int    g_captureIntervalMin;  // capture interval in minutes (default 10)

time_t calcNextBoundary();

// Apply an externally obtained KST time string ("YYYY-MM-DD HH:MM:SS") to the
// system clock.  Converts KST → UTC, calls settimeofday(), and sets ntpSynced.
// Called by simSyncTime() after AT+CCLK? read (LTE 시간동기화 전용).
bool   applyKSTTime(const char *kstTimeStr);

// Wake-up 후 RTC 드리프트 보정 전용 (settimeofday() 만 수행).
// applyKSTTime() 과 달리 ntpSynced / nextCaptureTime 을 변경하지 않음.
// simConnect() 내에서 HTTP 성공 후 호출.
bool   correctRtcFromModem(const char *kstTimeStr);
