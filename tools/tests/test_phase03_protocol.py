from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phase03_protocol import HISTOGRAM_BYTES, DEPTH_BYTES, decode_packet, decode_group
from phase03_verify import frame_fields


def packet(kind=0x20, layout=0, number=0, status=1):
    size = HISTOGRAM_BYTES if kind == 0x20 else DEPTH_BYTES
    result = bytearray(size)
    result[5:9] = bytes((kind, layout)) + (size - 9).to_bytes(2, "little")
    result[9:13] = number.to_bytes(4, "little")
    result[-4] = status
    result[-2:] = b"\xf7\xe0"
    return result


class HistogramProtocolTests(unittest.TestCase):
    def test_truncated_diagnostic_fails_with_explicit_reason(self):
        with self.assertRaisesRegex(ValueError, "truncated FRAME diagnostic"):
            frame_fields("FRAME seq=8 sensor=24 host_rx_us=40785254 hist0=22 hist1=23")

    def test_full_uint24_counts_reference_and_zone_mapping(self):
        p = packet()
        p[21:24] = b"\x56\x34\x12"  # Reference 0, bin 0.
        p[21+768:24+768] = b"\xef\xcd\xab"  # Zone x0,y0 bin0.
        p[21+768+31*768+255*3:24+768+31*768+255*3] = b"\xff\xff\xff"
        t0 = decode_packet(p)
        self.assertEqual(t0["references"][0][0], 0x123456)
        self.assertEqual(t0["zones"][0]["bins"][0], 0xabcdef)
        self.assertEqual(t0["zones"][-1], {"x": 3, "y": 7, "bins": [0]*255+[0xffffff]})
        self.assertEqual(sum(len(r) for r in t0["references"]), 256)
        self.assertEqual(sum(len(z["bins"]) for z in t0["zones"]), 8192)
        t1 = decode_packet(packet(layout=1))
        self.assertEqual((t1["zones"][0]["x"], t1["zones"][-1]["x"]), (4, 7))

    def test_complete_set_and_native_counter_wrap(self):
        group = decode_group([packet(number=0xfffffffe), packet(layout=1, number=0xffffffff),
                              packet(kind=0x10, layout=1, number=0)])
        self.assertEqual(len(group["histograms"]), 64)
        self.assertEqual([(z["x"], z["y"]) for z in group["histograms"]],
                         [(i % 8, i // 8) for i in range(64)])
        self.assertEqual(len(group["references"]), 2)

    def test_missing_duplicate_out_of_order_or_cross_frame_rejected(self):
        t0, t1, depth = packet(), packet(layout=1, number=1), packet(0x10, 1, 2)
        for packets in ([t0, depth], [t0, t0, depth], [t1, t0, depth],
                        [t0, t1, packet(0x10, 1, 5)]):
            with self.assertRaises(ValueError):
                decode_group(packets)

    def test_mixed_measurement_timestamps_rejected(self):
        depth = packet(0x10, 1, 2)
        depth[-12] = 1
        with self.assertRaisesRegex(ValueError, "integration timestamps"):
            decode_group([packet(), packet(layout=1, number=1), depth])

    def test_truncated_length_layout_footer_and_fifo_errors_rejected(self):
        valid = packet()
        variants = [valid[:-1]]
        for position, value in ((7, 0), (5, 0x22), (6, 2), (-1, 0), (0, 0x10)):
            bad = bytearray(valid); bad[position] = value; variants.append(bad)
        for bad in variants:
            with self.assertRaises(ValueError):
                decode_packet(bad)

    def test_aborted_histogram_rejected_but_invalid_result_retained(self):
        for status in (0, 0x41, 0x81):
            with self.assertRaises(ValueError):
                decode_packet(packet(status=status))
        self.assertEqual(decode_packet(packet(0x10, 1, 2, 0))["status"], 0)


if __name__ == "__main__":
    unittest.main()
