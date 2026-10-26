# ADR-0002: TMF8829 long-range dTOF logging and appendable recording format

- Status: Accepted; depth and histogram acquisition implemented, binary recording pending
- Date: 2026-10-24
- Target: ams OSRAM **TMF8829**, on MIKROE LightRanger 14 Click, connected over the DFR1154 v1.1 Gravity connector
- Related: [ADR-0001: RGB camera-to-SD PoC](0001-rgb-camera-sd-poc.md)
- Initial implementation scope: separate Arduino/C++ sensor PoC, [Phase02](../../Phase02/README.md)

## Staged implementation

The requested first implementation stage discovers the sensor over Gravity I2C
and saves depth CSVs at approximately 1 fps. It implements `DEPTH_ONLY`, including
vendor patch loading, long-range configuration/readback, clock correction, session
metadata and exFAT storage. Full histogram acquisition/export described below is
the following stage; the binary format remains a subsequent storage stage. This
depth-only milestone does not establish full ADR acceptance or the 11 m range.

Hardware discovery confirmed the installed connection uses **SDA GPIO43 / SCL
GPIO44**, at address `0x41`, chip ID/revision `0x9e/0x01`. This supersedes the
proposed pin assignment in the original hardware discussion below. Phase02 uses
the verified orientation first and probes the reverse orientation if absent.

[Phase03](../../Phase03/README.md) implements the full-histogram readout stage:
8×8 long-range T0, T1 and result packets are acquired as validated groups, with
complete native packets retained in a bounded PSRAM cache and CRC-checked host
export. Depth CSVs continue on SD; persistent histogram binary recording remains
the next stage. The requested validation run for this phase is 30–60 measurement
groups, rather than the depth stage's long endurance run. Physical long-range
accuracy and bin-to-distance calibration remain separate checks.

## Context and decision

The sensor referred to as “TMS8829” in the request is understood to be the **TMF8829** already identified for this project. The immediate objective is to acquire long-range **8 x 8 depth frames**, save one simple CSV per frame to the existing exFAT SD card, and support reading the **complete histogram output** as well. A later stage will append depth and histogram records to files spanning approximately one minute or a triggered event.

Use the vendor's **`CMD_LOAD_CFG_8X8_LONG_RANGE`** preset, rather than merely selecting an 8 x 8 grid. The datasheet lists an 11 m maximum for this preset, compared with 5.7 m for its default 8 x 8 preset [1, section 7.3]. Keep dual mode disabled initially. Eleven metres is a validation target, dependent on target reflectivity, illumination, integration settings, and confidence; it is not a guaranteed valid return in every zone. Sampling resolution is not measurement accuracy.

Reuse Phase01's Arduino build approach, SdFat/exFAT storage boundary, USB diagnostics, and monotonic timestamps. Keep acquisition independent of the output writer so CSV can later be replaced by an appendable binary writer. Initially leave the RGB camera, microphone, network, and board IR illumination disabled. The sensor uses its own illumination. Combined RGB/depth capture, motion triggers, and sleep/wakeup are subsequent work.

## Acquisition and hardware

### Gravity I2C connection

Use a dedicated external `TwoWire` bus, separate from the onboard camera/ALS bus on GPIO8/9. Gravity exposes GPIO43/44, power, and ground. Proposed assignment is **SDA = GPIO44, SCL = GPIO43**; confirm that the installed cable/adapter matches these assignments before implementation. Configure the pins explicitly and keep USB CDC for diagnostics rather than using these pins as UART.

The Click board requires 3.3 V logic and appropriate I2C interface jumper settings [2]. Verify its power requirements, pull-ups, shared ground, and Gravity supply capacity. Discover the device at its documented default **7-bit address `0x41`** and verify identity, not just an address acknowledgement [3]. Probe conservatively, then use 400 kHz as the acquisition baseline; enable 1 MHz only after checking sensor, host, wiring, and rise times.

The four-wire connector has no interrupt or enable line. Initially poll sensor status with bounded timeouts; ensure the Click board's EN signal is held in its required enabled state. Do not assume reset or power-cycle control is available over this connector. Additional INT/EN wiring is optional later.

### Driver and configuration

