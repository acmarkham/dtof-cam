#pragma once
#include <stdint.h>
#include <stddef.h>
struct DepthFrame {
  uint32_t number, tick, t0, t1;
  uint64_t hostUs;
  uint16_t ratio;
  uint8_t status;
  uint16_t raw[64], corrected[64];
  uint8_t snr[64];
  uint64_t rxBeginUs = 0, rxEndUs = 0;
  uint64_t opticalEarliestUs = 0, opticalLatestUs = 0;
  uint8_t native[225] = {};
};
bool beginDepth();
bool startDepth();
bool stopDepth();
// 0: no result yet, 1: complete frame, -1: I/O/protocol error.
int pollDepth(DepthFrame& frame);
void depthMetadata(char* output, size_t size);
bool validZone(const DepthFrame& frame, unsigned index);
extern uint32_t depthIoErrors;
bool depthStandby();
