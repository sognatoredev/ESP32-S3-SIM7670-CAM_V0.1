#ifndef SIM_TYPES_H
#define SIM_TYPES_H

#include <Arduino.h>

// SIM7670G signal / time info collected before each server session
typedef struct
{
  int    csq;        // raw CSQ (0-31, 99 = unknown)
  int    strength;   // mapped 0-5 bars
  int    rssi;       // dBm derived from CSQ
  int    sinr;       // dB from +CPSI field [13]
  String modemTime;  // "YYYY-MM-DD HH:MM:SS" from AT+CCLK
} SimInfo;

#endif // SIM_TYPES_H
