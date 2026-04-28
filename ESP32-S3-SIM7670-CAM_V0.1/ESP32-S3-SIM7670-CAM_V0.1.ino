#include "esp_camera.h"
#include <WiFi.h>
#include <FastLED.h>
#include "SD_MMC.h"
#include <HardwareSerial.h>
#include <vector>
#include <algorithm>

#include "board_config.h"
#include "sim_types.h"

// ===========================
// WiFi
// ===========================
const char *ssid     = "KT_GiGA_9748";
const char *password = "9cf0bkd529";

// ===========================
// NTP  (KST = UTC+9)
// ===========================
#define NTP_SERVER     "pool.ntp.org"
#define NTP_GMT_OFFSET (9L * 3600L)
#define NTP_DST_OFFSET 0

// ===========================
// SIM7670G UART
// ===========================
#define SIM_RX_PIN    17          // ESP32 RX <- SIM7670G TX
#define SIM_TX_PIN    18          // ESP32 TX -> SIM7670G RX
#define SIM_BAUD_INIT 115200
#define SIM_BAUD_WORK 921600

// ===========================
// Server
// ===========================
#define SERVER_HOST         "dev.neverlosewater.com"
#define SERVER_PORT         49152
#define DEVICE_MODEL_PREFIX "SM2-V3A"          // model code  (first  %s in template)
#define DEVICE_UNIT_CODE    "6002"             // unit serial (second %s in template)
#define DEVICE_SERIAL_NO    DEVICE_MODEL_PREFIX "-" DEVICE_UNIT_CODE
#define DEVICE_SW_VER       "0108"             // firmware version (formatted as %04d)
#define HTTP_BOUNDARY       "1818FFFF"

// ===========================
// SD folder structure
//   /RTU/Image/YYYY/MM/DD/YYYYMMDD_HHMMSS.jpg  <- saved right after capture
//   /Data/Image/YYYY/MM/DD/YYYYMMDD_HHMMSS.jpg <- moved after successful TX
// ===========================
#define SD_RTU_ROOT  "/RTU/Image"
#define SD_DATA_ROOT "/Data/Image"

// ===========================
// Device sensor placeholders
// TODO: replace with actual ADC / sensor reads
// ===========================
#define DEVICE_BATTERY_LEVEL 100
#define DEVICE_TEMPERATURE   25

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
void startCameraServer();
void setupLedFlash();

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
framesize_t current_cam_framesize;
int         current_cam_quality;
sensor_t   *camera_sensor2 = NULL;

HardwareSerial SimSerial(1);        // UART1 (GPIO17/18)
static bool    simReady = false;

// Settings fetched from GET /m2/device_setting
static int g_m2PointId  = 0;
static int g_m2DeviceId = 0;

// ─── WS2812B LED ─────────────────────────────────────────────────────────────
#define WS2812_PIN 38
#define WS2812_NUM  1

CRGB leds[WS2812_NUM];

void ledSet(uint8_t r, uint8_t g, uint8_t b)
{
  fill_solid(leds, WS2812_NUM, CRGB(r, g, b));
  FastLED.show();
}

void ledBlink(uint8_t r, uint8_t g, uint8_t b, int n, int periodMs)
{
  for (int i = 0; i < n; i++)
  {
    ledSet(r, g, b);  delay(periodMs / 2);
    ledSet(0, 0, 0);  delay(periodMs / 2);
  }
}

// ─── SD Card ─────────────────────────────────────────────────────────────────
#define SD_CLK_PIN 5
#define SD_CMD_PIN 4
#define SD_D0_PIN  6

static bool     sdReady      = false;
static uint32_t captureCount = 0;   // fallback counter when NTP not synced

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

// Create directory tree recursively (/A/B/C each level in order)
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

// ─── Capture timer ────────────────────────────────────────────────────────────
static time_t nextCaptureTime = 0;
static bool   ntpSynced       = false;

volatile bool capturePending = false;   // referenced as extern in app_httpd.cpp

time_t calcNextBoundary()
{
  time_t now; time(&now);
  return ((now / 600) + 1) * 600;
}

