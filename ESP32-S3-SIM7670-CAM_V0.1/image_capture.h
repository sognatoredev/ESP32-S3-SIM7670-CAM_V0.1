#pragma once

extern int g_captureTarget;       // number of captures before TX (default 1)
extern int g_lastCaptureWidth;    // resolution of most recent successful capture
extern int g_lastCaptureHeight;

void captureAndSave();
