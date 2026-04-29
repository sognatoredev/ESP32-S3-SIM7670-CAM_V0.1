#pragma once
#include <stdint.h>

void ledInit();
void ledSet(uint8_t r, uint8_t g, uint8_t b);
void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs);
