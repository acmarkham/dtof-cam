# ADR-0001: Arduino RGB camera-to-SD proof of concept

- Status: Accepted
- Date: 2026-10-22
- Target: DFRobot ESP32-S3 AI Camera, DFR1154 hardware v1.1
- Storage target: 128 GB microSDXC card formatted as exFAT
- Implementation: [Phase01](../../Phase01/README.md), with [hardware validation](../../Phase01/validation/README.md).

## Context

Establish a small, observable firmware baseline that saves one colour image per second to the onboard SD card and flashes the GPIO3 LED during acquisition. Subsequent stages will add ranging, ambient light, infrared illumination, audio recording, and reduced power operation.

The repository currently contains no Arduino/C++ source. Use DFRobot's existing **Timed camera** Arduino example as the implementation skeleton [1, 2]. It already provides camera initialisation, JPEG capture, SD storage, and LED control. Its capture-on-boot, 25-second deep-sleep cycle, volatile image counter, and storage backend need replacing for this PoC. When importing it, record the upstream commit, file path, attribution, and applicable licence alongside the firmware.

## Decision

Implement an Arduino/C++ sketch using `setup()` for initialisation and `loop()` for a single capture-and-save operation every 1,000 ms. Keep the camera initialised between captures. Phase 1 has no networking, audio capture, sensor dependency, or sleep cycle.

### Frame format and cadence

- Interpret RGB as a colour photograph. Configure the camera for sensor-generated JPEG (`PIXFORMAT_JPEG`) and write those bytes directly to `.jpg`; an intermediate RGB888 buffer and software JPEG encoder are unnecessary.
- Initially validated at VGA (640 x 480). As of 2026-10-24, select the OV3660's maximum QXGA (2048 x 1536) at the user's request, retaining JPEG quality setting 10, PSRAM frame buffers, and the example's latest-frame approach. FHD (1920 x 1080) is the lower-resolution 16:9 alternative. Keep resolution, quality, and interval in one configuration section so later measurements can justify changes. Confirm saved JPEG dimensions and remeasure cadence after changing modes.
- Enable PSRAM and use the board settings recommended by DFRobot. Record the tested Arduino-ESP32 core version, libraries, and board options when implementing; initialise configuration structures explicitly and adapt API names to that version.
- Schedule against a monotonic deadline, rather than sleeping for a second after each write. The nominal interval is between acquisition requests, not between completed file writes. Allow camera auto-exposure to settle before starting the measured sequence.
- If capture plus storage exceeds one second, log the overrun and skip elapsed deadlines. Resume on the next future deadline without a burst of catch-up captures. Report achieved cadence; sustained overruns fail the 1 fps acceptance criterion.

With continuous camera buffers, the saved image is the latest available frame; requesting a frame is not an exact exposure trigger. Check for stale/repeated frames with a changing scene. Hardware exposure synchronisation is outside this PoC.

### Board resources

Centralise pin definitions in a board configuration header, using the vendor mapping and checking against the v1.1 schematic before hardware bring-up [1, 3, 4].

| Resource | Phase 1 choice / reservation |
| --- | --- |
| Camera | Reuse the DFRobot camera mapping; retain the onboard camera/ALS control bus on GPIO8/9 and keep external Gravity I2C separate. |
| SD | Use SdFat v2 `SdFs` over the onboard SPI wiring; SD chip select remains GPIO10. Replace the skeleton's Arduino `SD` backend for exFAT support. |
| Acquisition indicator | GPIO3 onboard LED; example drives HIGH for on and LOW for off. |
| IR illumination | GPIO47; initialise off and reserve for a later stage. |
| Ambient light | Board already includes an LTR-308; reuse it in a later stage. |
| Gravity extension | GPIO43 and GPIO44 are exposed; v1.1 supplies 3.3 V on Gravity `+`. Reserve these for external I2C. |
| Microphone | Onboard PDM microphone; preserve its pins and peripheral resources for later recording. |

