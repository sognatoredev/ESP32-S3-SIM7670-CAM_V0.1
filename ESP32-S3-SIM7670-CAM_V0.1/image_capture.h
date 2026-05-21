#pragma once
#include <Arduino.h>

extern int g_captureTarget;       // number of captures before TX (default 1)
extern int g_lastCaptureWidth;    // resolution of most recent successful capture
extern int g_lastCaptureHeight;

void   captureAndSave();

// ── Deep Sleep 복귀 전용 ──────────────────────────────────────────────────────
// captureAndSaveToSD()  : SIM 초기화 전(1순위)에 이미지 캡처+저장만 수행.
//   TX 는 하지 않음. 성공 시 SD 파일 경로, 실패 시 "" 반환.
//
// txAfterWake()         : SIM 초기화 완료 후 TX.
//   scheduledCaptureTime: 이번 Wake-up 의 예약 시각 (nextCaptureTime 을 RTC 보정 전에 저장한 값).
//   TX 경계 판별: (hour×60+min) % (interval×cnt) == 0
//   RTC 보정 후에는 30-90 s 경과하여 현재 시각으로는 경계를 잡을 수 없으므로
//   반드시 예약 시각 기준으로 판별해야 한다.
//   fail-fast: sendWithRetry 실패 시 retryPendingFiles 건너뜀 → loop() 에서 Deep Sleep.
String captureAndSaveToSD();
void   txAfterWake(const String &capturedPath, time_t scheduledCaptureTime);