Port the ams OSRAM C core and its platform I/O shim, using the host communication note as the protocol authority [3, 4]. The MIKROE example is another bring-up reference; neither the Linux application nor mikroSDK is assumed to be a drop-in Arduino library. Pin source commits, firmware/RAM-patch bytes and hash, parser/protocol revision, and applicable licences.

Initialisation must verify identity, load the matching RAM patch when required, verify the running application, load the long-range preset, apply the requested period/iteration and output settings, and read back the effective configuration. Follow the vendor FIFO/chunk-read procedure when adapting to `Wire` buffer limits; do not treat a histogram as a small linear register read.

Start at a configurable **1 Hz target for depth-only logging**, with one return per zone. Preserve the preset's optical settings; tune measurement period and iterations using documented controls and measured long-range performance. Read and validate complete native frames, including declared length, type, status, and end marker, before publishing a depth frame. Apply the vendor's clock-correction/validity interpretation and retain its raw inputs [5].

Log requested and achieved rates separately. Use monotonic deadlines or the sensor's configured period, count sequence gaps and overruns, and never create duplicate frames to hide a missed acquisition. Store both host receive time and native sensor timing. Host receive time denotes when readout began, not an exact exposure timestamp. Extend counters in software where needed, while preserving their original values and clock units.

### Depth and full histogram access

Provide two explicit acquisition modes:

| Mode | Behaviour |
| --- | --- |
| `DEPTH_ONLY` — default | Acquire 8 x 8 results and commit one depth CSV per validly parsed frame. Histogram dumping is off. |
| `DEPTH_AND_HISTOGRAM` | Enable histogram dumping, collect every native histogram packet for a measurement and its associated result, and make the complete set available for host readback. Continue producing the corresponding depth CSV. Initially use on-demand capture or a conservative 0.2 Hz target; measure before increasing it. |

“Full histogram” means **all sensor-emitted histogram bins, reference-channel data, subframes, and native headers/footers**, with no peak-only substitution or bin decimation. The standard non-dual 8 x 8 stream has T0 and T1 histogram packets followed by a result. AN001096 describes 25,377 bytes per native histogram packet, including its preheader: 50,754 bytes for the pair [3, section 4.6.5].

For that documented layout, each packet contains four reference histograms of 64 bins and 32 zone histograms of 256 bins; bin counts use packed little-endian 24-bit integers. Retain the source ordering and reference data. **Do not infer the long-range bin timing or layout from an example for another preset.** Confirm the selected firmware's long-range dump layout before decoding it, and retain actual dump lengths and effective histogram settings. The datasheet's internal `histogram_bins` configuration and a protocol's exported-bin layout must not be assumed to be identical [1, 3]. A firmware/layout change requires a new recorded configuration/profile, not silent reinterpretation.

Associate native packets by the documented ordered subframes, configuration, and timing, and assign a host measurement ID to the set. Do not assume all native packet frame numbers are identical. Detect missing, repeated, out-of-order, or aborted packets; never attach a previous measurement's histogram to a newer depth map.

At 400 kHz, the two documented histogram packets alone require at least `50,754 x 9 / 400,000 = 1.142 s` on I2C, excluding addressing, integration, processing, and storage. At 1 MHz the lower bound is 0.457 s. Thus a 1 Hz depth target does **not** imply 1 Hz full-histogram acquisition. Histogram readout also changes sensor flow control; promptly drain its output rather than inserting arbitrary delays between packets [3].

Use bounded buffers, with complete measurement storage in PSRAM if needed. Drain the sensor before formatting CSV or waiting on a host. Retain at least one complete histogram set for explicit host retrieval; report replacement/drop counts if the host does not retrieve it before reuse. USB readback identifies session/measurement/configuration and supplies lengths, CRCs, acknowledgements, and bounded retries, following Phase01's diagnostic approach. Avoid printing every bin as console text. **Persistent histogram recording on SD is deferred to the binary stage**; the first PoC must still demonstrate actual full-histogram acquisition and host export.

## Initial CSV storage contract

Create a new session on each boot/start of an independent recording. Use exclusive creation and never append over another session's files. Example layout:

