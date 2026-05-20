#pragma once
#include <Arduino.h>
#include "SD_MMC.h"

extern bool sdReady;

bool sdSetup();
void sdMkdirRecursive(const char *dirPath);
void sdRemoveAll();

// Persist runtime settings to /config.txt and restore them on boot.
// saveConfig() returns true on success, false if the SD write failed
// (re-mounts SD and retries once before giving up).
void loadConfig();
bool saveConfig();
