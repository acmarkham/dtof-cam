# Phase01 hardware validation

The current firmware defaults to QXGA (2048 x 1536). See the
[2026-10-24 QXGA results](qxga-2026-10-24.md). The historical VGA baseline is
retained below.

Date: 2026-10-22. Board connected over native USB on COM12.

## Configuration verified

| Item | Observed configuration |
| --- | --- |
| MCU | ESP32-S3, revision v0.2; 8 MB embedded PSRAM |
| Flash | 16 MB, quad, 3.3 V |
| Firmware | Arduino-ESP32 3.3.2; SdFat 2.3.1 at `cda057318bec196183d4cc92b01bc1dd64bbfb02` |
| Camera | VGA 640 x 480 colour JPEG, quality setting 10, two PSRAM buffers, latest frame |
| SD | exFAT; volume capacity 128,144,375,808 bytes; SPI clock ceiling 10 MHz |
| Outputs | IR off; user confirmed GPIO3 LED visibly flashes once per second |
| Final application binary SHA-256 | `38ebbdb1a2fddee2a3600abce04ae77b34a8c9387fd9132eb6f5f9d4f0850cfd` |

The build succeeds with 419,419 bytes of program storage and 33,316 bytes of
static RAM. Upload verifies the flashed data hash. The initial clean build emitted
upstream ESP-IDF HAL missing-initialiser warnings; the application built without
application-source warnings. The project-local setup command was checked again
successfully after installing dependencies.

## Hardware checks

The five-frame smoke check passed JPEG decoding, capture/readback CRC32 matching,
one-second request cadence, increasing sensor frame timestamps, software restart
numbering, and preservation of an earlier JPEG. A downloaded image was visually
inspected. The camera points upward in the current setup; physical orientation
has not been independently calibrated.

The ten-minute acquisition run and full readback passed; see
[the acceptance summary](acceptance-summary.json) and [capture summary](capture-summary.json).

| Check | Result |
| --- | --- |
| Images saved | 600, sequence 000413 through 001012 |
| Mean request interval | 1,000,000 microseconds |
| Minimum / maximum request interval | 999,990 / 1,000,010 microseconds |
| Maximum capture-and-save duration | 609,243 microseconds |
| Capture failures / missed deadlines | 0 / 0 |
| Readback and JPEG decode | All 600 images matched their capture CRC32 and decoded as 640 x 480 RGB JPEGs |
| JPEG size range | 11,372 to 31,135 bytes |
| Software restart | Card remounted as exFAT; numbering resumed at 001275; earlier image unchanged |

The acquisition run used application SHA-256
`176474fc1eda4939ec9a7524e901f2e0d0650ad0cd2f78df27c5e092561a0d19`.
The final build changes USB diagnostics to acknowledge and checksum individual
download blocks; capture, scheduling, and SD writing are unchanged.

On a subsequent restart the SD card stopped responding to initialisation
(`SdError 0x1, 0xff`). The firmware correctly reports the storage fault and
disables recording without formatting the card. The user unplugged and reconnected
USB power; the card mounted again as exFAT and capture resumed above existing
sequence numbers. Full readback subsequently passed with the final download
protocol, including another software restart and comparison of a pre-existing
JPEG. The mount fault was observed once; its root cause has not been established.

The final download protocol's host-side tests pass recovery from corrupted USB
blocks, duplicate blocks after lost acknowledgements, and rejection of incomplete
or oversized transfers. The final firmware also passed all 600 hardware readbacks.

## Limits of the checks

The 128 GB volume mounts and supports JPEG write/sync/close/rename/readback. This
does not establish full-media reliability or actual writes beyond 32 GB. No
formatting or capacity-filling test was run on the user's card.

Power-cycle recovery from the mount fault was observed. Sudden power interruption
during a write, deliberate card removal/full-card faults, temporary-file
recovery, sequence exhaustion, and injected camera/write/rename faults remain
separate bench checks from the ADR. Restart validation uses a software reset.
The current root-directory layout still needs rotation/indexing for long runs;
write latency grows with the number of entries even after redundant scans were
removed. The observed ten-minute cadence must not be extrapolated to a full card.
After verification, recording was resumed successfully. With roughly 1,300
images in the root, observed capture-and-save times were 895–957 ms, leaving
little margin for sustained 1 Hz capture. Directory rotation is needed before
using this layout for extended unattended recording.

Local raw logs, decoded photographs, and smoke-test results are under the ignored
`local/` directory. Reproduce with the commands in [the Phase01 guide](../README.md).
