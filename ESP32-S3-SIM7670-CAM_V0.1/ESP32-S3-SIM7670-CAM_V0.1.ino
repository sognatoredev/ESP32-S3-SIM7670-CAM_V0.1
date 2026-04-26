#include "esp_camera.h"
#include <WiFi.h>

// ===========================
// Select camera model in board_config.h
// ===========================

#include "board_config.h"

// ===========================
// Enter your WiFi credentials
// ===========================
// const char *ssid = "TWOSOME F2";
// const char *password = "twosome123";
const char *ssid = "AndroidHotspot4216";
const char *password = "sognatore";

void startCameraServer();
void setupLedFlash();

framesize_t current_cam_framesize;
int current_cam_quality;
sensor_t *camera_sensor2 = NULL;

void SetCameraFramesize(int size)
{
  if (size < 0 || size > 21)
  {
    Serial.println("Not supported framesize!");
    return;
  }

  current_cam_framesize = (framesize_t)size;
  camera_sensor2->set_framesize(camera_sensor2, current_cam_framesize);
  Serial.println("Change the framesize!");
}

void SetCameraQuality(int quality)
{
  if (quality < 10 || quality > 63)
  {
    Serial.println("Not supported quality!");
    return;
  }

  current_cam_quality = quality;
  camera_sensor2->set_quality(camera_sensor2, current_cam_quality);
}

void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  // config.ledc_channel = LEDC_CHANNEL_0;
  // config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (config.pixel_format == PIXFORMAT_JPEG)
  {
    if (psramFound())
    {
      // PSRAM이 있을 때: 최대 해상도(QSXGA 5MP)로 버퍼 선할당하여 런타임 전환 시 FB-OVF 방지
      config.frame_size = FRAMESIZE_QSXGA;
      config.jpeg_quality = 10;
      config.fb_count = 1;  // 5MP는 버퍼 1개로 제한 (메모리 절약)
      config.grab_mode = CAMERA_GRAB_LATEST;
    }
    else
    {
      // PSRAM 없을 때: DRAM 한계로 SVGA 제한
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  }
  else
  {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID)
  {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
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

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif
  camera_sensor2 = esp_camera_sensor_get();

  // HD(11, 1280x720)로 초기화: UXGA 모드를 유지하여 모든 해상도 전환이 가능하다.
  SetCameraFramesize(11);

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

void loop()
{
  // Do nothing. Everything is done in another task by the web server
  delay(10000);
}