extern bool isStreaming;   // declared in app_httpd.cpp

// ─────────────────────────────────────────────────────────────────────────────
// SIM7670G — Low-level AT helpers
// ─────────────────────────────────────────────────────────────────────────────

// Send AT command; return full response (stops early on OK / ERROR)
String simSendAT(const String &cmd, uint32_t timeout = 3000)
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

// Wait until 'token' appears in the receive buffer (no command sent)
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

// Discard any buffered bytes from the modem
void simFlush(uint32_t delayMs = 200)
{
  delay(delayMs);
  while (SimSerial.available()) SimSerial.read();
}

// ─────────────────────────────────────────────────────────────────────────────
// SIM7670G — Signal / time helpers
// ─────────────────────────────────────────────────────────────────────────────
// SimInfo type is defined in sim_types.h

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
//   ZZ = UTC offset in quarter-hour units  (+36 = UTC+9 = KST)
//   The modem returns local time; ZZ tells us which local time that is.
//   KST offset = +36 quarters. Adjust if modem is set to a different timezone.
String simGetModemTime()
{
  String resp = simSendAT("AT+CCLK?", 3000);
  Serial.println("[SIM] CCLK raw: " + resp);

  int q1 = resp.indexOf('"');
  if (q1 == -1) return "";
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 == -1) return "";
  String s = resp.substring(q1 + 1, q2);   // e.g. "26/04/28,06:20:28+00" or "+36"
  if (s.length() < 17) return "";

  // Parse raw date/time
  int yy = s.substring(0, 2).toInt();
  int mo = s.substring(3, 5).toInt();
  int dd = s.substring(6, 8).toInt();
  int hh = s.substring(9, 11).toInt();
  int mm = s.substring(12, 14).toInt();
  int ss = s.substring(15, 17).toInt();

  // Parse UTC offset (quarter-hours).  "+36" -> +36 quarters = +540 min = KST
  int tzQuarters = 0;
  if ((int)s.length() > 17)
  {
    int sign  = (s[17] == '-') ? -1 : 1;
    tzQuarters = sign * s.substring(18).toInt();
  }
  Serial.printf("[SIM] CCLK parsed: 20%02d-%02d-%02d %02d:%02d:%02d  TZ=%+d quarters\n",
                yy, mo, dd, hh, mm, ss, tzQuarters);

  // Convert to KST: KST = local + (36 - tzQuarters) * 15 min
  int diffMin      = (36 - tzQuarters) * 15;   // minutes to add
  int totalMin     = hh * 60 + mm + diffMin;
  int dayOffset    = 0;

  while (totalMin < 0)    { totalMin += 1440; dayOffset--; }
  while (totalMin >= 1440) { totalMin -= 1440; dayOffset++; }

  hh = totalMin / 60;
  mm = totalMin % 60;
  dd += dayOffset;   // simple day shift (±1 day; month/year overflow not handled here)

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

  // CSQ
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

// Extract integer value of 'key' from flat JSON string
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
// SIM7670G — Raw TCP helpers  (used for device_status POST)
// ─────────────────────────────────────────────────────────────────────────────

// Open TCP connection to server
bool simTcpOpen()
{
  simSendAT("AT+CIPCLOSE", 1000);
  simFlush(300);

  String cmd = "AT+CIPSTART=\"TCP\",\""
             + String(SERVER_HOST) + "\",\""
             + String(SERVER_PORT) + "\"";
  String resp = simSendAT(cmd, 10000);
  Serial.println("[TCP] Open: " + resp);
  return (resp.indexOf("CONNECT OK")     != -1 ||
          resp.indexOf("ALREADY CONNECT") != -1);
}

// Close TCP connection
void simTcpClose()
{
  simSendAT("AT+CIPCLOSE", 2000);
  Serial.println("[TCP] Closed");
}

