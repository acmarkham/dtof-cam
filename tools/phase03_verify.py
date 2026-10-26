"""Acquire 30 full histogram/depth sets, validate SD CSV and CRC-checked raw export."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import time
import zlib

from phase01_verify import fields
from phase02_verify import DepthConsole, validate_csv
from phase03_protocol import decode_group

ROOT = Path(__file__).resolve().parents[1]


def frame_fields(line):
    row = fields(line)
    required = ("seq", "sensor", "host_rx_us", "valid", "status", "bytes", "ratio", "gaps",
                "write_us", "hist0", "hist1", "hist0_crc32", "hist1_crc32", "depth_crc32",
                "set_crc32", "hist0_read_us", "hist1_read_us", "group_start_us")
    missing = [key for key in required if key not in row]
    if missing:
        raise ValueError("truncated FRAME diagnostic; missing " + ", ".join(missing))
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--output", type=Path, default=ROOT / "Phase03/validation/local")
    args = parser.parse_args()
    if not 2 <= args.frames <= 60:
        parser.error("--frames must be between 2 and 60 (bounded 64-set RAM cache)")
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {"requested_frames": args.frames, "passed": False}
    log = (args.output / "serial.log").open("w", encoding="utf-8")
    console = DepthConsole(args.port, log)
    try:
        initial, storage = console.stop()
        if initial["ready"] != "1":
            raise RuntimeError("firmware not ready")
        console.send("STATUS")
        console.until("STATUS ")
        initial_histogram = fields(console.until("HISTOGRAM "))
        console.send("INFO")
        sensor = json.loads(console.until('{"sensor":'))
        summary.update(sensor=sensor, session=storage,
                       session_metadata=console.get_metadata("SESSION"),
                       config_metadata=console.get_metadata("CONFIG"))
        if (not sensor["histograms"] or sensor["preset"] != 65 or sensor["period_ms"] != 5000 or
                summary["config_metadata"]["effective_sensor"] != sensor or
                summary["session_metadata"]["session_id"] != storage["session_id"]):
            raise ValueError("effective config or persisted metadata mismatch")
        console.send("START")
        if fields(console.until("STATUS "))["recording"] != "1":
            raise RuntimeError("sensor did not start")
        frames = []
        deadline = time.monotonic() + args.frames * 6 + 30
        while len(frames) < args.frames and time.monotonic() < deadline:
            line = console.line()
            if line.startswith("FRAME "):
                frames.append(frame_fields(line))
                if len(frames) % 5 == 0:
                    print(f"Captured {len(frames)}/{args.frames} histogram/depth sets", flush=True)
            elif line.startswith(("ERROR", "STOP reason=")):
                raise RuntimeError(line)
        stopped, _ = console.stop()
        console.send("STATUS")
        console.until("STATUS ")
        counters = fields(console.until("HISTOGRAM "))
        summary["histogram_counters"] = counters
        if len(frames) != args.frames:
            raise ValueError("capture timed out")
        if any(initial[k] != stopped[k] for k in ("gaps", "rejected", "io_errors")):
            raise ValueError("acquisition gaps/rejections/I2C errors")
        if counters["incomplete"] != initial_histogram["incomplete"]:
            raise ValueError("incomplete measurement during validation")
        summary["cache_replacements_during_run"] = int(counters["cache_replaced"]) - int(initial_histogram["cache_replaced"])
        sequences = [int(f["seq"]) for f in frames]
        times = [int(f["group_start_us"]) for f in frames]
        intervals = [b-a for a, b in zip(times, times[1:])]
        if sequences != list(range(sequences[0], sequences[0] + args.frames)) or min(intervals) <= 0:
            raise ValueError("sequence/timestamp gap")
        if abs(statistics.mean(intervals) - 5e6) > 5e5:
            raise ValueError("histogram acquisition cadence outside 10% of requested period")
        summary.update(first_sequence=sequences[0], last_sequence=sequences[-1],
                       mean_interval_us=statistics.mean(intervals), min_interval_us=min(intervals),
                       max_interval_us=max(intervals), max_write_us=max(int(f["write_us"]) for f in frames),
                       mean_histogram_i2c_read_us=statistics.mean(int(f["hist0_read_us"])+int(f["hist1_read_us"]) for f in frames),
                       verified_sets=0, invalid_zone_rows=0, nonzero_histogram_bins=0)
        hashes = {}
        for index, frame in enumerate(frames):
            seq = int(frame["seq"])
            group_dir = args.output / f"{seq:09d}"
            group_dir.mkdir(exist_ok=True)
            csv_data = console.get(seq)
            meta, rows = validate_csv(csv_data, histogram_counts=(2, 2, 1))
            packets = [console.get(f"RAW {seq} {part}") for part in range(3)]
            native = decode_group(packets)
            for part, packet in enumerate(packets):
                key = ("hist0_crc32", "hist1_crc32", "depth_crc32")[part]
                if zlib.crc32(packet) != int(frame[key], 16):
                    raise ValueError("export differs from original acquisition CRC")
                csv_key = ("histogram_crc32_0", "histogram_crc32_1", "native_depth_crc32")[part]
                if int(meta[csv_key], 16) != zlib.crc32(packet):
                    raise ValueError("CSV packet CRC differs from native export")
                (group_dir / f"{part}.native").write_bytes(packet)
            if zlib.crc32(b"".join(packets)) != int(frame["set_crc32"], 16):
                raise ValueError("whole set CRC mismatch")
            for key, source in (("frame_seq", "seq"), ("sensor_frame_no", "sensor"), ("host_rx_us", "host_rx_us"),
                                ("clock_ratio_uq15", "ratio"), ("group_start_us", "group_start_us"),
                                ("histogram_frame_0", "hist0"), ("histogram_frame_1", "hist1")):
                if meta[key] != frame[source]:
                    raise ValueError(f"CSV acquisition metadata mismatch: {key}")
            if meta["session_id"] != storage["session_id"] or len(csv_data) != int(frame["bytes"]):
                raise ValueError("CSV session/size mismatch")
            if [p["frame_number"] for p in native["packets"]] != [int(frame[k]) for k in ("hist0", "hist1", "sensor")]:
                raise ValueError("native packet association mismatch")
            result_header = native["packets"][2]
            if (int(meta["sensor_tick_raw"]) != result_header["sensor_tick"] or
                    int(meta["frame_status_raw"]) != result_header["status"] or
                    int(meta["t0_integration_raw"]) != result_header["t0"] or
                    int(meta["t1_integration_raw"]) != result_header["t1"] or
                    sum(int(r["valid"]) for r in rows) != int(frame["valid"])):
                raise ValueError("CSV native timing/status/validity mismatch")
            for row, zone in zip(rows, native["depth"]):
                if any(int(row[k]) != zone[k] for k in ("x", "y", "distance_raw_qmm", "snr_raw")):
                    raise ValueError("SD depth differs from associated raw result")
            summary["invalid_zone_rows"] += sum(row["valid"] == "0" for row in rows)
            summary["nonzero_histogram_bins"] += sum(value != 0 for zone in native["histograms"] for value in zone["bins"])
            (group_dir / "depth.csv").write_bytes(csv_data)
            (group_dir / "decoded.json").write_text(json.dumps(native, separators=(",", ":")) + "\n")
            hashes[str(seq)] = hashlib.sha256(b"".join(packets)).hexdigest()
            summary["verified_sets"] += 1
            if (index + 1) % 5 == 0:
                print(f"Verified {index + 1}/{args.frames} complete histogram/depth sets", flush=True)
        if not summary["nonzero_histogram_bins"]:
            raise ValueError("all histogram data is empty")
        (args.output / "native-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
        summary["passed"] = True
        console.send("START")
        if fields(console.until("STATUS "))["recording"] != "1":
            raise RuntimeError("failed to resume capture")
    except Exception as exc:
        summary.update(passed=False, error=str(exc))
    finally:
        console.serial.close(); log.close()
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
