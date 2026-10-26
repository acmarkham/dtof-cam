#include "Depth.h"
#include "Storage.h"
#include <esp_timer.h>

namespace {
bool ready = false, recording = false;
uint32_t nextSequence = 1, saved = 0, rejected = 0, gaps = 0;
uint32_t lastNumber = 0;
bool haveNumber = false;
uint64_t lastRx = 0, lastProgress = 0;
char command[64];
size_t commandUsed = 0;
bool commandOverflow = false;
void halt(const char* reason) {
  recording = false;
  if (ready && !stopDepth()) { ready = false; Serial.println("ERROR sensor_stop_failed"); }
  digitalWrite(3, LOW);
  Serial.printf("STOP reason=%s saved=%lu rejected=%lu gaps=%lu\n", reason,
                (unsigned long)saved, (unsigned long)rejected, (unsigned long)gaps);
}
void status() {
  Serial.printf("STATUS ready=%u recording=%u saved=%lu next=%lu rejected=%lu gaps=%lu io_errors=%lu uptime_us=%llu\n",
    ready, recording, (unsigned long)saved, (unsigned long)nextSequence,
    (unsigned long)rejected, (unsigned long)gaps, (unsigned long)depthIoErrors, uint64_t(esp_timer_get_time()));
  Serial.printf("HISTOGRAM packets=%lu incomplete=%lu cache_replaced=%lu cache_sets=64\n",
    (unsigned long)histogramPackets, (unsigned long)histogramIncomplete, (unsigned long)histogramReplaced);
  printStorageStatus();
}
void runCommand() {
  command[commandUsed] = 0;
  if (!strcmp(command, "STATUS")) status();
  else if (!strcmp(command, "INFO")) {
    char metadata[2048]; depthMetadata(metadata, sizeof(metadata)); Serial.println(metadata); status();
  } else if (!strcmp(command, "STOP")) halt("console");
  else if (!strcmp(command, "START")) {
    if (ready && !recording) {
      recording = startDepth();
      haveNumber = false; lastRx = 0;
      lastProgress = esp_timer_get_time();
    }
    status();
  } else if (!strcmp(command, "REBOOT")) {
    halt("reboot"); Serial.flush(); delay(100); ESP.restart();
  } else if (!strncmp(command, "GET ", 4)) {
    char* end = nullptr;
    unsigned long sequence = strtoul(command + 4, &end, 10);
    if (recording) Serial.println("ERROR GET stop_recording_first");
    else if (!strncmp(command + 4, "RAW ", 4)) {
      unsigned long seq = 0; unsigned part = 0; char tail = 0;
      if (sscanf(command + 8, "%lu %u %c", &seq, &part, &tail) != 2 || !seq || seq >= nextSequence || part > 2)
        Serial.println("ERROR GET RAW bad_arguments");
      else downloadNative(seq, part);
    }
    else if (!strcmp(command + 4, "SESSION")) downloadDepth(0);
    else if (!strcmp(command + 4, "CONFIG")) downloadDepth(UINT32_MAX);
    else if (end == command + 4 || *end || sequence == 0 || sequence >= nextSequence) Serial.println("ERROR GET bad_sequence");
    else downloadDepth(sequence);
  } else if (strncmp(command, "ACK ", 4)) Serial.println("ERROR commands=STATUS,INFO,STOP,START,GET_sequence,REBOOT");
}
}
void setup() {
  pinMode(47, OUTPUT); digitalWrite(47, LOW);
  pinMode(3, OUTPUT); digitalWrite(3, LOW);
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(1000);
  Serial.begin(115200); delay(2500);
  Serial.println("PHASE03 depth and full histogram logger");
  const bool protocolOk = HistogramProtocol::selfTest();
  Serial.printf("SELFTEST histogram_protocol=%u\n", protocolOk);
  ready = protocolOk && beginDepth() && beginStorage();
  if (ready) recording = startDepth();
  if (!recording) ready = false;
  lastProgress = esp_timer_get_time();
  Serial.printf("READY ok=%u\n", ready);
}
void loop() {
  static DepthFrame frame;
  if (recording) {
    const uint64_t readStart = esp_timer_get_time();
    int r = pollDepth(frame);
    const uint64_t readUs = esp_timer_get_time() - readStart;
    if (r == 1) {
      lastProgress = esp_timer_get_time();
      if (haveNumber) {
        const uint32_t delta = frame.number - lastNumber;
        if (delta < 3 || delta > 0x7fffffff || delta % 3) { ++rejected; halt("sensor_sequence"); ready = false; return; }
        gaps += delta / 3 - 1;
      }
      lastNumber = frame.number; haveNumber = true;
      unsigned valid = 0;
      for (unsigned i = 0; i < 64; ++i) valid += validZone(frame, i);
      const uint32_t setCrc = cacheDepthSet(nextSequence);
      digitalWrite(3, HIGH);
      size_t bytes = 0;
      const uint64_t writeStart = esp_timer_get_time();
      const bool ok = saveDepth(nextSequence, frame, bytes);
      const uint64_t writeUs = esp_timer_get_time() - writeStart;
      delay(20); digitalWrite(3, LOW);
      if (!ok) { halt("sd_write_failed_preserving_tmp"); ready = false; return; }
      ++saved;
      Serial.printf("FRAME seq=%lu sensor=%lu host_rx_us=%llu interval_us=%llu valid=%u status=%u bytes=%u read_us=%llu write_us=%llu ratio=%u gaps=%lu hist0=%lu hist1=%lu hist0_crc32=%08lx hist1_crc32=%08lx depth_crc32=%08lx set_crc32=%08lx hist0_read_us=%lu hist1_read_us=%lu group_start_us=%llu\n",
        (unsigned long)nextSequence, (unsigned long)frame.number, frame.hostUs, lastRx ? frame.hostUs - lastRx : 0ULL,
        valid, frame.status, unsigned(bytes), readUs, writeUs, frame.ratio, (unsigned long)gaps,
        (unsigned long)frame.histogramNumbers[0], (unsigned long)frame.histogramNumbers[1],
        (unsigned long)frame.histogramCrc[0], (unsigned long)frame.histogramCrc[1],
        (unsigned long)frame.depthCrc, (unsigned long)setCrc,
        (unsigned long)frame.histogramReadUs[0], (unsigned long)frame.histogramReadUs[1], frame.groupStartUs);
      lastRx = frame.hostUs; ++nextSequence;
    }
    if (r < 0) { ++rejected; halt("depth_protocol_or_io"); ready = false; }
    if (recording && uint64_t(esp_timer_get_time()) - lastProgress > 20000000) {
      halt("sensor_timeout_20s"); ready = false;
    }
  }
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!commandOverflow) runCommand();
      else Serial.println("ERROR command_too_long");
      commandUsed = 0; commandOverflow = false;
    } else if (commandUsed < sizeof(command) - 1) command[commandUsed++] = c;
    else commandOverflow = true;
  }
  delay(2);
}
