#include "HistogramProtocol.h"
#include <string.h>

namespace HistogramProtocol {
bool selfTest() {
  // Exercise the actual on-device validator without I2C or measurement state.
  static uint8_t histogram[kHistogramBytes];
  uint8_t depth[kDepthBytes] = {};
  memset(histogram, 0, sizeof(histogram));
  histogram[5] = 0x20; histogram[7] = 0x18; histogram[8] = 0x63;
  histogram[kHistogramBytes - 4] = 1;
  histogram[kHistogramBytes - 2] = 0xf7; histogram[kHistogramBytes - 1] = 0xe0;
  depth[5] = 0x10; depth[6] = 1; depth[7] = 216; depth[9] = 2;
  depth[kDepthBytes - 4] = 1; depth[kDepthBytes - 2] = 0xf7; depth[kDepthBytes - 1] = 0xe0;
  GroupOrder good;
  if (!good.accept(histogram, sizeof(histogram))) return false;
  if (good.accept(depth, sizeof(depth))) return false; // Missing T1.
  if (good.accept(histogram, sizeof(histogram))) return false; // Duplicate T0.
  histogram[6] = 1; histogram[9] = 1;
  if (!good.accept(histogram, sizeof(histogram)) || !good.accept(depth, sizeof(depth)) || !good.complete()) return false;
  GroupOrder wrongOrder;
  if (wrongOrder.accept(histogram, sizeof(histogram))) return false; // T1 before T0.
  histogram[6] = 0; histogram[9] = 0;
  GroupOrder gap;
  if (!gap.accept(histogram, sizeof(histogram))) return false;
  histogram[6] = 1; histogram[9] = 2;
  if (gap.accept(histogram, sizeof(histogram))) return false;
  if (validPacket(histogram, sizeof(histogram) - 1)) return false;
  histogram[7] ^= 1;
  if (validPacket(histogram, sizeof(histogram))) return false;
  histogram[7] ^= 1; histogram[kHistogramBytes - 1] ^= 1;
  if (validPacket(histogram, sizeof(histogram))) return false;
  return true;
}
}
