# Phase03: depth frames and complete histograms

Extends [ADR-0002](../Design/ADR/0002-tmf8829-dtof-logging-poc.md) and the
Phase02 Arduino skeleton with **8×8 long-range histogram acquisition** on the
TMF8829 / LightRanger 14 Click. It uses the verified Gravity connection:
SDA GPIO43, SCL GPIO44, address `0x41`, 400 kHz I2C.

[Hardware validation, 2026-10-24](validation/2026-10-24.md) passed for 30 complete
sets: all 90 native packets and depth CSVs were exported and checked, with zero
sensor gaps, incomplete sets or I2C errors, at approximately 0.1975 Hz.

Each measurement consists of native histogram T0, histogram T1 and a depth
result, in that order. All native bytes are retained, including FIFO preheader,
header, reference channels, zone bins, timestamps, status and footer. The host
decoder keeps **64 × 256 zone bins** plus **two sets of four × 64 reference bins**.
Each count is a 24-bit little-endian integer. T0 maps to x=0–3 and T1 to x=4–7,
both y=0–7 in vendor order. This is histogram export with the sensor's associated
ranging result; no independent host peak-to-distance algorithm is introduced.

## Build and run

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase03.ps1 setup
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase03.ps1 build
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase03.ps1 flash -Port COM12
```

Uses Arduino-ESP32 3.3.2, SdFat 2.3.1 and the same pinned vendor core/patch as
Phase02. Board configuration remains ESP32-S3, 16 MB flash, OPI PSRAM and native
USB CDC. Flashing switches to the separate Phase03 sketch. Camera, microphone,
networking and GPIO47 IR illumination remain disabled; GPIO3 pulses when a
complete set is saved. Earlier SD recordings are preserved.

The long-range preset `0x41`, one peak per zone, and dual mode off remain in use.
Histogram dumping is enabled and the full configuration is read back. The
requested period is **5000 ms (0.2 Hz)**: transferring a complete histogram pair
already needs more than one second at 400 kHz. Actual cadence and per-packet
I2C durations are reported, rather than claiming Phase02's 1 fps in this mode.

## Storage, buffering and export

Depth CSVs and session/configuration JSON continue on the existing exFAT/FAT32
SD card under `/dtof/sXXXXXX/`, using Phase02's exclusive creation, checked
temporary writes and bucketed directories. CSVs include histogram completeness,
the two histogram native frame numbers and their acquisition CRC32 values.
Raw quarter-mm distances, vendor clock correction, SNR and empty invalid-zone
distances follow the Phase02 contract.

Complete native sets are held in a **64-set PSRAM ring** (about 3.3 MB with
working buffers). A set is 50,979 bytes: two 25,377-byte histogram packets and
one 225-byte result. No histogram is reduced to peaks or decimated. Each set is
tagged with its session-local CSV sequence; requesting an evicted sequence fails
explicitly. `cache_replaced` counts all cache replacements, including sets already
downloaded. At the default period this holds roughly five minutes of history.

Native frame numbers advance once **per packet**, so consecutive depth results
advance by three. Firmware validates type, layout, payload length, frame-number
continuity, focal plane and EOF before publishing a complete set. Histogram
status must be valid and not aborted. Unexpected packets, missing/out-of-order
subframes, I2C errors or a 20-second result timeout stop acquisition; no older
histogram is substituted. Intentional STOP during a partial measurement discards
it and increments `incomplete`. A boot self-test exercises the actual native
validator/order checker, including missing/duplicate/out-of-order packets.

Vendor clock correction is updated from every packet's host/sensor tick pair.
The host result timestamp identifies its own read start; `group_start_us`
identifies the T0 read start. Neither is an exact exposure timestamp. Original
integration timestamps and native packets are preserved for later interpretation.

**Histogram persistence on SD and the appendable binary format remain a later
phase.** This phase demonstrates sensor acquisition and host export. The host
verification command saves every native set locally, alongside decoded data.

## Console and short validation

115200 baud, uppercase newline-terminated commands:

| Command | Action |
| --- | --- |
| `STATUS` | Recording/session state, gaps, I2C errors, histogram packets, incomplete sets and cache replacements. |
| `INFO` | Effective sensor identity, preset, histogram profile and full configuration bytes. |
| `STOP` / `START` | Pause/resume capture; pause before downloads. |
| `GET 25` | Download the SD depth CSV for sequence 25. |
| `GET RAW 25 0` | Download native T0 from the PSRAM cache. |
| `GET RAW 25 1` | Download native T1 from the same set. |
| `GET RAW 25 2` | Download its native depth result. |
| `GET SESSION` / `GET CONFIG` | Download persisted session/configuration JSON. |
| `REBOOT` | Stop and create a new session after restart. |

Transfers use acknowledged 512-byte blocks, per-block CRC32 and bounded retries.
The USB transmit buffer is 4 KiB with a bounded 1-second write timeout, allowing
complete diagnostic lines while the host catches up after connection.
Acquisition logs also contain each original packet CRC and a whole-set CRC, so
the verifier compares exported bytes with the bytes captured from the sensor.
Avoid concurrent serial monitors. USB connection changes may restart this board;
always obtain the current session with `STATUS`.

```powershell
rtk python -m pip install --target .tools/python -r tools/requirements.txt
rtk python -m unittest discover -s tools/tests -v
rtk python tools/phase03_verify.py --port COM12 --frames 30
```

Use `--frames 60` for a longer short check; the verifier caps runs at 60 to stay
within the cache. It captures at the configured rate, stops, downloads every
SD CSV and all three native packets per measurement, and checks original CRCs,
64-zone mapping, reference counts, packet association, common integration times and agreement between the
native result and SD distances/SNR. It also preserves the complete decoded
histograms in JSON. Successful verification resumes recording.

Private logs, raw `.native` packets, `depth.csv`, `decoded.json`, hashes and the
machine summary stay under ignored `Phase03/validation/local/`. Native packet
blobs here are diagnostic exports, not the future ADR `.dtf` recording format.

The full effective configuration accompanies each session. Bin indices are kept
as indices: no unverified long-range bin-to-time or bin-to-distance conversion
is applied. Physical 1/5/8/11 m accuracy, orientation calibration and low-power
operation remain separate validation work.
