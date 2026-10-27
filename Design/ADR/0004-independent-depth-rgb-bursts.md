# ADR-0004: Independent 10 fps depth and 1 fps RGB burst recording

- Status: Accepted for Phase05 PoC
- Date: 2026-10-27
- Implementation: [Phase05](../../Phase05/README.md)
- Related: [dTOF acquisition](0002-tmf8829-dtof-logging-poc.md),
  [Phase04 burst recording](0003-synchronized-rgb-depth-bursts.md)

## Decision

Record independent TMF8829 **8×8 long-range depth at nominal 10 fps** and OV3660
**QXGA JPEG quality 6 at 1 fps**, using a shared ESP monotonic microsecond clock.
Capture for 30 seconds, commit the burst, then enter 10 seconds of timer-driven
deep sleep and repeat. GPIO3 is solid high during the acquisition window, low
after its deadline and throughout storage finalization and sleep. Keep histograms,
dual mode and board IR disabled.

The requested 2000k iteration preference was tested with the camera running:
24 results, with the final 20 used for cadence, measured **189,368 µs/frame**
(about **5.28 fps**) at a requested 100 ms period. Use **1000 kilo-iterations**
and period **100 ms** as the default to preserve nominal 10 fps. Retain the
manufacturer's long-range preset `0x41` and its optical settings. One register
iteration unit is 1024 iterations. Read back the actual configuration and verify
cadence before each burst; a measured mean outside 95–105 ms fails startup.

This follows the sensor's documented behavior: when ranging active time exceeds
the requested period, active time determines cadence. Increasing iterations
improves the integration budget but cannot preserve 10 fps indefinitely. Physical
11 m performance with this integration setting still requires target/range testing.

## Acquisition, buffering and storage

Reuse the Phase04 camera setup, exposure bound, native result parser, clock
mapping, exFAT writer and deterministic standby/reset sequence. Camera SCCB owns
I2C controller 1, GPIO8/9. Gravity uses controller 0, SDA43/SCL44, address `0x41`,
at 400 kHz. Require 8×8A focal-plane mode, one distance/SNR return per zone.
The undocumented configuration byte `0x2c` may normalize on write after standby;
log it, retain actual readback and check the remaining page exactly.

The core-0 dTOF task continuously drains the FIFO into a **384-frame PSRAM
archive**. Only that task writes it, and the storage writer reads it after joining
the task. Overflow, malformed packets, nonincreasing native counters and frame
gaps are explicit errors. The depth task observes its own 30-second deadline,
independently of the foreground SD writer, and turns the LED off at that deadline.

The core-1 camera task continuously drains native JPEG frames into a four-slot
PSRAM ring. The writer selects one complete RGB frame for each absolute one-second
slot and commits it to SD. Retain quality 6 and QXGA; count missed slots instead of
silently reducing quality or reusing a frame. Preserve native DMA and return times
and conservative optical-time envelopes for both streams. Only complete envelopes
inside the burst window are saved, so boundary frames can be omitted.

After acquisition stops, put dTOF in standby, stop the camera/XCLK, and stream the
small depth archive to one CSV and one binary file. This avoids 10 per-frame
create/sync/rename operations each second while a JPEG is being written. The
archive uses approximately 0.24 MB of PSRAM. **Depth is durable only after the
end-of-burst commit:** interruption during capture loses the RAM archive. This is
an explicit PoC tradeoff, recorded in metadata; periodic depth checkpoints or an
appendable writer remain later options.

Each unused folder `/phase05/bNNNNNN/` contains:

| File | Contents |
| --- | --- |
| `burst.json` | Random burst UUID, boot/wake information, configuration, clock and durability policy |
| `000001.jpg` … | Independently numbered QXGA JPEGs |
| `rgb.csv` | Per-image sequence, scheduled time, DMA/return times, optical interval, size and capture CRC |
| `depth.csv` | One row per depth frame, all timing inputs and 64 raw/corrected distance, SNR and validity tuples |
| `depth.bin` | Versioned fixed records containing the full native results and timestamp evidence |
| `summary.json` | Actual acquisition window/stop, counts/errors, file sizes/CRCs and shutdown outcome |