Flash GPIO3 for every acquisition attempt: turn it on immediately before requesting a frame, and off after the operation, with a minimum visible pulse of approximately 100 ms. Service the pulse deadline without adding a blocking delay to the capture schedule. Ensure all failure paths clear the LED. The pulse indicates activity; serial output reports whether storage succeeded.

### 128 GB card and exFAT backend

Target the user's 128 GB microSDXC card with an exFAT volume covering its usable capacity. DFRobot documents up to 32 GB FAT32 [3]; 128 GB/exFAT is therefore a project validation target beyond that documented configuration, with hardware compatibility to be established on the actual card and board.

Use **SdFat v2**, which supports FAT16/FAT32 and exFAT [7]. Pin a tested release during implementation (initial candidate: 2.3.1). Use explicit `SdFs` and `FsFile` types to support exFAT and retain FAT32 as an optional compatibility mode. Keep these types inside the storage module. Replace the skeleton's `SD.h`, `SD.begin()`, `fs::FS`, and `File` operations; SdFat is not a drop-in implementation of Arduino-ESP32's `fs::FS` interface.

Initialise the SPI instance with the verified onboard SCK/MISO/MOSI pins, then mount through `SdSpiConfig` with GPIO10 as CS and that explicit SPI instance. Start with a conservative 10 MHz transfer ceiling and measure before increasing it. Use dedicated SPI mode only after confirming that no other device shares that bus. Do not mount the same card simultaneously through Arduino `SD` and SdFat.

Prepare the card on a computer using a single MBR partition containing exFAT and the formatter's default allocation unit size. Preserve a suitable existing format. Firmware must never automatically format on mount failure. Log card identity, filesystem type, usable capacity, and free space at startup; reject unexpected filesystems with a clear error. Use 64-bit arithmetic for capacity, free-space bytes, and file offsets. The primary acceptance target must mount as exFAT; FAT32 compatibility does not satisfy it.

Retaining Arduino `SD` with a custom ESP-IDF build is an alternative only after checking exFAT support in that exact core/IDF version and its build configuration [8]. Adding a sketch-level define cannot enable functionality missing from prebuilt core libraries. SdFat keeps this PoC independent of that rebuild.

### Sequence naming and storage

Store files in the SD root, starting with `/000001.jpg`, then `/000002.jpg`, and so on. The stamp is a sequence number, not a wall-clock timestamp; no RTC or network time is required.

1. At startup, scan six-digit `.jpg` and `.tmp` names and choose one more than the highest allocated number. Start at 1 on an empty card. Existing images and abandoned temporary files reserve their numbers.
2. Create the temporary file with exclusive-create semantics (`O_WRONLY | O_CREAT | O_EXCL`) and use a rename operation that rejects an existing destination. Never append to or overwrite an existing image. Keep one storage writer in Phase 1. These exclusive operations enforce collisions without separate existence checks, avoiding redundant linear directory scans; Phase01 bring-up showed those scans materially increase write latency as images accumulate.
3. Write a new `NNNNNN.tmp`, require the returned byte count to equal the frame length, check SdFat sync and close results, then rename to `NNNNNN.jpg`. Announce success only after the full write, sync, close, and rename succeed. Always return the camera frame buffer, including on failures.
4. Allocate numbers monotonically during the run; failed attempts may leave gaps. Preserve failed temporary files for inspection and do not present them as successful JPEGs. Do not write a counter to internal flash every second.
5. At `999999`, stop and report sequence exhaustion rather than wrapping. Do not delete older images automatically when the card fills.

This reduces exposure of incomplete files during normal operation. Neither exFAT nor FAT32 makes this write-and-rename sequence transactional under sudden power loss; the last file or filesystem can still be damaged. On restart, ignore temporary files as images but include their numbers in recovery. A single root directory and startup scan are adequate for the basic PoC; long deployments will need directory rotation and a faster index strategy. exFAT supports larger files, but the later conventional RIFF/WAV implementation must still rotate recordings before its approximately 4 GiB format limit, or adopt a separate large-audio-file format decision.