```text
/dtof/s000001/session.json
/dtof/s000001/config/000001.json
/dtof/s000001/csv/b0000/000000001.csv
/dtof/s000001/csv/b0000/000000002.csv
/dtof/s000001/csv/b0001/000001001.csv
```

Bucket each 1,000 frames; cache the open bucket directory handle rather than repeatedly traversing all earlier filenames. Reserve abandoned temporary names when recovering session numbering. This avoids repeating Phase01's measured single-root-directory latency problem. Depth file numbering is independent of RGB image numbering.

`session.json` identifies the session UUID and initial configuration. Immutable `config/NNNNNN.json` descriptions identify the sensor/board, driver and patch versions/hashes, protocol profile, bus/pins/clock, preset and effective registers, iterations, requested rate, peak count, validity policy, clock/tick units, calibration identity/hash, and pixel orientation. A configuration change gets a new `config_id` and description before any affected frame. Default pixel order is the vendor's 8 x 8 row-major order, `index = y * 8 + x`; verify physical orientation on the bench. These are line-of-sight ranges, not a calibrated camera-aligned Z map.

Each CSV is UTF-8 with LF newlines, decimal point `.`, and millimetres. Use comment metadata and a normal column header; for example:

```csv
# dtof_csv_version=1
# session_id=550e8400-e29b-41d4-a716-446655440000
# config_id=1
# frame_seq=1
# sensor_frame_no=123
# host_rx_us=12345678
# sensor_tick_raw=987654
# clock_ratio_uq15=32768
# frame_status_raw=1
# histogram_expected=0
# histogram_received=0
# histogram_complete=0
# width=8,height=8,peaks=1
x,y,distance_mm,distance_raw_qmm,snr_raw,valid
0,0,10987.250,43949,55,1
1,0,,0,0,0
```

The file contains **64 data rows**, one per zone, with `x,y` in `0..7`; the example only illustrates two rows. This is a simple tabular representation of the 8 x 8 map, easily reshaped using `x,y` (e.g. a CSV reader that ignores `#` lines). For a later multi-return configuration, add a `peak` column and emit `64 x peaks` rows under a new CSV schema version.

`distance_raw_qmm` preserves the source quarter-millimetre code. `distance_mm` is the vendor-corrected result when the return is valid; the example uses a unity correction factor. Retain the raw SNR/confidence byte and use the vendor interpretation plus the recorded validity policy. An invalid return has `valid=0` and an empty `distance_mm` field, never a fabricated zero, maximum-range value, or last-known distance. Missing correction/timing values are explicitly marked unknown in metadata. Preserve frame status even when all returns are invalid; structurally corrupt frames are rejected and counted rather than published as complete CSVs.

Write `NNNNNNNNN.tmp` exclusively, check byte count, sync and close, then rename without replacing an existing CSV. Commit metadata before dependent frames. Stop recording on an SD error/full card; never auto-format or delete earlier data. Log frame ID, valid-zone count, sensor status, histogram completeness, byte counts, transfer/write duration, and loss counters. Partial histogram readout does not invalidate an independently valid depth result, but must be labelled incomplete.

## Suggested binary format: DTOFLOG version 1

This is the proposed **later-stage** on-disk contract, not an implementation requirement for the first CSV PoC. Use an uncompressed, append-only stream of typed, length-delimited records. A recorder can write each histogram packet as it arrives, then the result and decoded depth; it need not hold an entire minute in RAM. Keep raw native result and histogram frames alongside the compact depth representation so offline decoding remains reproducible.

All multibyte integers are **little-endian**. `uN` and `iN` mean unsigned and signed N-bit integers. Offsets are in bytes. Serialize fields explicitly; do not dump C/C++ structs, pointers, compiler padding, or host-endian memory. There is **no implicit alignment or padding**.

### File header: 64 bytes

| Offset | Type / size | Field |
| --- | --- | --- |
| 0 | 8 bytes | ASCII magic `DTOFLOG1` |
| 8 | u16 | Format major = 1 |
| 10 | u16 | Format minor = 0 |
| 12 | u32 | Header bytes = 64 |
| 16 | 16 bytes | Session UUID, opaque bytes in standard UUID byte order |
| 32 | u64 | Segment index within the session, starting at 1 |
| 40 | u64 | Segment start time, host monotonic microseconds since boot |
| 48 | i64 | Corresponding UTC Unix microseconds, or -1 if unknown |
| 56 | u32 | Flags: bit 0 UTC valid, bit 1 event-associated; other bits zero |
| 60 | u32 | CRC32 of header bytes 0 through 59 |

