// Camera setup adapted from DFRobot DFR1154 Timed camera; see ../UPSTREAM.md.
#include "BoardConfig.h"
#include "Camera.h"

bool initCamera() {
  if (!psramFound()) {
    Serial.println("ERROR camera PSRAM unavailable; enable OPI PSRAM");
    return false;
  }
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Board::kCameraData[0];
  config.pin_d1 = Board::kCameraData[1];
  config.pin_d2 = Board::kCameraData[2];
  config.pin_d3 = Board::kCameraData[3];
  config.pin_d4 = Board::kCameraData[4];
  config.pin_d5 = Board::kCameraData[5];
  config.pin_d6 = Board::kCameraData[6];
  config.pin_d7 = Board::kCameraData[7];
  config.pin_xclk = Board::kCameraXclk;
  config.pin_pclk = Board::kCameraPclk;
  config.pin_vsync = Board::kCameraVsync;
  config.pin_href = Board::kCameraHref;
  config.pin_sccb_sda = Board::kCameraSda;
  config.pin_sccb_scl = Board::kCameraScl;
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = Board::kFrameSize;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = Board::kJpegQuality;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;
  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERROR camera init code=0x%x\n", unsigned(err));
    return false;
  }
  sensor_t* sensor = esp_camera_sensor_get();
  if (!sensor) return false;
  if (sensor->status.framesize != Board::kFrameSize) {
    Serial.println("ERROR camera did not accept the requested frame size");
    return false;
  }
  if (sensor->id.PID == OV3660_PID) {
    if (sensor->set_vflip(sensor, 1) != 0 ||
        sensor->set_brightness(sensor, 1) != 0 ||
        sensor->set_saturation(sensor, -2) != 0) return false;
  }
  const resolution_info_t& size = resolution[sensor->status.framesize];
  Serial.printf("CAMERA pid=0x%x psram=%u width=%u height=%u quality=%d\n",
                sensor->id.PID, unsigned(ESP.getPsramSize()), size.width, size.height, Board::kJpegQuality);
  return true;
}

bool settleCamera() {
  // Continuously discard startup frames while auto-exposure settles.
  const uint32_t started = millis();
  do {
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame) return false;
    const bool valid = frame->format == PIXFORMAT_JPEG &&
                       frame->width == resolution[Board::kFrameSize].width &&
                       frame->height == resolution[Board::kFrameSize].height;
    esp_camera_fb_return(frame);
    if (!valid) {
      Serial.println("ERROR camera frame format or dimensions differ from configuration");
      return false;
    }
    delay(10);
  } while (millis() - started < 2000);
  return true;
}
