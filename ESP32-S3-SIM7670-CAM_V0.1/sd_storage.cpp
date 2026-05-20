#include "sd_storage.h"
#include "config.h"
#include "battery.h"
#include "image_capture.h"
#include "time_sync.h"
#include "sim_modem.h"
#include "SD_MMC.h"
#include <Arduino.h>

#define CONFIG_PATH "/config.txt"

// Forward declaration — saveConfig is defined after loadConfig in this file.
bool saveConfig();

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
    Serial.println("[CFG] No config.txt — creating with defaults");
    saveConfig();
    return;
  }

  int loaded = 0;
  while (f.available())
  {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int eq = line.indexOf('=');
    if (eq == -1) continue;   // info lines (no '=') are silently skipped

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
    // unknown keys are silently ignored
  }
  f.close();

  Serial.printf("[CFG] Loaded %d setting(s) — intv=%d min  cnt=%d\n",
                loaded, g_captureIntervalMin, g_captureTarget);
}

// ─────────────────────────────────────────────────────────────────────────────
// writeConfigContent()
//   열린 File 에 config 내용을 기록하고, settings 섹션(intv=, cnt=) 에
//   실제로 쓰여진 바이트 수를 반환.
//   반환값 > 0  → settings 섹션 정상 기록
//   반환값 == 0 → FatFS 버퍼 flush 단계에서 DMA 오류 발생 (0x107 Timeout 등)
//
// ※ f.printf()/println() 은 FatFS 내부 버퍼에 쓰고 바이트 수를 반환함.
//    실제 디스크 쓰기(DMA 전송)는 f.close() 의 flush 단계에서 수행되므로
//    f.printf() 반환값만으로는 디스크 오류를 감지할 수 없음.
//    따라서 close() 후 파일 크기를 재확인하여 성공 여부를 판별함.
// ─────────────────────────────────────────────────────────────────────────────
static size_t writeConfigContent(File &f)
{
  // ── Info section (loadConfig()에서 '='없는 줄은 무시됨) ─────────────────
  f.println("**************************************************");
  f.println(" Device Info  (auto-generated — do not edit)");
  f.println("**************************************************");
  f.printf (" Model Prefix    : %s\n",      DEVICE_MODEL_PREFIX);
  f.printf (" Serial Number   : %s\n",      DEVICE_UNIT_CODE);
  f.printf (" FW Build Date   : %s %s\n",   __DATE__, __TIME__);
  f.printf (" Server Host     : %s\n",      SERVER_HOST);
  f.printf (" Server Port     : %d\n",      SERVER_PORT);
  f.printf (" NTP Server      : %s\n",      NTP_SERVER);
  f.printf (" Time Zone       : UTC+%d\n",  (int)(NTP_GMT_OFFSET / 3600L));
  f.printf (" WiFi SSID       : %s\n",      WIFI_SSID);
  f.printf (" WiFi PW         : %s\n",      WIFI_PASSWORD);
  f.printf (" m2_point_id     : %d\n",      g_m2PointId);
  f.printf (" m2_device_id    : %d\n",      g_m2DeviceId);
  f.printf (" Battery         : %d%%\n",    g_batteryPercent);
  f.printf (" Sim Baud Rate   : %d bps\n",  SIM_BAUD_FAST);
  f.println(" Mesure_Mode     : -");
  if (g_lastCaptureWidth > 0)
    f.printf(" Image Resolution: %d x %d\n", g_lastCaptureWidth, g_lastCaptureHeight);
  else
    f.println(" Image Resolution: -");
  f.println("**************************************************");
  f.println();
  f.println(" [Settings] edit values below to change behavior");

  // ── Settings section (machine-readable) ──────────────────────────────────
  // 이 줄들의 실제 쓰기 성공 여부를 반환값으로 전달.
  size_t sw = 0;
  sw += f.printf("intv=%d\n", g_captureIntervalMin);
  sw += f.printf("cnt=%d\n",  g_captureTarget);
  return sw;
}

// ─────────────────────────────────────────────────────────────────────────────
// saveConfig()
//   /config.txt 에 설정을 기록하고 성공 여부를 반환.
//
//   SD DMA Timeout(0x107) 발생 시 SD 재마운트 후 1회 재시도.
//   재시도도 실패하면 false 반환 — 호출자는 사용자에게 오류를 알려야 함.
//
//   검증 방법: f.close() 후 파일을 다시 열어 크기를 확인.
//   FatFS 가 flush 단계에서 DMA 오류를 흡수해도 파일 크기로 감지 가능.
// ─────────────────────────────────────────────────────────────────────────────
bool saveConfig()
{
  if (!sdReady)
  {
    Serial.println("[CFG] SD not ready — cannot save");
    return false;
  }

  for (int attempt = 1; attempt <= 2; attempt++)
  {
    if (attempt > 1)
    {
      // SD 재마운트: DMA 오류 후 SDMMC 컨트롤러 + SD 카드 상태 리셋
      Serial.println("[CFG] Re-mounting SD for retry...");
      SD_MMC.end();
      delay(500);
      sdReady = sdSetup();
      if (!sdReady)
      {
        Serial.println("[CFG] SD re-mount failed — save aborted");
        return false;
      }
    }

    File f = SD_MMC.open(CONFIG_PATH, FILE_WRITE);
    if (!f)
    {
      Serial.printf("[CFG] Cannot open config.txt for write (attempt %d)\n", attempt);
      continue;   // 재마운트 후 재시도
    }

    size_t sw = writeConfigContent(f);
    f.close();   // FatFS flush → 실제 DMA 전송 발생 (실패 시 driver 레벨 에러 로그)

    // ── 쓰기 검증: 파일을 다시 열어 크기 확인 ─────────────────────────────
    // f.close() 의 flush 단계에서 DMA 타임아웃이 발생하면
    // 파일이 존재하지 않거나 크기가 0/매우 작음.
    // 최소 기대 크기: info 섹션 ~400 B + settings ~15 B = 200 B (보수적)
    File v      = SD_MMC.open(CONFIG_PATH, FILE_READ);
    size_t vsz  = v ? v.size() : 0;
    if (v) v.close();

    if (sw > 0 && vsz >= 200)
    {
      Serial.printf("[CFG] Saved — intv=%d min  cnt=%d  (%u bytes)\n",
                    g_captureIntervalMin, g_captureTarget, (unsigned)vsz);
      return true;
    }

    Serial.printf("[CFG] Write verify failed (attempt %d) — "
                  "settings=%u B  file=%u B\n",
                  attempt, (unsigned)sw, (unsigned)vsz);
  }

  Serial.println("[CFG] Save failed after 2 attempts");
  return false;
}
