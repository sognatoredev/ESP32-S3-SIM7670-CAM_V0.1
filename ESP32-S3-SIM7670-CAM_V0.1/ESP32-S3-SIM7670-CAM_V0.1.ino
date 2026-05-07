#include "config.h"
#include "led.h"
#include "camera_mgr.h"
#include "sd_storage.h"
#include "time_sync.h"
#include "sim_modem.h"
#include "image_capture.h"
#include "app_httpd.h"
#include "setup_server.h"
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
    saveConfig();                    // refresh m2_point_id / m2_device_id in config.txt
  }
  else
  {
    ledBlink(255, 80, 0, 5, 300);   // orange x5: modem fail
    Serial.println("[SIM] Init failed — WiFi-only mode");
  }

  // ── Setup mode (WiFi AP + 설정 페이지) ──
  // 5분 타임아웃 또는 "운영 시작" 버튼 → 운영 모드로 전환
  enterSetupMode();
}

// ─────────────────────────────────────────────────────────────────────────────
// get <field> — print a single config/device value to Serial
// ─────────────────────────────────────────────────────────────────────────────
static void handleGetCmd(const String &field)
{
  if (field.equalsIgnoreCase("Model Prefix"))
    Serial.printf("[GET] Model Prefix     : %s\n",        DEVICE_MODEL_PREFIX);
  else if (field.equalsIgnoreCase("Serial Number"))
    Serial.printf("[GET] Serial Number    : %s\n",        DEVICE_UNIT_CODE);
  else if (field.equalsIgnoreCase("FW Build Date"))
    Serial.printf("[GET] FW Build Date    : %s %s\n",     __DATE__, __TIME__);
  else if (field.equalsIgnoreCase("Server Host"))
    Serial.printf("[GET] Server Host      : %s\n",        SERVER_HOST);
  else if (field.equalsIgnoreCase("Server Port"))
    Serial.printf("[GET] Server Port      : %d\n",        SERVER_PORT);
  else if (field.equalsIgnoreCase("NTP Server"))
    Serial.printf("[GET] NTP Server       : %s\n",        NTP_SERVER);
  else if (field.equalsIgnoreCase("Time Zone"))
    Serial.printf("[GET] Time Zone        : UTC+%d\n",    (int)(NTP_GMT_OFFSET / 3600L));
  else if (field.equalsIgnoreCase("WiFi SSID"))
    Serial.printf("[GET] WiFi SSID        : %s\n",        WIFI_SSID);
  else if (field.equalsIgnoreCase("WiFi PW"))
    Serial.printf("[GET] WiFi PW          : %s\n",        WIFI_PASSWORD);
  else if (field.equalsIgnoreCase("m2_point_id"))
    Serial.printf("[GET] m2_point_id      : %d\n",        g_m2PointId);
  else if (field.equalsIgnoreCase("m2_device_id"))
    Serial.printf("[GET] m2_device_id     : %d\n",        g_m2DeviceId);
  else if (field.equalsIgnoreCase("Battery"))
    Serial.printf("[GET] Battery          : %d%%\n",      DEVICE_BATTERY_LEVEL);
  else if (field.equalsIgnoreCase("Sim Baud Rate"))
    Serial.printf("[GET] Sim Baud Rate    : %d bps\n",    SIM_BAUD_FAST);
  else if (field.equalsIgnoreCase("Mesure_Mode"))
    Serial.println("[GET] Mesure_Mode      : -");
  else if (field.equalsIgnoreCase("Image Resolution"))
  {
    if (g_lastCaptureWidth > 0)
      Serial.printf("[GET] Image Resolution : %d x %d\n", g_lastCaptureWidth, g_lastCaptureHeight);
    else
      Serial.println("[GET] Image Resolution : - (no capture yet)");
  }
  else if (field.equalsIgnoreCase("Image Capture Intv") || field.equalsIgnoreCase("intv"))
    Serial.printf("[GET] Image Capture Intv : %d min\n",  g_captureIntervalMin);
  else if (field.equalsIgnoreCase("Image Capture Cnt") || field.equalsIgnoreCase("cnt"))
    Serial.printf("[GET] Image Capture Cnt  : %d\n",      g_captureTarget);
  else if (field.isEmpty() || field.equalsIgnoreCase("help"))
  {
    Serial.println("[GET] Available fields:");
    Serial.println("  Model Prefix        Serial Number       FW Build Date");
    Serial.println("  Server Host         Server Port         NTP Server");
    Serial.println("  Time Zone           WiFi SSID           WiFi PW");
    Serial.println("  m2_point_id         m2_device_id        Battery");
    Serial.println("  Sim Baud Rate       Mesure_Mode         Image Resolution");
    Serial.println("  Image Capture Intv  (or: intv)");
    Serial.println("  Image Capture Cnt   (or: cnt)");
  }
  else
  {
    Serial.printf("[GET] Unknown field: \"%s\"  (type \"get help\" for list)\n", field.c_str());
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial command handler
//   "test capture" — capture one image, save to SD, send to server immediately
//   "get <field>"  — print a device/config value
//   "set intv <n>" — set capture interval in minutes
//   "set cnt  <n>" — set captures before TX
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
  else if (cmd.startsWith("get "))
  {
    handleGetCmd(cmd.substring(4));
  }
  else if (cmd == "get")
  {
    handleGetCmd("");
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
  else if (cmd == "setup mode")
  {
    Serial.println("[CMD] Re-entering setup mode...");
    enterSetupMode();
  }
  else if (cmd == "help")
  {
    Serial.println("[CMD] Available commands:");
    Serial.println("  test capture        — capture image, save to SD, send to server now");
    Serial.println("  get <field>         — print a device/config value");
    Serial.println("  get help            — list all gettable fields");
    Serial.println("  set intv <n>        — capture interval in minutes (1–1440, default 10)");
    Serial.println("  set cnt  <n>        — captures before TX (1–100, default 1)");
    Serial.println("  sim on              — power on modem (PWRKEY) and re-init");
    Serial.println("  sim off             — power off modem (PWRKEY)");
    Serial.println("  remove sd           — delete ALL files on SD card");
    Serial.println("  sd info             — show SD card usage");
    Serial.println("  setup mode          — re-enter setup mode (AP + config page)");
    Serial.println("  help                — show this list");
  }
  else
  {
    Serial.printf("[CMD] Unknown command: \"%s\"  (type \"help\" for list)\n", cmd.c_str());
  }
}

// 운영 모드로 첫 진입 시 WiFi 연결 + NTP 동기화
static void initOperationMode()
{
  Serial.println("[SYS] Initializing operation mode...");
  ledSet(50, 50, 0);

  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000UL)
  {
    ledSet(50, 50, 0); delay(250);
    ledSet(0,  0,  0); delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    timeSyncInit();
  }
  else
  {
    Serial.println("\n[WiFi] Connection failed — NTP sync skipped");
    ledBlink(255, 0, 0, 3, 300);
  }

  ledSet(0, 40, 0);   // green: standby
  Serial.println("[SYS] Operation mode ready");
}

void loop()
{
  // ── Serial command input (setup/operation 모드 공통) ──
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

  // ── Setup mode ──
  if (g_setupMode)
  {
    setupServerLoop();
    delay(50);
    return;
  }

  // ── 운영 모드 초기화 (setup mode 종료 후 1회) ──
  static bool s_operationInited = false;
  if (!s_operationInited)
  {
    s_operationInited = true;
    initOperationMode();
  }

  // setup mode 재진입 시 다음 복귀를 위해 플래그 리셋
  if (g_setupMode)
  {
    s_operationInited = false;
    delay(50);
    return;
  }

  // ── 캡처 스케줄러 ──
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
