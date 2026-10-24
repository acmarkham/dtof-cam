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
  Serial.begin(115200); delay(2500);
  Serial.println("PHASE02 depth CSV logger");
  ready = beginDepth() && beginStorage();
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
        if (delta == 0 || delta > 0x7fffffff) { ++rejected; halt("sensor_sequence"); ready = false; return; }
        gaps += delta - 1;
      }
      lastNumber = frame.number; haveNumber = true;
      unsigned valid = 0;
      for (unsigned i = 0; i < 64; ++i) valid += validZone(frame, i);
      digitalWrite(3, HIGH);
      size_t bytes = 0;
      const uint64_t writeStart = esp_timer_get_time();
      const bool ok = saveDepth(nextSequence, frame, bytes);
      const uint64_t writeUs = esp_timer_get_time() - writeStart;
      delay(20); digitalWrite(3, LOW);
      if (!ok) { halt("sd_write_failed_preserving_tmp"); ready = false; return; }
      ++saved;
      Serial.printf("FRAME seq=%lu sensor=%lu host_rx_us=%llu interval_us=%llu valid=%u status=%u bytes=%u read_us=%llu write_us=%llu ratio=%u gaps=%lu\n",
        (unsigned long)nextSequence, (unsigned long)frame.number, frame.hostUs, lastRx ? frame.hostUs - lastRx : 0ULL,
        valid, frame.status, unsigned(bytes), readUs, writeUs, frame.ratio, (unsigned long)gaps);
      lastRx = frame.hostUs; ++nextSequence;
    }
    if (r < 0) { ++rejected; halt("depth_protocol_or_io"); ready = false; }
    if (recording && uint64_t(esp_timer_get_time()) - lastProgress > 5000000) {
      halt("sensor_timeout_5s"); ready = false;
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
