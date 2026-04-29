#include "sim_modem.h"
#include "config.h"
#include "time_sync.h"
#include <Arduino.h>

HardwareSerial SimSerial(1);   // UART1 (GPIO17/18)
bool simReady    = false;
int  g_m2PointId  = 0;
int  g_m2DeviceId = 0;

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

void simPowerOff()
{
  Serial.println("[SIM] Power OFF: PWRKEY LOW " + String(SIM_PWROFF_PULSE_MS) + " ms");
  digitalWrite(SIM_PWRKEY_PIN, LOW);
  delay(SIM_PWROFF_PULSE_MS);
  digitalWrite(SIM_PWRKEY_PIN, HIGH);
  Serial.println("[SIM] Modem power off");
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level AT helpers
// ─────────────────────────────────────────────────────────────────────────────

String simSendAT(const String &cmd, uint32_t timeout)
{
  SimSerial.println(cmd);
  String   resp = "";
  uint32_t t    = millis();
  while (millis() - t < timeout)
  {
    while (SimSerial.available()) resp += (char)SimSerial.read();
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
    while (SimSerial.available()) resp += (char)SimSerial.read();
    if (resp.indexOf(token) != -1) break;
  }
  return resp;
}

void simFlush(uint32_t delayMs)
{
  delay(delayMs);
  while (SimSerial.available()) SimSerial.read();
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
    DEVICE_BATTERY_LEVEL,
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
}

// ─────────────────────────────────────────────────────────────────────────────
// SIM7670G init
//   115200 basic check -> upgrade to 921600 -> network log -> server connect
// ─────────────────────────────────────────────────────────────────────────────
bool simInit()
{
  Serial.println("[SIM] Initializing...");

  SimSerial.begin(SIM_BAUD_INIT, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  // Boot delay is handled by simPowerOn(); no extra delay needed here

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
  Serial.println("[SIM] 115200 bps OK");

  // Upgrade UART speed
  simSendAT("AT+IPR=921600", 1000);
  SimSerial.end();
  delay(100);
  SimSerial.begin(SIM_BAUD_WORK, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  delay(500);

  if (simSendAT("AT", 2000).indexOf("OK") == -1)
  {
    Serial.println("[SIM] 921600 upgrade failed, fallback to 115200");
    SimSerial.end();
    SimSerial.begin(SIM_BAUD_INIT, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
    delay(300);
  }
  else
  {
    Serial.println("[SIM] 921600 bps OK");
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
