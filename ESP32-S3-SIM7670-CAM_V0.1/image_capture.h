#pragma once
#include <Arduino.h>

extern int g_captureTarget;       // number of captures before TX (default 1)
extern int g_lastCaptureWidth;    // resolution of most recent successful capture
extern int g_lastCaptureHeight;

void   captureAndSave();

// ── Deep Sleep 복귀 전용 ──────────────────────────────────────────────────────
// captureAndSaveToSD(): SIM 초기화 전(1순위)에 이미지 캡처+저장만 수행.
//   TX 는 하지 않음. 성공 시 SD 파일 경로, 실패 시 "" 반환.
// txAfterWake()       : SIM 초기화 완료 후 TX (fail-fast: 실패 시 즉시 반환).
//   capturedPath 는 captureAndSaveToSD() 반환값을 그대로 전달.
String captureAndSaveToSD();
void   txAfterWake(const String &capturedPath);
