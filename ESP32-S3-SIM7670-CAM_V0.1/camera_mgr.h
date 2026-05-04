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