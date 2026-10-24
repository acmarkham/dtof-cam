"""Capture TMF8829 CSV acceptance run and validate SD readback over USB.

Uses the existing acknowledged, per-block CRC transport from Phase01.
"""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import statistics
import time

from phase01_verify import Console, fields

ROOT = Path(__file__).resolve().parents[1]


def validate_csv(data):
    text = data.decode("utf-8")
    if "\r" in text or not text.endswith("\n"):
        raise ValueError("CSV must use LF and end with a newline")
    metadata = {}
    lines = []
    for line in text.splitlines():
        if line.startswith("# "):
            key, value = line[2:].split("=", 1)
            if key in metadata:
                raise ValueError("duplicate metadata")
            metadata[key] = value
        else:
            lines.append(line)
    expected_meta = {
        "dtof_csv_version": "1", "config_id": "1", "width": "8,height=8,peaks=1",
        "histogram_expected": "0", "histogram_received": "0", "histogram_complete": "0",
    }
    if any(metadata.get(k) != v for k, v in expected_meta.items()):
        raise ValueError("unexpected schema/config/histogram metadata")
    import uuid
    uuid.UUID(metadata["session_id"])
    for key in ("frame_seq", "sensor_frame_no", "host_rx_us", "sensor_tick_raw",
                "t0_integration_raw", "t1_integration_raw", "clock_ratio_uq15", "frame_status_raw"):
        if int(metadata[key]) < 0:
            raise ValueError("negative frame metadata")
    ratio = int(metadata["clock_ratio_uq15"])
    status = int(metadata["frame_status_raw"])
    if not 0 < ratio <= 65535 or not 0 <= status <= 255:
        raise ValueError("invalid ratio/status")
    reader = csv.DictReader(io.StringIO("\n".join(lines)))
    if reader.fieldnames != ["x", "y", "distance_mm", "distance_raw_qmm", "snr_raw", "valid"]:
        raise ValueError("unexpected columns")
    rows = list(reader)
    if len(rows) != 64:
        raise ValueError("expected 64 zones")
    for i, row in enumerate(rows):
        if None in row or (int(row["x"]), int(row["y"])) != (i % 8, i // 8):
            raise ValueError("duplicate/missing/reordered zone")
        raw, snr = int(row["distance_raw_qmm"]), int(row["snr_raw"])
        if not 0 <= raw <= 65535 or not 0 <= snr <= 255:
            raise ValueError("raw value outside native bounds")
        valid = bool(status & 1 and not status & 0xc0 and raw and snr)
        if row["valid"] != str(int(valid)):
            raise ValueError("validity does not match raw status/zone")
        if valid:
            corrected = min(65535, (ratio * raw + (1 << 14)) >> 15)
            expected = f"{corrected // 4}.{corrected % 4 * 250:03d}"
            if row["distance_mm"] != expected:
                raise ValueError("quarter-mm units or clock correction mismatch")
        elif row["distance_mm"]:
            raise ValueError("invalid zone has a distance")
    return metadata, rows


class DepthConsole(Console):
    def stop(self):
        self.send("STOP")
        self.until("STOP reason=")
        self.send("STATUS")
        state = fields(self.until("STATUS "))
        storage = fields(self.until("STORAGE "))
        return state, storage

    def get_metadata(self, kind):
        return json.loads(self.get(kind))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--restart-check", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT / "Phase02/validation/local")
    args = parser.parse_args()
    if args.frames < 2:
        parser.error("--frames must be at least 2")
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {"requested_frames": args.frames, "passed": False}
    log = (args.output / "serial.log").open("w", encoding="utf-8")
    console = DepthConsole(args.port, log)
    try:
        initial, storage = console.stop()
        if initial["ready"] != "1":
            raise RuntimeError("firmware not ready")
        console.send("INFO")
        sensor = json.loads(console.until('{"sensor":'))
        summary["sensor"] = sensor
        summary["session"] = storage
        summary["session_metadata"] = console.get_metadata("SESSION")
        summary["config_metadata"] = console.get_metadata("CONFIG")
        if (summary["session_metadata"]["session_id"] != storage["session_id"] or
                summary["config_metadata"]["effective_sensor"] != sensor):
            raise ValueError("SD session/config metadata mismatch")
        console.send("START")
        if fields(console.until("STATUS "))["recording"] != "1":
            raise RuntimeError("sensor did not start")
        frames = []
        deadline = time.monotonic() + args.frames * 1.1 + 30
        while len(frames) < args.frames and time.monotonic() < deadline:
            line = console.line()
            if line.startswith("FRAME "):
                frames.append(fields(line))
                if len(frames) % 30 == 0:
                    print(f"Captured {len(frames)}/{args.frames}", flush=True)
            elif line.startswith(("ERROR", "STOP reason=")):
                raise RuntimeError(line)
        stopped, _ = console.stop()
        if len(frames) != args.frames:
            raise ValueError("capture timed out")
        for key in ("gaps", "rejected", "io_errors"):
            if stopped[key] != initial[key]:
                raise ValueError(f"new {key}")
        sequences = [int(f["seq"]) for f in frames]
        if sequences != list(range(sequences[0], sequences[0] + args.frames)):
            raise ValueError("CSV sequence gap")
        times = [int(f["host_rx_us"]) for f in frames]
        intervals = [b - a for a, b in zip(times, times[1:])]
        if min(intervals) <= 0 or abs(statistics.mean(intervals) - 1e6) > 50000:
            raise ValueError("cadence outside 5% of 1 Hz")
        summary.update(first_sequence=sequences[0], last_sequence=sequences[-1],
                       mean_interval_us=statistics.mean(intervals), min_interval_us=min(intervals),
                       max_interval_us=max(intervals), max_write_us=max(int(f["write_us"]) for f in frames),
                       max_read_us=max(int(f["read_us"]) for f in frames),
                       bucket_boundary=(sequences[0] - 1) // 1000 != (sequences[-1] - 1) // 1000)
        csv_hashes = {}
        for index, frame in enumerate(frames):
            seq = int(frame["seq"])
            data = console.get(seq)
            metadata, rows = validate_csv(data)
            for key, source in (("frame_seq", "seq"), ("sensor_frame_no", "sensor"),
                                ("host_rx_us", "host_rx_us"), ("frame_status_raw", "status"),
                                ("clock_ratio_uq15", "ratio")):
                if metadata[key] != frame[source]:
                    raise ValueError(f"SD metadata differs from acquisition: {key}")
            if metadata["session_id"] != storage["session_id"] or len(data) != int(frame["bytes"]):
                raise ValueError("session/size mismatch")
            if sum(int(row["valid"]) for row in rows) != int(frame["valid"]):
                raise ValueError("valid zone count mismatch")
            (args.output / f"{seq:09d}.csv").write_bytes(data)
            csv_hashes[str(seq)] = hashlib.sha256(data).hexdigest()
            if (index + 1) % 30 == 0:
                print(f"Verified {index + 1}/{args.frames} SD CSVs", flush=True)
        summary["verified_csvs"] = len(csv_hashes)
        (args.output / "csv-sha256.json").write_text(json.dumps(csv_hashes, indent=2) + "\n")
        if args.restart_check:
            console.send("REBOOT")
            console.until("STOP reason=reboot")
            console.serial.close()
            import serial
            reboot_deadline = time.monotonic() + 40
            while True:
                time.sleep(0.5)
                try:
                    console.serial = serial.Serial(args.port, 115200, timeout=2, write_timeout=5)
                    break
                except serial.SerialException:
                    if time.monotonic() >= reboot_deadline:
                        raise
            # Startup patch loading takes a few seconds. Query explicitly after it.
            time.sleep(4)
            after, new_storage = console.stop()
            if after["ready"] != "1" or new_storage["session_id"] == storage["session_id"] or new_storage["session"] == storage["session"]:
                raise ValueError("restart did not create a new working session")
            summary["restart_new_session"] = new_storage
        summary["passed"] = True
        console.send("START")  # Leave the board recording after a successful run.
    except Exception as exc:
        summary["error"] = str(exc)
    finally:
        console.serial.close()
        log.close()
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
