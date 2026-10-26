"""Independent decoder for the pinned TMF8829 native 8x8 packet profile.

Keeps reference channels and all 24-bit counts. No bin-to-distance conversion
is assumed; range estimates come from the sensor's associated result packet.
"""
import struct

HISTOGRAM_BYTES = 25377
DEPTH_BYTES = 225


def decode_packet(data):
    if len(data) < 21:
        raise ValueError("truncated native header")
    kind, focal_plane = data[5] & 0xf0, data[5] & 15
    payload = int.from_bytes(data[7:9], "little")
    expected = HISTOGRAM_BYTES if kind == 0x20 else DEPTH_BYTES if kind == 0x10 else 0
    if not expected or focal_plane > 1 or len(data) != expected or payload + 9 != len(data):
        raise ValueError("native type/length/focal-plane mismatch")
    if data[0] & 0x30 or data[-2:] != b"\xf7\xe0":
        raise ValueError("FIFO error or bad native footer")
    if kind == 0x20 and data[6] not in (0, 1) or kind == 0x10 and data[6] != 1:
        raise ValueError("unexpected native layout")
    decoded = {
        "kind": kind, "focal_plane": focal_plane, "layout": data[6],
        "frame_number": int.from_bytes(data[9:13], "little"),
        "sensor_tick": int.from_bytes(data[1:5], "little"),
        "temperature": list(struct.unpack("3b", data[13:16])),
        "t0": int.from_bytes(data[-12:-8], "little"),
        "t1": int.from_bytes(data[-8:-4], "little"), "status": data[-4],
    }
    if kind == 0x10:
        decoded["zones"] = [
            {"x": i % 8, "y": i // 8,
             "distance_raw_qmm": int.from_bytes(data[21 + 3*i:23 + 3*i], "little"),
             "snr_raw": data[23 + 3*i]}
            for i in range(64)
        ]
    else:
        if not decoded["status"] & 1 or decoded["status"] & 0xc0:
            raise ValueError("invalid/aborted histogram")
        body = memoryview(data)[21:-12]
        counts = [int.from_bytes(body[i:i+3], "little") for i in range(0, len(body), 3)]
        decoded["references"] = [counts[i*64:(i+1)*64] for i in range(4)]
        decoded["zones"] = [
            {"x": i % 4 + data[6]*4, "y": i // 4,
             "bins": counts[256 + 256*i:512 + 256*i]}
            for i in range(32)
        ]
    return decoded


def decode_group(packets):
    if len(packets) != 3:
        raise ValueError("incomplete group: expected T0, T1, result")
    decoded = [decode_packet(p) for p in packets]
    if [(p["kind"], p["layout"]) for p in decoded] != [(0x20, 0), (0x20, 1), (0x10, 1)]:
        raise ValueError("missing/duplicate/out-of-order packet")
    for a, b in zip(decoded, decoded[1:]):
        if (b["frame_number"] - a["frame_number"]) & 0xffffffff != 1 or a["focal_plane"] != b["focal_plane"]:
            raise ValueError("native frame gap or mixed focal planes")
        if (a["t0"], a["t1"]) != (b["t0"], b["t1"]):
            raise ValueError("mixed measurement integration timestamps")
    histograms = sorted(decoded[0]["zones"] + decoded[1]["zones"], key=lambda p: (p["y"], p["x"]))
    return {"packets": [{k: v for k, v in p.items() if k not in ("zones", "references")} for p in decoded],
            "depth": decoded[2]["zones"], "histograms": histograms,
            "references": [p["references"] for p in decoded[:2]]}
