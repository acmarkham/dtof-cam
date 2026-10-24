"""Capture acceptance run and verify SD JPEGs over the Phase01 USB console.

Install tools/requirements.txt into .tools/python, then run from the repo root.
Downloaded images and logs stay under Phase01/validation/local by default.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import statistics
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".tools/python"))
import serial
from PIL import Image


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


class Console:
    def __init__(self, port, log):
        self.port = port
        self.log = log
        self.serial = serial.Serial(port, 115200, timeout=2, write_timeout=5)

    def line(self):
        line = self.serial.readline().decode("ascii", errors="replace").strip()
        if line:
            self.log.write(line + "\n")
            self.log.flush()
        return line

    def send(self, command):
        self.serial.write((command + "\n").encode("ascii"))

    def until(self, prefix, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.line()
            if line.startswith(prefix):
                return line
            if line.startswith(("HALTED", "ERROR")):
                raise RuntimeError(line)
        raise TimeoutError(prefix)

    def stop(self):
        self.send("STOP")
        self.until("STOPPED")
        return fields(self.until("STATUS"))

    def get(self, sequence):
        argument = sequence if isinstance(sequence, str) else f"{sequence:06d}"
        self.send(f"GET {argument}")
        header = fields(self.until("FILE "))
        self.send("ACK FILE")
        expected = int(header["bytes"])
        if expected > 4 * 1024 * 1024:
            raise ValueError("unexpected image length")
        data = bytearray()
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            line = self.line()
            if line.startswith("FILE "):
                self.send("ACK FILE")
                continue
            if line.startswith("END_FILE"):
                end = fields(line)
                if end["ok"] != "1" or len(data) != expected or int(end["bytes"]) != expected:
                    raise ValueError("incomplete image transfer")
                return bytes(data)
            if line.startswith("DATA "):
                packet = fields(line)
                try:
                    payload = bytes.fromhex(packet["hex"])
                    offset = int(packet["offset"])
                    if len(payload) != int(packet["bytes"]) or zlib.crc32(payload) != int(packet["crc32"], 16):
                        continue  # No acknowledgement: firmware retransmits this block.
                except (KeyError, ValueError):
                    continue
                if offset == len(data):
                    data.extend(payload)
                elif offset < len(data) and data[offset:offset + len(payload)] == payload:
                    pass  # Duplicate after a lost acknowledgement.
                else:
                    continue
                if len(data) > expected:
                    raise ValueError("oversized image transfer")
                self.send(f"ACK {offset}")
        raise TimeoutError("image transfer")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--width", type=int, default=2048)
    parser.add_argument("--height", type=int, default=1536)
    parser.add_argument("--output", type=Path, default=ROOT / "Phase01/validation/local")
    parser.add_argument("--restart-check", action="store_true")
    parser.add_argument("--verify-log", type=Path,
                        help="verify an existing completed capture run instead of capturing again")
    args = parser.parse_args()
    if args.frames < 2:
        parser.error("--frames must be at least 2")
    if args.width < 1 or args.height < 1:
        parser.error("expected dimensions must be positive")
    if args.verify_log and args.verify_log.resolve() == (args.output / "serial.log").resolve():
        parser.error("--verify-log requires a different --output directory to preserve the capture log")
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {"requested_frames": args.frames, "expected_width": args.width,
               "expected_height": args.height, "passed": False}
    log = (args.output / "serial.log").open("w", encoding="utf-8")
    console = Console(args.port, log)
    try:
        initial = console.stop()
        if initial["ready"] != "1":
            raise RuntimeError("firmware not ready")
        frames = []
        if args.verify_log:
            in_capture = False
            for line in args.verify_log.read_text(encoding="utf-8").splitlines():
                if line == "STARTED":
                    in_capture = True
                elif in_capture and line == "STOPPED":
                    break
                elif in_capture and line.startswith("FRAME "):
                    row = fields(line)
                    if row.get("ok") != "1":
                        raise RuntimeError(line)
                    frames.append(row)
                elif in_capture and line.startswith(("ERROR", "HALTED", "OVERRUN")):
                    raise RuntimeError(line)
            summary["capture_log"] = args.verify_log.as_posix()
        else:
            console.send("START")
            console.until("STARTED")
            deadline = time.monotonic() + args.frames + 60
            while len(frames) < args.frames and time.monotonic() < deadline:
                line = console.line()
                if line.startswith("FRAME "):
                    row = fields(line)
                    if row.get("ok") != "1":
                        raise RuntimeError(line)
                    frames.append(row)
                    if len(frames) % 30 == 0:
                        print(f"Captured {len(frames)}/{args.frames}", flush=True)
                elif line.startswith(("ERROR", "HALTED", "OVERRUN")):
                    raise RuntimeError(line)
        stopped = console.stop()
        if len(frames) != args.frames:
            raise RuntimeError("capture timed out")
        sequences = [int(row["seq"]) for row in frames]
        if sequences != list(range(sequences[0], sequences[0] + args.frames)):
            raise ValueError("sequence gap")
        timestamps = [int(row["t_us"]) for row in frames]
        intervals = [b - a for a, b in zip(timestamps, timestamps[1:])]
        sensor_times = [int(row["frame_us"]) for row in frames]
        if any(b <= a for a, b in zip(sensor_times, sensor_times[1:])):
            raise ValueError("non-increasing camera frame timestamps")
        mean_us = statistics.mean(intervals)
        if abs(mean_us - 1_000_000) > 10_000:
            raise ValueError("mean capture interval outside 1%")
        if max(int(row["total_us"]) for row in frames) >= 1_000_000:
            raise ValueError("capture/storage operation exceeded budget")
        if int(stopped["missed"]) != int(initial["missed"]) or stopped["ir"] != "0" or stopped["led"] != "0":
            raise ValueError("deadline or output state failure")
        summary.update({
            "capture_timing_passed": True,
            "first_sequence": sequences[0], "last_sequence": sequences[-1],
            "mean_interval_us": mean_us,
            "min_interval_us": min(intervals), "max_interval_us": max(intervals),
            "max_operation_us": max(int(row["total_us"]) for row in frames),
        })
        hashes = set()
        first_image = None
        for index, row in enumerate(frames):
            data = console.get(int(row["seq"]))
            if len(data) != int(row["bytes"]) or zlib.crc32(data) != int(row["crc32"], 16):
                raise ValueError("SD readback differs from captured frame")
            with Image.open(io.BytesIO(data)) as image:
                image.load()
                if image.size != (args.width, args.height) or image.mode != "RGB":
                    raise ValueError(f"unexpected image {image.size} {image.mode}")
            (args.output / f"{int(row['seq']):06d}.jpg").write_bytes(data)
            hashes.add(hashlib.sha256(data).hexdigest())
            if first_image is None:
                first_image = data
            if (index + 1) % 30 == 0:
                print(f"Verified {index + 1}/{args.frames} SD JPEGs", flush=True)
        summary.update({
            "first_sequence": sequences[0], "last_sequence": sequences[-1],
            "decoded_images": len(frames), "unique_jpeg_hashes": len(hashes),
            "mean_interval_us": mean_us, "min_interval_us": min(intervals),
            "max_interval_us": max(intervals),
            "max_operation_us": max(int(row["total_us"]) for row in frames),
            "min_image_bytes": min(int(row["bytes"]) for row in frames),
            "max_image_bytes": max(int(row["bytes"]) for row in frames),
            "missed_delta": int(stopped["missed"]) - int(initial["missed"]),
        })
        if args.restart_check:
            expected_next = int(stopped["next"])
            console.send("REBOOT")
            console.until("REBOOTING")
            console.serial.close()
            # ESP32-S3 native USB can re-enumerate during software reset.
            reboot_deadline = time.monotonic() + 30
            while True:
                time.sleep(0.5)
                try:
                    console.serial = serial.Serial(args.port, 115200, timeout=2, write_timeout=5)
                    break
                except serial.SerialException:
                    if time.monotonic() >= reboot_deadline:
                        raise
            # Query state explicitly: USB may re-enumerate after the boot banner.
            after_restart = console.stop()
            if int(after_restart["next"]) < expected_next:
                raise ValueError("sequence regressed after restart")
            if console.get(sequences[0]) != first_image:
                raise ValueError("existing image changed after restart")
            summary["restart_numbering_and_prior_image"] = "passed"
        summary["passed"] = True
    except Exception as exc:
        summary["error"] = str(exc)
        raise
    finally:
        try:
            console.send("STOP")
        except (serial.SerialException, OSError):
            pass
        console.serial.close()
        log.close()
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
