#pragma once
#include "MotionDetector.h"

struct MotionTestReport { unsigned checks = 0, failures = 0; };

inline MotionSample uniformMotionSample(uint16_t qmm) {
  MotionSample sample;
  sample.validMask = UINT64_MAX;
  for (auto& distance : sample.distanceQmm) distance = qmm;
  return sample;
}

// Runs the actual C++ classifier compiled into the ESP firmware.
inline MotionTestReport testMotionDetector() {
  MotionTestReport report;
  auto expect = [&](bool condition) { ++report.checks; if (!condition) ++report.failures; };
  MotionDetector detector;
  const auto base = uniformMotionSample(4000); // 1 metre.
  expect(detector.requiredZones() == 4);
  auto result = detector.update(base);
  expect(result.mode == MotionMode::Baseline && !result.moving);
  result = detector.update(base);
  expect(result.comparable == 64 && result.changed == 0 && !result.moving);
  auto changed = base;
  for (unsigned i = 0; i < 3; ++i) changed.distanceQmm[i] = 6000;
  result = detector.update(changed);
  expect(result.changed == 3 && !result.moving);
  detector.reset(); detector.update(base);
  changed.distanceQmm[3] = 6000;
  result = detector.update(changed);
  expect(result.changed == 4 && result.moving && result.changedMask == 15);
  result = detector.update(changed);
  expect(!result.moving && result.changed == 0); // Holding at new distance.
  result = detector.update(base);
  expect(result.moving && result.changed == 4); // Returning is also movement.
  expect(!detector.update(base).moving);

  auto thresholdCase = [&](uint16_t before, uint16_t after, bool expected) {
    detector.reset(); detector.update(uniformMotionSample(before));
    auto current = uniformMotionSample(before);
    for (unsigned i = 0; i < 4; ++i) current.distanceQmm[i] = after;
    expect(detector.update(current).moving == expected);
  };
  thresholdCase(1000, 2200, false); // Exactly 30 cm absolute.
  thresholdCase(1000, 2201, true);
  thresholdCase(12000, 13600, false); // 40 cm, but less than 20% of 3 m.
  thresholdCase(12000, 14400, false); // Exactly 20%.
  thresholdCase(12000, 14401, true);
  thresholdCase(12000, 9600, false);
  thresholdCase(12000, 9599, true);
  thresholdCase(4000, 65535, true);

  detector.reset(); detector.update(base);
  changed.validMask &= ~uint64_t(15);
  result = detector.update(changed);
  expect(result.changed == 0 && result.comparable == 60 && !result.moving);
  detector.reset(); detector.update(changed);
  changed.validMask = UINT64_MAX;
  result = detector.update(changed);
  expect(result.changed == 0 && result.comparable == 60); // New validity not motion.
  auto absent = base; absent.validMask = 0;
  expect(!detector.update(absent).moving);
  result = detector.update(base);
  expect(result.mode == MotionMode::Insufficient && !result.moving);
  expect(detector.update(base).mode == MotionMode::Static);
  auto sparse = base; sparse.validMask = 7;
  detector.reset(); detector.update(sparse);
  for (unsigned i = 0; i < 3; ++i) sparse.distanceQmm[i] = 6000;
  result = detector.update(sparse);
  expect(result.mode == MotionMode::Insufficient && result.changed == 3 && !result.moving);

  MotionParameters parameters;
  parameters.scenePercent = 3;
  expect(detector.configure(parameters) && detector.requiredZones() == 2);
  detector.update(base);
  changed = base; changed.distanceQmm[0] = changed.distanceQmm[1] = 6000;
  expect(detector.update(changed).moving);
  parameters.scenePercent = 100;
  expect(detector.configure(parameters) && detector.requiredZones() == 64);
  parameters.scenePercent = 1;
  expect(detector.configure(parameters) && detector.requiredZones() == 1);
  parameters.scenePercent = 0;
  expect(!detector.configure(parameters));
  parameters.scenePercent = 101;
  expect(!detector.configure(parameters));
  parameters.scenePercent = 5; parameters.relativePercent = 1001;
  expect(!detector.configure(parameters));
  parameters.relativePercent = 20; parameters.minAbsoluteCm = 0;
  expect(!detector.configure(parameters));
  parameters.minAbsoluteCm = 1639;
  expect(!detector.configure(parameters));
  parameters.minAbsoluteCm = 1; parameters.relativePercent = 0;
  expect(detector.configure(parameters));
  thresholdCase(4000, 4040, false);
  thresholdCase(4000, 4041, true);
  detector.reset();
  expect(detector.update(base).mode == MotionMode::Baseline);
  return report;
}