Each segment starts with a `CONFIG` record defining the active configuration and clock domain. That record duplicates the essential session metadata, so each file can be decoded independently of `session.json`. A reboot creates a new session/clock domain. UTC is optional and must never be invented from an unsynchronised boot clock.

### Record framing: 48-byte header, payload, 4-byte trailer

| Offset | Type / size | Field |
| --- | --- | --- |
| 0 | 4 bytes | ASCII record magic `DTR1` |
| 4 | u16 | Record type |
| 6 | u16 | Flags; for `DEPTH`: bit 0 measurement group complete, bit 1 sensor frame valid, bit 2 clock correction valid; otherwise zero in v1 |
| 8 | u32 | Record header bytes = 48 |
| 12 | u32 | Payload bytes, excluding header/trailer |
| 16 | u64 | Record sequence, strictly increasing across session segments |
| 24 | u64 | Host measurement sequence shared by the related depth/result/histogram records; 0 for nonmeasurement records |
| 32 | u64 | Host monotonic receive time in microseconds; event time for metadata/events |
| 40 | u32 | Configuration ID; 0 only where no sensor configuration applies |
| 44 | u32 | Header CRC32 over bytes 0 through 43 |
| 48 | variable | Payload |
| 48 + payload bytes | u32 | CRC32 over the payload only |

The next record starts at `52 + payload_bytes` from this record's start. CRC32 is **CRC-32/ISO-HDLC**, reflected polynomial `0xEDB88320`, initial value and final XOR `0xFFFFFFFF`; `123456789` produces `0xCBF43926`, and an empty payload produces 0. CRC detects corruption, not malicious modification.

Readers validate the header CRC before trusting a length, impose a v1 payload limit of **1 MiB**, and check the payload CRC. Unknown record types may be skipped by their validated lengths. Unknown major versions are rejected; minor versions may add optional record types without changing existing layouts. Header sizes other than the defined v1 sizes require an explicitly supported extension. Reserved bits are written zero.

### Record types and payloads

| Type | Name | Payload / purpose |
| --- | --- | --- |
| 1 | `CONFIG` | UTF-8 JSON, without BOM or trailing NUL; schema `dtof_config_v1`. Includes sensor/patch/parser identities and hashes, long-range preset and effective registers, dimensions/peaks, histogram dump profile and expected subframes, time units, orientation, validity/correction policy, calibration identity, and any active event ID. |
| 2 | `DEPTH` | Fixed prefix plus packed per-return values, defined below. Written last for its measurement group. |
| 3 | `RAW_RESULT` | Exact native result frame plus a small wrapper, defined below; written for every binary-recorded measurement. |
| 4 | `HISTOGRAM` | Exact native histogram frame with the same wrapper; one record per packet, preserving T0/T1 and any future additional phases. |
| 5 / 6 | `EVENT_START` / `EVENT_END` | UTF-8 JSON with schema `dtof_event_v1`: event UUID, trigger source/reason, monotonic time, and continuation context as applicable. |
| 7 | `GAP` | UTF-8 JSON with schema `dtof_gap_v1`: reason, relevant measurement IDs/counter range, known lost count or null if unknown. Covers sensor/FIFO gaps, host queue loss, and incomplete histogram sets. |
| 8 | `END` | UTF-8 JSON with schema `dtof_end_v1`: close reason, record/group counts, last sequence, and loss counters. Written on every clean close; not required to recover earlier records after interruption. |

For `DEPTH`, the first **20 payload bytes** are:

| Payload offset | Type | Field |
| --- | --- | --- |
| 0 / 2 | u16 / u16 | Width / height, initially 8 / 8 |
| 4 / 5 | u8 / u8 | Peaks per pixel, 1..4 / distance encoding = 1, native quarter-millimetre codes |
| 6 | u16 | Bytes per return = 4 |
| 8 | u32 | Native result-frame number |
| 12 | u32 | Native sensor tick, unchanged; wrap/units described by `CONFIG` |
| 16 | u16 | Clock correction ratio in unsigned Q1.15; 32768 is unity; ignore unless the record's correction-valid flag is set |
| 18 | u16 | Native result footer status bytes, packed little-endian |

