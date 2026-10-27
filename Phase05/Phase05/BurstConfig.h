#pragma once
#include <stdint.h>
#include <stddef.h>
namespace BurstConfig {
constexpr unsigned kDepthFps = 10;
constexpr unsigned kRgbFps = 1;
constexpr uint32_t kDepthPeriodMs = 100;
// 2000k measured ~189 ms/frame. Preserve 10 fps with 1000k instead.
constexpr uint16_t kDepthIterations = 1000;
constexpr uint32_t kRgbPeriodUs = 1000000;
constexpr uint32_t kBurstUs = 30000000;
constexpr uint32_t kSleepSeconds = 10;
constexpr unsigned kDepthCapacity = 384;
constexpr size_t kMaxJpegBytes = 1024 * 1024;
constexpr unsigned kRgbSlots = 4;
constexpr unsigned kSdClockMHz = 40;
static_assert(kDepthFps <= 10 && kDepthPeriodMs >= 100, "Depth ceiling is 10 fps");
}
