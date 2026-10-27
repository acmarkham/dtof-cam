#include <Arduino.h>
#include <esp_timer.h>
#include <esp_rom_crc.h>
#include <driver/gpio.h>
#include "Depth.h"
#include "MotionConfig.h"
#include "MotionDetector.h"
#include "MotionSelfTest.h"

namespace {
MotionDetector detector;
MotionResult lastDecision;
bool sensorReady = false, running = false, fatal = false, moving = false, stale = false;
bool haveSensorNumber = false;
uint32_t sensorNumber = 0, frames = 0, events = 0, gaps = 0, counterResets = 0;
uint32_t faults = 0, insufficient = 0, epoch = 0, selfTests = 0;
unsigned warmupLeft = MotionConfig::kWarmupFrames;
uint64_t lastRxUs = 0;
char command[96];
size_t commandUsed = 0;
bool commandOverflow = false;

bool setLed(bool on) {
  digitalWrite(MotionConfig::kLed, on ? HIGH : LOW);
  return bool(digitalRead(MotionConfig::kLed)) == on;
}
void configuration() {
  const auto& p = detector.parameters();
  Serial.printf("CONFIG scene_percent=%u relative_percent=%u min_cm=%u required_zones=%u period_ms=%lu kilo_iterations=%u\n",
    p.scenePercent, p.relativePercent, p.minAbsoluteCm, detector.requiredZones(),
    (unsigned long)MotionConfig::kDepthPeriodMs, MotionConfig::kDepthIterations);
}
void status() {
  const auto& p = detector.parameters();
  Serial.printf("STATUS ready=%u fatal=%u frames=%lu events=%lu moving=%u led=%u comparable=%u changed=%u required=%u insufficient=%lu gaps=%lu counter_resets=%lu faults=%lu io_errors=%lu stale=%u warmup_left=%u epoch=%lu selftests=%lu scene_percent=%u relative_percent=%u min_cm=%u uptime_us=%llu\n",
    running, fatal, (unsigned long)frames, (unsigned long)events, moving, digitalRead(MotionConfig::kLed),
    lastDecision.comparable, lastDecision.changed, detector.requiredZones(), (unsigned long)insufficient,
    (unsigned long)gaps, (unsigned long)counterResets, (unsigned long)faults, (unsigned long)depthIoErrors,
    stale, warmupLeft, (unsigned long)epoch, (unsigned long)selfTests,
    p.scenePercent, p.relativePercent, p.minAbsoluteCm, uint64_t(esp_timer_get_time()));
}
void resetTracking(const char* reason, unsigned warmup = 0) {
  detector.reset();
  moving = false;
  setLed(false);
  lastDecision = MotionResult{};
  lastDecision.required = detector.requiredZones();
  warmupLeft = warmup;
  ++epoch;
  Serial.printf("BASELINE_RESET reason=%s epoch=%lu warmup=%u led=0\n", reason, (unsigned long)epoch, warmup);
}
void halt(const char* reason) {
  fatal = true; running = false; ++faults;
  resetTracking(reason);
  if (sensorReady) depthStandby();
  Serial.printf("ERROR reason=%s led=0\n", reason);
  status();
}
MotionSample sampleFrom(const DepthFrame& frame) {
  MotionSample sample;
  for (unsigned i = 0; i < 64; ++i) {
    sample.distanceQmm[i] = frame.corrected[i];
    if (validZone(frame, i) && frame.corrected[i]) sample.validMask |= uint64_t(1) << i;
  }
  return sample;
}
void emitFrame(const DepthFrame& frame, const MotionSample& sample, const char* mode) {
  char hex[451]; constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < sizeof(frame.native); ++i) {
    hex[2*i] = digits[frame.native[i] >> 4]; hex[2*i+1] = digits[frame.native[i] & 15];
  }
  hex[450] = 0;
  const auto& p = detector.parameters();
  Serial.printf("FRAME source=sensor seq=%lu epoch=%lu sensor_frame=%lu host_us=%llu ratio=%u status=%u mode=%s scene_percent=%u relative_percent=%u min_cm=%u valid_mask=%016llx comparable_mask=%016llx changed_mask=%016llx valid=%u comparable=%u changed=%u required=%u moving=%u led=%u native_crc32=%08lx native_hex=%s\n",
    (unsigned long)frames, (unsigned long)epoch, (unsigned long)frame.number, frame.hostUs, frame.ratio,
    frame.status, mode, p.scenePercent, p.relativePercent, p.minAbsoluteCm, sample.validMask,
    lastDecision.comparableMask, lastDecision.changedMask, countZones(sample.validMask),
    lastDecision.comparable, lastDecision.changed, detector.requiredZones(), moving,
    digitalRead(MotionConfig::kLed), (unsigned long)esp_rom_crc32_le(0,frame.native,sizeof(frame.native)), hex);
}
void acceptFrame(const DepthFrame& frame) {
  ++frames;
  if (haveSensorNumber) {
    const uint32_t delta = frame.number - sensorNumber;
    if (delta != 1) {
      if (delta > 1 && delta <= 0x7fffffff) gaps += delta - 1;
      else ++counterResets;
      resetTracking("frame_gap");
    }
  }
  sensorNumber = frame.number; haveSensorNumber = true;
  lastRxUs = frame.hostUs; stale = false;
  const MotionSample sample = sampleFrom(frame);
  const char* mode = "warming";
  if (warmupLeft) {
    --warmupLeft;
    detector.reset();
    moving = false;
    lastDecision = MotionResult{};
  } else {
    const bool wasMoving = moving;
    lastDecision = detector.update(sample);
    moving = lastDecision.moving;
    mode = motionModeName(lastDecision.mode);
    if (lastDecision.mode == MotionMode::Insufficient) ++insufficient;
    if (moving && !wasMoving) ++events;
    if (moving != wasMoving) {
      Serial.printf("MOTION source=sensor active=%u sensor_frame=%lu host_us=%llu changed=%u required=%u\n",
        moving, (unsigned long)frame.number, frame.hostUs, lastDecision.changed, detector.requiredZones());
    }
  }
  if (!setLed(moving)) { halt("led_gpio_mismatch"); return; }
  emitFrame(frame, sample, mode);
}
void selfTest() {
  if (!running || fatal) { Serial.println("ERROR SELFTEST requires_running_sensor"); return; }
  if (!stopDepth()) { halt("selftest_sensor_stop"); return; }
  running = false;
  resetTracking("selftest");
  const auto report = testMotionDetector();
  MotionDetector testDetector;
  auto scene = uniformMotionSample(4000);
  unsigned gpioFailures = 0;
  auto checkLed = [&](const char* name, const MotionResult& result, bool expected) {
    const bool outputOk = setLed(result.moving);
    const bool ok = outputOk && result.moving == expected;
    if (!ok) ++gpioFailures;
    Serial.printf("LED_TEST source=synthetic case=%s expected=%u moving=%u led=%u changed=%u required=%u ok=%u\n",
      name, expected, result.moving, digitalRead(MotionConfig::kLed), result.changed, result.required, ok);
    Serial.flush(); delay(200);
  };
  checkLed("baseline",testDetector.update(scene),false);
  for (unsigned i = 0; i < 4; ++i) scene.distanceQmm[i] = 6000;
  checkLed("motion",testDetector.update(scene),true);
  checkLed("held_still",testDetector.update(scene),false);
  setLed(false);
  ++selfTests;
  haveSensorNumber = false;
  resetTracking("selftest_complete",MotionConfig::kWarmupFrames);
  if (report.failures || gpioFailures) { halt("selftest_failed"); return; }
  if (!startDepth()) { halt("selftest_sensor_resume"); return; }
  running = true; stale = false; lastRxUs = esp_timer_get_time();
  Serial.printf("SELFTEST_DONE checks=%u failures=%u gpio_checks=3 gpio_failures=%u resumed=1\n",
    report.checks, report.failures, gpioFailures);
  configuration(); status();
}
void execute() {
  command[commandUsed] = 0;
  if (!strcmp(command,"STATUS")) status();
  else if (!strcmp(command,"RESET")) { resetTracking("command"); status(); }
  else if (!strcmp(command,"SELFTEST")) selfTest();
  else if (!strcmp(command,"REBOOT")) {
    setLed(false);
    if (sensorReady) depthStandby();
    Serial.println("REBOOTING"); Serial.flush(); delay(50); ESP.restart();
  } else if (!strncmp(command,"CONFIG ",7)) {
    unsigned long scene=0, relative=0, cm=0; char extra=0;
    const int count=sscanf(command+7,"%lu %lu %lu %c",&scene,&relative,&cm,&extra);
    if (count!=3 || scene<1 || scene>100 || relative>1000 || cm<1 || cm>1638) {
      Serial.println("ERROR CONFIG expects_scene_1_100_relative_0_1000_cm_1_1638"); return;
    }
    MotionParameters parameters;
    parameters.scenePercent=scene; parameters.relativePercent=relative; parameters.minAbsoluteCm=cm;
    if (!detector.configure(parameters)) { Serial.println("ERROR invalid_config"); return; }
    resetTracking("configuration"); configuration(); status();
  } else Serial.println("ERROR commands=STATUS,CONFIG_X_Y_Z,RESET,SELFTEST,REBOOT");
}
void console() {
  while (Serial.available()) {
    const char c=Serial.read(); if(c=='\r') continue;
    if(c=='\n') {
      if(!commandOverflow) execute(); else Serial.println("ERROR command_too_long");
      commandUsed=0; commandOverflow=false;
    } else if(commandUsed<sizeof(command)-1) command[commandUsed++]=c;
    else commandOverflow=true;
  }
}
}

