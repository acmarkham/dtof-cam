"""Replay Phase06 decisions independently from CRC-checked native sensor packets."""
import statistics
import zlib

from phase03_protocol import decode_packet


class MotionReplay:
    def __init__(self):
        self.previous = None
        self.last = None
        self.rows = []

    def accept(self, row):
        if row.get("source") != "sensor":
            raise ValueError("expected real sensor frame")
        native = bytes.fromhex(row["native_hex"])
        if zlib.crc32(native) != int(row["native_crc32"], 16):
            raise ValueError("native CRC mismatch")
        packet = decode_packet(native)
        if packet["kind"] != 0x10 or packet["focal_plane"] != 0:
            raise ValueError("expected 8x8A depth packet")
        if packet["frame_number"] != int(row["sensor_frame"]) or packet["status"] != int(row["status"]):
            raise ValueError("native frame/status mismatch")
        scene, relative, cm = (int(row[k]) for k in ("scene_percent", "relative_percent", "min_cm"))
        if not (1 <= scene <= 100 and 0 <= relative <= 1000 and 1 <= cm <= 1638):
            raise ValueError("invalid threshold configuration")
        required = (scene * 64 + 99) // 100
        ratio = int(row["ratio"])
        distances, valid = [], set()
        for i, zone in enumerate(packet["zones"]):
            corrected = min(65535, (zone["distance_raw_qmm"] * ratio + 16384) >> 15)
            distances.append(corrected)
            if packet["status"] & 1 and not packet["status"] & 0xc0 and zone["distance_raw_qmm"] and zone["snr_raw"] and corrected:
                valid.add(i)
        epoch = int(row["epoch"])
        if self.last:
            if int(row["seq"]) != int(self.last["seq"]) + 1:
                raise ValueError("host sequence gap")
            if epoch == int(self.last["epoch"]):
                if (packet["frame_number"] - int(self.last["sensor_frame"])) & 0xffffffff != 1:
                    raise ValueError("sensor sequence gap")
                if any(row[k] != self.last[k] for k in ("scene_percent", "relative_percent", "min_cm")):
                    raise ValueError("threshold changed without baseline reset")
            else:
                self.previous = None
            if int(row["host_us"]) <= int(self.last["host_us"]):
                raise ValueError("nonincreasing host timestamp")
        comparable, changed = set(), set()
        if row["mode"] == "warming":
            mode = "warming"
            self.previous = None
        elif self.previous is None:
            mode = "baseline"
            self.previous = (distances, valid)
        else:
            before, old_valid = self.previous
            comparable = valid & old_valid
            changed = {i for i in comparable
                       if abs(distances[i] - before[i]) > cm * 40
                       and abs(distances[i] - before[i]) * 100 > before[i] * relative}
            mode = "insufficient" if len(comparable) < required else "moving" if len(changed) >= required else "static"
            self.previous = (distances, valid)
        expected = {
            "valid": len(valid), "comparable": len(comparable), "changed": len(changed), "required": required,
            "valid_mask": sum(1 << i for i in valid),
            "comparable_mask": sum(1 << i for i in comparable),
            "changed_mask": sum(1 << i for i in changed),
            "moving": int(mode == "moving"), "led": int(mode == "moving"),
        }
        if row["mode"] != mode:
            raise ValueError(f"mode mismatch: expected {mode}, received {row['mode']}")
        for key, value in expected.items():
            if int(row[key], 16 if key.endswith("_mask") else 10) != value:
                raise ValueError(f"{key} mismatch: expected {value}, received {row[key]}")
        self.last = row
        self.rows.append(row)

    def summary(self):
        intervals = [int(b["host_us"]) - int(a["host_us"])
                     for a, b in zip(self.rows, self.rows[1:]) if a["epoch"] == b["epoch"]]
        if not intervals:
            raise ValueError("at least two consecutive frames required")
        return {
            "frames": len(self.rows), "mean_interval_us": statistics.mean(intervals),
            "min_interval_us": min(intervals), "max_interval_us": max(intervals),
            "measured_fps": 1e6 / statistics.mean(intervals),
            "moving_frames": sum(r["moving"] == "1" for r in self.rows),
            "static_frames": sum(r["mode"] == "static" for r in self.rows),
            "insufficient_frames": sum(r["mode"] == "insufficient" for r in self.rows),
            "min_valid_zones": min(int(r["valid"]) for r in self.rows),
            "max_changed_zones": max(int(r["changed"]) for r in self.rows),
            "independent_replay": "passed",
        }
