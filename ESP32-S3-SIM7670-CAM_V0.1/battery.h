#pragma once
#include <stdint.h>

// ── MAX17048G+T10  1-cell Li-Ion/LiPo Fuel Gauge ─────────────────────────────
// I²C address : 0x36  (fixed)
// SDA / SCL   : GPIO15 / GPIO16  (same physical bus as camera SCCB)
//
// Implementation note:
//   Wire / TwoWire is NOT used.
//   sccb-ng calls i2c_driver_install() (legacy ESP-IDF I2C driver) on I2C_NUM_0
//   during cameraInit().  MAX17048 (0x36) and OV5640 camera (0x3C) share the
//   same GPIO15/16 bus — batteryInit() simply reuses that installed driver and
//   targets address 0x36.  No second driver install, no new-API handle needed.
//
// Call order:
//   cameraInit()   ← sccb-ng installs legacy I2C driver on I2C_NUM_0
//   batteryInit()  ← shares the same driver, targets 0x36
// ─────────────────────────────────────────────────────────────────────────────

extern int   g_batteryPercent;   // 0–100 %  (updated by batteryRead)
extern float g_batteryVoltage;   // V        (updated by batteryRead)

bool batteryInit();    // must be called AFTER cameraInit()
bool batteryRead();    // read VCELL + SOC, update globals
bool batteryIsReady(); // true if batteryInit() succeeded
