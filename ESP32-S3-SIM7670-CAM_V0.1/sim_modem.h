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

// ── HTTP ──────────────────────────────────────────────────────────────────────
// Read <bodyLen> bytes from the modem HTTP buffer.
// Sends AT+HTTPREAD=0,<bodyLen>, waits for OK, then collects data until
// the +HTTPREAD: 0 end-marker.  Returns the body string (trimmed).
String simHttpReadBody(int bodyLen);

String simHttpGet(const String &url);
bool   simHttpPostEmpty(const String &url);

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

// ── Init ──────────────────────────────────────────────────────────────────────
bool simInit();
