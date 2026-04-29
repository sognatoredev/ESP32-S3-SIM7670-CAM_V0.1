#pragma once
#include <Arduino.h>
#include <vector>

bool   sendFileViaSim(const String &filePath);
String rtuToDataPath(const String &rtuPath);
bool   sendWithRetry(const String &rtuPath);
void   collectRtuFiles(const char *dirPath, std::vector<String> &fileList);
void   retryPendingFiles();
