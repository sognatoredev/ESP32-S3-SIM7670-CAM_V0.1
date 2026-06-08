#pragma once
#include "esp_camera.h"

extern framesize_t   current_cam_framesize;
extern int           current_cam_quality;
extern sensor_t     *camera_sensor2;
extern volatile bool capturePending;

bool cameraInit();
void SetCameraFramesize(int size);
void SetCameraQuality(int quality);
void SetCameraMirror(int enable);

// ── 포커스 저장 ───────────────────────────────────────────────────────────
// OV5640 VCM(Voice Coil Motor) 렌즈 위치를 저장하여
// 운영모드 캡처 시 동일 위치를 재현 (AF 탐색 없이 빠르게 고정 초점으로 촬영).
//
// g_savedFocusPos == FOCUS_POS_UNSET(-1) : 저장값 없음 → 캡처 시 SAF 자동 실행
// g_savedFocusPos   0 ~ 1023             : 저장된 VCM 위치
//                                          0 = 무한대(원거리), ~1023 = 접사(근거리)
#define FOCUS_POS_UNSET  (-1)

extern int g_savedFocusPos;   // RTC_DATA_ATTR — Deep Sleep 복귀 후에도 유지

// 현재 VCM 위치 읽기 (0 ~ 1023, 오류 시 -1)
int cameraGetVcmPos();

// 8051 MCU PAUSE → VCM 위치 직접 기록 → 렌즈 안정화 대기 (수동 포커스)
void cameraApplyFocusPos(int pos);

// SAF 실행 + 완료 대기 → 현재 VCM 위치 반환 (-1 = 실패 / 타임아웃)
int cameraDoSingleAF(uint32_t timeoutMs);