# Phase05 source provenance

This phase starts from [Phase04](../Phase04/UPSTREAM.md). Camera initialization
retains DFRobot example attribution and its upstream licence limitation described
in [Phase01 provenance](../Phase01/UPSTREAM.md). The build uses Arduino-ESP32
3.3.2 and SdFat 2.3.1.

TMF8829 source and RAM patch remain pinned to the same vendor commit and MIT
licence option in [vendor provenance](Phase05/src/tmf8829/README.md). The
14,032-byte patch and clock-ratio arithmetic are unchanged.

Phase05 changes acquisition/storage scheduling: independent depth and RGB
sequences, a bounded end-of-burst depth archive, 100 ms depth period with 1000
kilo-iterations, explicit 8×8A checks, per-stream CSV indexes, the versioned
native-result audit file, and acknowledged restart diagnostics. No firmware
histogram readout, microphone, ALS or network code is enabled.
