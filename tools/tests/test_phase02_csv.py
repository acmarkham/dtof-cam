"""Validate the readback verifier against known units and malformed CSVs."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phase02_verify import validate_csv


def fixture(status=1, ratio=32768):
    meta = {
        "dtof_csv_version": 1,
        "session_id": "d9dd691a-115a-43f4-9374-65968a09ba42",
        "config_id": 1, "frame_seq": 1, "sensor_frame_no": 34,
        "host_rx_us": 1000000, "sensor_tick_raw": 125000,
        "clock_ratio_uq15": ratio, "frame_status_raw": status,
        "t0_integration_raw": 120000, "t1_integration_raw": 121000,
        "histogram_expected": 0, "histogram_received": 0,
        "histogram_complete": 0, "width": "8,height=8,peaks=1",
    }
    lines = [f"# {k}={v}" for k, v in meta.items()]
    lines.append("x,y,distance_mm,distance_raw_qmm,snr_raw,valid")
    # Zone 0 = 1000.25mm at unity ratio. Other zones retain invalid raw values.
    valid = bool(status & 1 and not status & 0xc0)
    lines.append(f"0,0,{'1000.250' if valid else ''},4001,42,{int(valid)}")
    lines.extend(f"{i % 8},{i // 8},,0,0,0" for i in range(1, 64))
    return ("\n".join(lines) + "\n").encode()


class DepthCsvTests(unittest.TestCase):
    def test_units_and_invalid_zones(self):
        _, rows = validate_csv(fixture())
        self.assertEqual(rows[0]["distance_mm"], "1000.250")
        self.assertEqual(rows[-1]["distance_mm"], "")

    def test_aborted_and_invalid_frames_retain_raw_without_distance(self):
        for status in (0, 0x41, 0x81, 0xc1):
            _, rows = validate_csv(fixture(status=status))
            self.assertEqual(rows[0]["distance_raw_qmm"], "4001")
            self.assertEqual(rows[0]["valid"], "0")

    def test_correction_rounding(self):
        data = fixture(ratio=33190).replace(b"1000.250", b"1013.250")
        validate_csv(data)  # Independent known result: round(4001*33190/32768)/4.

    def test_bad_units_or_correction_rejected(self):
        for distance in (b"4001.000", b"1.000", b"1000.000", b"nan"):
            with self.subTest(distance=distance), self.assertRaises(ValueError):
                validate_csv(fixture().replace(b"1000.250", distance))

    def test_truncated_or_duplicate_zone_rejected(self):
        data = fixture()
        for bad in (data.rsplit(b"\n", 2)[0] + b"\n", data.replace(b"7,7,,0,0,0", b"0,0,,0,0,0")):
            with self.assertRaises(ValueError):
                validate_csv(bad)

    def test_invalid_zone_must_have_empty_distance(self):
        for bad in (fixture().replace(b"7,7,,0,0,0", b"7,7,0.000,0,0,0"),
                    fixture().replace(b"4001,42,1", b"4001,0,1")):
            with self.assertRaises(ValueError):
                validate_csv(bad)

    def test_bad_schema_or_line_endings_rejected(self):
        for bad in (fixture().replace(b"dtof_csv_version=1", b"dtof_csv_version=2"),
                    fixture().replace(b"\n", b"\r\n"), fixture()[:-1]):
            with self.assertRaises(ValueError):
                validate_csv(bad)


if __name__ == "__main__":
    unittest.main()