Create files exclusively through checked `.tmp` writes; the RGB manifest stays
`rgb.part` until close. Sync before renaming. The summary is committed last. A
consumer requires a successful complete summary and matching checksums. Preserve
interrupted folders and existing recordings. exFAT sync/rename is not transactional.

## Timestamp association

All host times use `esp_timer_get_time()` microseconds since the current ESP boot.
Depth stores SYSTICK, first/last integration-start ticks, host header-read bracket,
UQ15 clock ratio and the mapped optical interval. RGB stores the first-DMA and
frame-buffer return timestamps, observed period bound and optical interval.
Camera exposure is settled and frozen below VTS with night-mode extension off.

The independent streams have separate sequences. Match within the **burst UUID**;
do not compare bare timestamps across deep-sleep boots, where the clock restarts.
The host reassembly utility verifies native/CSV/manifest checksums and emits each
RGB frame's nearest depth frame by optical-interval midpoint, including timestamp
difference and interval widths. Retain all depth maps for interpolation or other
policies. Midpoint association is not simultaneous exposure, geometric registration
or an externally calibrated optical timing guarantee.

## `D5TOF1` binary format, version 1

This phase-specific native audit format is distinct from the future appendable
`DTF` record stream proposed in ADR-0002. It has a **64-byte header**, followed by
exactly `frame_count` **288-byte records**, all little-endian. It is finalized
from the complete RAM archive; no in-place count update is required.

| Header offset | Type/bytes | Meaning |
| --- | --- | --- |
| 0 | 8 bytes | `D5TOF1\0\0` |
| 8 / 10 | u16 / u16 | Version `1` / header bytes `64` |
| 12 / 16 / 20 | u32 each | Record bytes `288` / frame count / kilo-iterations |
| 24 / 32 | u64 each | Capture-window start / end, ESP microseconds |
| 40 | 16 bytes | UUID in RFC byte order |
| 56 | u32 | Reserved, zero |
| 60 | u32 | CRC-32 of header bytes 0–59 |

| Record offset | Type/bytes | Meaning |
| --- | --- | --- |
| 0 / 4 | u32 each | Archive sequence / native sensor frame number |
| 8 / 16 | u64 each | Host header-read start / end |
| 24 / 32 | u64 each | Mapped optical earliest / latest time |
| 40 / 44 / 48 | u32 each | Native SYSTICK / t0 / t1 |
| 52 / 54 / 55 | u16 / u8 / u8 | UQ15 ratio / native frame status / reserved zero |
| 56 | 225 bytes | Complete native preheader, header, 64 zones and footer |
| 281 | 3 bytes | Zero padding |
| 284 | u32 | CRC-32 of record bytes 0–283 |

Use CRC-32/ISO-HDLC, check vector `123456789` → `cbf43926`, for headers, records
and original-commit file checksums. Reject unsupported versions/sizes, nonzero
reserved bytes, truncated tails, bad CRCs and raw/native/timestamp disagreement.
All distances in the wide CSV are **quarter-millimetre integers**; divide corrected
`distance_qmm_NN` by four for millimetres. Invalid corrected distances are blank;
raw distance, SNR, status and `valid_NN` are preserved. Zone `NN` maps to
`x=NN%8`, `y=NN//8`, without rotation or RGB calibration.

## Validation and limits

Validate one complete 30-second burst by downloading and decoding every RGB JPEG,
comparing all native depth results to CSV values, independently recomputing optical
intervals, and checking source CRCs, rates, counts, overflow/gap/error counters and
LED states. Verify 10-second timer wake and a second complete burst in a new folder,
including all its depth data and preservation of an earlier JPEG. Restore automatic
cycling after success. Unit tests cover byte layout, corruption/truncation,
native association, clock wrap, CSV agreement and offline timestamp matching.

Warmup, checked storage finalization and the two-second diagnostic grace interval
add overhead to the 30-second acquisition and 10-second sleep. The camera rail
remains powered; `camera_off` denotes deinitialization/XCLK stop. Current
consumption and motion wake remain later work.

Reference: [TMF8829 datasheet DS001140 v2-00](https://look.ams-osram.com/m/4215c1ea1a30e283/original/TMF8829-Time-of-flight-sensor.pdf),
section 7.4; [host protocol AN001096 v3-00](https://look.ams-osram.com/m/4e718be661792a2b/original/TMF8829-Host-driver-communication.pdf).
