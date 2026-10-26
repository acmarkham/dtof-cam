"""Independent Phase04 manifest and optical-envelope validation."""


def sync_bounds(row):
    dma, done = int(row["camera_dma_us"]), int(row["camera_done_us"])
    period = int(row["camera_period_bound_us"])
    begin, end = int(row["depth_rx_begin_us"]), int(row["depth_rx_end_us"])
    tick, t0 = int(row["depth_systick"]), int(row["depth_t0_tick"])
    ratio = int(row["clock_ratio_uq15"])
    delta = (tick-t0) & 0xffffffff
    if not 0 < ratio <= 65535 or delta > 125000 or done < dma or end < begin or period <= 0:
        raise ValueError("invalid acquisition clock/envelope")
    ago = (delta * 8 * ratio + 16384) >> 15
    earliest = begin - ago - (ago // 200 + 1000)
    if earliest < 0:
        raise ValueError("negative mapped depth time")
    rgb = (max(0, dma-period), done)
    depth = (earliest, end)
    worst = max(abs(rgb[0]-depth[1]), abs(rgb[1]-depth[0]))
    midpoint = sum(rgb)//2 - sum(depth)//2
    return rgb, depth, worst, midpoint


def validate_row(row, tolerance, start, stop):
    rgb, depth, worst, midpoint = sync_bounds(row)
    if rgb != (int(row["rgb_earliest_us"]), int(row["rgb_latest_us"])) or depth != (int(row["depth_earliest_us"]), int(row["depth_latest_us"])):
        raise ValueError("stored optical envelope does not match raw timestamps")
    if worst != int(row["sync_bound_us"]) or midpoint != int(row["midpoint_delta_us"]):
        raise ValueError("stored synchronization result does not match recomputed result")
    if int(row["sync_tolerance_us"]) != tolerance or worst > tolerance:
        raise ValueError("half-frame synchronization tolerance exceeded")
    if min(rgb[0], depth[0]) < start or max(rgb[1], depth[1]) > stop:
        raise ValueError("capture outside burst acquisition window")
    return worst
