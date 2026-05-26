#include "sim_modem.h"
#include "config.h"
#include "battery.h"
#include "time_sync.h"
#include <Arduino.h>

HardwareSerial SimSerial(1);   // UART1 (GPIO17/18)
bool simReady    = false;
// RTC_DATA_ATTR: Deep Sleep 복귀 시에도 서버 설정값 보존 (서버 조회 실패 시 fallback)
RTC_DATA_ATTR int  g_m2PointId  = 0;
RTC_DATA_ATTR int  g_m2DeviceId = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Power control  (GPIO21 = PWRKEY, active-low pulse)
// ─────────────────────────────────────────────────────────────────────────────

void simPowerInit()
{
  pinMode(SIM_PWRKEY_PIN, OUTPUT);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);   // idle high (released)
  Serial.println("[SIM] PWRKEY GPIO init OK");
}

// simPowerKeyPulse() — PWRKEY 펄스만 발생시키고 즉시 반환.
// 모뎀 부팅 대기(SIM_BOOT_WAIT_MS)는 하지 않는다.
// Deep Sleep 복귀 경로에서 모뎀 부팅과 카메라 캡처를 병렬 진행할 때 사용.
// 반드시 simPowerInit() 이후에 호출해야 한다.
void simPowerKeyPulse()
{
  Serial.println("[SIM] PWRKEY pulse: LOW " + String(SIM_PWRON_PULSE_MS) + " ms — modem booting...");
  digitalWrite(SIM_PWRKEY_PIN, LOW);
  delay(SIM_PWRON_PULSE_MS);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);
}

// simWaitBoot() — simPowerKeyPulse() 이후 모뎀 UART 안정화 대기(SIM_BOOT_WAIT_MS).
// 경과 시간(elapsedMs)을 빼고 남은 시간만 대기하여 이미 흐른 시간을 보상한다.
// elapsedMs: simPowerKeyPulse() 호출 직후부터 현재까지 경과한 밀리초.
void simWaitBoot(uint32_t elapsedMs)
{
  uint32_t remaining = (elapsedMs < SIM_BOOT_WAIT_MS)
                       ? (SIM_BOOT_WAIT_MS - elapsedMs) : 0;
  if (remaining == 0)
  {
    Serial.println("[SIM] Boot wait already covered by parallel work — skipping");
    return;
  }
  Serial.printf("[SIM] Waiting for modem boot (%lu ms remaining)", (unsigned long)remaining);
  uint32_t step = 500;
  while (remaining > 0)
  {
    uint32_t w = (remaining > step) ? step : remaining;
    delay(w);
    Serial.print(".");
    remaining -= w;
  }
  Serial.println(" done");
}

// simPowerOn() — 기존 직렬 방식 (PWRKEY 펄스 + 전체 부팅 대기).
// 최초 부팅 경로 및 "sim on" 직렬 커맨드에서 사용.
void simPowerOn()
{
  simPowerKeyPulse();
  simWaitBoot(0);   // 경과 시간 없음 → 전체 SIM_BOOT_WAIT_MS 대기
}

// 펌웨어별 Power-Down URC 판별
// - 표준:       "NORMAL POWER DOWN"
// - SIM7670G:   "POWERD DOWN"  (firmware 오탈자 고유 문자열)
// - 일부 변종:  "POWER DOWN"
static bool isPowerDownUrc(const String &s)
{
  return s.indexOf("NORMAL POWER DOWN") != -1 ||
         s.indexOf("POWERD DOWN")       != -1 ||
         s.indexOf("POWER DOWN")        != -1;
}

