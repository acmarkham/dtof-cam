from pathlib import Path
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phase06_protocol import MotionReplay


def row(seq=1, distances=None, valid=(1 << 64) - 1, epoch=1, mode="baseline",
        comparable=0, changed=0, ratio=32768):
    packet = bytearray(225)
    packet[5:9] = b"\x10\x01\xd8\x00"
    packet[9:13] = seq.to_bytes(4, "little")
    for i, d in enumerate(distances or [4000] * 64):
        packet[21 + i * 3:23 + i * 3] = d.to_bytes(2, "little")
        packet[23 + i * 3] = 1 if valid & (1 << i) else 0
    packet[-4] = 1
    packet[-2:] = b"\xf7\xe0"
    moving = int(mode == "moving")
    return {k: str(v) for k, v in {
        "source": "sensor", "seq": seq, "epoch": epoch, "sensor_frame": seq,
        "host_us": seq * 500000, "ratio": ratio, "status": 1, "mode": mode,
        "scene_percent": 5, "relative_percent": 20, "min_cm": 30,
        "valid_mask": f"{valid:016x}", "comparable_mask": f"{comparable:016x}",
        "changed_mask": f"{changed:016x}", "valid": valid.bit_count(),
        "comparable": comparable.bit_count(), "changed": changed.bit_count(),
        "required": 4, "moving": moving, "led": moving,
        "native_crc32": f"{zlib.crc32(packet):08x}", "native_hex": packet.hex(),
    }.items()}


class MotionReplayTests(unittest.TestCase):
    def test_real_packet_replay_motion_then_held_still_and_coverage(self):
        replay = MotionReplay()
        replay.accept(row())
        distances = [6000] * 4 + [4000] * 60
        all_zones = (1 << 64) - 1
        replay.accept(row(2, distances, mode="moving", comparable=all_zones, changed=15))
        replay.accept(row(3, distances, mode="static", comparable=all_zones))
        self.assertEqual(replay.summary()["moving_frames"], 1)
        self.assertEqual(replay.summary()["measured_fps"], 2)

    def test_integrity_and_reported_decision_tampering_rejected(self):
        for key, value, error in (("native_crc32", "00000000", "CRC"),
                                  ("sensor_frame", "99", "frame/status"),
                                  ("led", "1", "led mismatch"),
                                  ("valid_mask", "0", "valid_mask")):
            bad = row()
            bad[key] = value
            with self.assertRaisesRegex(ValueError, error):
                MotionReplay().accept(bad)

    def test_threshold_strictness_after_native_clock_correction(self):
        # Raw 500 mm -> corrected 1000 mm; raw 650 mm -> corrected 1300 mm.
        all_zones = (1 << 64) - 1
        replay = MotionReplay()
        replay.accept(row(distances=[2000] * 64, ratio=65535))
        replay.accept(row(2, [2600] * 64, mode="static", comparable=all_zones, ratio=65535))
        replay = MotionReplay()
        replay.accept(row(distances=[2000] * 64, ratio=65535))
        replay.accept(row(2, [2601] * 64, mode="moving", comparable=all_zones,
                          changed=all_zones, ratio=65535))

    def test_missing_frame_and_configuration_change_rejected(self):
        replay = MotionReplay()
        replay.accept(row())
        with self.assertRaisesRegex(ValueError, "sequence gap"):
            replay.accept(row(3))
        bad = row(2)
        bad["scene_percent"] = "3"
        with self.assertRaisesRegex(ValueError, "threshold changed"):
            replay.accept(bad)

    def test_epoch_and_warmup_remove_previous_reference(self):
        replay = MotionReplay()
        replay.accept(row())
        replay.accept(row(2, [6000] * 64, epoch=2))
        replay.accept(row(3, epoch=3, mode="warming"))
        replay.accept(row(4, [6000] * 64, epoch=3))


if __name__ == "__main__":
    unittest.main()
