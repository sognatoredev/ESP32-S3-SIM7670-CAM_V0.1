#include "sd_storage.h"
#include "config.h"
#include "image_capture.h"
#include "time_sync.h"
#include "SD_MMC.h"
#include <Arduino.h>

#define CONFIG_PATH "/config.txt"

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

// ─────────────────────────────────────────────────────────────────────────────
// Config file  /config.txt
//   Format (one key=value per line):
//     intv=10
//     cnt=1
// ─────────────────────────────────────────────────────────────────────────────

void loadConfig()
{
  if (!sdReady)
  {
    Serial.println("[CFG] SD not ready — using defaults");
    return;
  }

  File f = SD_MMC.open(CONFIG_PATH, FILE_READ);
  if (!f)
  {
    Serial.println("[CFG] No config.txt — using defaults");
    return;
  }

  int loaded = 0;
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;

    int eq = line.indexOf('=');
    if (eq == -1) continue;

    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();
    int v = val.toInt();

    if (key == "intv" && v >= 1 && v <= 1440)
    {
      g_captureIntervalMin = v;
      loaded++;
    }
    else if (key == "cnt" && v >= 1 && v <= 100)
    {
      g_captureTarget = v;
      loaded++;
    }
    else
    {
      Serial.printf("[CFG] Unknown or invalid: %s=%s\n", key.c_str(), val.c_str());
    }
  }
  f.close();

  Serial.printf("[CFG] Loaded %d setting(s) — intv=%d min  cnt=%d\n",
                loaded, g_captureIntervalMin, g_captureTarget);
}

void saveConfig()
{
  if (!sdReady)
  {
    Serial.println("[CFG] SD not ready — cannot save");
    return;
  }

  File f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
  if (!f)
  {
    Serial.println("[CFG] Cannot open config.txt for write");
    return;
  }

  f.printf("intv=%d\n", g_captureIntervalMin);
  f.printf("cnt=%d\n",  g_captureTarget);
  f.close();

  Serial.printf("[CFG] Saved — intv=%d min  cnt=%d\n",
                g_captureIntervalMin, g_captureTarget);
}