Follow with `width x height x peaks` four-byte entries, in `y`, then `x`, then native peak order: **`u16 distance_raw_qmm; u8 snr_raw; u8 flags`**. Entry flag bit 0 denotes a valid return; the other bits are zero. Reserve distance `0xFFFF` for unavailable data; never infer validity from range alone. `RAW_RESULT` retains the original code even if normalisation marks it unavailable. For valid data, derive millimetres as `distance_raw_qmm / 4`, applying the recorded ratio `/ 32768` when available. Do not claim 0.25 mm accuracy from the encoding.

Raw distances and native packet bytes are captured **before host-side clock correction** or any in-place changes by the adapter/parser. Apply the ratio exactly once when deriving corrected distances; copying an already corrected driver result into a raw field would produce an incorrect second correction.

Thus an 8 x 8 single-return depth payload is `20 + 64 x 4 = 276` bytes, or **328 bytes including record framing**. At 1 Hz this is 19,680 bytes per minute, before configuration, raw results, and other records.

For `RAW_RESULT` and `HISTOGRAM`, the payload begins **`u16 wire_profile; u16 reserved=0; u32 native_bytes`**, followed by exactly `native_bytes` bytes. Profile 1 denotes the TMF8829 native FIFO frame envelope documented by AN001096 v3: retain its preheader, header, data, and footer unchanged. `CONFIG` identifies the exact firmware/parser/profile revision. Enforce `payload_bytes = 8 + native_bytes`; preserve native subframe IDs and timestamps instead of replacing them with guessed zone/bin metadata. Decode packed 24-bit bins on the host without widening or quantising the stored source. A different envelope requires a new profile.

In histogram mode, a typical complete group is:

```text
HISTOGRAM(T0, measurement=42)
HISTOGRAM(T1, measurement=42)
RAW_RESULT(measurement=42)
DEPTH(measurement=42, group_complete=1)
```

The native packet counters remain inside their raw payloads; `measurement=42` is the host association, not an assertion that those native counters match. With missing histogram packets, retain trustworthy received records, write a `GAP`, and leave the depth group's complete bit clear. Do not discard a valid depth map solely because its histogram set is incomplete. In depth-only mode, a validated result and its decoded depth form the complete group.

For the documented two-packet histogram set, records occupy `2 x (48 + 8 + 25377 + 4) = 50,874` bytes per measurement, excluding result/depth/configuration. At 1 Hz this is roughly 3.05 MB per minute; at 0.2 Hz, roughly 0.61 MB. These storage estimates do not override the I2C transfer-time limit.

### Rotation, append, and recovery

Create segments exclusively, for example `/dtof/s000001/bin/b0000/000000001.part`; append `END`, sync and close successfully before renaming to `.dtf`. Bucket segment files as well as CSV files. Default rotation is 60 seconds of host monotonic acquisition time, using a group's first host FIFO-read timestamp for segment membership. Rotate **between measurement groups**, before the next group starting at or after that boundary; do not split a complete group merely to hit an exact second. Record actual duration. A configuration change also closes the segment and starts a new one with its own `CONFIG`.

Event recording uses the same format: open on a trigger and close on event end/inactivity. Keep a configurable 60-second segment cap even during long events. Persist a common event UUID and active-event context in continuation segments. Pre-trigger buffering and automatic motion classification are future policies; this format supports manual/external events without defining their detector.

Append header/payload/trailer in order, check all byte counts, and sync after each completed measurement group by default. Use one SD writer and bounded acquisition buffers; measure sync latency and backpressure. Any later batching policy must record its durability interval. No total record count or end offset needs rewriting in the file header. Do not preallocate in v1; if introduced later, distinguish allocated bytes from committed data and truncate on clean close.