// Send raw bytes via open TCP connection (variable-length CIPSEND + Ctrl+Z)
// Returns the full received response string
String simTcpSendRaw(const char *data, size_t len)
{
  // AT+CIPSEND (variable-length mode): terminated by 0x1A (Ctrl+Z)
  SimSerial.println("AT+CIPSEND");
  String resp = simWaitFor(">", 5000);
  if (resp.indexOf(">") == -1)
  {
    Serial.println("[TCP] No > prompt");
    return "";
  }

  SimSerial.write((const uint8_t *)data, len);  // HTTP request + trailing \x1A

  // Wait for SEND OK, then collect HTTP response (up to 8 s)
  uint32_t t = millis();
  while (millis() - t < 8000)
  {
    while (SimSerial.available()) resp += (char)SimSerial.read();
    if (resp.indexOf("SEND OK") != -1 &&
        resp.indexOf("HTTP/1.1")!= -1 &&
        resp.indexOf("\r\n\r\n") != -1) break;
  }
  return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// SIM7670G — HTTP helpers  (used for device_setting GET + image POST)
// ─────────────────────────────────────────────────────────────────────────────

// HTTP GET → returns response body string; empty on error
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

  // Parse: +HTTPACTION: 0,<code>,<len>
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
    String readResp = simSendAT("AT+HTTPREAD=0," + String(dataLen), 5000);
    // Body starts after "+HTTPREAD: 0,<len>\n"
    int bi = readResp.indexOf("+HTTPREAD:");
    if (bi != -1)
    {
      bi = readResp.indexOf('\n', bi) + 1;
      int oi = readResp.lastIndexOf("\nOK");
      body = readResp.substring(bi, oi == -1 ? (int)readResp.length() : oi);
    }
  }

  simSendAT("AT+HTTPTERM", 2000);
  return body;
}

// HTTP POST with parameters in URL, no body (1-byte dummy sent for compatibility)
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
    SimSerial.write(' ');   // 1 dummy byte
    simWaitFor("OK", 3000);
  }

  SimSerial.println("AT+HTTPACTION=1");
  simWaitFor("OK", 3000);
  String actionResp = simWaitFor("+HTTPACTION:", 15000);
  Serial.println("[HTTP] POST action: " + actionResp);

  // Parse response code and body length
  int ai = actionResp.indexOf("+HTTPACTION:");
  if (ai == -1)
  {
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }
  int c1 = actionResp.indexOf(',', ai);
  int c2 = actionResp.indexOf(',', c1 + 1);
  int c3 = actionResp.indexOf('\n', c2 + 1);
  if (c1 == -1 || c2 == -1)
  {
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }
  int code    = actionResp.substring(c1 + 1, c2).toInt();
  int bodyLen = actionResp.substring(c2 + 1, c3 == -1 ? (int)actionResp.length() : c3).toInt();
  Serial.printf("[HTTP] POST code=%d  body_len=%d\n", code, bodyLen);

  // Read and print server response body
  if (bodyLen > 0)
  {
    String readResp = simSendAT("AT+HTTPREAD=0," + String(bodyLen), 3000);
    int bi = readResp.indexOf("+HTTPREAD:");
    if (bi != -1)
    {
      bi = readResp.indexOf('\n', bi) + 1;
      int oi = readResp.lastIndexOf("\nOK");
      String body = readResp.substring(bi, oi == -1 ? (int)readResp.length() : oi);
      Serial.println("[HTTP] POST body: " + body);
    }
  }

  simSendAT("AT+HTTPTERM", 2000);
  return (code >= 200 && code < 300);
}

// ─────────────────────────────────────────────────────────────────────────────
// Server communication
// ─────────────────────────────────────────────────────────────────────────────

