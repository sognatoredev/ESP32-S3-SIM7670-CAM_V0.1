#include "camera_mgr.h"
#include "board_config.h"
#include "led.h"
#include "ov5640_af.h"
#include <Arduino.h>
#include <Wire.h>

framesize_t   current_cam_framesize;
int           current_cam_quality;
sensor_t     *camera_sensor2 = NULL;
volatile bool capturePending = false;

// ── 포커스 저장 상태 ──────────────────────────────────────────────────────
RTC_DATA_ATTR int g_savedFocusPos = FOCUS_POS_UNSET;  // -1 = 자동 SAF 사용

bool cameraInit()
{
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
  // pin_sccb_sda/scl 를 -1 로 설정하면 esp_camera 가 자체 GPIO 초기화를 건너뛰고
  // sccb_i2c_port=0 으로 Wire(I2C_NUM_0) 의 버스를 그대로 재사용함.
  // 이렇게 해야 Wire 와 카메라 SCCB 가 같은 드라이버(driver_ng) 를 공유할 수 있음.
  config.pin_sccb_sda  = -1;
  config.pin_sccb_scl  = -1;
  config.sccb_i2c_port = 0;
  config.pin_pwdn      = PWDN_GPIO_NUM;
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

  // Wire 를 먼저 초기화해야 esp_camera_init() 이 sccb_i2c_port=0 으로
  // 동일한 Wire 버스 핸들(driver_ng)을 재사용할 수 있음.
  Wire.begin(SIOD_GPIO_NUM, SIOC_GPIO_NUM, 400000);

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("[CAM] Init failed 0x%x\n", err);
    ledBlink(255, 0, 0, 10, 200);
    return false;
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

  camera_sensor2 = esp_camera_sensor_get();

  SetCameraFramesize(11);   // HD 1280x720 (streaming default)

  SetCameraMirror(0x06); // 2026.05.04 feat.CSH : 이미지 좌우 반전

  // AF 펌웨어 로드 (OV5640 AF 모듈 전용 — 실패해도 카메라는 계속 동작)
  ov5640AfInit();

  return true;
}

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

void SetCameraMirror(int enable)
{
  Serial.println("[CAM] Set Mirror Enable.");
  camera_sensor2->set_hmirror(camera_sensor2, enable);
  Serial.printf("[CAM] Mirror Enable/Disable -> %d\n", enable);
}

// ─────────────────────────────────────────────────────────────────────────────
// 포커스 저장 구현
//
// OV5640 VCM(Voice Coil Motor) 레지스터:
//   0x3602[5:4] = slew rate  (00=slow / 10=fast)
//   0x3602[1:0] = pos[9:8]   (MSB 2비트)
//   0x3603[7:0] = pos[7:0]   (LSB 8비트)
//   pos 범위: 0(무한대) ~ ~1023(접사)
//
// 동작 흐름:
//   세팅모드  : cameraDoSingleAF()  — SAF + 완료 대기 → VCM 위치 읽기
//              cameraApplyFocusPos() — 수동 +/- 조절
//              저장 버튼           → g_savedFocusPos = cameraGetVcmPos()
//   운영모드  : g_savedFocusPos >= 0 → cameraApplyFocusPos(g_savedFocusPos) 로
//              AF 탐색 없이 즉시 동일 초점 재현 (빠름).
//              g_savedFocusPos == -1 → 기존 SAF 자동 실행.
// ─────────────────────────────────────────────────────────────────────────────

int cameraGetVcmPos()
{
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return -1;
  uint8_t r2 = (uint8_t)s->get_reg(s, 0x3602, 0xFF);
  uint8_t r3 = (uint8_t)s->get_reg(s, 0x3603, 0xFF);
  return ((r2 & 0x03) << 8) | r3;
}

void cameraApplyFocusPos(int pos)
{
  if (pos < 0)    pos = 0;
  if (pos > 1023) pos = 1023;

  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  // 8051 MCU 정지 — MCU 가 VCM 값을 덮어쓰지 않도록 PAUSE 명령 전송
  s->set_reg(s, 0x3022, 0xFF, 0x08);   // CMD_MAIN = PAUSE(0x08)
  delay(60);

  // VCM 위치 기록 (slew rate = fast : 0x20)
  s->set_reg(s, 0x3602, 0xFF, 0x20 | ((pos >> 8) & 0x03));
  s->set_reg(s, 0x3603, 0xFF,  pos & 0xFF);
  delay(120);   // VCM 렌즈 물리적 이동 + 안정화 대기

  Serial.printf("[FOCUS] VCM pos → %d\n", pos);
}

int cameraDoSingleAF(uint32_t timeoutMs)
{
  if (ov5640AfTriggerSingle() != 0) {
    Serial.println("[FOCUS] SAF trigger failed");
    return -1;
  }
  bool ok  = ov5640AfWaitFocus(timeoutMs);
  int  pos = cameraGetVcmPos();
  if (!ok)
    Serial.printf("[FOCUS] SAF timeout — VCM=%d\n", pos);
  else
    Serial.printf("[FOCUS] SAF done — VCM=%d\n", pos);
  return pos;   // 타임아웃이어도 현재 위치 반환 (사용자가 판단)
}
