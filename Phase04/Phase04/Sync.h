#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Depth.h"
struct RgbFrame {
  uint64_t dmaUs = 0, doneUs = 0, periodBoundUs = 0;
  uint64_t earliestUs = 0, latestUs = 0;
  size_t length = 0;
};
struct PairTiming {
  uint64_t depthEarliestUs = 0, depthLatestUs = 0;
  int64_t midpointDeltaUs = 0;
  uint64_t worstSeparationUs = 0;
};
inline uint64_t absoluteDifference(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }
inline bool evaluateSync(const RgbFrame& rgb, const DepthFrame& depth, uint32_t tolerance, PairTiming& timing) {
  if (rgb.earliestUs > rgb.latestUs || depth.opticalEarliestUs > depth.opticalLatestUs) return false;
  timing.depthEarliestUs = depth.opticalEarliestUs;
  timing.depthLatestUs = depth.opticalLatestUs;
  timing.midpointDeltaUs = int64_t((rgb.earliestUs + rgb.latestUs) / 2) - int64_t((timing.depthEarliestUs + timing.depthLatestUs) / 2);
  timing.worstSeparationUs = absoluteDifference(rgb.earliestUs, timing.depthLatestUs);
  const uint64_t other = absoluteDifference(rgb.latestUs, timing.depthEarliestUs);
  if (other > timing.worstSeparationUs) timing.worstSeparationUs = other;
  return timing.worstSeparationUs <= tolerance;
}
