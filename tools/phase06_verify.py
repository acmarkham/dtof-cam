"""Exercise Phase06 classifier/GPIO diagnostics and independently replay live data.

Leaves the default motion monitor running. The LED selftest uses SYNTHETIC inputs;
real motion decisions are independently checked against native sensor packets.
"""
import argparse
import json
from pathlib import Path
import time

from phase04_console import BurstConsole, fields, open_serial
from phase06_protocol import MotionReplay

ROOT = Path(__file__).resolve().parents[1]


def check_status(row):
    if row["ready"] != "1" or row["fatal"] != "0":
        raise ValueError(f"firmware not running: {row}")
    for key in ("gaps", "counter_resets", "faults", "io_errors", "stale"):
        if row[key] != "0":
            raise ValueError(f"nonzero {key}: {row[key]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--output", type=Path, default=ROOT / "Phase06/validation/local")
    parser.add_argument("--reboot", action="store_true", help="also verify initialization after software reset")
    args = parser.parse_args()
    if args.frames < 2:
        parser.error("at least two frames required")
    args.output.mkdir(parents=True, exist_ok=True)
    summary = {"passed": False, "port": args.port}
    console = None
    with (args.output / "serial.log").open("w", encoding="utf-8") as log:
        try:
            console = BurstConsole(args.port, log)
            if args.reboot:
                console.send("REBOOT")
                console.until("REBOOTING", 10)
                console.serial.close()
                deadline = time.monotonic() + 20
                while True:
                    time.sleep(0.5)
                    try:
                        console.serial = open_serial(args.port)
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                console.until("PHASE06", 20)
                startup = fields(console.until("CLASSIFIER_TEST", 10))
                if int(startup["checks"]) < 30 or startup["failures"] != "0":
                    raise ValueError("startup classifier selftest failed")
                console.until("READY phase=6", 40)
                summary["startup_classifier"] = startup
                summary["software_restart"] = "passed"
            console.send("STATUS")
            check_status(fields(console.until("STATUS")))
            console.send("SELFTEST")
            led_tests = []
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                line = console.line()
                if line.startswith("LED_TEST "):
                    led_tests.append(fields(line))
                elif line.startswith("SELFTEST_DONE "):
                    diagnostic = fields(line)
                    break
                elif line.startswith("ERROR"):
                    raise RuntimeError(line)
            else:
                raise TimeoutError("SELFTEST_DONE")
            if len(led_tests) != 3 or [r["led"] for r in led_tests] != ["0", "1", "0"] or any(r["source"] != "synthetic" or r["ok"] != "1" for r in led_tests):
                raise ValueError("synthetic classifier-to-GPIO test failed")
            if int(diagnostic["checks"]) < 30 or diagnostic["failures"] != "0" or diagnostic["gpio_failures"] != "0" or diagnostic["resumed"] != "1":
                raise ValueError("embedded diagnostic failed")
            summary["classifier_selftest"] = diagnostic
            summary["synthetic_gpio_tests"] = led_tests
            # SELFTEST emits its current CONFIG and STATUS after SELFTEST_DONE.
            # Consume them before matching replies to subsequent CONFIG commands.
            console.until("CONFIG ")
            check_status(fields(console.until("STATUS ")))
            for command, expected in (("CONFIG 3 20 30", "2"), ("CONFIG 5 20 30", "4")):
                console.send(command)
                config = fields(console.until("CONFIG "))
                if config["required_zones"] != expected or config["period_ms"] != "500" or config["kilo_iterations"] != "2000":
                    raise ValueError(f"config mismatch: {config}")
                check_status(fields(console.until("STATUS ")))
            console.send("CONFIG 0 20 30")
            if not console.until("ERROR CONFIG").startswith("ERROR CONFIG expects_"):
                raise ValueError("invalid config was not rejected")
            console.send("STATUS")
            unchanged = fields(console.until("STATUS "))
            if unchanged["scene_percent"] != "5" or unchanged["required"] != "4":
                raise ValueError("invalid configuration altered state")
            console.send("RESET")
            console.until("BASELINE_RESET reason=command")
            initial = fields(console.until("STATUS "))
            check_status(initial)
            replay = MotionReplay()
            deadline = time.monotonic() + args.frames * 0.6 + 15
            while len(replay.rows) < args.frames and time.monotonic() < deadline:
                line = console.line()
                if line.startswith("FRAME "):
                    replay.accept(fields(line))
                    if len(replay.rows) % 20 == 0:
                        print(f"Replayed {len(replay.rows)}/{args.frames} real sensor frames", flush=True)
                elif line.startswith(("ERROR", "WARN")):
                    raise RuntimeError(line)
            results = replay.summary()
            summary.update(results)
            if len(replay.rows) != args.frames:
                raise TimeoutError(f"motion observation: received {len(replay.rows)}/{args.frames} frames")
            if not 475000 <= results["mean_interval_us"] <= 525000:
                raise ValueError(f"cadence outside 2 fps +/-5%: {results}")
            if len({r["epoch"] for r in replay.rows}) != 1:
                raise ValueError("unexpected reset during observation")
            if not results["static_frames"] and not results["moving_frames"]:
                raise ValueError("no usable frame pairs")
            console.send("STATUS")
            final = fields(console.until("STATUS "))
            check_status(final)
            summary.update({"initial_status": initial, "final_status": final,
                            "config_command_checks": "passed", "passed": True})
        except Exception as exc:
            summary["error"] = str(exc)
            raise
        finally:
            if console is not None:
                console.serial.close()
            (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
