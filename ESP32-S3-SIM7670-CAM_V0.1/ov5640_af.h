#pragma once
#include <stdint.h>
#include <stdbool.h>

// OV5640 AF 펌웨어 로드 및 8051 MCU 초기화.
// cameraInit() 직후 1회 호출.
// 반환: 0=성공, 음수=실패
int ov5640AfInit();

// 단발 AF(SAF) 트리거. 캡처 직전에 호출.
// 반환: 0=명령 수락, 음수=센서 없음
int ov5640AfTriggerSingle();

// 연속 AF(CAF) 트리거. 스트리밍 모드에서 호출.
int ov5640AfTriggerContinuous();

// AF 완료 대기.
// 반환: true=포커스 완료, false=타임아웃 또는 초점 불가
bool ov5640AfWaitFocus(uint32_t timeoutMs);

// 현재 AF 펌웨어 상태 레지스터(0x3029) 값 반환.
uint8_t ov5640AfGetStatus();
