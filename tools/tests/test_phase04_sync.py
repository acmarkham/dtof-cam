from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phase04_protocol import sync_bounds, validate_row


def fixture():
    return dict(camera_dma_us=1000000, camera_done_us=1100000, camera_period_bound_us=100000,
                depth_rx_begin_us=1010000, depth_rx_end_us=1010500, depth_systick=10000,
                depth_t0_tick=7000, clock_ratio_uq15=32768, rgb_earliest_us=900000,
                rgb_latest_us=1100000, depth_earliest_us=984880, depth_latest_us=1010500,
                sync_bound_us=115120, midpoint_delta_us=2310, sync_tolerance_us=250000)


class BurstSyncTests(unittest.TestCase):
    def test_known_clock_mapping_and_exposure_envelopes(self):
        self.assertEqual(sync_bounds(fixture()), ((900000,1100000),(984880,1010500),115120,2310))
        self.assertEqual(validate_row(fixture(),250000,800000,1200000),115120)

    def test_exact_boundary_passes_one_microsecond_outside_fails(self):
        row = fixture(); row["sync_tolerance_us"] = 115120
        validate_row(row,115120,800000,1200000)
        row["sync_tolerance_us"] = 115119
        with self.assertRaisesRegex(ValueError,"tolerance"):
            validate_row(row,115119,800000,1200000)

    def test_near_midpoints_do_not_hide_large_exposure_separation(self):
        row = fixture(); row["camera_period_bound_us"] = 500000
        rgb, depth, bound, midpoint = sync_bounds(row)
        self.assertLess(abs(midpoint),250000)
        self.assertGreater(bound,250000)
        row.update(rgb_earliest_us=rgb[0],sync_bound_us=bound,midpoint_delta_us=midpoint)
        with self.assertRaisesRegex(ValueError,"tolerance"):
            validate_row(row,250000,0,2000000)

    def test_sensor_tick_wrap_is_unsigned(self):
        row = fixture(); row.update(depth_systick=1000,depth_t0_tick=0xfffffff0)
        self.assertEqual(sync_bounds(row)[1],(1000832,1010500))

    def test_corrupt_bounds_and_out_of_burst_frames_rejected(self):
        row = fixture(); row["sync_bound_us"] = 1
        with self.assertRaises(ValueError):
            validate_row(row,250000,800000,1200000)
        with self.assertRaisesRegex(ValueError,"outside burst"):
            validate_row(fixture(),250000,900001,1200000)

    def test_invalid_clock_brackets_or_stale_sensor_ticks_rejected(self):
        for values in (dict(depth_rx_end_us=1000000),dict(camera_done_us=999999),
                       dict(clock_ratio_uq15=0),dict(depth_t0_tick=10001)):
            row = fixture(); row.update(values)
            with self.assertRaises(ValueError):
                sync_bounds(row)


if __name__ == "__main__":
    unittest.main()
