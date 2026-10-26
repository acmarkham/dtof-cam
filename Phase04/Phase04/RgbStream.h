#pragma once
#include "Sync.h"
#include <atomic>
bool beginRgbStream();
bool copyPairedRgb(const DepthFrame& depth, uint64_t lastDmaUs, uint64_t burstStart,
                   uint64_t burstEnd, uint8_t* destination, RgbFrame& frame, PairTiming& timing);
bool stopRgbStream();
void rgbMetadata(char* output, size_t size);
extern std::atomic<uint32_t> rgbErrors, rgbFrames;
