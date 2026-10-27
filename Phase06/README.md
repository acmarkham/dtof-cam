# Phase06: dTOF motion detection

Standalone motion monitor for the DFRobot ESP32-S3 AI Camera v1.1 and TMF8829
LightRanger 14 on Gravity. Implements [ADR-0005](../Design/ADR/0005-consecutive-depth-motion-detection.md).

[Build/test results and hardware validation status](validation/2026-10-27.md).

- **8×8 long-range depth, nominal 2 fps, 2000 kilo-iterations**.
- Red GPIO3 LED on when motion is detected, off when consecutive frames are static.
- Default motion: at least **4 of 64 zones** change by more than **both 20% of the
  previous distance and 30 cm**. Four zones are the rounded-up 5% scene threshold.
- Holding an object still at a new distance turns the LED off on the next frame.

The camera, SD recording, burst cycling and sleep are disabled in Phase06. IR is
off. Existing recordings are preserved. This is continuous monitoring, with no
averaging or LED hold time. At 2 fps motion between acquisitions can be missed.

## Build and flash

Use the existing local Arduino-ESP32 **3.3.2** toolchain. Phase06 requires only
the core's Wire library; it does not link SdFat or initialize the camera.

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase06.ps1 setup
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase06.ps1 build
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase06.ps1 ports
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase06.ps1 flash -Port COM12
```

`setup` is unnecessary if the local core is already installed. The flash action
uploads the last build. Use the ESP's current port, which can change after USB
reconnection. If Phase05 sleep prevents upload, reconnect USB and upload while the
ESP is awake, or hold BOOT during reset to enter the ESP download loader.

[MotionConfig.h](Phase06/MotionConfig.h) sets the compiled defaults.
[MotionDetector.h](Phase06/MotionDetector.h) contains the classifier independently
of Arduino. The external bus uses controller 0, SDA43/SCL44, address `0x41`,
400 kHz. EN remains physically asserted; no new interrupt wiring is needed.

## Motion rules and USB console

Only compare zones with valid returns in **both** adjacent frames. Invalid/missing
readings do not count as movement. Fewer than four comparable zones yields
`insufficient`, with LED off. The denominator remains all 64 zones. A four-frame
warmup precedes the first baseline; baseline frames also leave the LED off.
Frame gaps clear the reference. Two seconds without data clears the LED and
reference; sensor I/O/protocol faults halt acquisition with LED off.

Only one process may open the USB port at a time. The existing console transport
works with Phase06 and keeps native USB DTR/RTS deasserted:

```powershell
rtk python tools/phase04_console.py --port COM12 --command STATUS --seconds 3
rtk python tools/phase04_console.py --port COM12 --command "CONFIG 3 20 30" --seconds 3
rtk python tools/phase04_console.py --port COM12 --command "CONFIG 5 20 30" --seconds 3
rtk python tools/phase04_console.py --port COM12 --command SELFTEST --seconds 8
```

| Command | Effect |
| --- | --- |
| `STATUS` | Motion/LED state, counts, coverage, thresholds, errors and uptime |
| `CONFIG X Y Z` | Scene %, previous-distance %, minimum cm; reset reference/LED |
| `RESET` | Reset baseline/LED without rebooting or clearing statistics |
| `SELFTEST` | Pause sensor, run C++ tests and a synthetic low/high/low GPIO check, resume |
| `REBOOT` | Acknowledge then reboot; restore compiled defaults |

`CONFIG` is RAM-only; reboot restores `5 20 30`. Scene percentage is rounded up:
5% requires four changed zones, while `CONFIG 3 20 30` requires two. Each changed
zone must pass both strict thresholds. GPIO readback verifies electrical output
state, while a physical observation verifies visible flashing.

`FRAME source=sensor` logs each decision, native frame number, `host_us` monotonic
microseconds since ESP boot, reset epoch, configuration, coverage/change masks and
the full 225-byte native result with CRC-32. Raw quarter-millimetre distances are
corrected using the logged UQ15 ratio. Native zone `i` maps to `x=i%8`, `y=i//8`.
`MOTION source=sensor` reports transitions. The LED selftest is explicitly tagged
`source=synthetic` and does not increment real motion-event counts.

## Verification

Install the existing [host requirements](../tools/requirements.txt) into
`.tools/python` if needed. The verifier checks C++ selftests, GPIO low/high/low,
runtime configuration and rejected invalid configuration, then independently
replays 120 live native frames over approximately one minute. It checks every
mask, count, classification and LED state, cadence and error counters, and leaves
the default monitor running.

```powershell
rtk python tools/phase06_verify.py --port COM12 --reboot --frames 120
rtk python -m unittest discover -s tools/tests -v
```

Detailed scene logs and JSON summaries go under ignored `validation/local/`.
Move a hand or object across the field of view, then hold still to check real
motion and visible LED behavior. The synthetic diagnostic covers classifier/GPIO
logic; a stationary live scene cannot establish real-target detection performance.
Maximum range, false-positive rate and current consumption need separate tests.
