# Phase02: TMF8829 depth maps to SD

Implements the first depth-only stage of [ADR-0002](../Design/ADR/0002-tmf8829-dtof-logging-poc.md).
The separate Arduino/C++ sketch discovers the LightRanger 14 Click, verifies
TMF8829 identity, loads the vendor RAM patch, selects **8×8 long range** (command
`0x41`), and logs one 64-zone CSV per sensor frame at a requested 1 Hz.
Camera, microphone and networking are not initialised. Board IR GPIO47 stays low;
GPIO3 pulses after each depth read while writing its CSV.

[Hardware validation, 2026-10-24](validation/2026-10-24.md): 1,005 CSVs acquired
and checked at 0.988 fps, including bucket rollover and a fresh session after
restart, with zero sensor gaps or I2C errors.

## Wiring and startup

Use the Gravity connector's 3.3 V and ground. The installed connection was
verified as **SDA GPIO43 / SCL GPIO44** (the reverse of the ADR's initial proposal).
Discovery probes both 43/44 orientations at 100 kHz, then uses 400 kHz after
checking address `0x41` and chip ID `0x9e`. Keep the Click configured for I2C and
hold its EN signal high; the four-wire Gravity connection has no EN/INT control.
Status is polled every few milliseconds with a 50 ms I2C transaction timeout.
Startup also handles a still-powered sensor after an ESP restart with a software
reset before patch loading. A different address/device is never configured blindly.

The sensor's long-range optical settings and iteration count are retained. The
measurement period is set to 1000 ms, dual mode and histograms are disabled, and
one peak is requested. The complete configuration is read back and compared
before recording. The raw configuration bytes and actual iteration count are
saved in the session's configuration JSON.

## Build and flash

Use the project-local toolchain already installed for Phase01, or run setup:

```powershell
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase02.ps1 setup
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase02.ps1 build
rtk proxy powershell -NoProfile -ExecutionPolicy Bypass -File tools/phase02.ps1 flash -Port COM12
```

Dependencies are Arduino-ESP32 3.3.2 and SdFat 2.3.1, with the same ESP32-S3,
16 MB flash, OPI PSRAM and USB CDC board configuration as Phase01. Vendor sensor
sources and the patch are included; their origin, licence and changes are in
[src/tmf8829/README.md](Phase02/src/tmf8829/README.md).
Flashing Phase02 replaces the running camera firmware. Phase01 sources and SD
JPEGs remain available; use its build/flash script to switch back.

## Files and validity

The existing card is mounted with SdFat at 10 MHz SPI. Both **exFAT (including
128 GB SDXC)** and FAT32 are accepted. Firmware never formats or deletes data.
A new, unused session directory and UUID are allocated on every boot:

```text
/dtof/s000001/session.json
/dtof/s000001/config/000001.json
/dtof/s000001/csv/b0000/000000001.csv
/dtof/s000001/csv/b0000/000001000.csv
/dtof/s000001/csv/b0001/000001001.csv
```

Pause/resume stays in the same session and continues its sequence. Booting
reserves an existing session even if startup or its last write was interrupted.
The bucket directory handle is cached; only 1,000 CSVs go into each bucket.
Files are exclusively created as `.tmp`; byte count, sync and close must succeed
before an exclusive rename to `.csv`. SD full/write failure, I2C/protocol errors,
duplicate/regressing native frame numbers, or five seconds without a frame stop
logging and require a restart. Abandoned `.tmp` files remain for diagnosis.
exFAT is not journalled; this protocol does not guarantee filesystem consistency
if power fails during an SD operation.

Each CSV has metadata comments and exactly 64 vendor row-major zones
(`index=y*8+x`), with this header:

```csv
x,y,distance_mm,distance_raw_qmm,snr_raw,valid
```

`distance_raw_qmm` preserves native quarter-mm values. `distance_mm` applies the
vendor UQ1.15 clock ratio with quarter-mm rounding and saturation, then prints
three decimal places. Host receive time is monotonic microseconds since boot;
sensor tick and integration timestamps retain their original 125 kHz units.
The ratio starts at unity until sufficient timestamp pairs exist. Metadata
preserves frame status, configuration/session IDs and the native frame number.

A zone is valid when frame status bit 0 is set, aborted bits 6/7 are clear, and
both raw distance and SNR are nonzero. Sensor confidence filtering remains at
the preset setting, recorded in `config_hex` (register `0x53`). Warning bits
remain visible in the status. Invalid zones have **empty `distance_mm`** while
retaining raw data. A well-formed frame with no valid returns still gets a CSV.
Unexpected type/layout/length, I/O failures and a bad end marker are rejected
before publication. The first stage deliberately accepts only the configured
one-peak 8×8 result layout.

The 11 m datasheet figure is a target for physical testing. Merely selecting
the long-range preset does not establish 11 m performance.

## USB diagnostics and verification

115200 baud, uppercase commands terminated by newline:

| Command | Action |
| --- | --- |
| `STATUS` | Readiness, recording, sequence, errors/gaps and session directory. |
| `INFO` | Effective sensor identity, firmware version and configuration. |
| `STOP` / `START` | Pause/resume healthy acquisition. |
| `GET 1` | Download CSV sequence 1 from the current session while stopped. |
| `GET SESSION` / `GET CONFIG` | Download the session/config JSON while stopped. |
| `REBOOT` | Stop and restart into a new session. |

Downloads use Phase01's bounded retries, acknowledgements and per-block CRC32.
`FRAME` diagnostics report sensor/host sequence, receive time, measured interval,
valid-zone count, status, bytes, read/write duration, clock ratio and gaps.
No synthetic frame is created to hide a missed measurement.

```powershell
rtk python -m pip install --target .tools/python -r tools/requirements.txt
rtk python -m unittest discover -s tools/tests -v
rtk python tools/phase02_verify.py --port COM12 --frames 600 --restart-check
```

Use `--frames 1005` to include the 1,000-frame bucket transition. Verification
downloads **every CSV**, checks the 64 unique ordered coordinates, raw bounds,
units, correction, invalid-zone handling and acquisition metadata, compares the
SD JSON to effective configuration, and requires no new errors or gaps. It also
measures actual cadence and can check a new session after software restart.
Successful verification leaves recording running. Avoid another serial monitor
while the verifier owns the port.
USB console reconnection can restart this board and create another session;
`STATUS` reports the current path.

Logs, CSVs, hashes and summaries go under ignored `Phase02/validation/local/`.
For a bounded status read, the existing console utility also works:

```powershell
rtk python tools/phase01_console.py --port COM12 --command INFO --seconds 5
```

Full histogram acquisition/export, binary append files, RGB/depth co-recording,
physical 1/5/8/11 m characterisation, motion wakeup and power optimisation are
subsequent stages.
