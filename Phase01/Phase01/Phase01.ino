#include "BoardConfig.h"
#include "Camera.h"
#include "Storage.h"
#include "esp_timer.h"
#include "esp_arduino_version.h"

namespace {
bool ready = false;
bool recording = false;
uint32_t nextSequence = 1;
uint32_t saved = 0;
uint32_t failures = 0;
uint32_t missed = 0;
uint32_t lastSaved = 0;
uint64_t nextDeadline = 0;
uint64_t ledOffDeadline = 0;
char command[48];
size_t commandLength = 0;
bool commandOverflow = false;

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

void clearLed() {
  digitalWrite(Board::kLed, LOW);
  ledOffDeadline = 0;
}

void haltRecording(const char* reason) {
  ready = false;
  recording = false;
  clearLed();
  Serial.printf("HALTED reason=%s restart_required=1\n", reason);
}

void status() {
  Serial.printf("STATUS ready=%u recording=%u next=%06lu last=%06lu saved=%lu failures=%lu missed=%lu led=%u ir=%u\n",
                ready, recording, static_cast<unsigned long>(nextSequence),
                static_cast<unsigned long>(lastSaved), static_cast<unsigned long>(saved),
                static_cast<unsigned long>(failures), static_cast<unsigned long>(missed),
                digitalRead(Board::kLed), digitalRead(Board::kIr));
  printStorageStatus();
}

void executeCommand() {
  // A delayed diagnostic acknowledgement must not become a user command error.
  if (!strncmp(command, "ACK ", 4)) return;
  if (!strcmp(command, "STATUS")) {
    status();
  } else if (!strcmp(command, "STOP")) {
    recording = false;
    clearLed();
    Serial.println("STOPPED");
    status();
  } else if (!strcmp(command, "START")) {
    if (ready) {
      recording = true;
      nextDeadline = nowUs() + Board::kIntervalUs;
      Serial.println("STARTED");
    } else Serial.println("ERROR START restart_required");
  } else if (!strcmp(command, "REBOOT")) {
    clearLed();
    Serial.println("REBOOTING");
    Serial.flush();
    ESP.restart();
  } else if (!strncmp(command, "GET ", 4)) {
    bool valid = strlen(command + 4) == 6;
    uint32_t sequence = 0;
    for (size_t i = 4; valid && i < 10; ++i) {
      valid = command[i] >= '0' && command[i] <= '9';
      if (valid) sequence = sequence * 10 + command[i] - '0';
    }
    if (recording) Serial.println("ERROR GET stop_recording_first");
    else if (!valid || sequence == 0) Serial.println("ERROR GET expected_six_digit_sequence");
    else downloadImage(sequence);
  } else Serial.println("ERROR command; use STATUS STOP START REBOOT GET NNNNNN");
}

void serviceCommands() {
  while (Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\r') continue;
    if (value == '\n') {
      command[commandLength] = 0;
      if (commandOverflow) Serial.println("ERROR command_too_long");
      else if (commandLength) executeCommand();
      commandLength = 0;
      commandOverflow = false;
    } else if (commandLength < sizeof(command) - 1) command[commandLength++] = value;
    else commandOverflow = true;
  }
}

void acquire() {
  if (nextSequence > Board::kLastSequence) {
    haltRecording("sequence_exhausted");
    return;
  }
  const uint32_t sequence = nextSequence++;
  const uint64_t started = nowUs();
  digitalWrite(Board::kLed, HIGH);
  ledOffDeadline = started + Board::kLedPulseUs;
  camera_fb_t* frame = esp_camera_fb_get();
  const uint64_t captured = nowUs();
  if (!frame) {
    ++failures;
    clearLed();
    Serial.printf("FRAME seq=%06lu t_us=%llu ok=0 error=null_frame\n",
                  static_cast<unsigned long>(sequence), started);
    return;
  }
  const size_t length = frame->len;
  const unsigned width = frame->width;
  const unsigned height = frame->height;
  const uint64_t frameUs = uint64_t(frame->timestamp.tv_sec) * 1000000ULL + frame->timestamp.tv_usec;
  const uint32_t crc = imageCrc32(frame->buf, length);
  const SaveResult result = frame->format == PIXFORMAT_JPEG && length >= 4 &&
                           width == resolution[Board::kFrameSize].width &&
                           height == resolution[Board::kFrameSize].height
                                ? saveImage(sequence, frame->buf, length)
                                : SaveResult{false, "invalid_jpeg_frame"};
  esp_camera_fb_return(frame);
  const uint64_t finished = nowUs();
  if (result.ok) {
    ++saved;
    lastSaved = sequence;
  } else {
    ++failures;
    haltRecording(result.error);
  }
  if (finished >= ledOffDeadline) clearLed();
  Serial.printf("FRAME seq=%06lu t_us=%llu frame_us=%llu width=%u height=%u bytes=%u capture_us=%llu write_us=%llu total_us=%llu crc32=%08lx ok=%u error=%s\n",
                static_cast<unsigned long>(sequence), started, frameUs, width, height, unsigned(length),
                captured - started, finished - captured, finished - started,
                static_cast<unsigned long>(crc), result.ok, result.error);
}
}

void setup() {
  digitalWrite(Board::kLed, LOW);
  pinMode(Board::kLed, OUTPUT);
  digitalWrite(Board::kIr, LOW);
  pinMode(Board::kIr, OUTPUT);
  Serial.begin(115200);
  // Bounded startup grace period; unattended capture does not require a USB host.
  delay(2000);
  Serial.printf("BOOT Phase01 core=%s interval_us=%llu led_pulse_us=%llu reset_reason=%d\n",
                ESP_ARDUINO_VERSION_STR, Board::kIntervalUs, Board::kLedPulseUs, int(esp_reset_reason()));
  if (!initStorage(nextSequence)) { haltRecording("storage_init"); return; }
  if (nextSequence > Board::kLastSequence) { haltRecording("sequence_exhausted"); return; }
  if (!initCamera() || !settleCamera()) { haltRecording("camera_init_or_settle"); return; }
  ready = true;
  recording = true;
  nextDeadline = nowUs() + Board::kIntervalUs;
  Serial.println("READY Phase01; commands: STATUS STOP START REBOOT GET NNNNNN");
  status();
}

void loop() {
  if (ledOffDeadline && nowUs() >= ledOffDeadline) clearLed();
  serviceCommands();
  if (recording && nowUs() >= nextDeadline) {
    acquire();
    nextDeadline += Board::kIntervalUs;
    const uint64_t current = nowUs();
    if (current >= nextDeadline) {
      const uint32_t skipped = (current - nextDeadline) / Board::kIntervalUs + 1;
      missed += skipped;
      nextDeadline += uint64_t(skipped) * Board::kIntervalUs;
      Serial.printf("OVERRUN skipped=%lu missed=%lu\n", static_cast<unsigned long>(skipped), static_cast<unsigned long>(missed));
    }
  }
  delay(1);
}
