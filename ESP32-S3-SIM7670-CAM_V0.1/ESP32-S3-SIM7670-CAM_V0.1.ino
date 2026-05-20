#include "config.h"
#include "led.h"
#include "camera_mgr.h"
#include "battery.h"
#include "sd_storage.h"
#include "time_sync.h"
#include "sim_modem.h"
#include "image_capture.h"
#include "app_httpd.h"
#include "setup_server.h"
#include <WiFi.h>
#include <time.h>
#include "esp_sleep.h"   // Deep Sleep / Wake-up API

void setup()
{
  // ── Deep Sleep 복귀 여부 판별 (Serial.begin 전에 확인) ──────────────────
  // Deep Sleep 은 완전 재부팅이므로 setup() 이 항상 실행됨.
  // wakeReason == TIMER → 스케줄된 캡처를 위한 Deep Sleep 복귀.
  // wakeReason == UNDEFINED (0) → 최초 부팅 (전원 인가 / 리셋 버튼).
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  bool isDeepSleepWake = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  ledInit();
  ledSet(0, 0, 50);   // blue: 초기화 중

  if (isDeepSleepWake)
    Serial.println("\n[SYS] ===== Deep Sleep Wake (Timer) =====");
  else
    Serial.printf("\n[SYS] ===== Cold Boot (cause=%d) =====\n", (int)wakeReason);

  // ── 공통 초기화 (최초 부팅 / Deep Sleep 복귀 공통) ───────────────────────

  // Camera init
  if (!cameraInit())
  {
    Serial.println("[CAM] Init failed, halting");
    return;
  }

  // Battery gauge init (Wire는 cameraInit() 내부에서 이미 초기화됨)
  if (!batteryInit())
    Serial.println("[BAT] MAX17048 not found — battery level fixed at 100%");

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  // SD init
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

  // SIM7670G power on + init
  ledSet(0, 0, 50);
  simPowerInit();
  simPowerOn();
  simReady = simInit();
  if (simReady)
  {
    ledBlink(0, 0, 255, isDeepSleepWake ? 2 : 3, 200);   // blue x2(wake) / x3(boot)
    Serial.println("[SIM] Ready");
    saveConfig();                    // refresh m2_point_id / m2_device_id in config.txt
  }
  else
  {
    ledBlink(255, 80, 0, 5, 300);   // orange x5: modem fail
    Serial.println("[SIM] Init failed");
  }

  // ── 분기: 최초 부팅 vs Deep Sleep 복귀 ─────────────────────────────────
  if (!isDeepSleepWake)
  {
    // 최초 부팅: Setup mode (WiFi AP + 설정 페이지)
    // 5분 타임아웃 또는 "운영 시작" 버튼 → 운영 모드로 전환
    enterSetupMode();
  }
  else
  {
    // Deep Sleep 복귀: Setup mode 스킵, 즉시 운영 모드
    // ntpSynced / nextCaptureTime 은 RTC 메모리에서 복원됨
    Serial.println("[SYS] Deep Sleep wake — skipping setup mode");
    ledSet(0, 40, 0);   // green: 운영 준비 완료
  }
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
  {
    if (batteryRead())
      Serial.printf("[GET] Battery          : %d%%  (%.3f V)\n", g_batteryPercent, g_batteryVoltage);
    else
      Serial.printf("[GET] Battery          : MAX17048 not ready — last known %d%%\n", g_batteryPercent);
  }
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
  else if (cmd == "bat init")
  {
    Serial.println("[CMD] Re-running batteryInit()...");
    if (batteryInit())
      Serial.printf("[BAT] Init OK  %d%%  (%.3f V)\n", g_batteryPercent, g_batteryVoltage);
    else
      Serial.println("[BAT] Init FAILED — IC not found");
  }
  else if (cmd == "led on")
  {
    flashLedSet(255, 255, 255);
    Serial.println("[CMD] Flash LEDs ON (white, max brightness)");
  }
  else if (cmd == "led off")
  {
    flashLedSet(0, 0, 0);
    Serial.println("[CMD] Flash LEDs OFF");
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
    Serial.println("  bat init            — re-run MAX17048 init + I2C bus scan");
    Serial.println("  led on              — flash LEDs (GPIO1 x8) white max brightness");
    Serial.println("  led off             — flash LEDs OFF");
    Serial.println("  setup mode          — re-enter setup mode (AP + config page)");
    Serial.println("  help                — show this list");
  }
  else
  {
    Serial.printf("[CMD] Unknown command: \"%s\"  (type \"help\" for list)\n", cmd.c_str());
  }
}

