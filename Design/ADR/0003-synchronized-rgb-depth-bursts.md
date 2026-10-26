# ADR-0003: Synchronized QXGA RGB and long-range depth bursts

- Status: Accepted for Phase04 PoC
- Date: 2026-10-26
- Related: [RGB recording](0001-rgb-camera-sd-poc.md),
  [TMF8829 acquisition and future binary format](0002-tmf8829-dtof-logging-poc.md)
- Implementation: [Phase04](../../Phase04/README.md)

## Context and decision

Capture paired OV3660 QXGA JPEGs and TMF8829 long-range 8×8 depth maps for
30 seconds, then enter ESP deep sleep for 10 seconds and repeat. Histograms are
disabled for this phase, as requested. GPIO3 is high throughout the active burst
and low during initialization, shutdown and sleep. Board IR illumination stays
off. The dTOF sensor uses its own illumination.

Use one exclusively allocated SD folder per burst. Keep JPEG and depth CSV files
easy to inspect, with a manifest binding their sequence, checksums and timing.
Retain each complete native depth result as a small audit file. Defer the binary
record stream in ADR-0002 until histogram persistence or sustained recording
requires it.

Choose JPEG quality **6** on the driver's scale (lower means higher quality),
at **2048×1536**. Default to **1 paired fps**: measured committed JPEG writes
exceeded the 500 ms budget at 2 fps. Keep the configured rate between 1 and
10 fps and never select two RGB frames less than 100 ms apart. Raising the
configured rate also tightens synchronization acceptance; it does not establish
that the board can sustain that rate at this quality. Preserve the long-range
preset `0x41`, 586 kilo-iterations, one return per zone and dual mode disabled.
The nominal 11 m range remains a separate physical validation target.

## Acquisition and synchronization

The four-wire Gravity connector provides no shared exposure trigger. Independently
acquire RGB and depth, retaining sensor-native timing and host monotonic microseconds
from the same ESP clock. Use dedicated reader tasks and bounded buffers: RGB on
core 1, depth on core 0, a four-frame PSRAM RGB ring and a 16-frame depth queue.
One foreground writer owns SD. Keep depth draining during SD setup. A gap, queue
drop, I/O error or buffer failure is visible; a buffer is never filled with a
duplicate frame to conceal lost data.

Camera SCCB owns hardware I2C controller **1** on GPIO8/9. External Gravity I2C
uses controller **0**, SDA43/SCL44, address `0x41`, at 400 kHz. On every wake,
explicitly enter the TMF8829 boot monitor and reload/verify its RAM patch and
configuration. Reading identity before powering the sensor out of standby returns
zeroes and cannot establish its absence.

Observe the documented 3 ms interval between ENABLE power-up selection and
soft reset. Compare the effective configuration after writing it, with one
explicit exception: undocumented byte `0x2c` was observed to normalize to `1`
after standby. Log that normalization and retain actual readback; continue to
require agreement for the remaining page, including every requested setting.

The acceptance tolerance is **half the configured paired-frame period**: 500 ms
at the default 1 fps. Apply it to conservative optical-time envelopes:

1. Settle camera auto exposure, freeze its current exposure at no more than VTS
   minus four lines, and disable night-mode frame extension. Read back manual
   exposure and night-mode control. Probe the native frame period; use its maximum
   plus 10% and 2 ms as an initial bound. Expand a frame's bound if its observed
   interframe interval is longer. RGB envelope: **[first-DMA time − period bound,
   frame-buffer return time]**. The extra period includes exposure before DMA and
   the return time includes rolling readout and buffering.
2. Bracket the depth preheader read with host timestamps. Map the unsigned,
   wrap-aware difference **SYSTICK − t0** from the 125 kHz sensor clock using the
   vendor's UQ15 clock ratio. Subtract that age and a margin of **0.5% of age +
   1 ms** from host read start. Use header-read end as the upper bound. The footer
   `t0` is the first integration start; `t1` is the last integration start, **not
   exposure end**. Warm up four results before recording; reject stale/invalid
   tick ordering. Preserve the raw ticks, ratio and transaction bracket.
