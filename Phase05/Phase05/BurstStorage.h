#pragma once
#include "Sync.h"
struct BurstStats {
  uint32_t rgbCount = 0, depthCount = 0, depthGaps = 0, depthOverflows = 0, rgbMissed = 0;
  uint32_t rgbErrors = 0, depthErrors = 0;
  uint64_t startUs = 0, stopUs = 0, maxWriteUs = 0;
  bool success = false, sensorStandby = false, cameraOff = false;
};
bool beginBurstStorage(uint32_t wakeReason, uint32_t bootCount);
bool saveRgb(uint32_t sequence, const uint8_t* jpeg, const RgbFrame& rgb, uint64_t dueUs, uint64_t& writeUs);
bool finishBurstStorage(const BurstStats& stats, const DepthFrame* frames);
bool closeBurstStorage();
void storageStatus();
void downloadBurstFile(uint32_t burstNumber, const char* kind, uint32_t sequence);
uint32_t burstNumber();
