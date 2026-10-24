# Phase01: camera to exFAT SD

Implements [ADR-0001](../Design/ADR/0001-rgb-camera-sd-poc.md) for the DFR1154
ESP32-S3 AI Camera v1.1. Saves QXGA (2048 x 1536) colour JPEGs at a target of 1 Hz as `/000001.jpg`,
`/000002.jpg`, etc. GPIO3 pulses on acquisition; GPIO47 IR illumination is off.
No networking or sleep is enabled.

`Board::kFrameSize` in `Phase01/BoardConfig.h` selects `FRAMESIZE_QXGA`, the
OV3660's maximum supported output. Use `FRAMESIZE_FHD` for 1920 x 1080 and rebuild
and flash if 16:9 output is preferred. JPEG quality remains 10. Startup logs
report the sensor's selected dimensions, and capture rejects unexpected frame
dimensions. The original 2026-10-22 validation used VGA; changing resolution
requires new timing and JPEG readback checks.

[QXGA hardware validation (2026-10-24)](validation/qxga-2026-10-24.md) confirms
30 full-resolution JPEGs at 1 Hz, including CRC readback and restart checks.

## Build and flash

Install Arduino CLI or Arduino IDE 2 on Windows. From the repository root:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase01.ps1 setup
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase01.ps1 ports
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase01.ps1 build
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase01.ps1 flash -Port COM12
```

The scripts keep dependencies in `.tools/` and build output in `.build/`.
Pinned dependencies: Arduino-ESP32 3.3.2 and SdFat 2.3.1. Board target:
ESP32S3 Dev Module, 16 MB flash, 3 MB app / 9.9 MB FATFS partition scheme (internal
FATFS partition unused), OPI PSRAM,
hardware USB CDC enabled at boot. Flashing replaces the running firmware.
Change `-Port` to the connected board's ESP32 USB serial port if needed.

Use the 128 GB card with one MBR/exFAT partition. Firmware mounts existing
media and never formats it. Default firmware requires exFAT; set
`Board::kAllowFat32` for a deliberate FAT32 compatibility build.

After a bounded USB startup delay, storage initialisation and two seconds of
camera settling, recording starts automatically. A boot scan reserves existing
six-digit `.jpg` and `.tmp` names, including uppercase extensions. Writes use
exclusive creation, byte-count checks, sync, close, and rename. Storage errors
stop recording until restart. Null frames skip that attempt and retry next tick.
Numbers stop at 999999, and images are never automatically deleted.

## USB diagnostics and verification

The console is 115200 baud. Commands are uppercase and newline-terminated:

| Command | Behaviour |
| --- | --- |
| `STATUS` | Show readiness, recording state, next/last image, counters, outputs and storage. |
| `STOP` | Pause after the current synchronous operation; clear the LED. |
| `START` | Resume a healthy system on a fresh one-second deadline. |
| `REBOOT` | Restart and recover numbering from the card. |
| `GET 000001` | Used by the verification tool while stopped: download that JPEG with block CRCs, acknowledgements, and bounded retries. |

Install host verification dependencies and run the ten-minute acceptance check:

```powershell
rtk python -m pip install --target .tools/python -r tools/requirements.txt
rtk python tools/phase01_verify.py --port COM12 --frames 600 --restart-check
```

The check pauses/resumes capture, measures cadence, requires zero overruns,
downloads every image in the run, compares CRC32 with the original capture,
decodes each JPEG as 2048 x 1536 RGB, and checks numbering plus an earlier file
after software restart. Logs, summary JSON, and JPEGs are saved under ignored
`Phase01/validation/local/`. Verification leaves recording stopped; send `START`
or reset the board to resume. Avoid opening another serial monitor concurrently.
Downloaded images may contain private scenes; local results are not tracked.
For an FHD build, pass `--width 1920 --height 1080`; for historical VGA capture
logs, pass `--width 640 --height 480`.

To retry readback of a completed capture run without another acquisition run,
use a different output directory to preserve the source log:

```powershell
rtk python tools/phase01_verify.py --port COM12 --frames 600 --restart-check --verify-log Phase01/validation/local/serial.log --output Phase01/validation/local/readback
```

For a bounded status read or to resume after verification:

```powershell
rtk python tools/phase01_console.py --port COM12 --command STATUS
rtk python tools/phase01_console.py --port COM12 --command START
```

`FRAME` logs distinguish application request time (`t_us`) from camera frame
timestamp (`frame_us`). They report capture and storage/CRC duration and content
CRC32. Sensor timestamps must increase; changing JPEG hashes alone do not prove
movement because image noise changes bytes. Observe a changing scene and inspect
orientation/colour for the visual acceptance check.

The automated check does not establish full-card reliability, physical power-loss
recovery, visible LED brightness, or the absence of stale frames in a stationary
scene. Capacity beyond 32 GB, missing/full/removed media, abandoned temporary
files, exhaustion, and fault injection remain separate ADR checks. Do not fill or
reformat a card containing wanted files for a capacity or fault test.

Host-side download protocol tests:

```powershell
rtk python -m unittest discover -s tools/tests -v
```
