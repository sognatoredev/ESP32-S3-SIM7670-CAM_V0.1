#pragma once
#include <stdint.h>

// ── 상태 표시 LED (GPIO38, 1개) ──────────────────────────
void ledInit();
void ledSet(uint8_t r, uint8_t g, uint8_t b);
void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs);

// ── 플래시 LED (GPIO1, 8개) ──────────────────────────────
void flashLedSet(uint8_t r, uint8_t g, uint8_t b);

// mask의 각 비트(bit0=LED0 … bit7=LED7)가 1인 LED만 r,g,b 색으로 켜고
// 0인 LED는 끈다.
void flashLedSetMask(uint8_t mask, uint8_t r, uint8_t g, uint8_t b);

// ── 캡처 플래시 영구 설정 ─────────────────────────────────
// 세팅모드에서 조절 → 운영모드 캡처 플래시에 적용.
// RTC_DATA_ATTR: Deep Sleep 복귀 후에도 유지.
// config.txt (flash_bright / flash_mask): 전원 재투입 후에도 복원.
extern int     g_flashBrightness;   // 0–100 %
extern uint8_t g_flashMask;         // bit0=LED0 … bit7=LED7 (1=켜기)
