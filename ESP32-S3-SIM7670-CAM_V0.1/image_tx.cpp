#include "image_tx.h"
#include "config.h"
#include "sim_modem.h"
#include "sd_storage.h"
#include "led.h"
#include "SD_MMC.h"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Image upload  POST /m2/point_image  (multipart/form-data)
// Method: AT+CIPSEND (raw TCP) — binary-safe, no 0x1A issue, no EFS size limit.
// Data is streamed from SD directly to TCP in 1460-byte chunks.
// ─────────────────────────────────────────────────────────────────────────────

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

  // ── Build the three header sections ──────────────────────────────────────
  //
  // Hdr2: --boundary + Content-Disposition(file_name) + blank + filename value
  // Hdr3: --boundary + Content-Disposition(img_file)  + Content-Type  + blank
  //        (JPEG binary follows immediately after hdr3)
  // closing: \r\n--boundary--\r\n  (delimiter \r\n + closing boundary)
  //
  // Build hdr2/hdr3/closing first so their sizes are known for Content-Length.

  String hdr2 = String("--") + HTTP_BOUNDARY + "\r\n"
              + "Content-Disposition: form-data; name=\"file_name\"\r\n"
              + "\r\n"
              + fileName + "\r\n";

  String hdr3 = String("--") + HTTP_BOUNDARY + "\r\n"
              + "Content-Disposition: form-data; name=\"img_file\"; filename=\""
              + fileName + "\"\r\n"
              + "Content-Type: image/jpeg\r\n"
              + "\r\n";

  String closing = "\r\n--" + String(HTTP_BOUNDARY) + "--\r\n";

  // Content-Length = multipart body size (hdr2 + hdr3 + JPEG + closing)
  size_t bodySize = hdr2.length() + hdr3.length() + fileSize + closing.length();

  // Hdr1: HTTP request line + HTTP headers (order matches reference firmware log)
  String hdr1 = String("POST /m2/point_image?serial_no=") + deviceSerialNo()
              + " HTTP/1.1\r\n"
              + "Host: " + SERVER_HOST + "\r\n"
              + "Connection: close\r\n"
              + "Content-Type: multipart/form-data; boundary=" + HTTP_BOUNDARY + "\r\n"
              + "Content-Length: " + String(bodySize) + "\r\n"
              + "\r\n";

  // hdr1 + hdr2 + hdr3 combined — sent as a single CIPSEND (~388 bytes)
  String allHdrs = hdr1 + hdr2 + hdr3;

  Serial.printf("[TX] %s  file=%u  body=%u  allHdrs=%u\n",
                fileName.c_str(), (unsigned)fileSize,
                (unsigned)bodySize, (unsigned)allHdrs.length());
  Serial.println("[TX] Headers:\n" + allHdrs);

  // ── Step 1: Open TCP connection ───────────────────────────────────────────
  if (!simNetOpen())
  {
    f.close();
    return false;
  }
  if (!simTcpOpen(0, SERVER_HOST, SERVER_PORT))
  {
    f.close();
    simNetClose();
    return false;
  }

  // ── Step 2: Send all headers in one CIPSEND ───────────────────────────────
  if (!simTcpSendChunk(0, (const uint8_t *)allHdrs.c_str(), allHdrs.length()))
  {
    Serial.println("[TX] headers send failed");
    f.close(); simTcpClose(0); simNetClose();
    return false;
  }

  // ── Step 3: Stream JPEG binary in 1400-byte chunks ───────────────────────
  // const size_t CHUNK = 1400;
  const size_t CHUNK = 1500; // 2026.05.06 csh : 최대 길이인 1500 으로 변경해서 테스트
  uint8_t  buf[CHUNK];
  size_t   bytesSent = 0;
  size_t   lastReport = 0;
  bool     sendOk = true;

  while (f.available() && sendOk)
  {
    size_t rd = f.read(buf, sizeof(buf));
    sendOk = simTcpSendChunk(0, buf, rd);
    if (sendOk)
    {
      // AT+CIPACK flow control: wait until modem TCP buffer drains below 4096 bytes.
      // This replaces the old fixed delay(10/50) — adapts to actual LTE uplink speed
      // and prevents the ~6 KB modem TCP send buffer from overflowing mid-transfer.
      simTcpWaitAck(0, 4096, 5000);
    }
    bytesSent += rd;

    if (bytesSent - lastReport >= 4096 || !f.available())
    {
      Serial.printf("[TX] JPEG %u / %u bytes\n",
                    (unsigned)bytesSent, (unsigned)fileSize);
      lastReport = bytesSent;
    }
  }
  f.close();

  if (!sendOk)
  {
    Serial.println("[TX] JPEG stream failed — reading server response");
    // Server may have sent an early HTTP response (4xx/5xx) that caused the TCP close.
    // CIPRXGET buffer mode retains data even after TCP closes, so this can still succeed.
    String earlyResp = simTcpReadResponse(0, 5000);
    if (!earlyResp.isEmpty())
    {
      int code = -1;
      int si   = earlyResp.indexOf("HTTP/1.");
      if (si != -1)
      {
        int sp = earlyResp.indexOf(' ', si);
        if (sp != -1) code = earlyResp.substring(sp + 1, sp + 4).toInt();
      }
      Serial.printf("[TX] Server early response HTTP %d:\n", code);
      Serial.println(earlyResp.substring(0, min((int)earlyResp.length(), 600)));
    }
    else
    {
      Serial.println("[TX] No server response buffered");
    }
    simTcpClose(0); simNetClose();
    return false;
  }
  Serial.printf("[TX] JPEG sent: %u bytes\n", (unsigned)bytesSent);

  // ── Step 6: Send closing boundary ────────────────────────────────────────
  if (!simTcpSendChunk(0, (const uint8_t *)closing.c_str(), closing.length()))
  {
    Serial.println("[TX] closing send failed");
    simTcpClose(0); simNetClose();
    return false;
  }

  // ── Step 7: Read HTTP response ────────────────────────────────────────────
  String httpResp = simTcpReadResponse(0, 30000);
  simTcpClose(0);
  simNetClose();

  if (httpResp.isEmpty())
  {
    Serial.println("[TX] No HTTP response received");
    return false;
  }

  // Parse status code from "HTTP/1.x NNN ..."
  int code = -1;
  int si   = httpResp.indexOf("HTTP/1.");
  if (si != -1)
  {
    int sp = httpResp.indexOf(' ', si);
    if (sp != -1) code = httpResp.substring(sp + 1, sp + 4).toInt();
  }
  Serial.printf("[TX] HTTP %d\n", code);
  Serial.println("[TX] Response:\n" +
                 httpResp.substring(0, min((int)httpResp.length(), 400)));

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
      Serial.printf("[TX] Failed (%d/3), retry in 5 s\n", attempt);
      ledBlink(255, 80, 0, 2, 300);   // orange: retrying
      delay(5000);    // sendFileViaSim 내부에서 CIPCLOSE+NETCLOSE 이미 완료, PDP 안정화만 대기
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
