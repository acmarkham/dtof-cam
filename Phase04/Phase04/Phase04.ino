#include "BurstConfig.h"
#include "BurstStorage.h"
#include "RgbStream.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

namespace {
RTC_DATA_ATTR uint32_t boots = 0;
RTC_DATA_ATTR bool holdSleep = false;
BurstStats stats;
bool ready = false, capturing = false, finished = false, fatal = false;
bool depthRunning = false, depthInitialized = false, failedCleanup = false;
std::atomic<bool> depthTaskRunning{false}, depthTaskAlive{false};
std::atomic<uint32_t> depthTaskErrors{0}, depthQueueDrops{0};
QueueHandle_t depthQueue = nullptr;
TaskHandle_t depthTaskHandle = nullptr;
uint8_t* pairedJpeg = nullptr;
uint64_t deadline = 0, sleepAt = 0, lastDma = 0, lastDepthHost = 0;
uint32_t lastDepthNumber = 0, sleepSeconds = BurstConfig::kSleepSeconds;
bool haveDepthNumber = false;
char command[96]; size_t commandUsed = 0; bool overflow = false;

void depthTask(void*) {
  DepthFrame frame;
  while (depthTaskRunning) {
    const int result = pollDepth(frame);
    if (result < 0) { ++depthTaskErrors; depthTaskRunning = false; break; }
    if (result == 1 && xQueueSend(depthQueue, &frame, 0) != pdTRUE) ++depthQueueDrops;
    delay(1);
  }
  depthTaskAlive = false;
  vTaskDelete(nullptr);
}
void status() {
  Serial.printf("STATUS ready=%u capturing=%u finished=%u fatal=%u hold=%u burst=%lu pairs=%lu sync_rejected=%lu depth_gaps=%lu queue_drops=%lu rgb_errors=%lu depth_errors=%lu led=%u boot_count=%lu wake_reason=%u uptime_us=%llu\n",
    ready, capturing, finished, fatal, holdSleep, (unsigned long)burstNumber(), (unsigned long)stats.pairs,
    (unsigned long)stats.syncRejected, (unsigned long)stats.depthGaps, (unsigned long)depthQueueDrops,
    (unsigned long)rgbErrors, (unsigned long)depthTaskErrors, digitalRead(3), (unsigned long)boots,
    unsigned(esp_sleep_get_wakeup_cause()), uint64_t(esp_timer_get_time()));
  storageStatus();
}
bool stopAcquisition() {
  capturing = false;
  digitalWrite(3, LOW);
  depthTaskRunning = false;
  const uint32_t start = millis();
  while (depthTaskAlive && millis() - start < 2000) delay(5);
  if (depthTaskAlive) return false;
  stats.sensorStandby = !depthInitialized || depthStandby(); depthRunning = false;
  stats.cameraOff = stopRgbStream();
  return stats.sensorStandby && stats.cameraOff;
}
void completeBurst(bool normal) {
  stats.stopUs = esp_timer_get_time();
  if (!stopAcquisition()) fatal = true;
  stats.queueDrops = depthQueueDrops;
  stats.rgbErrors = rgbErrors;
  stats.depthErrors = depthTaskErrors;
  stats.success = normal && !fatal && stats.pairs > 0 && !stats.depthErrors && !stats.queueDrops;
  if (!finishBurstStorage(stats)) fatal = true;
  finished = true;
  sleepAt = esp_timer_get_time() + 2000000;
  Serial.printf("BURST_DONE burst=%lu success=%u pairs=%lu sync_rejected=%lu max_sync_bound_us=%llu tolerance_us=%lu duration_us=%llu led=0 standby=%u camera_off=%u\n",
    (unsigned long)burstNumber(), stats.success && !fatal, (unsigned long)stats.pairs,
    (unsigned long)stats.syncRejected, stats.maxBoundUs, (unsigned long)BurstConfig::kToleranceUs,
    stats.stopUs - stats.startUs, stats.sensorStandby, stats.cameraOff);
  status();
}
void enterSleep() {
  if (capturing || fatal || !finished || !stats.sensorStandby || !stats.cameraOff) return;
  if (!closeBurstStorage()) { fatal = true; Serial.println("ERROR storage_close_before_sleep"); return; }
  digitalWrite(3, LOW);
  esp_sleep_enable_timer_wakeup(uint64_t(sleepSeconds) * 1000000);
  Serial.printf("SLEEP seconds=%lu led=0 next_boot=%lu\n", (unsigned long)sleepSeconds, (unsigned long)(boots + 1));
  Serial.flush(); delay(100);
  gpio_hold_en(GPIO_NUM_3); gpio_deep_sleep_hold_en();
  esp_deep_sleep_start();
}
void execute() {
  command[commandUsed] = 0;
  if (!strcmp(command, "STATUS")) status();
  else if (!strcmp(command, "HOLD")) { holdSleep = true; Serial.println("HOLD enabled=1"); status(); }
  else if (!strcmp(command, "ALLOW_SLEEP")) { holdSleep = false; sleepAt = esp_timer_get_time()+1000000; Serial.println("HOLD enabled=0"); status(); }
  else if (!strcmp(command, "STOP")) { holdSleep = true; if (capturing) completeBurst(false); else status(); }
  else if (!strcmp(command, "REBOOT")) { if (capturing) completeBurst(false); Serial.flush(); ESP.restart(); }
  else if (!strncmp(command, "SLEEP ", 6)) {
    char* end = nullptr; const unsigned long value = strtoul(command+6, &end, 10);
    if (!capturing && finished && !fatal && end != command+6 && !*end && value >= 1 && value <= 86400) {
      sleepSeconds = value; holdSleep = false; enterSleep();
    } else Serial.println("ERROR SLEEP requires_finished_healthy_burst_and_seconds_1_86400");
  } else if (!strncmp(command, "GET ", 4)) {
    if (capturing || !finished) { Serial.println("ERROR GET wait_for_burst_end"); return; }
    unsigned long burst = 0, sequence = 0; char kind[16], tail = 0;
    const int count = sscanf(command+4, "%lu %15s %lu %c", &burst, kind, &sequence, &tail);
    if (count < 2 || count > 3) Serial.println("ERROR GET syntax=GET_burst_KIND_sequence");
    else downloadBurstFile(burst, kind, sequence);
  } else if (strncmp(command, "ACK ", 4)) Serial.println("ERROR commands=STATUS,HOLD,ALLOW_SLEEP,GET,STOP,REBOOT,SLEEP_seconds");
}
void console() {
  while (Serial.available()) {
    const char c = Serial.read(); if (c == '\r') continue;
    if (c == '\n') {
      if (!overflow) execute(); else Serial.println("ERROR command_too_long");
      commandUsed = 0; overflow = false;
    } else if (commandUsed < sizeof(command)-1) command[commandUsed++] = c;
    else overflow = true;
  }
}
}