// POST /m2/device_status  (raw TCP, format matches other-device template)
//
// Reference template:
//   "POST /m2/device_status?serial_no=%s-%s&Battery_Level=%d&"
//   "Temperature=%d&Modem_Time=%4d-%02d-%02d%%20%02d:%02d:%02d&"
//   "Modem_Strength=%d&RSSI=%d&SINR=%d&Device_SW_Ver=%04d HTTP/1.1\r\n"
//   "Host: %s\r\nConnection: keep-alive\r\n\r\n\x1A"
//
bool simPostDeviceStatus(const SimInfo &info)
{
  // Parse KST time from "YYYY-MM-DD HH:MM:SS"
  int year = 2000, mon = 1, day = 1, hh = 0, mm = 0, ss = 0;
  if (!info.modemTime.isEmpty())
  {
    sscanf(info.modemTime.c_str(), "%d-%d-%d %d:%d:%d",
           &year, &mon, &day, &hh, &mm, &ss);
  }
  else
  {
    // Fallback: NTP time
    struct tm t;
    if (getLocalTime(&t))
    {
      year = t.tm_year + 1900; mon = t.tm_mon + 1; day = t.tm_mday;
      hh   = t.tm_hour;        mm  = t.tm_min;      ss  = t.tm_sec;
    }
  }

  // Build raw HTTP request (identical structure to the reference template)
  // %s-%s  → DEVICE_MODEL_PREFIX "-" DEVICE_UNIT_CODE  (e.g. "SM2-V3A-6002")
  // %%20   → literal "%20" in the URL (snprintf escapes %)
  // %04d   → Device_SW_Ver as zero-padded integer (e.g. 0108)
  // \x1A   → Ctrl+Z : AT+CIPSEND variable-length terminator (not sent over HTTP)
  char req[512];
  int reqLen = snprintf(req, sizeof(req) - 2,
    "POST /m2/device_status?serial_no=%s-%s&Battery_Level=%d&"
    "Temperature=%d&Modem_Time=%04d-%02d-%02d%%20%02d:%02d:%02d&"
    "Modem_Strength=%d&RSSI=%d&SINR=%d&Device_SW_Ver=%04d HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Connection: keep-alive\r\n\r\n",
    DEVICE_MODEL_PREFIX, DEVICE_UNIT_CODE,
    DEVICE_BATTERY_LEVEL,
    DEVICE_TEMPERATURE,
    year, mon, day, hh, mm, ss,
    info.strength, info.rssi, info.sinr,
    atoi(DEVICE_SW_VER),
    SERVER_HOST
  );
  req[reqLen]     = '\x1A';  // Ctrl+Z: AT+CIPSEND variable-length terminator
  req[reqLen + 1] = '\0';

  Serial.printf("[SIM] POST device_status (%d bytes + Ctrl+Z)\n", reqLen);
  // Print request (replace \x1A with visible marker for readability)
  Serial.println("[SIM] Request:\n" + String(req));

  if (!simTcpOpen())
  {
    Serial.println("[SIM] TCP open failed");
    return false;
  }

  String resp = simTcpSendRaw(req, reqLen + 1);  // reqLen HTTP bytes + 1 Ctrl+Z
  simTcpClose();

  Serial.println("[SIM] device_status response:\n" + resp);

  // Success: server replies with HTTP/1.1 2xx
  return (resp.indexOf("HTTP/1.1 2") != -1);
}

