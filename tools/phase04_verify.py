"""Verify a 30s RGB/depth burst, all SD files, and a 10s deep-sleep/wake cycle."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import statistics
import time
import zlib

from phase04_console import BurstConsole, fields, open_serial, serial
from phase02_verify import validate_csv
from phase03_protocol import decode_packet
from phase04_protocol import validate_row
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def state(console):
    console.send("STATUS")
    return fields(console.until("STATUS ", timeout=20))


def wait_for_burst(console, timeout=90):
    deadline = time.monotonic() + timeout
    saw_active_led = False
    last_query = 0
    while time.monotonic() < deadline:
        if time.monotonic() - last_query > 3:
            console.send("STATUS"); last_query = time.monotonic()
        line = console.line()
        if line.startswith("ERROR"):
            raise RuntimeError(line)
        if line.startswith("STATUS "):
            s = fields(line)
            if s["fatal"] != "0":
                raise RuntimeError(line)
            if s["capturing"] == "1":
                if s["led"] != "1":
                    raise ValueError("active capture LED is off")
                saw_active_led = True
            if s["finished"] == "1":
                if s["led"] != "0" or s["capturing"] != "0":
                    raise ValueError("inactive capture LED is on")
                return s, saw_active_led
    raise TimeoutError("30-second burst")


def get_json(console, burst, kind):
    return json.loads(console.get(f"{burst} {kind}"))


def check_manifest(meta, summary, index_data):
    rows = list(csv.DictReader(io.StringIO(index_data.decode("utf-8"))))
    if not summary["success"] or not summary["complete"]:
        raise ValueError("burst unsuccessful/incomplete")
    for key in ("depth_gaps", "queue_drops", "rgb_errors", "depth_errors"):
        if summary[key]:
            raise ValueError(f"nonzero {key}")
    if not summary["sensor_standby"] or not summary["camera_off"] or summary["led"]:
        raise ValueError("sensors/LED not shut down after capture")
    if meta["rgb"]["width"] != 2048 or meta["rgb"]["height"] != 1536 or meta["rgb"]["jpeg_quality"] > 6:
        raise ValueError("QXGA quality configuration changed")
    if not meta["rgb"]["exposure_manual"] or not 0 < meta["rgb"]["exposure_lines"] <= meta["rgb"]["vts_lines"]:
        raise ValueError("exposure is not bounded by a sensor frame")
    if meta["depth"]["preset"] != 65 or meta["depth"]["histograms"]:
        raise ValueError("wrong depth acquisition mode")
    if not 1 <= meta["target_fps"] <= 10 or meta["period_us"] < 100000:
        raise ValueError("capture exceeds 10 fps cap")
    tolerance = meta["period_us"] // 2
    if tolerance != meta["sync_tolerance_us"] or len(rows) != summary["pairs"]:
        raise ValueError("manifest/count mismatch")
    if len(rows) < meta["target_fps"] * 30 * 0.75:
        raise ValueError("too few synchronized frames for burst")
    start, stop = summary["capture_start_us"], summary["capture_stop_us"]
    if not 30000000 <= stop-start <= 32000000:
        raise ValueError("burst duration outside acquisition+flush budget")
    bounds = []
    for i, row in enumerate(rows):
        if int(row["pair_seq"]) != i+1:
            raise ValueError("pair index gap")
        bounds.append(validate_row(row, tolerance, start, start+30000000))
        if i:
            previous = rows[i-1]
            if int(row["camera_dma_us"]) - int(previous["camera_dma_us"]) < 100000:
                raise ValueError("reused RGB frame or capture faster than 10 fps")
            if int(row["depth_frame_no"]) <= int(previous["depth_frame_no"]):
                raise ValueError("duplicate/regressing depth frame")
    if max(bounds) != summary["max_sync_bound_us"]:
        raise ValueError("summary sync bound mismatch")
    return rows, bounds


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--output", type=Path, default=ROOT / "Phase04/validation/local")
    parser.add_argument("--skip-sleep-check", action="store_true")
    parser.add_argument("--restart", action="store_true", help="restart firmware and record startup diagnostics")
    parser.add_argument("--manifest-only", action="store_true", help="diagnose cycling without downloading images; not full acceptance")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"passed": False}
    log = (args.output/"serial.log").open("w", encoding="utf-8")
    console = BurstConsole(args.port, log)
    try:
        if args.restart:
            console.send("REBOOT")
            console.until("PHASE04 ", timeout=30)
        console.send("HOLD"); console.until("HOLD enabled=1", timeout=30)
        stopped, active_led = wait_for_burst(console)
        burst = int(stopped["burst"])
        meta, summary = get_json(console, burst, "META"), get_json(console, burst, "SUMMARY")
        index_data = console.get(f"{burst} INDEX")
        rows, bounds = check_manifest(meta, summary, index_data)
        report.update(burst=burst, metadata=meta, firmware_summary=summary, led_observed_active=active_led,
                      verified_pairs=0, max_sync_bound_us=max(bounds), min_sync_bound_us=min(bounds))
        (args.output/"burst.json").write_text(json.dumps(meta, indent=2)+"\n")
        (args.output/"summary-device.json").write_text(json.dumps(summary, indent=2)+"\n")
        (args.output/"pairs.csv").write_bytes(index_data)
        first_jpeg = None
        byte_counts = []
        report["file_readback_performed"] = not args.manifest_only
        for i, row in enumerate([] if args.manifest_only else rows):
            seq = int(row["pair_seq"])
            jpeg = console.get(f"{burst} JPG {seq}")
            depth_csv = console.get(f"{burst} DEPTH {seq}")
            native = console.get(f"{burst} RAW {seq}")
            for data, key in ((jpeg,"jpeg_crc32"), (depth_csv,"depth_csv_crc32"), (native,"native_crc32")):
                if zlib.crc32(data) != int(row[key],16):
                    raise ValueError("SD readback differs from original capture CRC")
            if len(jpeg) != int(row["jpeg_bytes"]) or len(depth_csv) != int(row["depth_csv_bytes"]):
                raise ValueError("SD file length mismatch")
            with Image.open(io.BytesIO(jpeg)) as image:
                image.load()
                if image.size != (2048,1536) or image.mode != "RGB":
                    raise ValueError("unexpected JPEG dimensions/colour")
            csv_meta, zones = validate_csv(depth_csv)
            packet = decode_packet(native)
            if (csv_meta["session_id"] != meta["burst_id"] or int(csv_meta["frame_seq"]) != seq or
                packet["frame_number"] != int(row["depth_frame_no"]) or
                packet["t0"] != int(row["depth_t0_tick"]) or packet["t1"] != int(row["depth_t1_tick"]) or
                packet["sensor_tick"] != int(row["depth_systick"])):
                raise ValueError("native/manifest depth association mismatch")
            for zone, raw in zip(zones,packet["zones"]):
                if any(int(zone[key]) != raw[key] for key in ("x","y","distance_raw_qmm","snr_raw")):
                    raise ValueError("CSV differs from native depth frame")
            for ext,data in (("jpg",jpeg),("csv",depth_csv),("tof",native)):
                (args.output/f"{seq:06d}.{ext}").write_bytes(data)
            if first_jpeg is None:
                first_jpeg = jpeg
            byte_counts.append(len(jpeg)); report["verified_pairs"] += 1
            if (i+1) % 10 == 0:
                print(f"Verified {i+1}/{len(rows)} synchronized QXGA/depth pairs", flush=True)
        times = [int(r["depth_earliest_us"]) for r in rows]
        report.update(mean_pair_interval_us=statistics.mean(b-a for a,b in zip(times,times[1:])))
        if byte_counts:
            report.update(jpeg_bytes_min=min(byte_counts), jpeg_bytes_max=max(byte_counts))
        if not args.skip_sleep_check:
            sleep_started = time.monotonic()
            console.send("SLEEP 10"); console.until("SLEEP seconds=10", timeout=20)
            console.serial.close()
            time.sleep(3)
            reconnect_deadline = time.monotonic()+50
            while True:
                try:
                    console.serial = open_serial(args.port)
                    console.send("HOLD")
                    console.until("HOLD enabled=1", timeout=15)
                    break
                except (serial.SerialException, OSError, TimeoutError):
                    try: console.serial.close()
                    except Exception: pass
                    if time.monotonic() >= reconnect_deadline:
                        raise
                    time.sleep(0.5)
            wake = state(console)
            if wake["wake_reason"] != "4" or int(wake["boot_count"]) != meta["boot_count"]+1:
                raise ValueError("wake did not come from the deep-sleep timer")
            report["sleep_seconds"] = 10
            report["wake_status"] = wake
            report["sleep_to_usb_response_seconds"] = time.monotonic()-sleep_started
            after, _ = wait_for_burst(console)
            new_burst = int(after["burst"])
            new_meta, new_summary = get_json(console,new_burst,"META"), get_json(console,new_burst,"SUMMARY")
            new_rows, new_bounds = check_manifest(new_meta,new_summary,console.get(f"{new_burst} INDEX"))
            if (new_burst == burst or new_meta["burst_id"] == meta["burst_id"] or
                (first_jpeg is not None and console.get(f"{burst} JPG 1") != first_jpeg)):
                raise ValueError("burst allocation or prior-file preservation failed")
            report["next_burst"] = {"number":new_burst,"pairs":len(new_rows),"max_sync_bound_us":max(new_bounds),"summary":new_summary}
        report["passed"] = True
        console.send("ALLOW_SLEEP"); console.until("HOLD enabled=0")
    except Exception as exc:
        report["error"] = str(exc)
    finally:
        console.serial.close(); log.close()
        (args.output/"summary.json").write_text(json.dumps(report,indent=2)+"\n", encoding="utf-8")
    print(json.dumps(report,indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
