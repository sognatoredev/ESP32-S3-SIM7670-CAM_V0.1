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

void simPowerOn()
{
  Serial.println("[SIM] Power ON: PWRKEY LOW " + String(SIM_PWRON_PULSE_MS) + " ms");
  digitalWrite(SIM_PWRKEY_PIN, LOW);
  delay(SIM_PWRON_PULSE_MS);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);

  Serial.print("[SIM] Waiting for modem boot");
  for (int i = 0; i < SIM_BOOT_WAIT_MS / 500; i++)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" done");
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
//   ZZ = UTC offset in quarter-hour units (+36 = UTC+9 = KST)
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

  // Convert to KST: KST = local + (36 - tzQuarters) * 15 min
  int diffMin   = (36 - tzQuarters) * 15;
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
  if (!info.modemTime.isEmpty())
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

    String cfg = String("AT+CNTP=\"") + ntpServers[s] + "\",36";
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
        if (kstStr.isEmpty())
        {
          Serial.println("[SIM] CNTP ok but CCLK read failed");
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
  // 1. NTP sync first — modem may auto-activate PDP for CNTP.
  //    If the modem already has a valid RTC this still refreshes it.
  bool timeOk = ntpSynced ? true : simSyncTime();

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

  // 5. RTC 드리프트 보정: Deep/Light Sleep 중 내부 RC 발진기(150kHz, ±5%) 사용으로
  //    10분 슬립 시 최대 ±30초 오차 누적 가능.
  //    HTTP 통신 성공 = 모뎀 LTE 등록됨 = 모뎀 RTC 가 네트워크 시간으로 확정됨.
  //    correctRtcFromModem() 은 settimeofday() 만 수행하고 nextCaptureTime 은 변경 안 함.
  //    applyKSTTime() 을 쓰면 nextCaptureTime 이 재계산되어 캡처 스케줄이 한 주기 밀림.
  if (statusOk || settingOk)
  {
    String freshKst = simGetModemTime();   // AT+CCLK? 재독 (HTTP 이후 → LTE 시간 확정)
    if (!freshKst.isEmpty())
      correctRtcFromModem(freshKst.c_str());   // settimeofday() 만 수행, nextCaptureTime 불변
  }
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

  // Network status
  Serial.println("[SIM] CGREG: " + simSendAT("AT+CGREG?", 3000));
  Serial.println("[SIM] CSQ:   " + simSendAT("AT+CSQ",    2000));
  Serial.println("[SIM] CPSI:  " + simSendAT("AT+CPSI?",  3000));

  // POST device status + GET device settings
  simConnect();

  Serial.println("[SIM] Init complete");
  return true;
}