void setup() {
  gpio_deep_sleep_hold_dis(); gpio_hold_dis(GPIO_NUM_3);
  pinMode(MotionConfig::kLed,OUTPUT); setLed(false);
  pinMode(47,OUTPUT); digitalWrite(47,LOW); // Board IR off.
  pinMode(5,OUTPUT); digitalWrite(5,LOW);   // No camera XCLK.
  pinMode(10,OUTPUT); digitalWrite(10,HIGH); // SD deselected; no storage access.
  Serial.setTxBufferSize(4096); Serial.setTxTimeoutMs(100); Serial.begin(115200);
  MotionParameters parameters;
  parameters.scenePercent=MotionConfig::kScenePercent;
  parameters.relativePercent=MotionConfig::kRelativePercent;
  parameters.minAbsoluteCm=MotionConfig::kMinAbsoluteCm;
  if(!detector.configure(parameters)) { fatal=true; return; }
  delay(1500);
  Serial.println("PHASE06 motion_detection=consecutive_frames camera=off sd=off ir=off");
  configuration();
  const auto tests=testMotionDetector();
  Serial.printf("CLASSIFIER_TEST checks=%u failures=%u\n",tests.checks,tests.failures);
  if(tests.failures) { halt("classifier_selftest"); return; }
  if(!beginDepth()) { halt("depth_initialization"); return; }
  sensorReady=true;
  if(!startDepth()) { halt("depth_start"); return; }
  resetTracking("startup",MotionConfig::kWarmupFrames);
  running=true; lastRxUs=esp_timer_get_time();
  Serial.println("READY phase=6");
  status();
}
void loop() {
  console();
  if(running && !fatal) {
    DepthFrame frame;
    const int result=pollDepth(frame);
    if(result<0) halt("depth_read");
    else if(result==1) acceptFrame(frame);
    if(running && !stale && uint64_t(esp_timer_get_time())-lastRxUs>MotionConfig::kStaleAfterUs) {
      stale=true;
      resetTracking("stale",MotionConfig::kWarmupFrames);
      Serial.println("WARN depth_stale led=0");
    }
  }
  delay(2);
}