// GET /m2/device_setting  — stores m2_point_id / m2_device_id in globals
bool simGetDeviceSetting()
{
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT
             + "/m2/device_setting?serial_no=" + DEVICE_SERIAL_NO;

  Serial.println("[SIM] GET device_setting");
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

// Combined connect: POST status -> GET settings
void simConnect()
{
  SimInfo info = simGetInfo();
  Serial.printf("[SIM] CSQ=%d  RSSI=%ddBm  Strength=%d  SINR=%ddB  Time=%s\n",
                info.csq, info.rssi, info.strength, info.sinr, info.modemTime.c_str());

  bool statusOk  = simPostDeviceStatus(info);
  bool settingOk = simGetDeviceSetting();
  Serial.printf("[SIM] Connect result: status=%s  setting=%s\n",
                statusOk ? "OK" : "FAIL",
                settingOk ? "OK" : "FAIL");
}

// ─────────────────────────────────────────────────────────────────────────────
// Image upload  POST /m2/point_image  (multipart/form-data)
// ─────────────────────────────────────────────────────────────────────────────
//
// Multipart body structure:
//   --1818FFFF\r\n
//   Content-Disposition: form-data; name="file_name"\r\n
//   \r\n
//   20260317_152007.jpg
//   \r\n--1818FFFF\r\n
//   Content-Disposition: form-data; name="img_file"; filename="20260317_152007.jpg"\r\n
//   Content-Type: image/jpeg\r\n
//   \r\n
//   [binary JPEG data]
//   \r\n--1818FFFF--\r\n
//
bool sendFileViaSim(const String &filePath)
{
  // ── Open file ──
  File f = SD_MMC.open(filePath, FILE_READ);
  if (!f)
  {
    Serial.printf("[TX] Cannot open: %s\n", filePath.c_str());
    return false;
  }
  size_t fileSize = f.size();
  if (fileSize == 0)
  {
    f.close();
    Serial.println("[TX] File is empty");
    return false;
  }

  String fileName = filePath.substring(filePath.lastIndexOf('/') + 1);

  // ── Build multipart parts ──
  String p1Hdr = String("--") + HTTP_BOUNDARY + "\r\n"
               + "Content-Disposition: form-data; name=\"file_name\"\r\n"
               + "\r\n";
  // p1 body = fileName (no trailing \r\n; next boundary prefix adds \r\n)

  String p2Hdr = "\r\n--" + String(HTTP_BOUNDARY) + "\r\n"
               + "Content-Disposition: form-data; name=\"img_file\"; filename=\"" + fileName + "\"\r\n"
               + "Content-Type: image/jpeg\r\n"
               + "\r\n";

  String closing = "\r\n--" + String(HTTP_BOUNDARY) + "--\r\n";

  size_t totalSize = p1Hdr.length()
                   + fileName.length()
                   + p2Hdr.length()
                   + fileSize
                   + closing.length();

  Serial.printf("[TX] %s  file=%u  total=%u bytes\n",
                fileName.c_str(), (unsigned)fileSize, (unsigned)totalSize);

  // ── Build URL ──
  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT
             + "/m2/point_image?serial_no=" + DEVICE_SERIAL_NO;

  // ── Init HTTP session ──
  simSendAT("AT+HTTPTERM", 1000);
  simFlush(100);

  if (simSendAT("AT+HTTPINIT", 3000).indexOf("OK") == -1)
  {
    Serial.println("[TX] HTTPINIT failed");
    f.close();
    return false;
  }
  simSendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);
  simSendAT("AT+HTTPPARA=\"CONTENT\",\"multipart/form-data; boundary=" + String(HTTP_BOUNDARY) + "\"", 3000);

  // ── Initiate data input (10 s window) ──
  String dataCmd = "AT+HTTPDATA=" + String(totalSize) + ",10000";
  SimSerial.println(dataCmd);
  String dlResp = simWaitFor("DOWNLOAD", 5000);
  if (dlResp.indexOf("DOWNLOAD") == -1)
  {
    Serial.println("[TX] DOWNLOAD prompt not received");
    f.close();
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }

  // ── Stream multipart body ──
  SimSerial.print(p1Hdr);
  SimSerial.print(fileName);
  SimSerial.print(p2Hdr);

  // Stream JPEG file in 512-byte chunks
  uint8_t buf[512];
  while (f.available())
  {
    size_t rd = f.read(buf, sizeof(buf));
    SimSerial.write(buf, rd);
  }
  f.close();

  SimSerial.print(closing);

  // Wait for modem to confirm data received
  if (simWaitFor("OK", 5000).indexOf("OK") == -1)
  {
    Serial.println("[TX] HTTPDATA OK not received");
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }

  // ── Execute POST ──
  SimSerial.println("AT+HTTPACTION=1");
  simWaitFor("OK", 3000);
  // Allow up to 30 s for server to respond (large payload upload)
  String actionResp = simWaitFor("+HTTPACTION:", 30000);
  Serial.println("[TX] HTTPACTION: " + actionResp);

  // Parse: +HTTPACTION: 1,<code>,<len>
  int ai = actionResp.indexOf("+HTTPACTION:");
  if (ai == -1)
  {
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }
  int c1 = actionResp.indexOf(',', ai);
  int c2 = actionResp.indexOf(',', c1 + 1);
  int c3 = actionResp.indexOf('\n', c2 + 1);
  if (c1 == -1 || c2 == -1)
  {
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }
  int code    = actionResp.substring(c1 + 1, c2).toInt();
  int bodyLen = actionResp.substring(c2 + 1, c3 == -1 ? (int)actionResp.length() : c3).toInt();
  Serial.printf("[TX] HTTP response code: %d  body_len: %d\n", code, bodyLen);

  // Read and print server response body
  if (bodyLen > 0)
  {
    String readResp = simSendAT("AT+HTTPREAD=0," + String(bodyLen), 3000);
    int bi = readResp.indexOf("+HTTPREAD:");
    if (bi != -1)
    {
      bi = readResp.indexOf('\n', bi) + 1;
      int oi = readResp.lastIndexOf("\nOK");
      String body = readResp.substring(bi, oi == -1 ? (int)readResp.length() : oi);
      Serial.println("[TX] Server response: " + body);
    }
  }

  simSendAT("AT+HTTPTERM", 2000);
  return (code >= 200 && code < 300);
}

