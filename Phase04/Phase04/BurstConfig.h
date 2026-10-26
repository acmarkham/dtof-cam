#pragma once
#include <stdint.h>
#include <stddef.h>
namespace BurstConfig {
// Quality-first default: measured QXGA SD commits exceed the 2 fps budget.
constexpr unsigned kFps = 1;
static_assert(kFps >= 1 && kFps <= 10, "Paired capture rate must be 1..10 fps");
constexpr uint32_t kPeriodUs = 1000000 / kFps;
constexpr uint32_t kDepthPeriodMs = 1000 / kFps;
constexpr uint32_t kToleranceUs = kPeriodUs / 2;
constexpr uint32_t kBurstUs = 30000000;
constexpr uint32_t kSleepSeconds = 10;
constexpr size_t kMaxJpegBytes = 1024 * 1024;
constexpr unsigned kRgbSlots = 4;
constexpr unsigned kSdClockMHz = 40;
}
