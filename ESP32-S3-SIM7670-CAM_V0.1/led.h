#pragma once
#include <stdint.h>

// ── 상태 표시 LED (GPIO38, 1개) ──────────────────────────
void ledInit();
void ledSet(uint8_t r, uint8_t g, uint8_t b);
void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs);

// ── 플래시 LED (GPIO1, 8개) ──────────────────────────────
void flashLedSet(uint8_t r, uint8_t g, uint8_t b);
