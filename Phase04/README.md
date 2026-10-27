# Phase04: synchronized RGB/depth bursts

Arduino/C++ firmware for DFRobot ESP32-S3 AI Camera v1.1, OV3660 and a
TMF8829 LightRanger 14 on Gravity I2C. Implements
[ADR-0003](../Design/ADR/0003-synchronized-rgb-depth-bursts.md).

[Hardware validation: two successful bursts and timer wake](validation/2026-10-26.md).

- Record for **30 seconds**, enter **10 seconds of ESP deep sleep**, then repeat.
- Save **2048×1536 RGB JPEGs, quality 6**, paired with **8×8 long-range depth CSVs**.
- Default **1 paired fps**, configurable up to a **10 fps ceiling**. Quality takes
  priority: measured checked SD commits were too slow to sustain 2 fps.
- GPIO3 red LED stays on during the recording burst and off otherwise.
- Histograms and board IR are off. TMF8829 is placed in standby between bursts.

Initialization/warmup, shutdown and the two-second diagnostic grace period add
overhead to the recording/sleep cycle. This PoC verifies timer wake, not current
consumption or motion wake. Long-range configuration does not establish 11 m
accuracy in the mounted scene.

## Build and flash

Reuse the project-local Arduino-ESP32 **3.3.2**, SdFat **2.3.1**, OPI PSRAM and
16 MB flash configuration from earlier phases. From the repository root:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase04.ps1 setup
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase04.ps1 build
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase04.ps1 flash -Port COM12
```

Settings are in [BurstConfig.h](Phase04/BurstConfig.h) and
[BoardConfig.h](Phase04/BoardConfig.h). Rebuild/reflash after changes. Lower JPEG
quality numbers mean higher image quality. SD clock defaults to 40 MHz; validate
full readback when changing cards or speed. Existing exFAT/FAT32 recordings are
preserved. Never format the card to install this phase.

Camera SCCB uses hardware controller **1**, GPIO8/9; external I2C uses controller
**0**, SDA43/SCL44, address `0x41`, 400 kHz. Using controller 1 for both corrupts
the bus configuration. SD uses FSPI CS10/MOSI11/SCK12/MISO13. GPIO47 IR stays low.
See [source provenance](UPSTREAM.md).

## Files and synchronization

Each boot creates an unused `/bursts/bNNNNNN/` folder with `burst.json`,
`pairs.csv`, `summary.json` and matching `000001.jpg`, `000001.csv`, `000001.tof`
files. Numbering restarts within each burst; the folder and random UUID identify
the burst. The 225-byte `.tof` preserves the complete native depth result for
verification. Depth CSVs retain Phase02 units, raw values and validity flags.

The manifest binds every pair to the original capture CRCs and timing inputs.
The summary reports actual acquisition bounds, losses, errors and shutdown.
Files commit through exclusive `.tmp` files; `pairs.part` remains until clean
close. Interrupted files are preserved for diagnosis. A completed file alone does
not establish a completed burst; check its summary and manifest.

Every saved pair must pass the **half-frame** bound: **500 ms at 1 fps**.
Pairing compares the *worst endpoint separation* of conservative RGB/depth
optical-time envelopes, rather than only host receive times or interval midpoints.
The camera's exposure is frozen after auto-exposure settles, and night-mode
frame extension is disabled. Native depth ticks are mapped through the vendor
clock ratio with an explicit host read bracket and margin. Rejected pairs are
counted. Both envelopes must lie inside the 30-second recording window.

This relies on driver timestamps, bounded exposure and the clock-mapping model
described in the ADR. External optical synchronization calibration and geometric
RGB/depth alignment are not established by reading metadata. A higher configured
rate is accepted only where its tighter bound and storage budget can be met.

## USB diagnostics and validation

Use `phase04_console.py` so DTR/RTS remain deasserted; some serial monitors reset
native USB on opening/closing. One program may own COM12 at a time.

```powershell
rtk python tools/phase04_console.py --command HOLD --seconds 3
rtk python tools/phase04_console.py --command STATUS --seconds 3
rtk python tools/phase04_console.py --command ALLOW_SLEEP --seconds 3
rtk python tools/phase04_verify.py --restart --output Phase04/validation/local/acceptance-final
rtk python -m unittest discover -s tools/tests -v
```

`HOLD` prevents sleep after a burst so its files remain available. `ALLOW_SLEEP`
restores automatic cycling. `STOP` ends a burst early and marks it unsuccessful.
`REBOOT` restarts; `SLEEP 10` requires a finished healthy burst. `GET 1 META`,
`GET 1 INDEX`, `GET 1 SUMMARY`, and `GET 1 JPG|DEPTH|RAW 1` use the acknowledged
download protocol; use the verifier's `BurstConsole.get()` for binary downloads.

The verifier holds sleep, decodes **every JPEG**, validates all CSV/native results,
compares acquisition CRCs, independently recomputes synchronization, then tests
10-second timer wake, the next burst and preservation of an earlier image.
Successful verification restores automatic cycling. On failure it leaves the
device held for diagnosis. `--skip-sleep-check` is available for bring-up.
`--manifest-only` skips data-file downloads for quick wakeup diagnosis and is
not full acceptance verification.
Downloaded scene data and detailed logs stay under ignored `validation/local/`.

## RGB/depth videos

With Pillow (from `tools/requirements.txt`) and FFmpeg/FFprobe on PATH, render
every completed burst on a capture card:

```powershell
rtk python tools/burst_video.py E:/bursts --output Phase04/validation/local/videos --speed 2
```

Each burst produces an H.264 MP4 with RGB beside the 8×8 depth grid rotated
90 degrees clockwise then mirrored left/right, whole centimetres in each valid
cell, and a preview JPEG.
Cell labels round to the nearest integer (half up); colors use the full distance
precision. Invalid cells are grey
and marked `N/A`. A shared reversed Viridis color scale (near yellow, far purple)
covers all input bursts, rounded
up to the next 100 cm. Source CSV coordinates `(x, y)` map to display
`(y, x)`; RGB retains its original orientation.

The renderer checks the manifest RGB/depth CRCs and depth frame association.
Recorded camera timestamp intervals are divided by `--speed`; the last pair is
held for one nominal period divided by that speed. Output uses 30 fps with held
frames, so timing is quantized to 1/30 second. `render_report.json` records the
color range, output dimensions and verified durations. Existing MP4s are never
overwritten; use a fresh output directory to render again.
