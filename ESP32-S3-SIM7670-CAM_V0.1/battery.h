#pragma once

// MAX17048G+T10  1-cell Li-Ion/LiPo Fuel Gauge
// I2C address : 0x36  (fixed)
// SDA / SCL   : GPIO15 / GPIO16  (카메라 SCCB 버스 공유)
//
// camera_mgr.cpp 에서 esp_camera_init() 전에 ESP-IDF legacy I2C driver 를 I2C_NUM_0 에 설치.
// sccb_i2c_port=0 으로 카메라가 해당 드라이버를 재사용하고,
// battery.cpp 도 i2c_master_write_read_device(I2C_NUM_0, ...) 로 같은 드라이버를 공유.

extern float g_batteryVoltage;   // V  (예: 3.85)
extern int   g_batteryPercent;   // %  (0–100)

bool batteryInit();   // IC 탐색 + 첫 읽기. 미발견 시 false 반환
bool batteryRead();   // 전압·잔량 갱신. s_ready == false 이면 즉시 false
