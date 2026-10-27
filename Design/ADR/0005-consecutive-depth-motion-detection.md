# ADR-0005: Consecutive depth frames for motion detection

- Status: Accepted for Phase06 PoC
- Date: 2026-10-27
- Implementation: [Phase06](../../Phase06/README.md)
- Related: [dTOF acquisition](0002-tmf8829-dtof-logging-poc.md),
  [independent recording](0004-independent-depth-rgb-bursts.md)

## Decision

Run the TMF8829 continuously in **8×8A long-range preset `0x41` at 2 fps**,
using a 500 ms period and **2000 kilo-iterations**. Compare each frame with the
immediately preceding frame. GPIO3 red LED is high while the current comparison
indicates motion and low as soon as the scene becomes static. A target held at a
new distance becomes static on the next frame; no return to the startup scene is
required. Camera acquisition, SD recording, burst cycling and sleep are disabled
for this standalone phase. Board IR remains off.

For a zone valid in both frames, let `d` be the previous corrected distance and
`delta` the absolute change. The zone counts as changed only when **both**:

```
delta > d * Y / 100
delta > Z cm
```

Defaults: **X = 5% of the full scene, Y = 20%, Z = 30 cm**. Motion requires at
least `ceil(64 * X / 100)` changed zones. Thus 5% requires **4 zones**; 2 zones
are 3.125% of the scene and can be selected with integer `X = 3`. The denominator
always remains 64, including when some zones are invalid. Compare integer products
in quarter-millimetre units to preserve strict percentage/absolute boundaries.
At 1 m the effective threshold is greater than 30 cm; at 3 m it is greater than
60 cm. Apply the previous frame's distance for either direction of movement.

## Validity and state

Retain the proven Phase05 bounded 225-byte native reader, sensor patch, corrected
distance calculation and deterministic boot-monitor reset. Gravity uses I2C
controller 0, SDA43/SCL44, address `0x41`, 400 kHz. Read back the configuration;
require one raw-distance/SNR return per zone, focal plane A only, no histogram
or dual mode. The integration setting fits within the low-rate budget; it does
not establish physical performance at the datasheet's maximum range.

Use a zone only when the frame result-valid bit is set, neither invalid/abort bit
is set, raw and corrected distances are nonzero, and SNR is nonzero. Validity loss
and recovery do not themselves count as movement. Do not treat a missing return
as distance zero. Fewer comparable valid zones than the required scene count
produces an explicit `insufficient` decision with LED off. Consequently movement
into or out of an otherwise unmeasurable region can be missed in this first pass.

The first frame establishes a baseline and leaves the LED off. Update the baseline
after **every** complete frame, even when movement is below threshold. Discard four
warmup frames after initialization, selftest restart or stale data. Reset the
reference and LED after a sensor counter discontinuity. No frame for two seconds
clears the LED and reference; a malformed packet or I2C error halts acquisition,
keeps the LED off and exposes a diagnostic fault over USB. LED off alone cannot
distinguish static, warming, insufficient data or a fault.

## Configuration and diagnostics

`CONFIG X Y Z` changes integer scene percentage, relative percentage and absolute
centimetres in RAM, clears the reference and turns the LED off. Supported ranges
are 1–100, 0–1000 and 1–1638 respectively; invalid input leaves state unchanged.
`RESET` clears the reference; `STATUS` reports validity, motion, GPIO and error
counts; `REBOOT` restores compiled defaults. These commands do not touch SD.

USB frame diagnostics retain monotonic host microseconds since boot, native frame
number, corrected-distance ratio, complete CRC-protected native packet, validity/
comparison/change masks, thresholds, classifier state and GPIO readback. An epoch
number separates baseline resets. Host-side logging is optional and does not
introduce on-device persistence.

## Validation and later work

Run the actual C++ classifier tests on the ESP for scene rounding, strict distance
boundaries, both directions, sparse/invalid zones and movement followed by holding
still at a new distance. `SELFTEST` pauses the sensor and exercises synthetic
baseline → motion → static through the normal LED setter, checking GPIO low →
high → low before resuming real ranging. This validates logic/output and is clearly
labeled synthetic; it does not substitute for moving a real physical target.

Independently decode native packets on the host and recompute every decision and
LED state over a short live observation. Check approximately 500 ms cadence,
sequence continuity, thresholds and zero protocol/I2C faults. Physical target
testing must check the visible LED and long-range scene conditions separately.

Later stages may add interframe averaging, adaptive noise/coverage thresholds,
hysteresis/hold time, a persistent background model, motion-triggered RGB/depth
recording and low-power wake. None of these is needed for this first classifier.
