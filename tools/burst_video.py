"""Render Phase04 burst folders as annotated RGB/depth videos (requires FFmpeg)."""
import argparse
import csv
from functools import lru_cache
import io
import json
import math
from pathlib import Path
import subprocess
import tempfile
import zlib

from PIL import Image, ImageDraw, ImageFont


# Reversed Viridis anchors: yellow (near) to dark purple (far).
COLORS = [(253, 231, 37), (94, 201, 98), (33, 145, 140),
          (59, 82, 139), (68, 1, 84)]


def color(value, maximum):
    position = min(1, max(0, value / maximum)) * (len(COLORS) - 1)
    index = min(int(position), len(COLORS) - 2)
    fraction = position - index
    return tuple(round(a + (b - a) * fraction)
                 for a, b in zip(COLORS[index], COLORS[index + 1]))


@lru_cache(maxsize=None)
def font(size):
    for name in ("C:/Windows/Fonts/arial.ttf", "DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def load_burst(folder):
    metadata = json.loads((folder / "burst.json").read_text())
    summary = json.loads((folder / "summary.json").read_text())
    if not summary.get("complete") or not summary.get("success"):
        raise ValueError(f"{folder}: burst is incomplete or unsuccessful")
    with (folder / "pairs.csv").open(newline="") as handle:
        pairs = list(csv.DictReader(handle))
    if not pairs or len(pairs) != summary["pairs"]:
        raise ValueError(f"{folder}: manifest count differs from summary")
    frames = []
    for pair in pairs:
        seq = int(pair["pair_seq"])
        stem = folder / f"{seq:06d}"
        for extension, key in ((".jpg", "jpeg_crc32"), (".csv", "depth_csv_crc32")):
            data = stem.with_suffix(extension).read_bytes()
            if zlib.crc32(data) != int(pair[key], 16):
                raise ValueError(f"{stem}{extension}: capture checksum mismatch")
        text = stem.with_suffix(".csv").read_text()
        headers = dict(line[2:].split("=", 1) for line in text.splitlines()
                       if line.startswith("# "))
        if headers["session_id"] != metadata["burst_id"] or int(headers["frame_seq"]) != seq:
            raise ValueError(f"{stem}: depth association mismatch")
        zones = list(csv.DictReader(io.StringIO("\n".join(
            line for line in text.splitlines() if not line.startswith("#")))))
        if [(int(z["x"]), int(z["y"])) for z in zones] != [(i % 8, i // 8) for i in range(64)]:
            raise ValueError(f"{stem}: expected ordered 8x8 depth cells")
        distances = [float(z["distance_mm"]) / 10 if z["valid"] == "1" else None
                     for z in zones]
        if any(v is not None and (not math.isfinite(v) or v < 0) for v in distances):
            raise ValueError(f"{stem}: invalid distance")
        frames.append({"seq": seq, "image": stem.with_suffix(".jpg"),
                       "time_us": int(pair["camera_dma_us"]), "distances_cm": distances})
    if any(b["time_us"] <= a["time_us"] for a, b in zip(frames, frames[1:])):
        raise ValueError(f"{folder}: timestamps must increase")
    return metadata, frames


def render(folder, frame, first_time, maximum, speed):
    canvas = Image.new("RGB", (1920, 928), (18, 22, 30))
    draw = ImageDraw.Draw(canvas)
    with Image.open(frame["image"]) as rgb:
        rgb = rgb.convert("RGB")
        rgb.thumbnail((1024, 768), Image.Resampling.LANCZOS)
        canvas.paste(rgb, ((1024 - rgb.width) // 2, 80 + (768 - rgb.height) // 2))
    draw.text((24, 20), f"{folder.name}  |  RGB", font=font(28), fill="white")
    draw.text((1056, 20), "Depth 8 x 8  |  cm  |  CW 90 + mirror", font=font(28), fill="white")
    label_font = font(18)
    for i, value in enumerate(frame["distances_cm"]):
        # Rotate clockwise, then mirror left/right: (x, y) -> (y, x).
        x, y = 1056 + (i // 8) * 96, 80 + (i % 8) * 96
        fill = color(value, maximum) if value is not None else (64, 68, 76)
        draw.rectangle((x, y, x + 95, y + 95), fill=fill, outline=(18, 22, 30), width=2)
        label = str(math.floor(value + 0.5)) if value is not None else "N/A"
        draw.text((x + 48, y + 42), label, anchor="mm", font=label_font,
                  fill="white", stroke_width=2, stroke_fill="black")
        if value is not None:
            draw.text((x + 48, y + 66), "cm", anchor="mm", font=font(15),
                      fill="white", stroke_width=1, stroke_fill="black")
    for y in range(768):
        draw.line((1842, 80 + y, 1864, 80 + y), fill=color(maximum * (1 - y / 767), maximum))
    for step in range(6):
        y = 80 + step * 767 / 5
        draw.text((1870, y), f"{maximum * (1-step/5):g}", font=font(14), fill="white", anchor="lm")
    elapsed = (frame["time_us"] - first_time) / 1e6
    draw.text((24, 875), f"Pair {frame['seq']:06d}  |  capture +{elapsed:.3f} s  |  {speed:g}x playback",
              font=font(22), fill="white")
    draw.text((1056, 875), "Near: yellow   Far: purple   Grey: N/A", font=font(21), fill="white")
    return canvas


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Burst folder or parent containing burst folders")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--speed", type=float, default=2)
    args = parser.parse_args()
    if not math.isfinite(args.speed) or args.speed <= 0:
        parser.error("--speed must be positive and finite")
    folders = [args.source] if (args.source / "burst.json").exists() else sorted(
        p.parent for p in args.source.glob("*/burst.json"))
    if not folders:
        parser.error("no burst folders found")
    bursts = [(folder, *load_burst(folder)) for folder in folders]
    values = [v for _, _, frames in bursts for f in frames for v in f["distances_cm"] if v is not None]
    maximum = max(100, math.ceil(max(values, default=100) / 100) * 100)
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"speed": args.speed, "colormap": "Reversed Viridis (interpolated anchors)",
              "color_range_cm": [0, maximum], "invalid_cells": "grey, N/A",
              "depth_orientation": "90 degrees clockwise then mirrored left/right: CSV (x, y) -> display (y, x)",
              "cell_labels": "integer cm, rounded to nearest (half up)",
              "videos": []}
    for folder, metadata, frames in bursts:
        output = args.output.resolve() / f"{folder.name}_rgb_depth_{args.speed:g}x.mp4"
        if output.exists():
            raise FileExistsError(output)
        # Use source timestamps; hold the final pair for one nominal capture period.
        durations = [(b["time_us"] - a["time_us"]) / 1e6 / args.speed
                     for a, b in zip(frames, frames[1:])]
        durations.append(metadata["period_us"] / 1e6 / args.speed)
        with tempfile.TemporaryDirectory(prefix="burst-video-") as temporary:
            temporary = Path(temporary)
            lines = ["ffconcat version 1.0"]
            for index, (frame, duration) in enumerate(zip(frames, durations)):
                image = render(folder, frame, frames[0]["time_us"], maximum, args.speed)
                image.save(temporary / f"{index:06d}.png")
                if index == 0:
                    image.save(args.output / f"{folder.name}_preview.jpg", quality=92)
                lines.extend([f"file '{index:06d}.png'", "option framerate 1000", f"duration {duration:.9f}"])
            lines.extend([f"file '{len(frames)-1:06d}.png'", "option framerate 1000"])
            manifest = temporary / "frames.ffconcat"
            manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")
            print(f"Encoding {folder.name}: {len(frames)} pairs, {sum(durations):.3f}s", flush=True)
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-n",
                            "-safe", "0", "-i", str(manifest), "-t", f"{sum(durations):.9f}",
                            "-vf", "fps=30", "-c:v", "libx264",
                            "-preset", "medium", "-crf", "18", "-pix_fmt", "yuv420p",
                            "-video_track_timescale", "30000", "-movflags", "+faststart", str(output)], check=True)
        probe = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-count_frames", "-show_streams", "-show_format",
            "-of", "json", str(output)], text=True))
        stream = probe["streams"][0]
        duration = float(probe["format"]["duration"])
        if abs(int(stream["nb_read_frames"]) - sum(durations) * 30) > 1 or abs(duration - sum(durations)) > 1 / 30:
            raise ValueError(f"{output}: encoded frame count or duration mismatch")
        report["videos"].append({"burst": folder.name, "file": str(output),
                                 "pairs": len(frames), "duration_s": duration,
                                 "capture_playback_span_s": sum(durations) * args.speed,
                                 "width": stream["width"], "height": stream["height"]})
        print(f"Verified {output.name}: {duration:.3f}s", flush=True)
    (args.output / "render_report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
