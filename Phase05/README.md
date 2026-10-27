# Phase05: 10 fps depth and 1 fps QXGA RGB

Independent timestamped streams for DFRobot ESP32-S3 AI Camera v1.1 and TMF8829
LightRanger 14 on Gravity. Implements [ADR-0004](../Design/ADR/0004-independent-depth-rgb-bursts.md).

[Hardware validation and iteration-rate measurements](validation/2026-10-27.md).

- **8×8 long-range depth**, nominal **10 fps**, **1000 kilo-iterations**.
- **2048×1536 RGB JPEGs**, **1 fps**, quality **6**.
- **30-second recording**, commit files, **10-second deep sleep**, repeat.
- GPIO3 red LED high during the capture window, low otherwise. Board IR off.

The preferred **2000k** setting was measured at **5.28 fps** with the long-range
preset, because integration exceeded the requested 100 ms frame period. The
default uses **1000k** to retain approximately 10 fps. Actual configuration and
timestamps are saved; startup checks cadence rather than assuming the period was
achieved. Range/SNR performance at 11 m needs a separate physical test.

## Build and flash

Use the existing local Arduino-ESP32 3.3.2 / SdFat 2.3.1 toolchain, OPI PSRAM and
16 MB flash settings. From the repository root:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase05.ps1 setup
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase05.ps1 build
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase05.ps1 flash -Port COM12
```

[BurstConfig.h](Phase05/BurstConfig.h) sets iterations, cadence, burst length and
sleep. Increasing iterations beyond the cadence budget fails the startup check.
The external bus uses controller 0, SDA43/SCL44, address `0x41`, 400 kHz. Camera
SCCB uses controller 1 on GPIO8/9. SD uses FSPI CS10/MOSI11/SCK12/MISO13, 40 MHz,
with exFAT or FAT32; the 128 GB card does not require formatting.

## Burst files and reassembly

Recordings go to unused `/phase05/bNNNNNN/` folders, preserving earlier phases.
Each has a UUID in `burst.json`, independently numbered `000001.jpg` files,
`rgb.csv`, `depth.csv`, `depth.bin` and a final `summary.json`.

`rgb.csv` gives each JPEG's scheduled time, DMA/return timestamps, conservative
optical interval, size and original capture CRC. **`depth.csv` contains one entire
8×8 frame per row**: timestamps and sensor status, followed by 64 groups of
`raw_qmm_NN,snr_NN,distance_qmm_NN,valid_NN`. Divide quarter-millimetre distances
by four for millimetres. Invalid corrected distances are empty. Raw coordinates
are `x=NN%8`, `y=NN//8`.

All host timestamps are ESP microseconds since boot. Use the burst UUID/folder to
separate clock epochs after sleep. Native ticks, clock ratio and I2C transaction
brackets remain available in both depth files; `depth.bin` also preserves every
complete 225-byte native packet in CRC-protected fixed records. Its exact format
is specified in the ADR and decoded independently by `tools/phase05_protocol.py`.

Copy a complete burst from SD and generate nearest-depth associations:

```powershell
rtk python tools/phase05_reassemble.py E:/phase05/b000001 --output Phase05/validation/local/associations.csv
```

The utility checks source checksums and timestamps, then outputs RGB filename,
nearest depth sequence, both interval midpoints, their difference and interval
widths. It preserves existing output files. The association policy can later be
replaced by interpolation; it does not establish simultaneous exposure or RGB/depth
geometric alignment. All depth maps are retained independently of the RGB stream.

## Buffering and durability

Depth acquisition runs independently of JPEG writes and retains up to 384 frames
in PSRAM. At capture end, sensors stop and the archive is written as one CSV and
one binary file before sleep. **Depth is committed at burst end**; interruption
during capture loses the RAM archive. Metadata explicitly records this durability
policy. Images are committed during recording. The LED indicates acquisition
and is already off during final SD writes. Files use checked exclusive temporary
writes; `rgb.part` and `.tmp` files remain after interrupted recording.
Require a successful summary and matching checksums to accept a complete burst.

Only full optical-time envelopes inside the 30-second window are saved, so
boundary frames may be omitted. A successful burst requires at least 285 depth
frames and 28 RGB images with zero gaps, overflows, reader errors or missed RGB
slots. Warmup, final writes and diagnostic grace add overhead to the 30-second
capture and 10-second timer sleep. Sleep powers down ESP execution, stops camera
XCLK and places dTOF in standby; this phase does not measure current consumption.

## USB verification

One process may own COM12 at a time. The Phase04 console transport is compatible
and keeps DTR/RTS deasserted, avoiding accidental USB reset:

```powershell
rtk python tools/phase04_console.py --command HOLD --seconds 3
rtk python tools/phase04_console.py --command STATUS --seconds 3
rtk python tools/phase05_verify.py --restart --output Phase05/validation/local/acceptance
rtk python -m unittest discover -s tools/tests -v
```

`HOLD` keeps SD available after capture; `ALLOW_SLEEP` restores automatic cycling.
`STOP` ends early and marks a burst unsuccessful. `GET <burst> META|INDEX|SUMMARY`
reads metadata/RGB index/summary; `GET <burst> DEPTH|RAW` reads complete depth
files; `GET <burst> JPG <sequence>` reads an image through the acknowledged
download protocol. Use `BurstConsole.get()` or the verifier for binary transport.
`REBOOT` acknowledges `REBOOTING` before restart so the verifier can reject stale
startup output. `SLEEP 10` tests timer wake from a finished burst.

The default verifier checks every JPEG and all depth frames, then timer wake,
the next burst's complete depth files and preservation of an earlier image.
It emits `associations.csv` for each checked burst. `--manifest-only` skips JPEG
downloads for diagnosis; `--skip-sleep-check` omits the wake test. Successful
validation restores automatic cycling; failures leave the board held for diagnosis.
Scene data and detailed logs remain under ignored `validation/local/`.
