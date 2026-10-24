# Phase01 upstream sources

The camera initialisation and capture structure follow DFRobot's Arduino Timed
camera example:

- Repository: https://github.com/DFRobot/DFR1154_Examples
- Commit: `f63ef72ad7cb277e850e0602f1a2ed84207f3da8`
- File: `6.9 Timed camera/source code/6_9/6_9.ino`
- Vendor walkthrough: https://wiki.dfrobot.com/dfr1154/docs/21371
- v1.1 schematic checked for camera, SD SPI, LED and IR pins:
  https://dfimg.dfrobot.com/62d52567aa9508d63a4247a1/wikien/62f7850198e513d61ce2e02ecde3eb42.pdf

The upstream repository has no root licence file or licence notice in this
example at that commit. Attribution is retained here; the project's MIT licence
does not establish a licence grant for upstream material. Check with DFRobot
before redistributing their example as licensed source.

Storage is independently implemented using SdFat 2.3.1, under its MIT licence:
https://github.com/greiman/SdFat/blob/2.3.1/LICENSE.md
The library is installed as a build dependency and is not vendored into this repo.

Phase01 replaces deep sleep with a monotonic 1 Hz loop, adds guarded sequence
allocation, exFAT writes, explicit error handling, LED pulse timing, and USB
diagnostics. Camera output was initially VGA rather than the example's UXGA;
the 2026-10-24 update selects the OV3660 maximum QXGA (2048 x 1536).
