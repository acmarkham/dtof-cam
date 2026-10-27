#pragma once
#include <stdint.h>

namespace MotionConfig {
constexpr int kLed = 3;
constexpr uint32_t kDepthPeriodMs = 500;
constexpr uint16_t kDepthIterations = 2000;
constexpr unsigned kWarmupFrames = 4;
constexpr uint64_t kStaleAfterUs = 2000000;
constexpr uint16_t kScenePercent = 5; // ceil(64*5/100) = 4 zones.
constexpr uint16_t kRelativePercent = 20;
constexpr uint16_t kMinAbsoluteCm = 30;
}
