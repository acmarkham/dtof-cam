#pragma once
#include <stdint.h>
#include <stddef.h>

namespace HistogramProtocol {
constexpr size_t kHeaderBytes = 21;
constexpr size_t kHistogramBytes = 25377;
constexpr size_t kDepthBytes = 225;
constexpr size_t kSetBytes = 2 * kHistogramBytes + kDepthBytes;
constexpr unsigned kCacheSets = 64;
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t* p) { return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16; }
inline size_t packetSize(const uint8_t* p) {
  if ((p[0] & 0x30) || (p[5] & 15) > 1) return 0;
  const uint8_t kind = p[5] & 0xf0;
  if (kind == 0x20 && p[6] <= 1 && u16(p + 7) == 25368) return kHistogramBytes;
  if (kind == 0x10 && p[6] == 1 && u16(p + 7) == 216) return kDepthBytes;
  return 0;
}
inline bool validPacket(const uint8_t* p, size_t length) {
  return length >= kHeaderBytes && packetSize(p) == length &&
         u16(p + length - 2) == 0xe0f7;
}
struct GroupOrder {
  uint8_t expected = 0, focalPlane = 0;
  uint32_t previous = 0;
  bool accept(const uint8_t* p, size_t length) {
    if (!validPacket(p, length)) return false;
    const uint8_t kind = p[5] & 0xf0;
    if (expected < 2 ? (kind != 0x20 || p[6] != expected) : kind != 0x10) return false;
    if (expected && (u32(p + 9) - previous != 1 || (p[5] & 15) != focalPlane)) return false;
    // A histogram must be usable; result status remains visible per zone in CSV.
    const uint8_t status = p[length - 4];
    if (kind == 0x20 && (!(status & 1) || (status & 0xc0))) return false;
    focalPlane = p[5] & 15; previous = u32(p + 9);
    ++expected; return true;
  }
  bool complete() const { return expected == 3; }
};
bool selfTest();
}