// ─────────────────────────────────────────────────────────────────────────────
// RTU file transfer logic
// ─────────────────────────────────────────────────────────────────────────────

// /RTU/... -> /Data/...
String rtuToDataPath(const String &rtuPath)
{
  String p = rtuPath;
  p.replace("/RTU/", "/Data/");
  return p;
}

// Single file: up to 3 TX attempts; on success rename to /Data/
bool sendWithRetry(const String &rtuPath)
{
  String dataPath = rtuToDataPath(rtuPath);

  for (int attempt = 1; attempt <= 3; attempt++)
  {
    Serial.printf("[TX] Attempt %d/3: %s\n", attempt, rtuPath.c_str());
    ledSet(0, 0, 50);   // blue: transmitting

    if (sendFileViaSim(rtuPath))
    {
      String dataDir = dataPath.substring(0, dataPath.lastIndexOf('/'));
      sdMkdirRecursive(dataDir.c_str());

      if (SD_MMC.rename(rtuPath.c_str(), dataPath.c_str()))
      {
        Serial.printf("[TX] OK -> %s\n", dataPath.c_str());
      }
      else
      {
        // TX succeeded but rename failed; leave in RTU to avoid data loss
        Serial.printf("[TX] OK but rename failed (RTU retained): %s\n", rtuPath.c_str());
      }
      ledSet(0, 40, 0);
      return true;
    }

    if (attempt < 3)
    {
      Serial.printf("[TX] Failed (%d/3), retry in 3 s\n", attempt);
      ledBlink(255, 80, 0, 2, 300);  // orange: retrying
      delay(3000);
    }
  }

  Serial.printf("[TX] All 3 attempts failed: %s\n", rtuPath.c_str());
  ledBlink(255, 0, 0, 3, 300);       // red: permanent fail
  ledSet(0, 40, 0);
  return false;
}

