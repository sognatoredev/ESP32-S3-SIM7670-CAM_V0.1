# ESP32-S3-SIM7670-CAM Firmware

수자원공사 유량계 이미지 캡처 장치 펌웨어.  
ESP32-S3 기반으로 10분 주기 자동 촬영 → SD 저장 → LTE 서버 전송을 수행합니다.

---

## 목차

1. [하드웨어 사양](#1-하드웨어-사양)
2. [GPIO 핀 배치](#2-gpio-핀-배치)
3. [소프트웨어 구조](#3-소프트웨어-구조)
4. [동작 흐름](#4-동작-흐름)
5. [설정 값 (config.h)](#5-설정-값-configh)
6. [SD 카드 폴더 구조](#6-sd-카드-폴더-구조)
7. [서버 통신 프로토콜](#7-서버-통신-프로토콜)
8. [LED 상태 표시](#8-led-상태-표시)
9. [시리얼 커맨드](#9-시리얼-커맨드)
10. [웹 인터페이스](#10-웹-인터페이스)
11. [빌드 환경](#11-빌드-환경)
12. [플래시 파티션](#12-플래시-파티션)
13. [TODO / 향후 과제](#13-todo--향후-과제)

---

## 1. 하드웨어 사양

| 항목 | 사양 |
|------|------|
| MCU | ESP32-S3 (PSRAM 내장) |
| 보드 | Waveshare ESP32-S3 + SIM7670G |
| 카메라 | OV5640 (최대 5MP) |
| 모뎀 | SIM7670G (LTE Cat-M1 / NB-IoT) |
| 저장장치 | microSD (1-bit SDMMC) |
| LED | WS2812B RGB LED × 1 |

---

## 2. GPIO 핀 배치

### 카메라 (OV5640)

| 신호 | GPIO |
|------|------|
| XCLK | 39 |
| SIOD (SDA) | 15 |
| SIOC (SCL) | 16 |
| VSYNC | 42 |
| HREF | 41 |
| PCLK | 46 |
| Y9 ~ Y2 | 14, 13, 12, 11, 10, 9, 8, 7 |
| PWDN | — (없음) |
| RESET | — (없음) |

### SD 카드 (1-bit SDMMC)

| 신호 | GPIO |
|------|------|
| CLK | 5 |
| CMD | 4 |
| D0 | 6 |

### SIM7670G 모뎀

| 신호 | GPIO |
|------|------|
| UART RX | 17 |
| UART TX | 18 |
| PWRKEY | 21 |

### 기타

| 장치 | GPIO |
|------|------|
| WS2812B LED | 38 |

---

## 3. 소프트웨어 구조

```
ESP32-S3-SIM7670-CAM_V0.1.ino   ← setup() / loop() / 시리얼 커맨드
│
├── config.h             ← 모든 설정 상수 (WiFi, 서버, 핀, 경로 등)
├── board_config.h       ← 보드 모델 선택 (#define CAMERA_MODEL_...)
├── camera_pins.h        ← 보드별 카메라 GPIO 매핑
├── sim_types.h          ← SimInfo 구조체 (CSQ, RSSI, SINR, 시간)
│
├── led.h / led.cpp                   ← WS2812B LED 제어
├── sd_storage.h / sd_storage.cpp     ← SD 마운트, 디렉토리, 전체 삭제
├── camera_mgr.h / camera_mgr.cpp     ← 카메라 초기화, 해상도/품질 제어
├── time_sync.h / time_sync.cpp       ← NTP 동기화, 시스템 시각 적용
├── sim_modem.h / sim_modem.cpp       ← SIM7670G 전체 (AT, HTTP, NTP, 전원)
├── image_tx.h / image_tx.cpp         ← 이미지 파일 전송, 재시도, RTU 관리
├── image_capture.h / image_capture.cpp ← 캡처 파이프라인 (captureAndSave)
│
├── app_httpd.h / app_httpd.cpp       ← WiFi HTTP 스트리밍 서버 (포트 80/81)
└── camera_index.h                    ← 임베디드 웹 UI (HTML/CSS/JS)
```

### 모듈 의존 관계

```
.ino
 ├── config.h          (모든 모듈이 참조)
 ├── led.cpp           (독립)
 ├── sd_storage.cpp    → SD_MMC
 ├── camera_mgr.cpp    → esp_camera, led
 ├── time_sync.cpp     → led
 ├── sim_modem.cpp     → time_sync, sim_types
 ├── image_tx.cpp      → sim_modem, sd_storage, led
 ├── image_capture.cpp → camera_mgr, sd_storage, sim_modem, image_tx, led, app_httpd
 └── app_httpd.cpp     → camera_mgr
```

---

## 4. 동작 흐름

### 부팅 시퀀스 (setup)

```
LED 파란색 점등 (부팅 중)
│
├─ [CAM]  카메라 초기화 (OV5640, PSRAM 버퍼 할당)
├─ [SD]   SD 카드 마운트
├─ [SIM]  PWRKEY 펄스 → 모뎀 전원 ON → 부팅 대기 (8초)
│          AT 통신 확인 → 921600 bps 업그레이드
│          AT+CNTP NTP 동기화 (time.google.com → pool.ntp.org → time.cloudflare.com)
│          POST /m2/device_status (장치 상태 전송)
│          GET  /m2/device_setting (m2_point_id, m2_device_id 수신)
├─ [WiFi] WiFi 접속
├─ [NTP]  WiFi NTP 동기화 (모뎀 동기화 완료 시 건너뜀)
├─ [HTTP] 스트리밍 서버 시작 (포트 80 / 81)
│
LED 초록색 점등 (대기)
```

### 10분 주기 동작 (loop)

```
매 500ms: 현재 시각 확인
│
└─ 10분 경계 도달 시 captureAndSave() 실행
    │
    ├─ 스트리밍 일시정지 (capturePending = true)
    ├─ 해상도 FHD(1920×1080)로 전환
    ├─ LED 흰색 플래시 → 이미지 캡처
    ├─ 이전 해상도 복원, 스트리밍 재개
    ├─ /RTU/Image/YYYY/MM/DD/YYYYMMDD_HHMMSS.jpg 저장
    ├─ LTE POST /m2/point_image (multipart/form-data)
    │    ├─ 성공: /Data/Image/... 로 이동
    │    └─ 실패: 최대 3회 재시도 → RTU 폴더 보관
    └─ RTU 폴더 잔여 파일 일괄 재전송 (오래된 순)
```

### 시간 동기화 우선순위

```
1순위: 모뎀 AT+CNTP (time.google.com / pool.ntp.org / time.cloudflare.com)
2순위: WiFi NTP (pool.ntp.org) — 모뎀 동기화 실패 시만 사용
```

---

## 5. 설정 값 (config.h)

| 항목 | 기본값 | 설명 |
|------|--------|------|
| `WIFI_SSID` | `KT_GiGA_9748` | WiFi SSID |
| `WIFI_PASSWORD` | `9cf0bkd529` | WiFi 비밀번호 |
| `NTP_SERVER` | `pool.ntp.org` | WiFi NTP 서버 |
| `NTP_GMT_OFFSET` | `32400` (9시간) | KST = UTC+9 |
| `SIM_RX_PIN` | `17` | 모뎀 UART RX |
| `SIM_TX_PIN` | `18` | 모뎀 UART TX |
| `SIM_PWRKEY_PIN` | `21` | 모뎀 PWRKEY |
| `SIM_BAUD_INIT` | `115200` | 모뎀 초기 통신속도 |
| `SIM_BAUD_WORK` | `921600` | 모뎀 운용 통신속도 |
| `SIM_PWRON_PULSE_MS` | `1200` | 전원 ON PWRKEY 펄스 (ms) |
| `SIM_PWROFF_PULSE_MS` | `2500` | 전원 OFF PWRKEY 펄스 (ms) |
| `SIM_BOOT_WAIT_MS` | `8000` | 부팅 후 AT 명령 대기 (ms) |
| `SERVER_HOST` | `dev.neverlosewater.com` | 서버 호스트 |
| `SERVER_PORT` | `49152` | 서버 포트 |
| `DEVICE_MODEL_PREFIX` | `SM2-V3A` | 장치 모델 코드 |
| `DEVICE_UNIT_CODE` | `6002` | 장치 시리얼 번호 |
| `DEVICE_SW_VER` | `0108` | 펌웨어 버전 |
| `SD_RTU_ROOT` | `/RTU/Image` | 캡처 임시 저장 경로 |
| `SD_DATA_ROOT` | `/Data/Image` | 전송 완료 이동 경로 |
| `WS2812_PIN` | `38` | LED GPIO |
| `DEVICE_BATTERY_LEVEL` | `100` | 배터리 잔량 (placeholder) |
| `DEVICE_TEMPERATURE` | `25` | 온도 (placeholder) |

---

## 6. SD 카드 폴더 구조

```
/
├── RTU/
│   └── Image/
│       └── YYYY/
│           └── MM/
│               └── DD/
│                   └── YYYYMMDD_HHMMSS.jpg   ← 캡처 직후 저장
│                                               (전송 대기 / 재시도 대상)
│
└── Data/
    └── Image/
        └── YYYY/
            └── MM/
                └── DD/
                    └── YYYYMMDD_HHMMSS.jpg   ← 서버 전송 성공 후 이동
```

> NTP 미동기 상태에서 캡처된 파일은 `/RTU/IMG_XXXXXX.jpg` 형식으로 저장됩니다.

---

## 7. 서버 통신 프로토콜

### 공통

- 프로토콜: HTTP/1.1 via AT+HTTPINIT / AT+HTTPPARA / AT+HTTPACTION
- 모뎀 HTTP 스택 사용 (SIM7670G 내장)

### POST /m2/device_status — 장치 상태 전송

```
POST /m2/device_status
  ?serial_no=SM2-V3A-6002
  &Battery_Level=100
  &Temperature=25
  &Modem_Time=2026-04-29%2013:00:00
  &Modem_Strength=4
  &RSSI=-65
  &SINR=6
  &Device_SW_Ver=0108
```

- 응답: `HTTP 201 Created`

### GET /m2/device_setting — 장치 설정 수신

```
GET /m2/device_setting?serial_no=SM2-V3A-6002
```

- 응답 JSON:
  ```json
  { "m2_point_id": 1, "m2_device_id": 2 }
  ```

### POST /m2/point_image — 이미지 업로드

```
POST /m2/point_image?serial_no=SM2-V3A-6002
Content-Type: multipart/form-data; boundary=1818FFFF

--1818FFFF
Content-Disposition: form-data; name="file_name"

20260429_130000.jpg
--1818FFFF
Content-Disposition: form-data; name="img_file"; filename="20260429_130000.jpg"
Content-Type: image/jpeg

[binary JPEG data]
--1818FFFF--
```

- 응답: `HTTP 2xx`
- 전송 실패 시 최대 3회 재시도 (3초 간격)

### NTP 시간 동기화

```
AT+CNTP="time.google.com",36     ← 설정 (36 = KST, UTC+9)
AT+CNTP                           ← 실행
+CNTP: 0                          ← 성공 (err_code 0 = no error)
AT+CCLK?                          ← 시각 읽기 → KST 변환 → settimeofday()
```

서버 시도 순서: `time.google.com` → `pool.ntp.org` → `time.cloudflare.com`  
서버당 최대 2회 시도.

---

## 8. LED 상태 표시

| 색상 / 패턴 | 의미 |
|-------------|------|
| 파란색 (점등) | 부팅 중 / 모뎀 초기화 중 |
| 노란색 (점멸) | WiFi 연결 중 |
| 초록색 × 2 점멸 | SD 카드 OK |
| 파란색 × 3 점멸 | 모뎀 초기화 OK |
| 주황색 × 5 점멸 | 모뎀 초기화 실패 |
| 빨간색 × 5 점멸 | SD 카드 오류 |
| 주황색 × 5 점멸 | NTP 동기화 실패 |
| 초록색 (점등) | 대기 (정상) |
| 흰색 (점등) | 이미지 캡처 중 (플래시) |
| 파란색 (점등) | LTE 전송 중 |
| 주황색 × 2 점멸 | 전송 재시도 중 |
| 빨간색 × 3 점멸 | 전송 3회 모두 실패 |

---

## 9. 시리얼 커맨드

시리얼 모니터 설정: **115200 bps**, 줄끝 `NL` 또는 `CR+NL`

| 명령어 | 동작 |
|--------|------|
| `sim on` | 모뎀 전원 ON (PWRKEY 펄스) 후 재초기화 |
| `sim off` | 모뎀 전원 OFF (PWRKEY 펄스) |
| `remove sd` | SD 카드 전체 데이터 삭제 |
| `sd info` | SD 카드 전체 / 사용 / 여유 용량 출력 |
| `help` | 커맨드 목록 표시 |

---

## 10. 웹 인터페이스

WiFi 연결 후 `http://<ESP32 IP>` 접속

| 포트 | 엔드포인트 | 기능 |
|------|-----------|------|
| 80 | `/` | 웹 UI (카메라 뷰어) |
| 80 | `/capture` | 단일 JPEG 스냅샷 |
| 80 | `/status` | 카메라 센서 상태 JSON |
| 80 | `/control?var=X&val=Y` | 카메라 파라미터 제어 |
| 81 | `/stream` | MJPEG 실시간 스트리밍 |

**지원 해상도 (웹 UI):**

| 코드 | 해상도 |
|------|--------|
| QQVGA (1) | 160 × 120 |
| QVGA (5) | 320 × 240 |
| VGA (8) | 640 × 480 |
| SVGA (9) | 800 × 600 |
| XGA (10) | 1024 × 768 |
| HD (11) | 1280 × 720 ← 스트리밍 기본값 |
| SXGA (12) | 1280 × 1024 |
| UXGA (13) | 1600 × 1200 |
| FHD | 1920 × 1080 ← 캡처 전용 |

---

## 11. 빌드 환경

| 항목 | 버전 |
|------|------|
| Arduino IDE | 2.x 권장 |
| arduino-esp32 | 3.x |
| ESP32 보드 | `ESP32S3 Dev Module` |
| PSRAM | `OPI PSRAM` 또는 `QSPI PSRAM` |
| Partition Scheme | 커스텀 (`partitions.csv` 사용) |
| Flash Size | 4MB 이상 |
| CPU Frequency | 240 MHz |

**필수 라이브러리:**

| 라이브러리 | 용도 |
|-----------|------|
| esp32-camera | OV5640 드라이버 |
| FastLED | WS2812B LED 제어 |
| ESP32 SD_MMC | SD 카드 (내장) |
| WiFi | WiFi 연결 (내장) |
| esp_http_server | 웹 서버 (내장) |

---

## 12. 플래시 파티션

`partitions.csv` 커스텀 파티션 사용:

| 이름 | 타입 | 크기 | 설명 |
|------|------|------|------|
| nvs | data/nvs | 20 KB | 비휘발성 저장소 |
| otadata | data/ota | 8 KB | OTA 메타데이터 |
| app0 | app/ota_0 | 3840 KB | 애플리케이션 |
| fr | data | 128 KB | 예약 영역 |
| coredump | data/coredump | 64 KB | 코어덤프 |

---

## 13. TODO / 향후 과제

- [ ] `DEVICE_BATTERY_LEVEL` — 실제 ADC 배터리 전압 측정으로 교체
- [ ] `DEVICE_TEMPERATURE` — 실제 온도 센서 읽기로 교체
- [ ] WiFi 자격증명 하드코딩 → NVS 또는 BLE 프로비저닝으로 개선
- [ ] OTA (Over-The-Air) 펌웨어 업데이트 적용
- [ ] 캡처 주기 서버 설정값(`device_setting`)으로 동적 변경
- [ ] 이미지 화질/해상도 서버 설정값으로 동적 변경
- [ ] 네트워크 오프라인 시 로컬 버퍼링 전략 고도화

---

## 버전 이력

| 버전 | 날짜 | 내용 |
|------|------|------|
| 0.1.08 | 2026-04-29 | 모듈화 리팩토링, SIM7670G HTTP 방식 전환, PWRKEY 전원 제어, 시간 동기화(CNTP) |
| 0.1.07 | 2026-04-28 | SIM7670G AT+CIPSEND raw TCP → AT+HTTPPARA HTTP 방식으로 전환 |
| 0.1.06 | — | WiFi 스트리밍 기본 구현 |