void setup() {
  gpio_deep_sleep_hold_dis(); gpio_hold_dis(GPIO_NUM_3);
  pinMode(3, OUTPUT); digitalWrite(3, LOW);
  pinMode(47, OUTPUT); digitalWrite(47, LOW);
  ++boots;
  Serial.setTxBufferSize(4096); Serial.setTxTimeoutMs(1000); Serial.begin(115200);
  delay(1500); console();
  Serial.printf("PHASE04 boot=%lu wake_reason=%u burst_seconds=30 sleep_seconds=10 target_fps=%u jpeg_quality=6\n", (unsigned long)boots, unsigned(esp_sleep_get_wakeup_cause()), BurstConfig::kFps);
  if (!beginRgbStream()) { fatal = true; Serial.println("ERROR camera_initialization"); return; }
  if (!beginDepth()) { fatal = true; Serial.println("ERROR depth_initialization"); return; }
  depthInitialized = true;
  if (!startDepth()) { fatal = true; Serial.println("ERROR depth_start"); return; }
  depthRunning = true;
  DepthFrame warm;
  unsigned samples = 0; const uint64_t warmDeadline = esp_timer_get_time()+10000000;
  while (samples < 4 && uint64_t(esp_timer_get_time()) < warmDeadline) {
    console(); const int result = pollDepth(warm);
    if (result < 0) break;
    if (result == 1) ++samples;
    delay(1);
  }
  if (samples != 4) { fatal = true; Serial.println("ERROR depth_clock_warmup"); return; }
  pairedJpeg = static_cast<uint8_t*>(heap_caps_malloc(BurstConfig::kMaxJpegBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  depthQueue = xQueueCreate(16, sizeof(DepthFrame));
  if (!pairedJpeg || !depthQueue) { fatal = true; Serial.println("ERROR capture_buffers"); return; }
  // Keep draining the sensor while SD allocates the burst directory/metadata.
  depthTaskRunning = true; depthTaskAlive = true;
  if (xTaskCreatePinnedToCore(depthTask, "depth_capture", 4096, nullptr, 2, &depthTaskHandle, 0) != pdPASS) { fatal = true; depthTaskAlive = false; return; }
  // Allocate directory/metadata before starting the timed acquisition window.
  stats.startUs = esp_timer_get_time()+1000000;
  if (!beginBurstStorage(stats.startUs, esp_sleep_get_wakeup_cause(), boots)) { fatal = true; Serial.println("ERROR storage_initialization"); return; }
  while (uint64_t(esp_timer_get_time()) < stats.startUs) { console(); delay(1); }
  stats.startUs = esp_timer_get_time(); deadline = stats.startUs + BurstConfig::kBurstUs;
  ready = true; capturing = true;
  digitalWrite(3, HIGH);
  Serial.printf("BURST_START burst=%lu start_us=%llu deadline_us=%llu tolerance_us=%lu led=1\n", (unsigned long)burstNumber(), stats.startUs, deadline, (unsigned long)BurstConfig::kToleranceUs);
  status();
}
void loop() {
  console();
  if (fatal) {
    digitalWrite(3, LOW);
    if (!finished && !failedCleanup) { stopAcquisition(); failedCleanup = true; ready = false; }
    delay(20); return;
  }
  if (capturing && uint64_t(esp_timer_get_time()) >= deadline) { completeBurst(true); return; }
  if (capturing && (depthTaskErrors || depthQueueDrops || rgbErrors)) { fatal = true; completeBurst(false); return; }
  if (capturing) {
    static DepthFrame depth;
    if (xQueueReceive(depthQueue, &depth, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (haveDepthNumber) {
        const uint32_t delta = depth.number - lastDepthNumber;
        if (!delta || delta > 0x7fffffff) { fatal = true; completeBurst(false); return; }
        stats.depthGaps += delta - 1;
      }
      lastDepthNumber = depth.number; haveDepthNumber = true; lastDepthHost = depth.hostUs;
      if (depth.opticalEarliestUs < stats.startUs || depth.opticalLatestUs > deadline) return;
      RgbFrame rgb; PairTiming timing; bool paired = false;
      const uint64_t waitEnd = min(deadline, uint64_t(esp_timer_get_time()) + BurstConfig::kToleranceUs);
      do {
        paired = copyPairedRgb(depth, lastDma, stats.startUs, deadline, pairedJpeg, rgb, timing);
        if (paired) break;
        console(); delay(1);
      } while (capturing && uint64_t(esp_timer_get_time()) < waitEnd);
      if (!capturing) return;
      if (!paired) { ++stats.syncRejected; return; }
      uint64_t writeUs = 0;
      if (!savePair(stats.pairs+1, pairedJpeg, rgb, depth, timing, writeUs)) { fatal = true; completeBurst(false); return; }
      ++stats.pairs; lastDma = rgb.dmaUs;
      if (timing.worstSeparationUs > stats.maxBoundUs) stats.maxBoundUs = timing.worstSeparationUs;
      if (writeUs > stats.maxWriteUs) stats.maxWriteUs = writeUs;
      Serial.printf("PAIR seq=%lu dtof=%lu rgb_dma_us=%llu depth_start_us=%llu midpoint_delta_us=%lld sync_bound_us=%llu tolerance_us=%lu jpeg_bytes=%u write_us=%llu\n",
        (unsigned long)stats.pairs, (unsigned long)depth.number, rgb.dmaUs, depth.opticalEarliestUs,
        timing.midpointDeltaUs, timing.worstSeparationUs, (unsigned long)BurstConfig::kToleranceUs, unsigned(rgb.length), writeUs);
    }
    if (lastDepthHost && uint64_t(esp_timer_get_time())-lastDepthHost > 5000000) { fatal = true; completeBurst(false); }
  }
  if (finished && !holdSleep && uint64_t(esp_timer_get_time()) >= sleepAt) enterSleep();
  delay(1);
}