### Control flow and failures

Keep small C++ functions or modules for board configuration, camera capture, image storage/sequence allocation, activity indication, and capture scheduling. Start with one synchronous writer and bounded operations; introduce queues or additional tasks only when audio or measured latency requires them.

Initialisation follows `LED/IR safe state -> serial -> SD mount and sequence scan -> camera init and settling -> ready`. Log the configuration at startup. Check camera initialisation before accessing its sensor handle. Failed initialisation leaves capture disabled with a clear serial error; never continue with an invalid camera or SD handle.

For a null camera frame, log a failed attempt and try at the next deadline. For a storage failure, full card, or removed card, stop recording and require a restart after correcting the fault. Log sequence, acquisition time since boot, byte count, capture/write durations, success/failure, and missed deadlines at 115200 baud. Keep logs concise enough to preserve cadence. A capture failure must release resources and leave the next capture possible.

## Staged development

Later stages preserve the capture/storage boundary and sequence policy. Each stage gets its own bring-up measurements before integration.

| Stage | Scope and completion evidence |
| --- | --- |
| 1: Basic PoC | Colour JPEG to SD at 1 fps; GPIO3 activity pulse; recovery and error behaviour verified below. |
| 2: dTOF integration | Attach MIKROE LightRanger 14 Click (TMF8829) through a Gravity-to-mikroBUS wiring adapter. Configure external I2C with explicit SDA/SCL assignments on GPIO43/44 after checking wiring. Verify 3.3 V supply capacity, pull-ups, address, interface jumpers, and shared ground. Port/adapt the vendor driver rather than assuming its mikroSDK example is an Arduino library. Correlate valid range measurements with image sequence and monotonic time. |
| 3: Ambient light and IR | Read the onboard LTR-308, then add explicit GPIO47 illumination control with configurable thresholds, hysteresis, and exposure settling. Keep LED3 activity independent. Validate dark/light transitions and image quality; IR illumination changes colour response. |
| 4: PDM audio to WAV | Reuse the vendor recording example as a reference [5]. Convert PDM to PCM and store valid RIFF/WAV files, initially mono 16 kHz, 16-bit. Finalise header sizes on close. Verify playback, image cadence, I2S/DMA compatibility, buffering, and combined SD throughput. Assign audio file IDs and correlate them with images through monotonic timestamps; add a single storage owner/queue if needed. |
| 5: Motion wake and power | Establish a current baseline, then implement and measure idle, sensing, capture, and sleep states. Define motion using valid range changes, debounce, hysteresis, and an inactivity timeout. Compare timer-driven sampling, light sleep, deep sleep, and camera/peripheral shutdown. Preserve numbering after every wake. |

The Gravity connector carries power, ground, SDA, and SCL only; sensor interrupt/enable signals need additional wiring. LightRanger 14 exposes INT and EN and uses 3.3 V logic [6]. Its documented example uses a **data-ready** interrupt; autonomous motion-only wake is not established. Before choosing deep sleep, prove the sensor can detect the required event without the ESP32 running, keep the sensing path powered, and route INT to an available ESP32-S3 pin capable of waking from the chosen sleep mode. GPIO availability on this board must be verified. Otherwise use periodic wake-and-read or light sleep and measure the tradeoff. I2C traffic alone is not a deep-sleep wake source.

## Alternatives and consequences

- **Keep the vendor deep-sleep cycle:** useful for later low-duty-cycle capture, but repeated boot and camera settling complicate a stable 1 fps baseline. Defer until power measurements justify it.
- **Use ESP-IDF or build a new camera driver:** adds bring-up work; the Arduino skeleton already covers the required peripherals. Reconsider if later peripheral or power requirements exceed Arduino support.
- **Capture raw RGB and encode in software:** increases memory and processing costs without improving the required JPEG output.

