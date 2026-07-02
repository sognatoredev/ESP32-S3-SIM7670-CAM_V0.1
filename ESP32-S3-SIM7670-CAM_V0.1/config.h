#pragma once
#include <Arduino.h>

// ===========================
// WiFi
// ===========================
#define WIFI_SSID        "KT_GiGA_9748"
#define WIFI_PASSWORD    "9cf0bkd529"

// ===========================
// NTP  (KST = UTC+9)
// ===========================
#define NTP_SERVER       "pool.ntp.org"
#define NTP_GMT_OFFSET   (9L * 3600L)
#define NTP_DST_OFFSET   0

// ===========================
// SIM7670G UART & Power
// ===========================
#define SIM_RX_PIN      17
#define SIM_TX_PIN      18
#define SIM_PWRKEY_PIN  21    // PWRKEY: LOW pulse to power on/off
#define SIM_BAUD_INIT   115200   // default power-on baud rate
#define SIM_BAUD_FAST   230400   // upgraded rate set via AT+IPR every boot

// PWRKEY pulse durations (ms)
#define SIM_PWRON_PULSE_MS   1200   // > 1 s to power on
#define SIM_PWROFF_PULSE_MS  2500   // > 2 s to power off
#define SIM_BOOT_WAIT_MS     8000   // wait after power-on before sending AT

// ===========================
// Server
// ===========================
#define SERVER_HOST         "dev.neverlosewater.com"
#define SERVER_PORT         49152
#define DEVICE_MODEL_PREFIX "SM2-V3A"
#define DEVICE_UNIT_CODE_DEFAULT  "6001"   // "set sn <code>" 로 변경 가능한 기본값
#define DEVICE_SW_VER       "0108"

// 런타임 시리얼 넘버 (기본값 = DEVICE_UNIT_CODE_DEFAULT, config.txt "sn=" 키로 복원)
// RTC_DATA_ATTR — Deep Sleep 복귀 후에도 유지. 정의는 sd_storage.cpp.
extern char g_deviceUnitCode[16];

inline String deviceSerialNo() { return String(DEVICE_MODEL_PREFIX) + "-" + g_deviceUnitCode; }
// Boundary must be long enough that it cannot appear by chance in JPEG binary data.
// 8-char boundary ("1818FFFF") is too short — a 79 KB JPEG can contain \r\n--1818FFFF
// as an accidental byte sequence, causing the server multipart parser to truncate the
// image halfway.  A 28-char boundary makes accidental collision practically impossible.
// #define HTTP_BOUNDARY       "ImageBoundary1818FFFF00000000"
#define HTTP_BOUNDARY       "1818FFFF"

// ===========================
// SD card pins & folder structure
//   /RTU/Image/YYYY/MM/DD/YYYYMMDD_HHMMSS.jpg  <- saved right after capture
//   /Data/Image/YYYY/MM/DD/YYYYMMDD_HHMMSS.jpg <- moved after successful TX
// ===========================
#define SD_CLK_PIN   5
#define SD_CMD_PIN   4
#define SD_D0_PIN    6
#define SD_RTU_ROOT  "/RTU/Image"
#define SD_DATA_ROOT "/Data/Image"

// ===========================
// WS2812B LED
// ===========================
#define WS2812_PIN  38          // 상태 표시 LED (1개)
#define WS2812_NUM   1

#define WS2812_FLASH_PIN  1     // 카메라 플래시 LED (8개, GPIO1)
#define WS2812_FLASH_NUM  8

// ===========================
// Device sensor placeholders
// ===========================
// DEVICE_BATTERY_LEVEL 는 battery.h 의 g_batteryPercent 로 대체됨
#define DEVICE_TEMPERATURE   25

// ===========================
// Setup AP mode
// ===========================
#define SETUP_AP_SSID        "WP_Test_01"
#define SETUP_AP_TIMEOUT_MS  (5UL * 60UL * 1000UL)   // 5분
