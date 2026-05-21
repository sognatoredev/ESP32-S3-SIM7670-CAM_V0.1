#pragma once
#include <Arduino.h>
#include <HardwareSerial.h>
#include "sim_types.h"

extern HardwareSerial SimSerial;
extern bool simReady;
extern int  g_m2PointId;
extern int  g_m2DeviceId;

// ── Low-level AT ──────────────────────────────────────────────────────────────
String simSendAT(const String &cmd, uint32_t timeout = 3000);
String simWaitFor(const String &token, uint32_t timeout);
void   simFlush(uint32_t delayMs = 200);

// ── Signal / time ─────────────────────────────────────────────────────────────
int     csqToRssi(int csq);
int     csqToStrength(int csq);
String  simGetModemTime();
int     simGetSinr();
SimInfo simGetInfo();

// ── HTTP (used during boot: device_status POST, device_setting GET) ───────────
String simHttpReadBody(int bodyLen);
String simHttpGet(const String &url);
bool   simHttpPostEmpty(const String &url);

// ── TCP / CIPSEND (used for image upload) ────────────────────────────────────
bool   simNetOpen();
void   simNetClose();
bool   simTcpOpen(int link, const char *host, int port);
void   simTcpClose(int link);
bool   simTcpSendChunk(int link, const uint8_t *data, size_t len);
bool   simTcpWaitAck(int link, size_t maxUnacked, uint32_t timeoutMs);  // AT+CIPACK flow control
String simTcpPollResponse(int link);          // fast non-blocking: returns buffered data or ""
String simTcpReadResponse(int link, uint32_t timeoutMs);

// ── Server communication ──────────────────────────────────────────────────────
bool simPostDeviceStatus(const SimInfo &info);
bool simGetDeviceSetting();
void simConnect();

// ── Time sync ─────────────────────────────────────────────────────────────────
// NTP sync via modem AT+CNTP, converts to KST, applies to system clock.
bool simSyncTime();

// ── Utility ───────────────────────────────────────────────────────────────────
int parseJsonInt(const String &json, const char *key);

// ── Power control ─────────────────────────────────────────────────────────────
void simPowerInit();   // GPIO setup (call once in setup)
void simPowerOn();     // PWRKEY pulse to turn modem ON, then waits for boot
void simPowerOff();    // PWRKEY pulse to turn modem OFF

// ── Network registration wait ─────────────────────────────────────────────────
// LTE 망 등록(AT+CEREG? stat=1/5)을 확인할 때까지 2초 간격으로 폴링.
// timeoutMs(기본 90초) 내에 등록되지 않으면 false 반환.
// simInit() 내부에서 simConnect() 이전에 호출됨.
bool simWaitNetReg(uint32_t timeoutMs = 90000);

// ── Init ──────────────────────────────────────────────────────────────────────
bool simInit();
