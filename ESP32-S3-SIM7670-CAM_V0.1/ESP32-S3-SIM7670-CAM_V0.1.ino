#include "config.h"
#include "led.h"
#include "camera_mgr.h"
#include "sd_storage.h"
#include "time_sync.h"
#include "sim_modem.h"
#include "image_capture.h"
#include "app_httpd.h"
#include <WiFi.h>
#include <time.h>

void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // LED: blue = booting
  ledInit();
  ledSet(0, 0, 50);

  // ── Camera init ──
  if (!cameraInit())
  {
    Serial.println("[CAM] Init failed, halting");
    return;
  }

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // ── SD init ──
  if (sdSetup())
  {
    sdReady = true;
    ledBlink(0, 255, 0, 2, 200);    // green x2: SD OK
    loadConfig();                    // restore saved settings (intv, cnt)
  }
  else
  {
    ledBlink(255, 0, 0, 5, 300);    // red x5: SD fail
  }

  // ── SIM7670G power on + init ──
  ledSet(0, 0, 50);
  simPowerInit();
  simPowerOn();
  simReady = simInit();
  if (simReady)
  {
    ledBlink(0, 0, 255, 3, 200);    // blue x3: modem OK
    Serial.println("[SIM] Ready");
  }
  else
  {
    ledBlink(255, 80, 0, 5, 300);   // orange x5: modem fail
    Serial.println("[SIM] Init failed — WiFi-only mode");
  }

  // ── WiFi ──
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    ledSet(50, 50, 0);  delay(250);
    ledSet(0,  0,  0);  delay(250);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());

  // ── NTP sync ──
  timeSyncInit();

  Serial.println("[SYS] Ready (WiFi stream/capture disabled)");
  ledSet(0, 40, 0);   // green: standby
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial command handler
//   "test capture" — capture one image, save to SD, send to server immediately
//   "sim on"       — power on  SIM7670G modem (PWRKEY pulse) and re-init
//   "sim off"      — power off SIM7670G modem (PWRKEY pulse)
//   "remove sd"    — delete all files on the SD card
//   "sd info"      — print SD card usage
//   "help"         — list available commands
// ─────────────────────────────────────────────────────────────────────────────
static String serialBuf;

static void handleSerialCmd(const String &cmd)
{
  if (cmd == "test capture")
  {
    Serial.println("[CMD] Test capture: capture → save → send");
    captureAndSave();
  }
  else if (cmd == "sim on")
  {
    Serial.println("[CMD] SIM power ON...");
    simPowerOn();
    simReady = simInit();
    if (simReady)
    {
      ledBlink(0, 0, 255, 3, 200);
      Serial.println("[SIM] Ready");
    }
    else
    {
      ledBlink(255, 80, 0, 3, 300);
      Serial.println("[SIM] Init failed");
    }
  }
  else if (cmd == "sim off")
  {
    Serial.println("[CMD] SIM power OFF...");
    simPowerOff();
    simReady = false;
    ledBlink(255, 80, 0, 2, 300);
  }
  else if (cmd == "remove sd")
  {
    Serial.println("[CMD] WARNING: deleting ALL data on SD card...");
    sdRemoveAll();
  }
  else if (cmd == "sd info")
  {
    if (sdReady)
    {
      Serial.printf("[SD] Total: %llu MB  Used: %llu MB  Free: %llu MB\n",
                    SD_MMC.totalBytes() / (1024ULL * 1024ULL),
                    SD_MMC.usedBytes()  / (1024ULL * 1024ULL),
                    (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL));
    }
    else
    {
      Serial.println("[SD] Not ready");
    }
  }
  else if (cmd.startsWith("set intv "))
  {
    int val = cmd.substring(9).toInt();
    if (val >= 1 && val <= 1440)
    {
      g_captureIntervalMin = val;
      if (ntpSynced)
      {
        nextCaptureTime = calcNextBoundary();
        struct tm nextTm;
        localtime_r(&nextCaptureTime, &nextTm);
        Serial.printf("[SET] Capture interval = %d min  (next: %02d:%02d:00 KST)\n",
                      g_captureIntervalMin, nextTm.tm_hour, nextTm.tm_min);
      }
      else
      {
        Serial.printf("[SET] Capture interval = %d min\n", g_captureIntervalMin);
      }
      saveConfig();
    }
    else
    {
      Serial.println("[SET] intv: value must be 1–1440 (minutes)");
    }
  }
  else if (cmd.startsWith("set cnt "))
  {
    int val = cmd.substring(8).toInt();
    if (val >= 1 && val <= 100)
    {
      g_captureTarget = val;
      Serial.printf("[SET] Capture count = %d (send after every %d capture(s))\n",
                    g_captureTarget, g_captureTarget);
      saveConfig();
    }
    else
    {
      Serial.println("[SET] cnt: value must be 1–100");
    }
  }
  else if (cmd == "help")
  {
    Serial.println("[CMD] Available commands:");
    Serial.println("  test capture  — capture image, save to SD, send to server now");
    Serial.println("  set intv <n>  — capture interval in minutes (1–1440, default 10)");
    Serial.println("  set cnt  <n>  — captures before TX (1–100, default 1)");
    Serial.println("  sim on        — power on modem (PWRKEY) and re-init");
    Serial.println("  sim off       — power off modem (PWRKEY)");
    Serial.println("  remove sd     — delete ALL files on SD card");
    Serial.println("  sd info       — show SD card usage");
    Serial.println("  help          — show this list");
  }
  else
  {
    Serial.printf("[CMD] Unknown command: \"%s\"  (type \"help\" for list)\n", cmd.c_str());
  }
}

void loop()
{
  // ── Serial command input ──
  while (Serial.available())
  {
    char c = Serial.read();
    if (c == '\n' || c == '\r')
    {
      serialBuf.trim();
      if (serialBuf.length() > 0)
      {
        Serial.println("[CMD] > " + serialBuf);
        handleSerialCmd(serialBuf);
      }
      serialBuf = "";
    }
    else
    {
      serialBuf += c;
    }
  }

  // ── 10-minute capture scheduler ──
  if (ntpSynced && nextCaptureTime > 0)
  {
    time_t now;
    time(&now);

    if (now >= nextCaptureTime)
    {
      nextCaptureTime = calcNextBoundary();

      struct tm nextTm;
      localtime_r(&nextCaptureTime, &nextTm);
      Serial.printf("[CAP] Next: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);

      captureAndSave();
    }
  }
  delay(500);
}
