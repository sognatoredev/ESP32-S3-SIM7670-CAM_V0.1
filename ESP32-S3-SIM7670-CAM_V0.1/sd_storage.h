#pragma once
#include <Arduino.h>
#include "SD_MMC.h"

extern bool sdReady;

bool sdSetup();
void sdMkdirRecursive(const char *dirPath);
void sdRemoveAll();
