#pragma once
#include "Sync.h"
struct BurstStats {
  uint32_t pairs = 0, syncRejected = 0, depthGaps = 0, queueDrops = 0;
  uint32_t rgbErrors = 0, depthErrors = 0;
  uint64_t startUs = 0, stopUs = 0, maxBoundUs = 0, maxWriteUs = 0;
  bool success = false, sensorStandby = false, cameraOff = false;
};
bool beginBurstStorage(uint64_t start, uint32_t wakeReason, uint32_t bootCount);
bool savePair(uint32_t sequence, const uint8_t* jpeg, const RgbFrame& rgb, const DepthFrame& depth,
              const PairTiming& timing, uint64_t& writeUs);
bool finishBurstStorage(const BurstStats& stats);
bool closeBurstStorage();
void storageStatus();
void downloadBurstFile(uint32_t burstNumber, const char* kind, uint32_t sequence);
uint32_t burstNumber();
