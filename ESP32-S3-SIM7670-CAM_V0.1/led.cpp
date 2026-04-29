#include "led.h"
#include "config.h"
#include <FastLED.h>
#include <Arduino.h>

static CRGB leds[WS2812_NUM];

void ledInit()
{
  FastLED.addLeds<WS2812B, WS2812_PIN, RGB>(leds, WS2812_NUM);
  FastLED.setBrightness(255);
}

void ledSet(uint8_t r, uint8_t g, uint8_t b)
{
  fill_solid(leds, WS2812_NUM, CRGB(r, g, b));
  FastLED.show();
}

void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs)
{
  for (int i = 0; i < n; i++)
  {
    ledSet(r, g, b);  delay(periodMs / 2);
    ledSet(0, 0, 0);  delay(periodMs / 2);
  }
}