3. For RGB interval `[r0,r1]` and depth interval `[d0,d1]`, require
   **max(|r0−d1|, |r1−d0|) ≤ period/2**. Select the best available unused RGB
   frame satisfying this bound; otherwise count a synchronization rejection.
   Both complete envelopes must lie inside the 30-second recording window.

This is a software synchronization contract based on the driver timestamp,
bounded sensor exposure and clock mapping assumptions above. An independent host
reader recomputes every bound. Metadata validation is not an external optical
timing measurement; an optical strobe/target test would be needed to calibrate
absolute timing or establish a tighter physical guarantee. There is no claim of
simultaneous shutters, pixel-level registration or proven 10 fps operation.

## SD layout and commit rules

```text
/bursts/b000001/
  burst.json           # schema, UUID, configuration, clocks and planned start
  pairs.csv            # committed pair index; pairs.part while recording
  000001.jpg           # QXGA RGB
  000001.csv           # 64 zones; Phase02 CSV schema, same burst UUID/sequence
  000001.tof           # complete 225-byte native depth result
  ...
  summary.json         # actual recording times, counters and shutdown outcome
```

Continue after the highest allocated folder without reusing existing recordings.
Each burst has a random UUID and its own sequence beginning at 1. Use SdFat 2.3.1
with exFAT or FAT32; the installed 128 GB exFAT card needs no reformatting. SD uses
the board's FSPI GPIO10/11/12/13 at a configured 40 MHz. Verify original-capture
CRCs against full SD readback after changing bus speed.

Create each data file exclusively as `.tmp`, check writes, sync and close, then
rename to its final name. Append/sync its manifest row only after all three files
commit. The manifest stores sequence, native depth number/ticks, camera DMA and
return times, period bound, host I2C bracket, UQ15 ratio, both optical envelopes,
midpoint offset, worst separation, tolerance, sizes and CRC-32/ISO-HDLC values
(check vector `123456789` → `cbf43926`).

On clean finish, sync/rename the manifest to `pairs.csv` and write the summary.
Actual `capture_start_us`/`capture_stop_us` in the summary are authoritative;
`planned_start_us` in configuration can precede the completed SD setup. After
power loss, a `.part` manifest or orphan `.tmp`/data files remain diagnostic
evidence. A consumer accepts only valid rows whose files/checksums agree, and
requires a successful summary for a complete burst. Never auto-delete or append
to an interrupted burst. Sync/rename does not make exFAT transactional.

## Sleep, diagnostics and acceptance

Stop acquisition tasks, place TMF8829 in standby, deinitialize the camera, finish
and close SD, hold GPIO3 low, then arm the 10-second deep-sleep timer. Warmup,
checked shutdown and a two-second USB diagnostic grace period add overhead to
the 30-second recording and 10-second sleep. SD completion can briefly extend
the active LED interval; no selected exposure extends beyond the recording window.
The depth sensor remains powered in standby because Gravity provides no EN line.
Motion wake and measured power optimization remain later work.

USB `HOLD` prevents sleep for inspection; `ALLOW_SLEEP` restores cycling.
`STATUS`, `STOP`, `REBOOT`, `SLEEP seconds`, and acknowledged per-file `GET`
support bounded hardware tests. Preserve existing recordings across timer wake.

Acceptance uses one 30-second burst, full JPEG decode and CSV/native/checksum
comparison for every saved pair, independent timestamp-bound recomputation,
zero gaps/drops/reader errors, verified LED states, then 10-second timer wake and
a second successful burst with a new folder. Re-read an earlier JPEG to verify
preservation. Document actual rate, sync bound, configuration, binary hash and
limitations in the Phase04 validation report. A long endurance run is unnecessary
for this burst PoC.

## References

- [TMF8829 host protocol AN001096 v3-00](https://look.ams-osram.com/m/4e718be661792a2b/original/TMF8829-Host-driver-communication.pdf),
  especially startup, SYSTICK sampling and result footer fields.
- [TMF8829 datasheet DS001140 v2-00](https://look.ams-osram.com/m/4215c1ea1a30e283/original/TMF8829-Time-of-flight-sensor.pdf).
- [esp32-camera v2.1.3](https://github.com/espressif/esp32-camera/tree/v2.1.3),
  frame timestamp contract, `cam_hal.c` and OV3660 exposure controls.
- [Source provenance](../../Phase04/UPSTREAM.md).
