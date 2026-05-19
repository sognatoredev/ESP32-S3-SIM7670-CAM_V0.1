#include "battery.h"
#include <Wire.h>
#include <Arduino.h>

// ── MAX17048 register map ─────────────────────────────────────────────────────
#define MAX17048_ADDR  0x36
#define REG_VCELL      0x02   // 78.125 µV / LSB  (big-endian 16-bit)
#define REG_SOC        0x04   // upper byte = integer %, lower = 1/256 %
#define REG_VERSION    0x08   // version / presence check

// ─────────────────────────────────────────────────────────────────────────────
// Wire.begin() is called inside cameraInit() BEFORE esp_camera_init().
// sccb-ng reuses that Wire bus (sccb_i2c_port = 0).
// batteryInit() / batteryRead() simply call Wire directly — no ESP-IDF
// I2C driver headers needed, no driver_ng / old-driver conflict possible.
// ─────────────────────────────────────────────────────────────────────────────

int   g_batteryPercent = 100;
float g_batteryVoltage = 0.0f;

static bool s_ready = false;

// ── Read 2 bytes from a MAX17048 register ────────────────────────────────────
// Tries repeated-start first; falls back to STOP+START if that fails.
static uint16_t readReg(uint8_t reg)
{
  // Method A: write reg addr with repeated-start, then read 2 bytes
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  uint8_t err = Wire.endTransmission(false);   // false = repeated start
  if (err == 0)
  {
    if (Wire.requestFrom((uint8_t)MAX17048_ADDR, (uint8_t)2) == 2)
    {
      uint8_t h = Wire.read();
      uint8_t l = Wire.read();
      return ((uint16_t)h << 8) | l;
    }
  }

  // Method B: STOP then new START (some I2C controllers prefer this)
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  err = Wire.endTransmission(true);            // true = STOP
  if (err != 0) return 0xFFFF;
  delayMicroseconds(200);
  if (Wire.requestFrom((uint8_t)MAX17048_ADDR, (uint8_t)2) == 2)
  {
    uint8_t h = Wire.read();
    uint8_t l = Wire.read();
    return ((uint16_t)h << 8) | l;
  }
  return 0xFFFF;
}

// ─────────────────────────────────────────────────────────────────────────────
// batteryInit  — must be called AFTER cameraInit()
// ─────────────────────────────────────────────────────────────────────────────
bool batteryInit()
{
  Serial.println("[BAT] Probing I2C bus for MAX17048...");

  // ── Step 1: simple ACK probe ──────────────────────────────────────────────
  Wire.beginTransmission(MAX17048_ADDR);
  uint8_t probe = Wire.endTransmission(true);
  Serial.printf("[BAT] Wire.probe(0x36) = %d  (0=ACK, else=NACK/error)\n", probe);

  // ── Step 2: full bus scan (shows all responding devices) ─────────────────
  Serial.print("[BAT] I2C scan: ");
  bool anyFound = false;
  for (uint8_t a = 1; a < 127; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission(true) == 0)
    {
      Serial.printf("0x%02X ", a);
      anyFound = true;
    }
  }
  if (!anyFound) Serial.print("(none)");
  Serial.println();

  // ── Step 3: read VERSION register ────────────────────────────────────────
  uint16_t ver = readReg(REG_VERSION);
  Serial.printf("[BAT] VERSION reg = 0x%04X  (%s)\n",
                ver, ver == 0xFFFF ? "FAIL" : "OK");

  if (ver == 0xFFFF)
  {
    Serial.println("[BAT] MAX17048 not found — using default 100%");
    return false;
  }

  Serial.printf("[BAT] MAX17048 ready  ver=0x%04X\n", ver);
  s_ready = true;
  batteryRead();
  return true;
}

bool batteryIsReady() { return s_ready; }

// ─────────────────────────────────────────────────────────────────────────────
// batteryRead
// ─────────────────────────────────────────────────────────────────────────────
bool batteryRead()
{
  if (!s_ready) return false;

  uint16_t vcell = readReg(REG_VCELL);
  uint16_t soc   = readReg(REG_SOC);

  if (vcell == 0xFFFF || soc == 0xFFFF)
  {
    Serial.println("[BAT] Read error");
    return false;
  }

  g_batteryVoltage = (float)vcell * 78.125e-6f;
  float soc_f      = (float)(soc >> 8) + (float)(soc & 0xFF) / 256.0f;
  g_batteryPercent = (int)soc_f;
  if (g_batteryPercent > 100) g_batteryPercent = 100;
  if (g_batteryPercent <   0) g_batteryPercent = 0;

  Serial.printf("[BAT] %.3f V  %d%%\n", g_batteryVoltage, g_batteryPercent);
  return true;
}
