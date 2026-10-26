#include "RgbStream.h"
#include "BurstConfig.h"
#include "BoardConfig.h"
#include "Camera.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>

namespace {
struct Slot { RgbFrame frame; uint8_t* bytes = nullptr; } slots[BurstConfig::kRgbSlots];
SemaphoreHandle_t mutex = nullptr;
TaskHandle_t task = nullptr;
std::atomic<bool> running{false}, taskAlive{false};
uint64_t periodBound = 0;
uint32_t exposureLines = 0, vtsLines = 0;
unsigned nextSlot = 0;
bool cameraReady = false;
uint64_t timestamp(camera_fb_t* fb) { return uint64_t(fb->timestamp.tv_sec) * 1000000 + fb->timestamp.tv_usec; }
int reg(sensor_t* s, uint16_t address) { return s->get_reg(s, address, 255); }
void captureTask(void*) {
  uint64_t previous = 0;
  while (running) {
    camera_fb_t* fb = esp_camera_fb_get();
    const uint64_t done = esp_timer_get_time();
    if (!fb) { ++rgbErrors; continue; }
    const uint64_t dma = timestamp(fb);
    const bool valid = fb->format == PIXFORMAT_JPEG && fb->width == 2048 && fb->height == 1536 &&
      fb->len <= BurstConfig::kMaxJpegBytes && fb->len >= 4 && dma > previous && done >= dma;
    if (valid) {
      uint64_t bound = periodBound;
      if (previous) {
        const uint64_t observed = (dma - previous) * 11 / 10 + 2000;
        if (observed > bound) bound = observed;
      }
      previous = dma;
      xSemaphoreTake(mutex, portMAX_DELAY);
      Slot& slot = slots[nextSlot];
      slot.frame = {dma, done, bound, dma > bound ? dma - bound : 0, done, fb->len};
      memcpy(slot.bytes, fb->buf, fb->len);
      nextSlot = (nextSlot + 1) % BurstConfig::kRgbSlots;
      ++rgbFrames;
      xSemaphoreGive(mutex);
    } else ++rgbErrors;
    esp_camera_fb_return(fb);
  }
  taskAlive = false;
  vTaskDelete(nullptr);
}
}
std::atomic<uint32_t> rgbErrors{0}, rgbFrames{0};

bool beginRgbStream() {
  if (!initCamera()) { Serial.println("ERROR RGB initCamera"); return false; }
  cameraReady = true;
  if (!settleCamera()) { Serial.println("ERROR RGB settleCamera"); return false; }
  sensor_t* s = esp_camera_sensor_get();
  if (!s || s->id.PID != OV3660_PID) { Serial.println("ERROR RGB unsupported_sensor"); return false; }
  // Freeze the exposure chosen by AEC and disable night-mode frame extension.
  // This keeps exposure <= one sensor frame; gain and white balance may adapt.
  if (s->set_exposure_ctrl(s, 0) || s->set_aec2(s, 0)) { Serial.println("ERROR RGB exposure_freeze"); return false; }
  const int a = reg(s, 0x3500), b = reg(s, 0x3501), c = reg(s, 0x3502);
  const int vh = reg(s, 0x380e), vl = reg(s, 0x380f);
  if (a < 0 || b < 0 || c < 0 || vh < 0 || vl < 0) { Serial.println("ERROR RGB exposure_read"); return false; }
  exposureLines = ((a & 15) << 12) | (b << 4) | (c >> 4);
  vtsLines = (vh << 8) | vl;
  if (vtsLines < 8) { Serial.println("ERROR RGB bad_vts"); return false; }
  if (exposureLines >= vtsLines) exposureLines = vtsLines - 4;
  if (s->set_aec_value(s, exposureLines)) { Serial.println("ERROR RGB exposure_write"); return false; }
  const int manual = reg(s, 0x3503), night = reg(s, 0x3a00);
  Serial.printf("RGB_EXPOSURE lines=%lu vts=%lu manual_reg=%x night_reg=%x\n", (unsigned long)exposureLines, (unsigned long)vtsLines, manual, night);
  if ((manual & 1) != 1 || (night & 4)) { Serial.println("ERROR RGB exposure_verify"); return false; }
  uint64_t previous = 0, maximum = 0;
  for (unsigned i = 0; i < 14; ++i) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println("ERROR RGB period_probe_no_frame"); return false; }
    const uint64_t value = timestamp(fb);
    if (i > 3 && value > previous && value - previous > maximum) maximum = value - previous;
    previous = value;
    esp_camera_fb_return(fb);
  }
  Serial.printf("RGB_PERIOD observed_max_us=%llu psram_free=%u\n", maximum, unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (!maximum || maximum > 1000000) { Serial.println("ERROR RGB bad_period"); return false; }
  periodBound = maximum * 11 / 10 + 2000;
  mutex = xSemaphoreCreateMutex();
  if (!mutex) return false;
  for (auto& slot : slots) {
    slot.bytes = static_cast<uint8_t*>(heap_caps_malloc(BurstConfig::kMaxJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slot.bytes) { Serial.println("ERROR RGB ring_memory"); return false; }
  }
  running = true;
  taskAlive = true;
  if (xTaskCreatePinnedToCore(captureTask, "rgb_capture", 4096, nullptr, 2, &task, 1) != pdPASS) { taskAlive = false; running = false; return false; }
  char metadata[512]; rgbMetadata(metadata, sizeof(metadata)); Serial.println(metadata);
  return true;
}
bool copyPairedRgb(const DepthFrame& depth, uint64_t lastDmaUs, uint64_t burstStart,
                   uint64_t burstEnd, uint8_t* destination, RgbFrame& frame, PairTiming& timing) {
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  Slot* chosen = nullptr;
  PairTiming best;
  for (auto& slot : slots) {
    const auto& candidate = slot.frame;
    if (!candidate.length || candidate.dmaUs <= lastDmaUs ||
        (lastDmaUs && candidate.dmaUs - lastDmaUs < 100000) ||
        candidate.earliestUs < burstStart || candidate.latestUs > burstEnd) continue;
    PairTiming evaluated;
    if (!evaluateSync(candidate, depth, BurstConfig::kToleranceUs, evaluated)) continue;
    if (!chosen || evaluated.worstSeparationUs < best.worstSeparationUs) { chosen = &slot; best = evaluated; }
  }
  if (chosen) {
    frame = chosen->frame; timing = best;
    memcpy(destination, chosen->bytes, frame.length);
  }
  xSemaphoreGive(mutex);
  return chosen != nullptr;
}
bool stopRgbStream() {
  running = false;
  const uint32_t start = millis();
  while (taskAlive && millis() - start < 3000) delay(5);
  if (taskAlive) return false;
  if (cameraReady && esp_camera_deinit() != ESP_OK) return false;
  cameraReady = false;
  return true;
}
void rgbMetadata(char* output, size_t size) {
  snprintf(output, size, "{\"sensor\":\"OV3660\",\"width\":2048,\"height\":1536,\"jpeg_quality\":%d,\"exposure_manual\":true,\"exposure_lines\":%lu,\"vts_lines\":%lu,\"period_bound_us\":%llu,\"timestamp\":\"esp_camera first DMA\",\"optical_envelope\":\"[dma-period_bound, fb_get_return]\"}",
    Board::kJpegQuality, (unsigned long)exposureLines, (unsigned long)vtsLines, periodBound);
}