The selected approach makes the first result easy to inspect and extend. It consumes more power than an event-driven system, and synchronous SD writes can introduce jitter. Storage use is approximately `mean JPEG bytes x 86,400` per day at 1 fps (100 kB/frame is about 8.64 GB/day). A nominal 128 GB card would hold roughly 14.8 days at that rate before filesystem overhead, allocation rounding, and reserved free space; the six-digit sequence limit is reached sooner, after approximately 11.6 days. Resolution and retention decisions must use measured image sizes and latency. SdFat adds a dependency and requires storage API adaptation, while the larger card requires explicit hardware validation.

## Acceptance checks

Run these on the actual v1.1 board; documentation review cannot establish hardware performance.

Before capture testing, mount the actual 128 GB card as exFAT and confirm its usable capacity is approximately 128 GB decimal (119 GiB), less formatting overhead. Create, sync, close, rename, reopen, and verify a scratch file, then repeat after a restart. On expendable media, perform a checksum-verified write/read test that actually allocates and writes beyond 32 GB; preallocation alone does not prove access to that capacity. A computer-side full-capacity test can establish media health, but the board must also demonstrate access beyond 32 GB. Record card model, partition layout, allocation unit size, library/core versions, SPI clock, and results. An invalid or unsupported format must fail without reformatting or altering existing files.

1. On an empty card, record for 10 minutes after settling. Expect 600 saved images for 600 scheduled slots, zero missed deadlines, and a mean acquisition interval of 1,000 ms within 1%. Record worst-case timing and file sizes; each capture-and-save must complete within its one-second budget.
2. Decode every saved JPEG, confirm configured colour output (currently 2048 x 1536 QXGA), correct orientation, ordered six-digit filenames, and scene changes without stale-frame sequences. Observe one distinct GPIO3 pulse per attempt and IR off. Historical VGA results remain separate from QXGA validation.
3. Restart and power-cycle with existing images present. Continue above the highest allocated number, leave prior files unchanged, and confirm an abandoned `.tmp` cannot cause a collision. Test sequence exhaustion with a prepared card.
4. Exercise missing/full/removed SD, simulated partial write/rename failure, camera initialisation failure, and null-frame handling. Confirm clear errors, no false success or overwrite, LED off after failure, and frame-buffer release. Use temporary fault injection where faults cannot be reproduced safely on the bench.
5. Perform a controlled power-interruption test with expendable media. Inspect prior files, the last image/temporary file, filesystem health, and restart numbering; record limitations rather than claiming transactional storage.

## References

Sources consulted 2026-10-22. Pin assignments and electrical behaviour require verification against the physical board and schematic during implementation.

1. [DFRobot Timed camera Arduino example](https://wiki.dfrobot.com/dfr1154/docs/21371).
2. [DFRobot DFR1154 example sources](https://github.com/DFRobot/DFR1154_Examples), `6.9 Timed camera`.
3. [DFRobot reference, SD limits, and v1.1 schematic link](https://wiki.dfrobot.com/dfr1154/docs/21367).
4. [DFRobot board overview and pin/component descriptions](https://wiki.dfrobot.com/dfr1154/).
5. [DFRobot example sources](https://github.com/DFRobot/DFR1154_Examples), `5.1 Obtain ambient light data` and `5.2 Recording & Playback`.
6. [MIKROE LightRanger 14 Click specification, pinout, and driver example](https://www.mikroe.com/lightranger-14-click).
7. [SdFat v2 filesystem support and configuration](https://github.com/greiman/SdFat), [SPI initialisation API](https://github.com/greiman/SdFat/blob/master/src/SdFat.h), and [SdInfo diagnostics example](https://github.com/greiman/SdFat/blob/master/examples/SdInfo/SdInfo.ino).
8. [ESP-IDF FatFs documentation and optional exFAT support](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/fatfs.html); consult the documentation matching the chosen core/IDF release during implementation.
