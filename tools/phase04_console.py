"""Phase04 USB console; keep DTR/RTS deasserted to avoid resetting native USB."""
import argparse
from pathlib import Path
import time

from phase01_verify import Console, fields, serial


def open_serial(port):
    stream = serial.Serial(port=None, baudrate=115200, timeout=1, write_timeout=5)
    stream.dtr = False
    stream.rts = False
    stream.port = port
    stream.open()
    return stream


class BurstConsole(Console):
    def __init__(self, port, log):
        self.port, self.log = port, log
        self.serial = open_serial(port)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--command", default="STATUS")
    parser.add_argument("--seconds", type=float, default=5)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if "\n" in args.command or "\r" in args.command:
        parser.error("one command per invocation")
    if args.log:
        args.log.parent.mkdir(parents=True, exist_ok=True)
    log = args.log.open("w", encoding="utf-8") if args.log else None
    try:
        with open_serial(args.port) as stream:
            stream.write((args.command + "\n").encode("ascii"))
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                line = stream.readline().decode("ascii", errors="replace").strip()
                if line:
                    print(line, flush=True)
                    if log:
                        log.write(line + "\n"); log.flush()
    finally:
        if log:
            log.close()


if __name__ == "__main__":
    main()
