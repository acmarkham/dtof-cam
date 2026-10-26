# Phase04 upstream sources

Camera initialization derives from [Phase01](../Phase01/UPSTREAM.md), including
DFRobot DFR1154 Examples commit `f63ef72ad7cb277e850e0602f1a2ed84207f3da8`.
That provenance and the absence of an upstream example licence still apply.
Phase04 uses QXGA, JPEG quality 6 and neutral brightness/saturation, then bounds
exposure and continuously drains frames into a PSRAM ring for pairing.

The TMF8829 core and RAM patch derive from the Phase02 port; see
[vendor provenance](Phase04/src/tmf8829/README.md) and its retained MIT licence.
The Phase04 native result reader brackets sensor clock sampling with host time.
The one added core entry point, `tmf8829TrackClock`, exposes the existing vendor
clock correction calculation without changing its arithmetic.

The build uses the camera driver bundled with Arduino-ESP32 3.3.2. Timing was
also inspected in esp32-camera v2.1.3, commit
`b3c2aa273a88c7debf2b1683448933ca2b28aad9`, specifically `esp_camera.h`,
`driver/cam_hal.c` and `sensors/ov3660.c`. That checkout is reference material,
not a replacement linked library.
