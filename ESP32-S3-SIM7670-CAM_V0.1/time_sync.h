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