On interruption, keep the `.part` file. An offline reader accepts only the consecutive prefix with a valid file header and complete, CRC-valid records; it stops at the first incomplete/corrupt record and reports the valid byte offset and incomplete measurement groups. Do not blindly search payload bytes for another magic string, or treat a truncated histogram as complete. Never append new data after a damaged tail: start a new segment/session and preserve the original for inspection. A missing `END` means unclean close, not that all preceding records are unusable. CRCs and sync do not make exFAT transactional or guarantee SD-card survival of power loss.

## Stages, alternatives, and acceptance

1. **Sensor bring-up and CSV:** prove identity/patch loading and the actual Gravity wiring; read back the long-range configuration; acquire and export coherent 8 x 8 maps with validity and timing; save one CSV per frame.
2. **Full histogram readout:** prove complete long-range histogram acquisition, mapping/reference preservation, host download integrity, measured transfer rate, and correct depth association. This read capability is part of the PoC; persistent histogram SD files are not.
3. **Binary recording:** implement the v1 writer and an independent desktop reader/exporter; add minute and manually triggered event segments. Keep CSV as an optional export/debug path rather than the primary sustained histogram format.
4. **Later integration:** coordinate camera/depth timestamps and storage scheduling; then evaluate event detection and power/wakeup under separate decisions.

CSV is selected first for inspection and tooling simplicity. Serial-only logging would not meet SD persistence. Histogram CSV/JSON would substantially expand traffic and files. Native frame blobs alone preserve evidence but lack stable session/configuration association and convenient depth access. HDF5, databases, and indexed multimedia containers add dependencies and metadata-update complexity; the proposed record stream covers this PoC's append/recovery needs with a small writer and sequential reader.

Before declaring the respective stage complete:

- Record effective preset/iterations/period, calibration, firmware versions, bus clock, achieved cadence, per-stage latency, and all gap/drop/error counts. Distinguish configured acquisition rate from durable logged rate and histogram export rate.
- Test known targets at approximately 1, 5, 8, and 11 m, documenting lighting, reflectivity, geometry, and invalid zones. Verify no-object/low-confidence handling and that distance is not clipped or wrapped at long range. Calibrate orientation using a target moved across the field of view.
- Run depth-only logging for at least ten minutes at the selected sustainable rate. Independently parse every CSV, verify 64 unique zone coordinates, units, missing values, sequences, and timestamps; exercise restart numbering and a bucket boundary without overwriting existing RGB or dTOF data.
- In histogram mode, compare a complete exported set with the source packet bytes/CRCs, confirm reference channels and all subframes, check selected-mode bin interpretation, and report measured I2C throughput. Inject a missing/out-of-order packet and require an incomplete flag/gap, never cross-frame substitution.
- For binary v1, use golden byte-layout and CRC vectors, round-trip CSV/depth and raw histograms, skip an unknown optional record, and reject bad lengths/versions. Test truncated headers/payloads/trailers, CRC damage, unclean close, rotation across a measurement/event boundary, continuation metadata, queue overflow, and full/removed media. Demonstrate prefix recovery without modifying the original recording.

## References

Consulted 2026-10-24. Datasheet/protocol versions must be matched to the firmware actually loaded during implementation.

1. [ams OSRAM TMF8829 datasheet, DS001140 v2-00, 2025-11-12](https://look.ams-osram.com/m/4215c1ea1a30e283/original/TMF8829-Time-of-flight-sensor.pdf), especially sections 7.1–7.4 and Table 6.
2. [MIKROE LightRanger 14 Click: board, logic levels, interface selection, and pinout](https://www.mikroe.com/lightranger-14-click).
3. [TMF8829 Host driver communication, AN001096 v3-00, 2026-02-17](https://look.ams-osram.com/m/4e718be661792a2b/original/TMF8829-Host-driver-communication.pdf), particularly startup, FIFO readout, and section 4.6 frame/histogram layouts.
4. [ams OSRAM TMF8829 userspace C driver and porting reference](https://github.com/ams-OSRAM/tmf8829_driver_linux_userspace), and [kernel-driver configuration reference](https://github.com/ams-OSRAM/tmf8829_driver_linux/blob/main/README.md) for preset identifiers.
5. [Vendor frame parser, units, and correction implementation](https://github.com/ams-OSRAM/tmf8829_driver_linux_userspace/blob/main/tmf8829_frameparser.c).
