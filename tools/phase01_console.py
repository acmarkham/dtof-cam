"""Send a Phase01 console command and collect bounded serial output."""
import argparse
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".tools/python"))
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", default="COM12")
parser.add_argument("--command", default="STATUS")
parser.add_argument("--seconds", type=float, default=5)
parser.add_argument("--log", type=Path)
args = parser.parse_args()
if "\n" in args.command or "\r" in args.command:
    parser.error("send one command at a time")
if args.log:
    args.log.parent.mkdir(parents=True, exist_ok=True)
log = args.log.open("w", encoding="utf-8") if args.log else None
try:
    with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=5) as console:
        console.write((args.command + "\n").encode("ascii"))
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = console.readline().decode("ascii", errors="replace").strip()
            if line:
                print(line, flush=True)
                if log:
                    log.write(line + "\n")
                    log.flush()
finally:
    if log:
        log.close()
