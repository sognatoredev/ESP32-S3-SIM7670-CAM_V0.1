#include "battery.h"
#include <Wire.h>
#include <Arduino.h>

// Wire(I2C_NUM_0) 는 camera_mgr.cpp 에서 esp_camera_init() 전에 초기화됨.
// pin_sccb_sda/scl = -1, sccb_i2c_port = 0 설정으로 카메라가 Wire 버스를 공유하며,
// 여기서도 동일한 Wire 인스턴스로 MAX17048 에 접근함.

static constexpr uint8_t MAX17048_ADDR = 0x36;
static constexpr uint8_t REG_VCELL     = 0x02;  // 전압 (78.125 µV/LSB, big-endian)
static constexpr uint8_t REG_SOC       = 0x04;  // 잔량 (상위 8bit=정수%, 하위 8bit=1/256%)
static constexpr uint8_t REG_VERSION   = 0x08;  // IC 존재 확인

float g_batteryVoltage = 0.0f;
int   g_batteryPercent = 100;

static bool s_ready = false;

// MAX17048 레지스터 2바이트 읽기. 실패 시 0xFFFF 반환.
static uint16_t readReg(uint8_t reg)
{
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) == 0)  // repeated-start
  {
    if (Wire.requestFrom((uint8_t)MAX17048_ADDR, (uint8_t)2) == 2)
    {
      uint8_t h = Wire.read();
      uint8_t l = Wire.read();
      return ((uint16_t)h << 8) | l;
    }
  }
  // fallback: STOP → new START
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return 0xFFFF;
  delayMicroseconds(200);
  if (Wire.requestFrom((uint8_t)MAX17048_ADDR, (uint8_t)2) == 2)
  {
    uint8_t h = Wire.read();
    uint8_t l = Wire.read();
    return ((uint16_t)h << 8) | l;
  }
  return 0xFFFF;
}

bool batteryInit()
{
  s_ready = false;
  Serial.println("[BAT] Probing MAX17048 on I2C bus...");

  Wire.beginTransmission(MAX17048_ADDR);
  uint8_t probe = Wire.endTransmission(true);
  Serial.printf("[BAT] Wire.probe(0x36) = %d  (0=ACK, else=NACK/error)\n", probe);

  // I2C 버스 전체 스캔 (디버그용)
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

  uint16_t ver = readReg(REG_VERSION);
  Serial.printf("[BAT] VERSION reg = 0x%04X  (%s)\n",
                ver, ver == 0xFFFF ? "FAIL" : "OK");

  if (ver == 0xFFFF)
  {
    Serial.println("[BAT] MAX17048 not found — battery level fixed at 100%%");
    return false;
  }

  Serial.printf("[BAT] MAX17048 ready  ver=0x%04X\n", ver);
  s_ready = true;
  batteryRead();
  return true;
}

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
