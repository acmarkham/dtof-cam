"""Exercise USB download recovery without a connected camera or SD card."""
from pathlib import Path
import sys
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from phase01_verify import Console


def packet(offset, payload):
    return (f"DATA offset={offset} bytes={len(payload)} "
            f"crc32={zlib.crc32(payload):08x} hex={payload.hex()}")


class DownloadProtocolTests(unittest.TestCase):
    def console(self, size, lines):
        console = Console.__new__(Console)
        console.sent = []
        console.send = console.sent.append
        console.until = lambda prefix: f"FILE path=/000001.jpg bytes={size}"
        stream = iter(lines)
        console.line = lambda: next(stream)
        return console

    def test_corrupt_block_retried_and_duplicate_acknowledged_once_in_payload(self):
        first = bytes(range(256)) * 2
        second = b"remaining JPEG payload"
        size = len(first) + len(second)
        console = self.console(size, [
            "FILE path=/000001.jpg bytes=" + str(size),  # Lost header ACK.
            "DATA offset=0 bytes=512 crc32=00000000 hex=zz",  # Torn USB frame.
            packet(512, second),  # Out of order: do not acknowledge.
            packet(0, first),
            packet(0, first),  # Lost block ACK: retransmission must not append.
            packet(512, second).replace(f"{zlib.crc32(second):08x}", "00000000"),
            packet(512, second),
            f"END_FILE ok=1 bytes={size}",
        ])
        self.assertEqual(console.get(1), first + second)
        self.assertEqual(console.sent,
                         ["GET 000001", "ACK FILE", "ACK FILE", "ACK 0", "ACK 0", "ACK 512"])

    def test_incomplete_transfer_rejected_even_when_device_reports_success(self):
        console = self.console(10, [packet(0, b"short"), "END_FILE ok=1 bytes=10"])
        with self.assertRaisesRegex(ValueError, "incomplete image transfer"):
            console.get(1)

    def test_device_failure_rejected(self):
        console = self.console(10, ["END_FILE ok=0 bytes=0"])
        with self.assertRaisesRegex(ValueError, "incomplete image transfer"):
            console.get(1)

    def test_transfer_cannot_exceed_announced_file_size(self):
        console = self.console(1, [packet(0, b"too long")])
        with self.assertRaisesRegex(ValueError, "oversized image transfer"):
            console.get(1)


if __name__ == "__main__":
    unittest.main()