void simPowerOff()
{
  // ── 사전 처리: 활성 네트워크 세션 종료 ─────────────────────────────────
  // NETOPEN 상태에서 AT+CPOF 가 일부 펌웨어에서 거부될 수 있음
  Serial.println("[SIM] Power OFF: closing network session...");
  simSendAT("AT+NETCLOSE", 5000);
  simFlush(300);

  // ── 1단계: AT+CPOF (소프트웨어 종료) ────────────────────────────────────
  // simSendAT() 는 "OK" 수신 즉시 리턴 → Power-Down URC 는 비동기 도착
  // simWaitFor() 로 별도 수신 대기 필요
  Serial.println("[SIM] AT+CPOF...");
  String resp = simSendAT("AT+CPOF", 3000);
  Serial.println("[SIM] AT+CPOF resp: [" + resp + "]");

  String urc = simWaitFor("DOWN", 5000);   // "POWERD DOWN" / "NORMAL POWER DOWN" 공통 토큰
  Serial.println("[SIM] Power-Down URC: [" + urc + "]");
  if (isPowerDownUrc(resp) || isPowerDownUrc(urc))
  {
    Serial.println("[SIM] Power DOWN confirmed — modem OFF");
    delay(500);
    return;
  }
  Serial.println("[SIM] AT+CPOF: Power-Down URC not received");

  // ── 2단계: AT+CPOWD=1 (대체 소프트웨어 종료 커맨드) ─────────────────────
  Serial.println("[SIM] AT+CPOWD=1...");
  resp = simSendAT("AT+CPOWD=1", 3000);
  Serial.println("[SIM] AT+CPOWD=1 resp: [" + resp + "]");

  urc = simWaitFor("DOWN", 5000);
  Serial.println("[SIM] Power-Down URC: [" + urc + "]");
  if (isPowerDownUrc(resp) || isPowerDownUrc(urc))
  {
    Serial.println("[SIM] Power DOWN confirmed (CPOWD) — modem OFF");
    delay(500);
    return;
  }
  Serial.println("[SIM] AT+CPOWD=1: Power-Down URC not received");

  // ── 3단계: PWRKEY 하드웨어 강제 종료 ────────────────────────────────────
  Serial.printf("[SIM] PWRKEY hardware OFF (GPIO%d LOW %d ms)\n",
                SIM_PWRKEY_PIN, SIM_PWROFF_PULSE_MS);
  digitalWrite(SIM_PWRKEY_PIN, LOW);
  delay(SIM_PWROFF_PULSE_MS);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);
  Serial.println("[SIM] PWRKEY pulse done");

  urc = simWaitFor("DOWN", 4000);
  if (isPowerDownUrc(urc))
    Serial.println("[SIM] Power DOWN confirmed (PWRKEY) — modem OFF");
  else
    Serial.println("[SIM] No Power-Down URC after PWRKEY — assumed OFF");

  delay(1000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level AT helpers
// ─────────────────────────────────────────────────────────────────────────────

String simSendAT(const String &cmd, uint32_t timeout)
{
  Serial.print("\r\n>> "); Serial.println(cmd);
  SimSerial.println(cmd);
  String   resp = "";
  uint32_t t    = millis();
  while (millis() - t < timeout)
  {
    while (SimSerial.available())
    {
      char c = (char)SimSerial.read();
      resp  += c;
      Serial.write(c);
    }
    if (resp.indexOf("OK")    != -1 ||
        resp.indexOf("ERROR") != -1) break;
  }
  return resp;
}

String simWaitFor(const String &token, uint32_t timeout)
{
  String   resp = "";
  uint32_t t    = millis();
  while (millis() - t < timeout)
  {
    while (SimSerial.available())
    {
      char c = (char)SimSerial.read();
      resp  += c;
      Serial.write(c);
    }
    if (resp.indexOf(token) != -1) break;
  }
  return resp;
}

void simFlush(uint32_t delayMs)
{
  delay(delayMs);
  while (SimSerial.available())
  {
    Serial.write((char)SimSerial.read());
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal / time helpers
// ─────────────────────────────────────────────────────────────────────────────

int csqToRssi(int csq)
{
  if (csq == 0 || csq == 99) return -113;
  return -113 + 2 * csq;
}

int csqToStrength(int csq)
{
  if (csq == 0 || csq == 99) return 0;
  if (csq < 7)  return 1;
  if (csq < 14) return 2;
  if (csq < 20) return 3;
  if (csq < 26) return 4;
  return 5;
}

// Parse modem time and convert to KST (UTC+9)
// AT+CCLK response: +CCLK: "YY/MM/DD,HH:MM:SS±ZZ"
//   ZZ = UTC offset in quarter-hour units
//
// ■ SIM7670G 실측 동작:
//   AT+CNTP 의 tz 파라미터가 모뎀 내부 RTC 에 저장되는 시각을 결정한다.
//     tz=0  → 모뎀이 UTC 로 저장 → CCLK 가 UTC 반환 (+00)
//     tz=36 → 모뎀이 KST 로 저장 → CCLK 가 KST 반환 (+36)
//
//   simSyncTime() 에서 tz=0 을 사용하므로 CCLK 는 항상 UTC 를 반환.
//   simGetModemTime() 은 UTC 에 +9h 를 더해 KST 를 산출.
//
//   ※ tz=36 을 사용하면 CCLK=KST 이고 여기서 또 +9h → KST+9h(=UTC+18h) 이중 변환 발생.
String simGetModemTime()
{
  String resp = simSendAT("AT+CCLK?", 3000);
  Serial.println("[SIM] CCLK raw: " + resp);

  int q1 = resp.indexOf('"');
  if (q1 == -1) return "";
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 == -1) return "";
  String s = resp.substring(q1 + 1, q2);
  if (s.length() < 17) return "";

  int yy = s.substring(0, 2).toInt();
  int mo = s.substring(3, 5).toInt();
  int dd = s.substring(6, 8).toInt();
  int hh = s.substring(9, 11).toInt();
  int mm = s.substring(12, 14).toInt();
  int ss = s.substring(15, 17).toInt();

  int tzQuarters = 0;
  if ((int)s.length() > 17)
  {
    int sign  = (s[17] == '-') ? -1 : 1;
    tzQuarters = sign * s.substring(18).toInt();
  }
  Serial.printf("[SIM] CCLK parsed: 20%02d-%02d-%02d %02d:%02d:%02d  TZ=%+d quarters\n",
                yy, mo, dd, hh, mm, ss, tzQuarters);

  // UTC → KST 변환 (고정 오프셋):
  //   AT+CTZU=0 적용으로 simInit() 이후 AT+CCLK? 는 항상 UTC 를 반환.
  //   tzQuarters 는 로그 확인용으로만 파싱하고 변환에는 사용하지 않는다.
  //   NTP_GMT_OFFSET (config.h, KST = UTC+9 = 32400 s) 으로 고정 변환.
  int diffMin   = (int)(NTP_GMT_OFFSET / 60L);    // 540 min = +9h (KST)
  int totalMin  = hh * 60 + mm + diffMin;
  int dayOffset = 0;

  while (totalMin < 0)     { totalMin += 1440; dayOffset--; }
  while (totalMin >= 1440) { totalMin -= 1440; dayOffset++; }

  hh  = totalMin / 60;
  mm  = totalMin % 60;
  dd += dayOffset;

  char buf[20];
  snprintf(buf, sizeof(buf), "20%02d-%02d-%02d %02d:%02d:%02d",
           yy, mo, dd, hh, mm, ss);
  Serial.printf("[SIM] CCLK KST:    %s\n", buf);
  return String(buf);
}

// Extract SINR from AT+CPSI? (field [13] after "+CPSI:")
// Example: +CPSI: LTE,Online,450-05,0x3AF2,104793101,391,EUTRAN-BAND3,1650,4,4,-930,-110,-640,5
int simGetSinr()
{
  String resp = simSendAT("AT+CPSI?", 3000);
  int idx = resp.indexOf("+CPSI:");
  if (idx == -1) return 0;
  int pos = idx;
  for (int i = 0; i < 13; i++)
  {
    pos = resp.indexOf(',', pos + 1);
    if (pos == -1) return 0;
  }
  int end = resp.indexOf(',', pos + 1);
  if (end == -1) end = resp.indexOf('\n', pos + 1);
  if (end == -1) end = resp.length();
  return resp.substring(pos + 1, end).toInt();
}

SimInfo simGetInfo()
{
  SimInfo info = {99, 0, -113, 0, ""};

  String csqResp = simSendAT("AT+CSQ", 2000);
  int idx = csqResp.indexOf("+CSQ:");
  if (idx != -1)
  {
    int comma = csqResp.indexOf(',', idx);
    info.csq = csqResp.substring(idx + 5, comma).toInt();
  }
  info.rssi      = csqToRssi(info.csq);
  info.strength  = csqToStrength(info.csq);
  info.sinr      = simGetSinr();
  info.modemTime = simGetModemTime();

  return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────

int parseJsonInt(const String &json, const char *key)
{
  String searchKey = String("\"") + key + "\":";
  int idx = json.indexOf(searchKey);
  if (idx == -1) return -1;
  idx += searchKey.length();
  while (idx < (int)json.length() && json[idx] == ' ') idx++;
  bool neg = (json[idx] == '-');
  if (neg) idx++;
  int val = 0;
  while (idx < (int)json.length() && isdigit((unsigned char)json[idx]))
  {
    val = val * 10 + (json[idx++] - '0');
  }
  return neg ? -val : val;
}


// ─────────────────────────────────────────────────────────────────────────────
// HTTP helpers  (used for device_setting GET + image POST)
// ─────────────────────────────────────────────────────────────────────────────

// AT+HTTPREAD protocol (SIM767XX HTTP Application Note §2.2.6):
//   Send  : AT+HTTPREAD=0,<bodyLen>
//   Recv  : OK                      ← command acknowledgment (comes first)
//           +HTTPREAD: <data_len>   ← actual byte count returned
//           <data>
//           +HTTPREAD: 0            ← end marker
//
// simSendAT() must NOT be used here because it stops at the first "OK",
// which arrives before the data.
String simHttpReadBody(int bodyLen)
{
  SimSerial.println("AT+HTTPREAD=0," + String(bodyLen));

  // Step 1: wait for the acknowledgment OK
  simWaitFor("OK", 3000);

  // Step 2: collect data until the +HTTPREAD: 0 end marker (up to 10 s)
  String data = simWaitFor("+HTTPREAD: 0", 10000);
  Serial.println("[HTTP] HTTPREAD raw:\n" + data);

  // Extract body: skip "+HTTPREAD: <n>\n", stop before "+HTTPREAD: 0"
  int headerEnd = data.indexOf('\n');
  if (headerEnd == -1) return "";
  int endMarker = data.lastIndexOf("+HTTPREAD: 0");
  String body = (endMarker > headerEnd)
                ? data.substring(headerEnd + 1, endMarker)
                : data.substring(headerEnd + 1);
  body.trim();
  return body;
}

String simHttpGet(const String &url)
{
  simSendAT("AT+HTTPTERM", 1000);
  simFlush(100);

  if (simSendAT("AT+HTTPINIT", 3000).indexOf("OK") == -1)
  {
    Serial.println("[HTTP] GET: HTTPINIT failed");
    return "";
  }
  simSendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);

  SimSerial.println("AT+HTTPACTION=0");
  simWaitFor("OK", 3000);
  String actionResp = simWaitFor("+HTTPACTION:", 15000);
  Serial.println("[HTTP] GET action: " + actionResp);

  int httpCode = -1, dataLen = 0;
  int ai = actionResp.indexOf("+HTTPACTION:");
  if (ai != -1)
  {
    int c1 = actionResp.indexOf(',', ai);
    int c2 = actionResp.indexOf(',', c1 + 1);
    int c3 = actionResp.indexOf('\n', c2 + 1);
    if (c1 != -1 && c2 != -1)
    {
      httpCode = actionResp.substring(c1 + 1, c2).toInt();
      dataLen  = actionResp.substring(c2 + 1, c3 == -1 ? (int)actionResp.length() : c3).toInt();
    }
  }
  Serial.printf("[HTTP] GET code=%d  len=%d\n", httpCode, dataLen);

  String body = "";
  if (httpCode == 200 && dataLen > 0)
  {
    body = simHttpReadBody(dataLen);
    Serial.println("[HTTP] GET body: " + body);
  }

  simSendAT("AT+HTTPTERM", 2000);
  return body;
}

bool simHttpPostEmpty(const String &url)
{
  simSendAT("AT+HTTPTERM", 1000);
  simFlush(100);

  if (simSendAT("AT+HTTPINIT", 3000).indexOf("OK") == -1)
  {
    Serial.println("[HTTP] POST: HTTPINIT failed");
    return false;
  }
  simSendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);
  simSendAT("AT+HTTPPARA=\"CONTENT\",\"application/x-www-form-urlencoded\"", 3000);

  // Some SIM7670G firmware requires HTTPDATA before HTTPACTION=1
  SimSerial.println("AT+HTTPDATA=1,3000");
  if (simWaitFor("DOWNLOAD", 3000).indexOf("DOWNLOAD") != -1)
  {
    SimSerial.write(' ');
    simWaitFor("OK", 3000);
  }

  SimSerial.println("AT+HTTPACTION=1");
  simWaitFor("OK", 3000);
  String actionResp = simWaitFor("+HTTPACTION:", 15000);
  Serial.println("[HTTP] POST action: " + actionResp);

  int ai = actionResp.indexOf("+HTTPACTION:");
  if (ai == -1) { simSendAT("AT+HTTPTERM", 2000); return false; }

  int c1 = actionResp.indexOf(',', ai);
  int c2 = actionResp.indexOf(',', c1 + 1);
  int c3 = actionResp.indexOf('\n', c2 + 1);
  if (c1 == -1 || c2 == -1) { simSendAT("AT+HTTPTERM", 2000); return false; }

  int code    = actionResp.substring(c1 + 1, c2).toInt();
  int bodyLen = actionResp.substring(c2 + 1, c3 == -1 ? (int)actionResp.length() : c3).toInt();
  Serial.printf("[HTTP] POST code=%d  body_len=%d\n", code, bodyLen);

  // Read and print response headers
  String headResp = simSendAT("AT+HTTPHEAD", 5000);
  Serial.println("[HTTP] Headers:\n" + headResp);

  // Read and print response body
  if (bodyLen > 0)
  {
    String body = simHttpReadBody(bodyLen);
    Serial.println("[HTTP] POST body: " + body);
  }

  simSendAT("AT+HTTPTERM", 2000);
  return (code >= 200 && code < 300);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server communication
// ─────────────────────────────────────────────────────────────────────────────

bool simPostDeviceStatus(const SimInfo &info)
{
  int year = 2000, mon = 1, day = 1, hh = 0, mm = 0, ss = 0;
  // modemTime 이 유효한 시각(2000-01-01 기본값이 아님)일 때만 사용.
  // NTP 실패 시 모뎀 CCLK 는 2000-01-01 을 반환 → ESP32 RTC(Deep Sleep 보존) 를 대신 사용.
  bool modemTimeValid = !info.modemTime.isEmpty() &&
                        !info.modemTime.startsWith("2000-01-01");
  if (modemTimeValid)
  {
    sscanf(info.modemTime.c_str(), "%d-%d-%d %d:%d:%d",
           &year, &mon, &day, &hh, &mm, &ss);
  }
  else
  {
    struct tm t;
    if (getLocalTime(&t))
    {
      year = t.tm_year + 1900; mon = t.tm_mon + 1; day = t.tm_mday;
      hh   = t.tm_hour;        mm  = t.tm_min;      ss  = t.tm_sec;
    }
  }

  // POST 직전 배터리 최신값 갱신
  batteryRead();
  Serial.printf("[BAT] %.3f V  %d%%\n", g_batteryVoltage, g_batteryPercent);

  // All parameters go into the URL query string (space → %20)
  char url[512];
  snprintf(url, sizeof(url),
    "http://%s:%d/m2/device_status"
    "?serial_no=%s-%s"
    "&Battery_Level=%d"
    "&Temperature=%d"
    "&Modem_Time=%04d-%02d-%02d%%20%02d:%02d:%02d"
    "&Modem_Strength=%d"
    "&RSSI=%d"
    "&SINR=%d"
    "&Device_SW_Ver=%s",
    SERVER_HOST, SERVER_PORT,
    DEVICE_MODEL_PREFIX, DEVICE_UNIT_CODE,
    g_batteryPercent,
    DEVICE_TEMPERATURE,
    year, mon, day, hh, mm, ss,
    info.strength, info.rssi, info.sinr,
    DEVICE_SW_VER
  );

  Serial.println("[SIM] POST device_status: " + String(url));
  return simHttpPostEmpty(String(url));
}

bool simGetDeviceSetting()
{
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT
             + "/m2/device_setting?serial_no=" + DEVICE_SERIAL_NO;

  Serial.println("[SIM] GET device_setting");

  Serial.println(url);

  String body = simHttpGet(url);
  if (body.isEmpty())
  {
    Serial.println("[SIM] device_setting: no response");
    return false;
  }
  Serial.println("[SIM] device_setting: " + body);

  int pointId  = parseJsonInt(body, "m2_point_id");
  int deviceId = parseJsonInt(body, "m2_device_id");
  if (pointId  > 0) g_m2PointId  = pointId;
  if (deviceId > 0) g_m2DeviceId = deviceId;
  Serial.printf("[SIM] m2_point_id=%d  m2_device_id=%d\n", g_m2PointId, g_m2DeviceId);
  return true;
}

bool simSyncTime()
{
  // Server order: time.google.com → pool.ntp.org → time.cloudflare.com
  // Timezone 36 = UTC+9 = KST  (+CNTP: 0 means success per SIM767XX manual)
  const char *ntpServers[] = { "time.google.com", "pool.ntp.org", "time.cloudflare.com" };

  for (int s = 0; s < 3; s++)
  {
    Serial.printf("[SIM] CNTP: trying %s ...\n", ntpServers[s]);

    // tz=0 (UTC): 모뎀 내부 RTC 를 UTC 로 저장.
    // tz=36(KST) 을 사용하면 CCLK 가 KST 를 반환하는데 simGetModemTime() 이
    // 거기에 +9h 를 추가하여 이중 변환(UTC+18h) 이 발생함.
    String cfg = String("AT+CNTP=\"") + ntpServers[s] + "\",0";
    if (simSendAT(cfg, 5000).indexOf("OK") == -1)
    {
      Serial.println("[SIM] CNTP config failed, next server");
      continue;
    }

    // Execute NTP sync; up to 2 attempts per server
    for (int attempt = 1; attempt <= 2; attempt++)
    {
      SimSerial.println("AT+CNTP");
      simWaitFor("OK", 3000);
      String ntpResp = simWaitFor("+CNTP:", 15000);
      Serial.printf("[SIM] CNTP attempt %d: %s\n", attempt, ntpResp.c_str());

      // err_code 0 = success (SIM767XX AT Command Manual §12.2.3)
      if (ntpResp.indexOf("+CNTP: 0") != -1)
      {
        String kstStr = simGetModemTime();
        // CNTP 가 성공을 보고해도 모뎀 RTC 가 아직 2000-01-01 기본값이면 실패 처리.
        // (LTE 등록 직후 모뎀 내부 RTC 반영 지연 발생 가능)
        if (kstStr.isEmpty() || kstStr.startsWith("2000-01-01"))
        {
          Serial.println("[SIM] CNTP ok but CCLK still default — treating as fail");
          break;
        }
        return applyKSTTime(kstStr.c_str());
      }
      if (attempt < 2) delay(3000);
    }
  }

  Serial.println("[SIM] CNTP sync failed on all servers");
  return false;
}

void simConnect()
{
  // 1. NTP sync — 모뎀은 TX 사이클마다 전원이 끊겼다가 켜지므로 내부 RTC 가 항상 리셋됨.
  //    ntpSynced(RTC_DATA_ATTR) 가 true 여도 모뎀 RTC 는 무효 → 무조건 재동기화.
  bool timeOk = simSyncTime();

  // 2. Read modem info (CCLK now reflects NTP-synced time when step 1 succeeded)
  SimInfo info = simGetInfo();
  Serial.printf("[SIM] CSQ=%d  RSSI=%ddBm  Strength=%d  SINR=%ddB  Time=%s\n",
                info.csq, info.rssi, info.strength, info.sinr, info.modemTime.c_str());

  // 3. POST device status with accurate timestamp
  bool statusOk = simPostDeviceStatus(info);

  // 4. If CNTP failed before HTTP (PDP was not yet active), retry now that
  //    the HTTP session has activated the bearer
  if (!timeOk)
    timeOk = simSyncTime();

  bool settingOk = simGetDeviceSetting();
  Serial.printf("[SIM] Connect: time=%s  status=%s  setting=%s\n",
                timeOk    ? "OK" : "FAIL",
                statusOk  ? "OK" : "FAIL",
                settingOk ? "OK" : "FAIL");
  // NOTE: correctRtcFromModem() 제거.
  //   NTP 성공 → simSyncTime() 내 applyKSTTime() 이 ESP32 RTC 를 이미 정확하게 설정.
  //   NTP 실패 → 모뎀 CCLK=2000-01-01 → ESP32 RTC 에 쓰면 Deep Sleep 보존값을 파괴.
  //   어느 경우도 POST/GET 이후 CCLK 재독이 도움이 되지 않으므로 제거.
}

// ─────────────────────────────────────────────────────────────────────────────
// TCP helpers  (NETOPEN / CIPOPEN / CIPSEND)
// Used for image upload; avoids AT+HTTPDATA's 0x1A-termination bug.
// ─────────────────────────────────────────────────────────────────────────────

bool simNetOpen()
{
  // AT+HTTPTERM 잔류 상태 정리: HTTP 세션이 완전히 닫히지 않은 경우 PDP 컨텍스트
  // 충돌로 AT+NETOPEN 이 +NETOPEN: 1 을 반환할 수 있음.
  // HTTPTERM 은 이미 닫혀 있어도 ERROR 없이 무시되므로 항상 선행 실행.
  simSendAT("AT+HTTPTERM", 1000);
  simFlush(500);   // HTTP PDP 해제를 위한 최소 정착 시간

  for (int attempt = 1; attempt <= 3; attempt++)
  {
    String resp = simSendAT("AT+NETOPEN", 12000);

    if (resp.indexOf("already opened") != -1)
    {
      // NETOPEN 상태가 이미 열려 있으면 닫고 재시도
      Serial.printf("[TCP] NETOPEN: already open (attempt %d) — closing first\n", attempt);
      simSendAT("AT+NETCLOSE", 8000);
      simFlush(2000);
      continue;
    }
    if (resp.indexOf("+NETOPEN: 0") != -1) return true;
    // 일부 펌웨어는 OK 먼저 응답 후 +NETOPEN URC 비동기 수신
    if (resp.indexOf("OK") != -1)
    {
      String urc = simWaitFor("+NETOPEN:", 8000);
      if (urc.indexOf("+NETOPEN: 0") != -1) return true;
    }

    Serial.printf("[TCP] NETOPEN failed (attempt %d/3): %s\n", attempt, resp.c_str());
    if (attempt < 3)
    {
      // PDP 컨텍스트 충돌 해소를 위해 NETCLOSE 후 2초 대기 후 재시도
      simSendAT("AT+NETCLOSE", 5000);
      simFlush(2000);
    }
  }

  Serial.println("[TCP] NETOPEN failed after 3 attempts");
  return false;
}

void simNetClose()
{
  simSendAT("AT+NETCLOSE", 10000);
  simFlush(200);
}

bool simTcpOpen(int link, const char *host, int port)
{
  simSendAT("AT+CIPRXGET=1", 2000);   // buffer mode: data held until AT+CIPRXGET=2

  String cmd = String("AT+CIPOPEN=") + link
             + ",\"TCP\",\"" + host + "\"," + port;
  Serial.print("\r\n>> "); Serial.println(cmd);
  SimSerial.println(cmd);

  String resp  = simWaitFor("+CIPOPEN:", 30000);
  String token = "+CIPOPEN: " + String(link) + ",0";
  if (resp.indexOf(token) == -1)
  {
    Serial.println("[TCP] CIPOPEN failed: " + resp);
    return false;
  }
  Serial.println("[TCP] TCP connected");
  return true;
}

void simTcpClose(int link)
{
  simSendAT("AT+CIPCLOSE=" + String(link), 5000);
}

// Send exactly <len> bytes (max 1500) through an open TCP socket.
bool simTcpSendChunk(int link, const uint8_t *data, size_t len)
{
  if (len == 0) return true;

  String cmd = "AT+CIPSEND=" + String(link) + "," + String(len);
  Serial.print("\r\n>> "); Serial.println(cmd);
  SimSerial.println(cmd);

  // Wait for '>' prompt; break immediately on ERROR so we don't stall 5 s on a closed link.
  String promptResp = "";
  uint32_t t = millis();
  while (millis() - t < 5000)
  {
    while (SimSerial.available())
    {
      char c = (char)SimSerial.read();
      promptResp += c;
      Serial.write(c);
    }
    if (promptResp.indexOf('>') != -1) break;
    if (promptResp.indexOf("ERROR") != -1) break;
  }

  if (promptResp.indexOf('>') == -1)
  {
    Serial.println("[TCP] CIPSEND: no '>' prompt");
    return false;
  }
  Serial.println("[TCP] > prompt OK — sending data");

  SimSerial.write(data, len);
  SimSerial.flush();

  // Wait for +CIPSEND: ACK; also abort immediately on ERROR to avoid 10 s stall.
  String resp = "";
  uint32_t t2 = millis();
  while (millis() - t2 < 10000)
  {
    while (SimSerial.available())
    {
      char c = (char)SimSerial.read();
      resp += c;
      Serial.write(c);
    }
    if (resp.indexOf("+CIPSEND:") != -1) break;
    if (resp.indexOf("ERROR")     != -1) break;
  }

  if (resp.indexOf("OK") != -1)
    Serial.println("[TCP] SEND OK");

  // If server closed the connection while we were waiting, the modem queues a
  // +CIPCLOSE: URC inside the CIPSEND response window — catch it here.
  if (resp.indexOf("+CIPCLOSE:") != -1)
    Serial.println("[TCP] +CIPCLOSE detected in CIPSEND window — server closed connection");

  int ci = resp.indexOf("+CIPSEND:");
  if (ci == -1) { Serial.println("[TCP] CIPSEND: no response"); return false; }

  // +CIPSEND: <link>,<reqLen>,<cnfLen>
  int c1 = resp.indexOf(',', ci);
  int c2 = resp.indexOf(',', c1 + 1);
  int c3 = resp.indexOf('\n', c2 + 1);
  if (c1 == -1 || c2 == -1) return false;

  int req = resp.substring(c1 + 1, c2).toInt();
  int cnf = resp.substring(c2 + 1, c3 == -1 ? (int)resp.length() : c3).toInt();
  Serial.printf("[TCP] CIPSEND req=%d cnf=%d\n", req, cnf);
  // cnf < 0 → connection dropped; cnf == 0 → TCP send buffer full
  return (cnf > 0 && cnf == req);
}

// AT+CIPACK flow control: poll until unacknowledged bytes <= maxUnacked, or timeout.
// SIM7670G TCP send buffer is ~6 KB.  UART pushes ~11.5 KB/s at 115200 baud, which is
// faster than LTE uplink drain — so we must gate each chunk on actual TCP ACKs from
// the server rather than using a fixed inter-chunk delay.
//
// AT+CIPACK=<link>  →  +CIPACK: <txlen>,<acklen>,<nacklen>
//   txlen   : total bytes sent to modem TCP stack so far (cumulative)
//   acklen  : bytes ACKed by remote peer (cumulative)
//   nacklen : txlen - acklen = bytes still in modem TCP buffer, NOT yet ACKed
//
// Returns true when nacklen <= maxUnacked within timeoutMs.
// Returns false on timeout (caller should treat subsequent chunk as risky).
bool simTcpWaitAck(int link, size_t maxUnacked, uint32_t timeoutMs)
{
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs)
  {
    String cmd = "AT+CIPACK=" + String(link);
    Serial.print("\r\n>> "); Serial.println(cmd);
    SimSerial.println(cmd);
    String resp = simWaitFor("OK", 2000);

    // +CIPACK: <txlen>,<acklen>,<nacklen>
    int ci = resp.indexOf("+CIPACK:");
    if (ci != -1)
    {
      int c1 = resp.indexOf(',', ci);
      int c2 = resp.indexOf(',', c1 + 1);
      int c3 = resp.indexOf('\n', c2 + 1);
      if (c1 != -1 && c2 != -1)
      {
        int nacklen = resp.substring(c2 + 1,
                                     c3 == -1 ? (int)resp.length() : c3).toInt();
        Serial.printf("[TCP] CIPACK nacklen=%d (max=%u)\n",
                      nacklen, (unsigned)maxUnacked);
        if ((size_t)nacklen <= maxUnacked)
          return true;
      }
    }
    delay(50);   // 50 ms poll interval — gives LTE ~50 ms to drain before re-querying
  }
  Serial.printf("[TCP] CIPACK wait timeout (%u ms) — proceeding anyway\n",
                (unsigned)timeoutMs);
  return false;
}

// Non-blocking poll: if the modem's CIPRXGET buffer already has data, read and return it.
// Returns "" when there is nothing buffered yet.  Used to detect early server responses
// (e.g. HTTP 4xx/5xx sent before the full request body is received).
String simTcpPollResponse(int link)
{
  String qCmd = "AT+CIPRXGET=4," + String(link);
  SimSerial.println(qCmd);
  Serial.print("\r\n>> "); Serial.println(qCmd);
  String qr = simWaitFor("OK", 1000);

  int qi = qr.indexOf("+CIPRXGET: 4,");
  if (qi == -1) return "";

  int c1 = qr.indexOf(',', qi + 13);
  if (c1 == -1) return "";
  int lineEnd = qr.indexOf('\n', c1 + 1);
  int pending = qr.substring(c1 + 1,
                              lineEnd == -1 ? (int)qr.length() : lineEnd).toInt();
  if (pending == 0) return "";

  int toRead = min(pending, 1500);
  String rCmd = "AT+CIPRXGET=2," + String(link) + "," + String(toRead);
  SimSerial.println(rCmd);
  Serial.print("\r\n>> "); Serial.println(rCmd);
  String dr = simWaitFor("OK", 3000);

  int di = dr.indexOf("+CIPRXGET: 2,");
  if (di == -1) return "";
  int nl = dr.indexOf('\n', di);
  if (nl == -1) return "";

  String hdr = dr.substring(di, nl);
  int hc1 = hdr.indexOf(',', 13);
  int hc2 = hdr.indexOf(',', hc1 + 1);
  int dataLen = hdr.substring(hc1 + 1,
                               hc2 == -1 ? (int)hdr.length() : hc2).toInt();
  if (dataLen <= 0) return "";
  return dr.substring(nl + 1, nl + 1 + dataLen);
}

// Read buffered HTTP response after all request data has been sent.
// Returns the raw HTTP response text (status line + headers + body).
String simTcpReadResponse(int link, uint32_t timeoutMs)
{
  Serial.println("[TCP] Waiting for HTTP response...");
  if (simWaitFor("+CIPRXGET: 1,", timeoutMs).indexOf("+CIPRXGET: 1,") == -1)
  {
    Serial.println("[TCP] Response timeout");
    return "";
  }

  String fullResp = "";
  int    zeroReads = 0;    // consecutive "pending = 0" reads → idle guard

  for (int iter = 0; iter < 40 && zeroReads < 6; iter++)
  {
    // ── Query pending byte count ──────────────────────────────────────────
    String qCmd = "AT+CIPRXGET=4," + String(link);
    SimSerial.println(qCmd);
    Serial.print("\r\n>> "); Serial.println(qCmd);
    String qr = simWaitFor("OK", 2000);

    int qi = qr.indexOf("+CIPRXGET: 4,");
    if (qi == -1) { zeroReads++; delay(200); continue; }

    // +CIPRXGET: 4,<link>,<pending>
    int c1 = qr.indexOf(',', qi + 13);   // comma after link number
    if (c1 == -1) { zeroReads++; delay(200); continue; }
    int lineEnd = qr.indexOf('\n', c1 + 1);
    int pending = qr.substring(c1 + 1,
                               lineEnd == -1 ? (int)qr.length() : lineEnd).toInt();

    if (pending == 0) { zeroReads++; delay(200); continue; }
    zeroReads = 0;

    // ── Read up to 1500 bytes ─────────────────────────────────────────────
    int    toRead = min(pending, 1500);
    String rCmd   = "AT+CIPRXGET=2," + String(link) + "," + String(toRead);
    SimSerial.println(rCmd);
    Serial.print("\r\n>> "); Serial.println(rCmd);
    String dr = simWaitFor("OK", 5000);

    int di = dr.indexOf("+CIPRXGET: 2,");
    if (di == -1) continue;
    int nl = dr.indexOf('\n', di);
    if (nl == -1) continue;

    // +CIPRXGET: 2,<link>,<datalen>,<remaining>
    String hdr = dr.substring(di, nl);
    int hc1 = hdr.indexOf(',', 13);              // comma after link
    int hc2 = hdr.indexOf(',', hc1 + 1);         // comma after datalen
    int dataLen = hdr.substring(hc1 + 1,
                                hc2 == -1 ? (int)hdr.length() : hc2).toInt();
    if (dataLen > 0)
      fullResp += dr.substring(nl + 1, nl + 1 + dataLen);
  }

  Serial.printf("[TCP] Response: %u bytes\n", (unsigned)fullResp.length());
  return fullResp;
}

// ─────────────────────────────────────────────────────────────────────────────
// simWaitNetReg()
//   SIM7670G 가 LTE 망에 등록될 때까지 대기.
//
//   [왜 필요한가]
//   simPowerOn() 이 반환되면 모뎀 UART 가 응답하는 "부팅 완료" 상태이지,
//   LTE 망에 등록된 상태가 아님. 망 등록은 이후 15~60초(커버리지에 따라)
//   추가로 소요됨. 이 대기 없이 simConnect()를 호출하면:
//     - AT+CNTP (NTP)  → PDP 컨텍스트 없음 → 실패
//     - AT+CCLK?       → 모뎀 RTC 미초기화 → 시간 없음
//     - HTTP POST/GET  → LTE 베어러 없음 → 실패
//     - correctRtcFromModem() → 호출 불가 → ESP32 RTC 드리프트 누적
//
//   [판별 방법]
//   AT+CEREG?  +CEREG: <n>,<stat>
//     stat=1 → 홈 망 등록됨
//     stat=5 → 로밍 등록됨
//     stat=2 → 검색 중 (계속 대기)
//     stat=0 → 미등록/검색 안 함
//     stat=3 → 등록 거부 (SIM 문제 등)
//
//   [타임아웃] 기본 90초. 실패해도 simConnect()는 호출되어 서버 오류가
//   로그에 남음 (무한 대기 방지).
// ─────────────────────────────────────────────────────────────────────────────
bool simWaitNetReg(uint32_t timeoutMs)
{
  // 이미 등록된 경우 즉시 반환 (Deep Sleep 복귀 후 빠른 재등록 시)
  String first = simSendAT("AT+CEREG?", 3000);
  int fi = first.indexOf("+CEREG:");
  if (fi != -1)
  {
    int comma = first.indexOf(',', fi);
    int statStart = (comma != -1) ? comma + 1 : fi + 7;
    int stat = first.substring(statStart).toInt();
    if (stat == 1 || stat == 5)
    {
      Serial.printf("[SIM] LTE already registered (stat=%d)\n", stat);
      return true;
    }
  }

  Serial.print("[SIM] Waiting for LTE registration (up to " +
               String(timeoutMs / 1000) + " s)");

  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs)
  {
    delay(2000);
    Serial.print(".");

    String resp = simSendAT("AT+CEREG?", 3000);
    int idx = resp.indexOf("+CEREG:");
    if (idx == -1) continue;

    // +CEREG: <n>,<stat>  (query response: two fields separated by comma)
    int comma    = resp.indexOf(',', idx);
    int statStart = (comma != -1) ? comma + 1 : idx + 7;
    int stat = resp.substring(statStart).toInt();

    if (stat == 1 || stat == 5)
    {
      Serial.printf("\n[SIM] LTE registered (stat=%d, elapsed=%lu ms)\n",
                    stat, (unsigned long)(millis() - t0));
      return true;
    }

    if (stat == 3)   // 등록 거부 (SIM 인증 실패, APN 설정 오류 등)
    {
      Serial.println("\n[SIM] Registration denied (stat=3) — check SIM/APN");
      return false;
    }
    // stat=0,2: 대기 계속
  }

  Serial.println("\n[SIM] LTE registration timeout");
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// SIM7670G init
//   Boot at SIM_BAUD_INIT (115200) then upgrade to SIM_BAUD_FAST (230400).
//   AT+IPR is volatile — reverts to 115200 on power-off, so it is set every boot.
// ─────────────────────────────────────────────────────────────────────────────
bool simInit()
{
  Serial.println("[SIM] Initializing...");

  SimSerial.begin(SIM_BAUD_INIT, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);

  // Retry AT up to 10 times to tolerate cold-boot modem startup delay
  bool gotOk = false;
  for (int i = 0; i <= 10; i++)
  {
    if (simSendAT("AT", 2000).indexOf("OK") != -1)
    {
      gotOk = true;
      break;
    }
    Serial.printf("[SIM] No response (%d/10), retrying...\n", i + 1);
  }
  if (!gotOk)
  {
    Serial.println("[SIM] No modem response — init failed");
    return false;
  }
  Serial.println("[SIM] " + String(SIM_BAUD_INIT) + " bps OK");

  // Disable echo: CIPSEND binary chunks are echoed back with ATE1, which injects
  // null bytes into String responses and breaks indexOf("+CIPSEND:").
  simSendAT("ATE0", 1000);

  // ── Baud-rate upgrade ────────────────────────────────────────────────────
  // AT+IPR is volatile: resets to 115200 on every power-off, so upgrade each boot.
  // Modem sends OK at the current (115200) baud rate and then immediately switches.
  Serial.println("[SIM] Supported baud rates: " + simSendAT("AT+IPR=?", 5000));
  Serial.println("[SIM] Upgrading baud to " + String(SIM_BAUD_FAST) + " bps...");
  simSendAT("AT+IPR=" + String(SIM_BAUD_FAST), 5000);  // datasheet max response time 5000 ms
  delay(100);                 // modem settling time after baud switch
  SimSerial.end();
  delay(50);
  SimSerial.begin(SIM_BAUD_FAST, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  delay(100);
  simFlush(50);               // drain any line noise from UART reinitialization

  bool upgraded = false;
  for (int i = 0; i < 5 && !upgraded; i++)
  {
    if (simSendAT("AT", 1000).indexOf("OK") != -1)
      upgraded = true;
    else
      delay(200);
  }

  if (!upgraded)
  {
    // Revert to default baud so the device can continue operating
    Serial.println("[SIM] Baud upgrade failed — reverting to " + String(SIM_BAUD_INIT) + " bps");
    SimSerial.end();
    delay(50);
    SimSerial.begin(SIM_BAUD_INIT, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    delay(100);
    if (simSendAT("AT", 2000).indexOf("OK") == -1)
    {
      Serial.println("[SIM] Revert failed — modem unresponsive");
      return false;
    }
    Serial.println("[SIM] Reverted to " + String(SIM_BAUD_INIT) + " bps");
  }
  else
  {
    Serial.println("[SIM] " + String(SIM_BAUD_FAST) + " bps OK");
  }

  // ── NITZ 자동 시간 동기화 비활성화 ──────────────────────────────────────
  // AT+CTZU=0: 망이 제공하는 NITZ(Network Identity and Time Zone) 로
  //   모뎀 클럭이 자동 업데이트되는 것을 막는다.
  //
  // 배경: SIM7670G 는 AT+CNTP(NTP 동기화) 후에는 UTC 를 AT+CCLK? 로 반환하지만,
  //   LTE 등록 중 NITZ 업데이트가 발생하면 현지 시각(KST)으로 클럭이 덮어써진다.
  //   결과적으로 같은 simGetModemTime() 호출이 상황에 따라 UTC 또는 KST 를 반환하여
  //   KST 변환이 불규칙(+9h 중복 또는 누락)하게 된다.
  //
  // AT+CTZU=0 적용 후: 시각은 오직 AT+CNTP 로만 갱신되고 항상 UTC 를 반환.
  //   simGetModemTime() 에서 항상 +9h(NTP_GMT_OFFSET) 를 더해 KST 를 산출.
  simSendAT("AT+CTZU=0", 1000);
  Serial.println("[SIM] NITZ auto-tz update disabled (AT+CTZU=0)");

  // ── LTE 망 등록 대기 ──────────────────────────────────────────────────────
  // 이 대기가 없으면 AT+CNTP/AT+CCLK?/HTTP 가 모두 실패함.
  // 등록 실패(타임아웃)해도 simConnect()는 호출 — 서버 오류가 로그에 기록됨.
  bool netReg = simWaitNetReg(90000);
  if (!netReg)
    Serial.println("[SIM] Warning: proceeding without confirmed LTE registration");

  // ── 등록 후 망 상태 확인 ──────────────────────────────────────────────────
  Serial.println("[SIM] CEREG: " + simSendAT("AT+CEREG?", 3000));
  Serial.println("[SIM] CGREG: " + simSendAT("AT+CGREG?", 3000));
  Serial.println("[SIM] CSQ:   " + simSendAT("AT+CSQ",    2000));
  Serial.println("[SIM] CPSI:  " + simSendAT("AT+CPSI?",  3000));

  // ── 디바이스 상태 POST + 설정값 GET ──────────────────────────────────────
  simConnect();

  Serial.println("[SIM] Init complete");
  return true;
}


