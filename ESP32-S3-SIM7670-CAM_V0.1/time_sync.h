#pragma once
#include <time.h>

extern bool   ntpSynced;
extern time_t nextCaptureTime;

void   timeSyncInit();
time_t calcNextBoundary();

// Apply an externally obtained KST time string ("YYYY-MM-DD HH:MM:SS") to the
// system clock.  Converts KST → UTC, calls settimeofday(), and sets ntpSynced.
bool   applyKSTTime(const char *kstTimeStr);
