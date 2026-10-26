#pragma once
#include <stdint.h>
#include <stddef.h>
#include "HistogramProtocol.h"
struct DepthFrame {
  uint32_t number, tick, t0, t1;
  uint64_t hostUs;
  uint16_t ratio;
  uint8_t status;
  uint16_t raw[64], corrected[64];
  uint8_t snr[64];
  uint32_t histogramNumbers[2], histogramCrc[2], depthCrc;
  uint32_t histogramReadUs[2], depthReadUs;
  uint64_t groupStartUs;
};
bool beginDepth();
bool startDepth();
bool stopDepth();
// 0: no result yet, 1: complete frame, -1: I/O/protocol error.
int pollDepth(DepthFrame& frame);
void depthMetadata(char* output, size_t size);
bool validZone(const DepthFrame& frame, unsigned index);
extern uint32_t depthIoErrors;
uint32_t cacheDepthSet(uint32_t sequence);
const uint8_t* findDepthSet(uint32_t sequence, unsigned packet, size_t& length);
uint32_t histogramCrc32(const uint8_t* data, size_t size);
extern uint32_t histogramReplaced, histogramIncomplete, histogramPackets;
int readSensorPacket(uint8_t* packet, size_t& length, uint64_t& hostUs, uint32_t& readUs);
void decodeSensorDepth(const uint8_t* packet, uint64_t hostUs, DepthFrame& frame);
bool initDepthGroups();
void resetDepthGroups();
