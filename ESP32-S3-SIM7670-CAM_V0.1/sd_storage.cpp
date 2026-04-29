#include "sd_storage.h"
#include "config.h"
#include "SD_MMC.h"
#include <Arduino.h>

bool sdReady = false;

bool sdSetup()
{
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
  if (!SD_MMC.begin("/sdcard", true))
  {
    Serial.println("[SD] Mount failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE)
  {
    Serial.println("[SD] No card detected");
    return false;
  }
  Serial.printf("[SD] Ready  %llu MB\n", SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}

void sdMkdirRecursive(const char *dirPath)
{
  char tmp[64];
  strncpy(tmp, dirPath, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  for (char *p = tmp + 1; *p; p++)
  {
    if (*p == '/')
    {
      *p = '\0';
      if (!SD_MMC.exists(tmp)) SD_MMC.mkdir(tmp);
      *p = '/';
    }
  }
  if (!SD_MMC.exists(tmp)) SD_MMC.mkdir(tmp);
}

// Recursively delete all files and subdirectories under dirPath.
// dirPath itself is NOT removed (call SD_MMC.rmdir after if needed).
static void sdRemoveDir(const char *dirPath)
{
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  File entry = dir.openNextFile();
  while (entry)
  {
    // Copy path before closing entry
    char entryPath[128];
    strncpy(entryPath, entry.path(), sizeof(entryPath) - 1);
    entryPath[sizeof(entryPath) - 1] = '\0';
    bool isDir = entry.isDirectory();
    entry.close();

    if (isDir)
    {
      sdRemoveDir(entryPath);
      SD_MMC.rmdir(entryPath);
      Serial.printf("[SD] rmdir  %s\n", entryPath);
    }
    else
    {
      SD_MMC.remove(entryPath);
      Serial.printf("[SD] remove %s\n", entryPath);
    }

    entry = dir.openNextFile();
  }
  dir.close();
}

void sdRemoveAll()
{
  if (!sdReady)
  {
    Serial.println("[SD] Not ready — cannot remove");
    return;
  }

  Serial.println("[SD] Removing all data...");
  uint64_t before = SD_MMC.usedBytes();

  sdRemoveDir("/");

  uint64_t after = SD_MMC.usedBytes();
  Serial.printf("[SD] Done. Freed ~%llu KB\n", (before - after) / 1024ULL);
}
