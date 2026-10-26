# Phase03 vendor source provenance

Upstream: <https://github.com/ams-OSRAM/tmf8829_driver_linux_userspace>

Pinned commit: `9c478afcf3a4bc7fdfc4b3e7c0721dac865696db`.

`tmf8829.h`, `tmf8829_fw.h` and the starting `tmf8829.c` come from that commit.
The headers and core carry `GPL-2.0 OR MIT`; this project uses the **MIT option**,
with upstream copyright notices retained and [LICENSE-MIT.TXT](LICENSE-MIT.TXT)
included. The patch header also carries the same dual licence notice.

Patch array length: **14,032 bytes**. SHA-256 of the actual array bytes (not
the C header text):
`473fbc7c5a3e54df9b02d6b00ee8424e10463f63d42737d8ee248511e2e06c28`.
These identifiers are also compiled into session metadata via `Provenance.h`.
The running sensor application version is read from the device on each boot.

Local changes:

Phase03 begins with the Phase02 vendor port below. Its active acquisition path
is the bounded reader in `Depth.cpp`, using the independently checked native
profile and group ordering in `HistogramProtocol.h`. The core additionally
exports `tmf8829TrackClock`, a wrapper around the unchanged vendor estimator,
so histogram and depth packet headers all contribute timestamp pairs. No patch
bytes or core clock-correction algorithm were changed.

- `tmf8829_shim.h` replaces the Linux shim. Arduino I/O, timers and depth callbacks
  live in `Depth.cpp`; the transfer buffer is 1 KiB. EN is physically asserted.
- The retained Phase02 `tmf8829ReadResults` is bounded to the single-peak 8×8 format. It checks
  preheader flags, frame type/focal-plane/layout, exact declared length and EOF,
  and checks I/O return values before callbacks. It retains raw quarter-mm values
  rather than overwriting them with corrected distances. Vendor clock-ratio
  calculation remains in the core; a separate output copy is corrected.
- FIFO reads are split into 128-byte transactions at register `0xff`; ordinary
  register windows advance their address. The 21-byte preheader/header transaction
  spans the FIFO status/tick registers and the start of the FIFO as documented.
- No Linux CLI, JSON library, POSIX shim, point-cloud parser or keystone code is
  linked. Phase03 uses its own bounded raw histogram reader and USB export
  commands rather than the legacy core read functions or the Linux parser.

Protocol reference: ams OSRAM **AN001096 v3-00**, 2026-02-17.
Datasheet: **DS001140 v2-00**, 2025-11-12. Long range uses preset command `0x41`;
the datasheet's 11 m maximum remains subject to physical characterisation.
