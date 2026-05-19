#include "camera_mgr.h"
#include "board_config.h"
#include "led.h"
#include <Wire.h>
#include <Arduino.h>

framesize_t   current_cam_framesize;
int           current_cam_quality;
sensor_t     *camera_sensor2 = NULL;
volatile bool capturePending = false;

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
  config.pin_sccb_sda   = SIOD_GPIO_NUM;
  config.pin_sccb_scl   = SIOC_GPIO_NUM;
  config.sccb_i2c_port  = 0;   // reuse Wire's I2C_NUM_0 — avoids driver conflict
  config.pin_pwdn       = PWDN_GPIO_NUM;
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

  // Initialize I2C bus BEFORE camera so sccb-ng reuses it (sccb_i2c_port=0)
  // and batteryInit() can also use Wire without any driver conflict.
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
