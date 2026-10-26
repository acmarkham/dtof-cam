#pragma once
#include <Arduino.h>
#include "esp_camera.h"

namespace Board {
constexpr int kLed = 3;
constexpr int kIr = 47;
// DFR1154 v1.1 onboard SPI SD interface (ESP32-S3 FSPI default pins).
constexpr int kSdCs = 10;
constexpr int kSdMosi = 11;
constexpr int kSdSck = 12;
constexpr int kSdMiso = 13;
constexpr int kCameraData[8] = {16, 18, 21, 17, 14, 7, 6, 4};
constexpr int kCameraXclk = 5;
constexpr int kCameraPclk = 15;
constexpr int kCameraVsync = 1;
constexpr int kCameraHref = 2;
constexpr int kCameraSda = 8;
constexpr int kCameraScl = 9;
// Reserved: Gravity GPIO43/44, onboard ALS, and PDM microphone.
constexpr int kGravity43 = 43;
constexpr int kGravity44 = 44;
constexpr uint64_t kIntervalUs = 1000000;
constexpr uint64_t kLedPulseUs = 100000;
constexpr uint32_t kLastSequence = 999999;
// OV3660 maximum 4:3 output. Use FRAMESIZE_FHD for 1920 x 1080 instead.
constexpr framesize_t kFrameSize = FRAMESIZE_QXGA;
constexpr int kJpegQuality = 6;
constexpr bool kAllowFat32 = false;
}
