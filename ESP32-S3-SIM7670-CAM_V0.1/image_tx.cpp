#include "image_tx.h"
#include "config.h"
#include "sim_modem.h"
#include "sd_storage.h"
#include "led.h"
#include "SD_MMC.h"
#include <algorithm>

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

  String p1Hdr = String("--") + HTTP_BOUNDARY + "\r\n"
               + "Content-Disposition: form-data; name=\"file_name\"\r\n"
               + "\r\n";

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

  // SIM7670G AT+HTTPDATA hard limit: 319488 bytes (per AT command manual)
  // AT+HTTPDATA= with a larger value returns ERROR immediately.
  if (totalSize > 319488)
  {
    Serial.printf("[TX] ABORT: payload %u bytes exceeds modem AT+HTTPDATA limit (319488). "
                  "Reduce JPEG quality or resolution.\n", (unsigned)totalSize);
    f.close();
    return false;
  }

  Serial.printf("[TX] %s  file=%u  total=%u bytes\n",
                fileName.c_str(), (unsigned)fileSize, (unsigned)totalSize);

  String url = String("http://") + SERVER_HOST + ":" + SERVER_PORT
             + "/m2/point_image?serial_no=" + DEVICE_SERIAL_NO;
  Serial.println("[TX] POST " + url);

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

  // 60 s DOWNLOAD window — at 115200 baud fallback a 400 KB payload needs ~35 s
  String dataCmd = "AT+HTTPDATA=" + String(totalSize) + ",60000";
  SimSerial.println(dataCmd);
  String dlResp = simWaitFor("DOWNLOAD", 5000);
  if (dlResp.indexOf("DOWNLOAD") == -1)
  {
    Serial.println("[TX] DOWNLOAD prompt not received");
    f.close();
    simSendAT("AT+HTTPTERM", 2000);
    return false;
  }
  Serial.println("[TX] DOWNLOAD ready, sending data...");

  // Brief pause so the modem fully enters data-receive mode before the first byte
  delay(100);

  SimSerial.print(p1Hdr);
  SimSerial.print(fileName);
  SimSerial.print(p2Hdr);

  // Stream JPEG in 256-byte chunks with a 5 ms inter-chunk delay.
  // At 921600 baud 256 bytes = ~2.8 ms + 5 ms gap → ~33 KB/s effective rate.
  // This prevents the SIM7670G HTTPDATA input buffer from overflowing, which
  // causes the modem to abort with ERROR before receiving all bytes.
  uint8_t buf[256];
  size_t  bytesSent = 0;
  while (f.available())
  {
    size_t rd = f.read(buf, sizeof(buf));
    SimSerial.write(buf, rd);
    delay(5);
    bytesSent += rd;
  }
  f.close();
  Serial.printf("[TX] %u bytes streamed\n", (unsigned)bytesSent);

  SimSerial.print(closing);
  SimSerial.flush();

  // Log whatever the modem sends back so failures are diagnosable
  String dataResp = simWaitFor("OK", 30000);
  Serial.println("[TX] HTTPDATA resp: " + dataResp);
  if (dataResp.indexOf("OK") == -1)
  {
    Serial.println("[TX] HTTPDATA OK not received");
    simSendAT("AT+HTTPTERM", 3000);
    return false;
  }

  SimSerial.println("AT+HTTPACTION=1");
  // Do NOT use a separate simWaitFor("OK") here.
  // The modem sends "OK" (command ACK) immediately, then "+HTTPACTION:" asynchronously.
  // If the server responds within 3 s both arrive together and a two-step read
  // discards "+HTTPACTION:" inside the first call.  One 60 s wait captures both.
  String actionResp = simWaitFor("+HTTPACTION:", 60000);
  Serial.println("[TX] HTTPACTION: " + actionResp);

  int ai = actionResp.indexOf("+HTTPACTION:");
  if (ai == -1) { simSendAT("AT+HTTPTERM", 2000); return false; }

  int c1 = actionResp.indexOf(',', ai);
  int c2 = actionResp.indexOf(',', c1 + 1);
  int c3 = actionResp.indexOf('\n', c2 + 1);
  if (c1 == -1 || c2 == -1) { simSendAT("AT+HTTPTERM", 2000); return false; }

  int code    = actionResp.substring(c1 + 1, c2).toInt();
  int bodyLen = actionResp.substring(c2 + 1, c3 == -1 ? (int)actionResp.length() : c3).toInt();
  Serial.printf("[TX] HTTP response code: %d  body_len: %d\n", code, bodyLen);

  // Print response headers
  String headResp = simSendAT("AT+HTTPHEAD", 5000);
  Serial.println("[TX] Headers:\n" + headResp);

  // Print response body
  if (bodyLen > 0)
  {
    String body = simHttpReadBody(bodyLen);
    Serial.println("[TX] Body: " + body);
  }

  simSendAT("AT+HTTPTERM", 2000);
  return (code >= 200 && code < 300);
}

String rtuToDataPath(const String &rtuPath)
{
  String p = rtuPath;
  p.replace("/RTU/", "/Data/");
  return p;
}

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
      Serial.printf("[TX] Failed (%d/3), retry in 10 s\n", attempt);
      ledBlink(255, 80, 0, 2, 300);   // orange: retrying
      delay(10000);   // give modem time to fully settle before next HTTPINIT
    }
  }

  Serial.printf("[TX] All 3 attempts failed: %s\n", rtuPath.c_str());
  ledBlink(255, 0, 0, 3, 300);        // red: permanent fail
  ledSet(0, 40, 0);
  return false;
}

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
