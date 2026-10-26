#include "Depth.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

namespace {
using namespace HistogramProtocol;
uint8_t* pending = nullptr;
uint8_t* cache = nullptr;
uint8_t* resultScratch = nullptr;
uint32_t cachedSequence[kCacheSets] = {};
GroupOrder order;
bool pendingComplete = false;
uint64_t groupStart = 0;
uint32_t durations[3] = {};
}
uint32_t histogramReplaced = 0, histogramIncomplete = 0, histogramPackets = 0;

uint32_t histogramCrc32(const uint8_t* p, size_t size) {
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < size; ++i) {
    crc ^= p[i];
    for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
bool initDepthGroups() {
  pending = static_cast<uint8_t*>(heap_caps_malloc(kSetBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  cache = static_cast<uint8_t*>(heap_caps_malloc(kSetBytes * kCacheSets, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  resultScratch = static_cast<uint8_t*>(heap_caps_malloc(kHistogramBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  Serial.printf("HISTOGRAM cache_sets=%u allocated_bytes=%u psram_free=%u\n", kCacheSets,
                unsigned(kSetBytes * (kCacheSets + 1) + kHistogramBytes), unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return pending && cache && resultScratch;
}
void resetDepthGroups() {
  if (order.expected) ++histogramIncomplete;
  order = GroupOrder{}; pendingComplete = false;
}
int pollDepth(DepthFrame& frame) {
  const unsigned part = order.expected;
  size_t length = 0;
  uint64_t hostUs = 0;
  uint32_t readUs = 0;
  // Space for the maximum packet even if the sensor sends an unexpected type.
  // Result is read into a separate full-size scratch area, then copied to pending.
  uint8_t* destination = part == 2 ? resultScratch : pending + part * kHistogramBytes;
  const int result = readSensorPacket(destination, length, hostUs, readUs);
  if (result <= 0) {
    if (result < 0) { ++histogramIncomplete; order = GroupOrder{}; pendingComplete = false; }
    return result;
  }
  if (!order.accept(destination, length)) {
    ++histogramIncomplete; order = GroupOrder{}; pendingComplete = false; return -1;
  }
  if (part == 0) { groupStart = hostUs; pendingComplete = false; }
  durations[part] = readUs;
  if (part < 2) { ++histogramPackets; return 0; }
  memcpy(pending + 2 * kHistogramBytes, destination, length);
  decodeSensorDepth(destination, hostUs, frame);
  frame.groupStartUs = groupStart;
  frame.depthReadUs = durations[2];
  frame.depthCrc = histogramCrc32(destination, length);
  for (unsigned i = 0; i < 2; ++i) {
    frame.histogramNumbers[i] = u32(pending + i * kHistogramBytes + 9);
    frame.histogramCrc[i] = histogramCrc32(pending + i * kHistogramBytes, kHistogramBytes);
    frame.histogramReadUs[i] = durations[i];
  }
  pendingComplete = true; order = GroupOrder{};
  return 1;
}
uint32_t cacheDepthSet(uint32_t sequence) {
  if (!pendingComplete || !sequence) return 0;
  const unsigned slot = (sequence - 1) % kCacheSets;
  if (cachedSequence[slot]) ++histogramReplaced;
  memcpy(cache + slot * kSetBytes, pending, kSetBytes);
  cachedSequence[slot] = sequence; pendingComplete = false;
  return histogramCrc32(cache + slot * kSetBytes, kSetBytes);
}
const uint8_t* findDepthSet(uint32_t sequence, unsigned packet, size_t& length) {
  if (!sequence || packet > 2 || !cache) return nullptr;
  const unsigned slot = (sequence - 1) % kCacheSets;
  if (cachedSequence[slot] != sequence) return nullptr;
  length = packet < 2 ? kHistogramBytes : kDepthBytes;
  return cache + slot * kSetBytes + packet * kHistogramBytes;
}
