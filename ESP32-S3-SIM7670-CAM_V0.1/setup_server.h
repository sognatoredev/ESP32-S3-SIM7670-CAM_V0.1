#pragma once
#include <stdint.h>

// true  = setup 모드 (AP + HTTP 설정 페이지 동작 중)
// false = 운영 모드 (캡처 스케줄러 동작)
extern bool     g_setupMode;
extern uint32_t g_setupStartMs;   // enterSetupMode() 호출 시각 (millis)

// setup() 에서 1회 호출: WiFi AP 시작 + HTTP 서버 기동
void enterSetupMode();

// loop() 에서 g_setupMode == true 동안 매 루프 호출.
// 5분 타임아웃 또는 "운영 시작" 요청 감지 시 g_setupMode = false 로 변경.
void setupServerLoop();
