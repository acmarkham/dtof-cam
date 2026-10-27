#pragma once
#include <stdint.h>

struct MotionParameters {
  uint16_t scenePercent = 5;
  uint16_t relativePercent = 20;
  uint16_t minAbsoluteCm = 30;
};

struct MotionSample {
  uint16_t distanceQmm[64] = {};
  uint64_t validMask = 0;
};

enum class MotionMode { Baseline, Static, Moving, Insufficient };

struct MotionResult {
  MotionMode mode = MotionMode::Baseline;
  unsigned valid = 0, comparable = 0, changed = 0, required = 0;
  uint64_t comparableMask = 0, changedMask = 0;
  bool moving = false;
};

inline unsigned countZones(uint64_t mask) {
  unsigned count = 0;
  while (mask) { mask &= mask - 1; ++count; }
  return count;
}

class MotionDetector {
  MotionParameters parameters_;
  MotionSample previous_;
  bool havePrevious_ = false;
public:
  const MotionParameters& parameters() const { return parameters_; }
  unsigned requiredZones() const { return (64u * parameters_.scenePercent + 99u) / 100u; }
  void reset() { previous_ = MotionSample{}; havePrevious_ = false; }
  bool configure(const MotionParameters& parameters) {
    if (parameters.scenePercent < 1 || parameters.scenePercent > 100 ||
        parameters.relativePercent > 1000 || parameters.minAbsoluteCm < 1 ||
        parameters.minAbsoluteCm > 1638) return false;
    parameters_ = parameters;
    reset();
    return true;
  }
  MotionResult update(const MotionSample& sample) {
    MotionSample current = sample;
    for (unsigned i = 0; i < 64; ++i) {
      if (!current.distanceQmm[i]) current.validMask &= ~(uint64_t(1) << i);
    }
    MotionResult result;
    result.required = requiredZones();
    result.valid = countZones(current.validMask);
    if (havePrevious_) {
      result.comparableMask = current.validMask & previous_.validMask;
      result.comparable = countZones(result.comparableMask);
      for (unsigned i = 0; i < 64; ++i) {
        const uint64_t bit = uint64_t(1) << i;
        if (!(result.comparableMask & bit)) continue;
        const uint32_t before = previous_.distanceQmm[i], after = current.distanceQmm[i];
        const uint32_t delta = before > after ? before - after : after - before;
        // Strictly greater than BOTH thresholds. 1 cm = 40 quarter-mm units.
        // Compare products to preserve exact fractional percentage boundaries.
        if (delta > uint32_t(parameters_.minAbsoluteCm) * 40u &&
            uint64_t(delta) * 100u > uint64_t(before) * parameters_.relativePercent) {
          result.changedMask |= bit;
          ++result.changed;
        }
      }
      result.moving = result.changed >= result.required;
      result.mode = result.comparable < result.required ? MotionMode::Insufficient :
        result.moving ? MotionMode::Moving : MotionMode::Static;
    }
    // Always follow the latest scene: an object that stops at a NEW distance
    // becomes static on the next comparison; no return to the boot scene needed.
    previous_ = current;
    havePrevious_ = true;
    return result;
  }
};

inline const char* motionModeName(MotionMode mode) {
  switch (mode) {
    case MotionMode::Baseline: return "baseline";
    case MotionMode::Moving: return "moving";
    case MotionMode::Insufficient: return "insufficient";
    default: return "static";
  }
}
