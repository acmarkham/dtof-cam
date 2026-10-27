#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Depth.h"
struct RgbFrame {
  uint64_t dmaUs = 0, doneUs = 0, periodBoundUs = 0;
  uint64_t earliestUs = 0, latestUs = 0;
  size_t length = 0;
};
// Both independent streams use esp_timer microseconds. Association is offline;
// Phase05 does not gate or discard depth frames to form one-to-one RGB pairs.