// 운영 모드로 첫 진입 시 초기화
// 시간 동기화는 simInit() → simConnect() → simSyncTime() (AT+CNTP/CCLK?) 에서 이미 수행됨.
// WiFi 는 운영모드에서 완전 비활성화 (LTE 전용).
static void initOperationMode()
{
  Serial.println("[SYS] Initializing operation mode...");
  ledSet(50, 50, 0);

  // WiFi 완전 비활성화 — 세팅모드 AP가 종료된 상태이지만 명시적으로 OFF
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[WiFi] OFF — operation mode uses LTE only");

  // 시간 동기화 상태 확인
  if (ntpSynced)
  {
    struct tm t;
    getLocalTime(&t);
    Serial.printf("[SYS] Time synced (LTE): %04d/%02d/%02d %02d:%02d:%02d KST\n",
                  t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec);
    struct tm nextTm;
    localtime_r(&nextCaptureTime, &nextTm);
    Serial.printf("[CAP] Next capture: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);
  }
  else if (simReady)
  {
    // simInit() 중 CNTP 실패한 경우 재시도
    Serial.println("[SYS] Time not synced — retrying via LTE (AT+CNTP)...");
    simConnect();
  }
  else
  {
    Serial.println("[SYS] Modem not ready — operating without time sync");
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
    delay(20);   // 20ms: 호흡 무드등 ~50Hz 갱신을 위해 50ms → 20ms
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
      captureAndSave();

      // calcNextBoundary() 를 captureAndSave() 이후에 호출해야 함.
      // 이전 방식(before)처럼 captureAndSave() 전에 호출하면,
      // TX 재시도로 captureAndSave() 가 인터벌(예: 10분)을 초과할 경우
      // nextCaptureTime 이 이미 과거가 되어 다음 루프에서 즉시 재캡처가 발동됨.
      // capture 완료 후 현재 시각 기준으로 다음 경계를 계산하면
      // 항상 미래 값이 보장되어 올바른 슬립 시간이 산출됨.
      nextCaptureTime = calcNextBoundary();

      struct tm nextTm;
      localtime_r(&nextCaptureTime, &nextTm);
      Serial.printf("[CAP] Next: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);
    }
  }

  // ── Deep Sleep: 다음 캡처 시각까지 대기 ────────────────────────────────
  // esp_deep_sleep_start() 는 반환되지 않음 → Wake-up 후 setup() 부터 재시작.
  // ntpSynced / nextCaptureTime 은 RTC_DATA_ATTR 로 선언되어 Deep Sleep 에서도 보존.
  if (ntpSynced && nextCaptureTime > 0)
  {
    time_t now;
    time(&now);
    // 2초 여유: Wake-up → setup() 내 SIM 재초기화(~25초) 는 sleepSec 외 별도 진행
    int64_t sleepSec = (int64_t)(nextCaptureTime - now) - 2LL;

    if (sleepSec > 5)
    {
      struct tm nextTm;
      localtime_r(&nextCaptureTime, &nextTm);
      Serial.printf("[SYS] Deep Sleep %lld s  (next capture: %02d:%02d:00 KST)\n",
                    sleepSec, nextTm.tm_hour, nextTm.tm_min);

      // ── Sleep 전 주변장치 정리 ───────────────────────────────────────

      // [B] SIM7670G 완전 전원 차단
      if (simReady)
      {
        simPowerOff();
        simReady = false;
        Serial.println("[SYS] SIM7670 powered OFF");
      }

      // [C] OV5640 소프트웨어 대기 모드 (Deep Sleep 중 XCLK 정지로도 저전력이지만 명시적 처리)
      // Wake-up 후 setup() 의 cameraInit() 에서 전체 재초기화되므로 복귀 처리 불필요.
      sensor_t *camSensor = esp_camera_sensor_get();
      if (camSensor)
      {
        camSensor->set_reg(camSensor, 0x3008, 0x40, 0x40);  // bit6=1: 소프트웨어 대기
        Serial.println("[SYS] OV5640 software standby");
      }

      // [D] SD 카드 언마운트 (파일 시스템 정합성 보장)
      if (sdReady)
      {
        SD_MMC.end();
        sdReady = false;
        Serial.println("[SYS] SD card unmounted");
      }

      Serial.flush();   // UART TX 버퍼 비우기 (sleep 전 로그 보장)
      ledSet(0, 0, 0);  // Sleep 중 상태 LED OFF

      esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
      esp_deep_sleep_start();   // ← 전원 차단, 이후 setup() 부터 재시작 (반환 안 됨)
    }
    else
    {
      delay(200);  // 잔여 시간이 짧을 때 단순 대기
    }
  }
  else
  {
    delay(500);    // 시간 미동기화 상태: 캡처 없이 폴링 유지
  }
}
