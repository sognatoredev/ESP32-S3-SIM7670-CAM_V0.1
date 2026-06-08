#include "led.h"
#include "config.h"
#include <FastLED.h>
#include <Arduino.h>

static CRGB leds[WS2812_NUM];               // GPIO38 — 상태 표시 (1개)
static CRGB flashLeds[WS2812_FLASH_NUM];    // GPIO1  — 플래시 (8개)

// 캡처 플래시 영구 설정 — Deep Sleep 복귀 후에도 보존
RTC_DATA_ATTR int     g_flashBrightness = 100;   // 0–100 %
RTC_DATA_ATTR uint8_t g_flashMask       = 0xFF;  // 기본: 8개 전부 켜기

void ledInit()
{
  // 상태 LED (GPIO38) 와 플래시 LED (GPIO1) 를 FastLED 에 등록.
  // FastLED.show() 호출 시 두 스트립이 동시에 갱신된다.
  FastLED.addLeds<WS2812B, WS2812_PIN,       RGB>(leds,       WS2812_NUM);
  FastLED.addLeds<WS2812B, WS2812_FLASH_PIN, RGB>(flashLeds,  WS2812_FLASH_NUM);
  FastLED.setBrightness(255);
}

// ── 상태 표시 LED ────────────────────────────────────────
void ledSet(uint8_t r, uint8_t g, uint8_t b)
{
  fill_solid(leds, WS2812_NUM, CRGB(r, g, b));
  FastLED.show();   // 플래시 LEDs 는 현재 값(보통 꺼짐) 유지
}

void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs)
{
  for (int i = 0; i < n; i++)
  {
    ledSet(r, g, b);  delay(periodMs / 2);
    ledSet(0, 0, 0);  delay(periodMs / 2);
  }
}

// ── 플래시 LED (GPIO1, 8개) ──────────────────────────────
void flashLedSet(uint8_t r, uint8_t g, uint8_t b)
{
  fill_solid(flashLeds, WS2812_FLASH_NUM, CRGB(r, g, b));
  FastLED.show();   // 상태 LED 는 현재 값 유지
}

void flashLedSetMask(uint8_t mask, uint8_t r, uint8_t g, uint8_t b)
{
  for (int i = 0; i < WS2812_FLASH_NUM; i++) {
    flashLeds[i] = (mask & (1 << i)) ? CRGB(r, g, b) : CRGB(0, 0, 0);
  }
  FastLED.show();
}