// Recursively collect .jpg paths under dirPath
void collectRtuFiles(const char *dirPath, std::vector<String> &fileList)
{
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  File entry = dir.openNextFile();
  while (entry)
  {
    if (entry.isDirectory())
    {
      collectRtuFiles(entry.path(), fileList);
    }
    else
    {
      String p = String(entry.path());
      if (p.endsWith(".jpg") || p.endsWith(".JPG")) fileList.push_back(p);
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
}

// Retry all files in /RTU/Image, oldest first (filename YYYYMMDD_HHMMSS = alphabetical order)
void retryPendingFiles()
{
  std::vector<String> pending;
  collectRtuFiles(SD_RTU_ROOT, pending);

  if (pending.empty())
  {
    Serial.println("[RTU] No pending files");
    return;
  }

  std::sort(pending.begin(), pending.end());
  Serial.printf("[RTU] Pending: %d file(s) — retrying oldest first\n", (int)pending.size());

  int succ = 0, fail = 0;
  for (const String &path : pending)
  {
    Serial.printf("[RTU] Retry: %s\n", path.c_str());
    if (sendWithRetry(path)) succ++;
    else                     fail++;
  }
  Serial.printf("[RTU] Done  success=%d  failed=%d\n", succ, fail);
}

// ─────────────────────────────────────────────────────────────────────────────
// SIM7670G init
//   115200 basic check -> upgrade to 921600 -> network log -> server connect
// ─────────────────────────────────────────────────────────────────────────────
bool simInit()
{
  Serial.println("[SIM] Initializing...");

  SimSerial.begin(SIM_BAUD_INIT, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  delay(2000);

  #if 1 // fixed csh 2026.04.28
  for (int i =0; i<=10; i++)
  {
    if (simSendAT("AT", 2000).indexOf("OK") == -1)
    {
      Serial.println("[SIM] No modem response — init failed");
      
    }
  }
  return false;
  #else
  if (simSendAT("AT", 2000).indexOf("OK") == -1)
  {
    Serial.println("[SIM] No modem response — init failed");
    return false;
  }
  #endif
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

// ─────────────────────────────────────────────────────────────────────────────
// Capture -> /RTU/ save -> LTE TX -> retry pending
// ─────────────────────────────────────────────────────────────────────────────
void captureAndSave()
{
  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo);

  if (hasTime)
  {
    Serial.printf("\n[CAP] %04d/%02d/%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }
  else
  {
    Serial.println("\n[CAP] No time sync");
  }

  if (!sdReady)
  {
    Serial.println("[CAP] SD not ready, skipping");
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }

  // Pause streaming
  capturePending = true;
  if (isStreaming)
  {
    delay(300);   // let current frame finish
    Serial.println("[CAP] Streaming paused");
  }

  // Switch to FHD for the still capture
  framesize_t prevFramesize = current_cam_framesize;
  sensor_t   *s             = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_FHD);
  delay(300);

  // Discard stale frame from previous resolution
  {
    camera_fb_t *fl = esp_camera_fb_get();
    if (fl) esp_camera_fb_return(fl);
  }

  // Flash on -> capture -> flash off
  ledSet(255, 255, 255);  delay(200);
  camera_fb_t *fb = esp_camera_fb_get();
  ledSet(0, 40, 0);

  // Restore streaming resolution
  s->set_framesize(s, prevFramesize);
  current_cam_framesize = prevFramesize;
  capturePending = false;

  if (!fb)
  {
    Serial.println("[CAP] Capture failed");
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }

  size_t imgLen = fb->len;   // save before fb_return
  char   dirPath[56], filePath[80];

  if (hasTime)
  {
    snprintf(dirPath, sizeof(dirPath), "%s/%04d/%02d/%02d",
             SD_RTU_ROOT,
             timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday);
    snprintf(filePath, sizeof(filePath), "%s/%04d%02d%02d_%02d%02d%02d.jpg",
             dirPath,
             timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    sdMkdirRecursive(dirPath);
  }
  else
  {
    snprintf(filePath, sizeof(filePath), "/RTU/IMG_%06lu.jpg", (unsigned long)captureCount);
  }

  // Write to /RTU/
  File file = SD_MMC.open(filePath, FILE_WRITE);
  if (!file)
  {
    Serial.printf("[CAP] Cannot open: %s\n", filePath);
    esp_camera_fb_return(fb);
    ledBlink(255, 0, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }

  size_t written = file.write(fb->buf, imgLen);
  file.close();
  esp_camera_fb_return(fb);
  captureCount++;

  if (written != imgLen)
  {
    Serial.printf("[CAP] Write error %u/%u bytes\n", (unsigned)written, (unsigned)imgLen);
    ledBlink(255, 80, 0, 3, 400);
    ledSet(0, 40, 0);
    return;
  }
  Serial.printf("[CAP] Saved: %s (%u bytes)\n", filePath, (unsigned)written);

  if (simReady)
  {
    sendWithRetry(String(filePath));
    retryPendingFiles();
  }
  else
  {
    Serial.println("[SIM] Not ready — file retained in RTU");
  }

  ledSet(0, 40, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Camera helpers
// ─────────────────────────────────────────────────────────────────────────────
void SetCameraFramesize(int size)
{
  if (size < 0 || size > 21)
  {
    Serial.println("[CAM] Unsupported framesize");
    return;
  }
  current_cam_framesize = (framesize_t)size;
  camera_sensor2->set_framesize(camera_sensor2, current_cam_framesize);
  Serial.printf("[CAM] Framesize -> %d\n", size);
}

void SetCameraQuality(int quality)
{
  if (quality < 10 || quality > 63)
  {
    Serial.println("[CAM] Unsupported quality");
    return;
  }
  current_cam_quality = quality;
  camera_sensor2->set_quality(camera_sensor2, current_cam_quality);
}

// ─────────────────────────────────────────────────────────────────────────────
// setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  // LED: blue = booting
  FastLED.addLeds<WS2812B, WS2812_PIN, RGB>(leds, WS2812_NUM);
  FastLED.setBrightness(255);
  ledSet(0, 0, 50);

  // ── Camera init ──
  camera_config_t config;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 1;

  if (config.pixel_format == PIXFORMAT_JPEG)
  {
    if (psramFound())
    {
      // Pre-allocate max (5MP) buffer to prevent FB-OVF on runtime res switch
      config.frame_size   = FRAMESIZE_QSXGA;
      config.jpeg_quality = 10;
      config.fb_count     = 1;
      config.grab_mode    = CAMERA_GRAB_LATEST;
    }
    else
    {
      config.frame_size  = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  }
  else
  {
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("[CAM] Init failed 0x%x\n", err);
    ledBlink(255, 0, 0, 10, 200);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("[CAM] Sensor PID: 0x%04X\n", s->id.PID);

  if (s->id.PID == OV3660_PID)
  {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
  }
  if (config.pixel_format == PIXFORMAT_JPEG)
  {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  camera_sensor2 = esp_camera_sensor_get();
  SetCameraFramesize(11);   // HD 1280x720 (streaming default)

  // ── SD init ──
  if (sdSetup())
  {
    sdReady = true;
    ledBlink(0, 255, 0, 2, 200);    // green x2: SD OK
  }
  else
  {
    ledBlink(255, 0, 0, 5, 300);    // red x5: SD fail
  }

  // ── SIM7670G init ──
  ledSet(0, 0, 50);
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
  WiFi.begin(ssid, password);
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
  configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
  Serial.print("[NTP] Syncing");
  struct tm timeinfo;
  int ntpRetry = 0;
  while (!getLocalTime(&timeinfo) && ntpRetry < 20)
  {
    delay(500); Serial.print("."); ntpRetry++;
  }

  if (ntpRetry < 20)
  {
    ntpSynced = true;
    Serial.printf("\n[NTP] %04d/%02d/%02d %02d:%02d:%02d KST\n",
                  timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    nextCaptureTime = calcNextBoundary();
    struct tm nextTm;
    localtime_r(&nextCaptureTime, &nextTm);
    Serial.printf("[CAP] Next capture: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);
  }
  else
  {
    Serial.println("\n[NTP] Sync failed");
    ledBlink(255, 80, 0, 5, 200);
  }

  startCameraServer();
  Serial.printf("[SYS] Ready  http://%s\n", WiFi.localIP().toString().c_str());
  ledSet(0, 40, 0);   // green: standby
}

// ─────────────────────────────────────────────────────────────────────────────
// loop()  — check 10-minute boundary every 500 ms
// ─────────────────────────────────────────────────────────────────────────────
void loop()
{
  if (ntpSynced && nextCaptureTime > 0)
  {
    time_t now;
    time(&now);

    if (now >= nextCaptureTime)
    {
      nextCaptureTime = ((now / 600) + 1) * 600;

      struct tm nextTm;
      localtime_r(&nextCaptureTime, &nextTm);
      Serial.printf("[CAP] Next: %02d:%02d:00 KST\n", nextTm.tm_hour, nextTm.tm_min);

      captureAndSave();
    }
  }
  delay(500);
}
