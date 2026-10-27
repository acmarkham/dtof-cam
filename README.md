# dtof-cam
ESP32 cam + dTOF sensor

[Phase01 firmware, build/flash instructions and hardware verification](Phase01/README.md).

[Architecture decision](Design/ADR/0001-rgb-camera-sd-poc.md).

[Phase02 TMF8829 depth CSV logger, build/flash and verification](Phase02/README.md).

[dTOF architecture and future binary format](Design/ADR/0002-tmf8829-dtof-logging-poc.md).

[Phase03 complete histogram/depth acquisition and short validation](Phase03/README.md).

[Phase04 synchronized QXGA RGB/depth bursts and timer sleep](Phase04/README.md).

[Burst recording and synchronization decision](Design/ADR/0003-synchronized-rgb-depth-bursts.md).

[Phase05 independent 10 fps depth and 1 fps QXGA burst recording](Phase05/README.md).

[Independent stream timing and storage decision](Design/ADR/0004-independent-depth-rgb-bursts.md).

[Phase06 continuous 2 fps depth motion detection and red LED](Phase06/README.md).

[Motion threshold and validity decision](Design/ADR/0005-consecutive-depth-motion-detection.md).
