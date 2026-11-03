#!/usr/bin/env python3
"""Compare deterministic band-renderer captures; host time is not board FPS."""

import argparse
import json
import math
import subprocess
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

TIMES = (0, 1000, 2500, 4000, 8100, 8500, 9000)
MODES = ("default", "ax3", "opaque", "ax3-opaque")


def measure(first, second, mask):
    difference = ImageChops.difference(first, second)
    rgb = difference.tobytes()
    pixels = [
        (r, g, b)
        for r, g, b, active in zip(rgb[0::3], rgb[1::3], rgb[2::3], mask.tobytes(), strict=True)
        if active
    ]
    if not pixels:
        raise ValueError("empty comparison mask")
    mse = sum(c * c for p in pixels for c in p) / (3 * len(pixels))
    return {
        "pixels": len(pixels),
        "changed_percent": 100 * sum(any(p) for p in pixels) / len(pixels),
        "mae_rgb": sum(sum(p) for p in pixels) / (3 * len(pixels)),
        "psnr_db": 10 * math.log10(255 * 255 / mse) if mse else None,
        "max_channel_error": max(max(p) for p in pixels),
    }


def compare(output, mode, elapsed):
    stem = f"{mode}-{elapsed:05d}"
    first = Image.open(output / f"reference-{stem}.png").convert("RGB")
    second = Image.open(output / f"candidate-{stem}.png").convert("RGB")
    if first.size != (466, 466) or second.size != first.size:
        raise ValueError("expected two 466 x 466 panel captures")
    # Static chrome must survive the complete multi-capture scroll/press path.
    # Check encoded RGB pixels: some PNG previews omit unchanged clock pixels.
    for capture in (first, second):
        clock = capture.crop((160, 37, 307, 98)).tobytes()
        white = sum(min(rgb) > 200 for rgb in zip(clock[0::3], clock[1::3], clock[2::3]))
        if white < 200:
            raise ValueError(f"clock disappeared: {stem}")
    circle = Image.new("L", first.size)
    ImageDraw.Draw(circle).ellipse((0, 0, 465, 465), fill=255)
    controls = Image.new("L", first.size)
    draw = ImageDraw.Draw(controls)
    # Include the full 1.06 press envelope rather than hiding AA rim changes.
    draw.ellipse((62, 62, 140, 140), fill=255)
    draw.ellipse((326, 62, 404, 140), fill=255)
    draw.rounded_rectangle((130, 354, 336, 432), radius=39, fill=255)
    content = Image.new("L", first.size)
    ImageDraw.Draw(content).rectangle((50, 145, 415, 345), fill=255)
    pair = Image.new("RGB", (932, 496))
    pair.paste(first, (0, 30))
    pair.paste(second, (466, 30))
    labels = ImageDraw.Draw(pair)
    labels.text((12, 8), f"Reference | {mode} | {elapsed} ms", fill="white")
    labels.text((478, 8), f"Candidate | {mode} | {elapsed} ms", fill="white")
    pair.save(output / f"pair-{stem}.png")
    ImageChops.difference(first, second).point(lambda v: min(255, v * 4)).save(
        output / f"diff4x-{stem}.png"
    )
    # Nearest-neighbour enlargement exposes AA, glyph and dither coverage.
    for name, bounds in (("toolbar", (58, 58, 145, 145)), ("reply", (125, 350, 341, 436))):
        crop = Image.new("RGB", ((bounds[2] - bounds[0]) * 8, (bounds[3] - bounds[1]) * 4))
        for index, source in enumerate((first, second)):
            enlarged = source.crop(bounds).resize(
                ((bounds[2] - bounds[0]) * 4, (bounds[3] - bounds[1]) * 4),
                Image.Resampling.NEAREST,
            )
            crop.paste(enlarged, (index * enlarged.width, 0))
        crop.save(output / f"crop-{name}-{stem}.png")
    return {
        "mode": mode,
        "elapsed_ms": elapsed,
        "visible_circle": measure(first, second, circle),
        "glass_controls": measure(first, second, controls),
        "content_center": measure(first, second, content),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--modes", nargs="+", choices=MODES, default=MODES)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    log = []
    for mode in args.modes:
        for name, build in (("reference", args.reference), ("candidate", args.candidate)):
            command = [str(build / "orb_sim"), "--headless"]
            if "ax3" in mode:
                command += ["--text-size", "ax3"]
            if "opaque" in mode:
                command += ["--reduce-transparency"]
            for elapsed in TIMES:
                command += [
                    "--shot",
                    f"{elapsed}={args.output / f'{name}-{mode}-{elapsed:05d}.png'}",
                ]
            result = subprocess.run(command, check=True, text=True, capture_output=True)
            log.append(result.stdout + result.stderr)
        for surface in ("face", "list", "card"):
            for weight in ([], ["bold"]):
                suffix = "-bold" if weight else ""
                result = subprocess.run(
                    [
                        str(args.candidate / "test_glass_design"),
                        surface,
                        str(args.output / f"gate-{surface}-{mode}{suffix}.png"),
                        mode,
                        *weight,
                    ],
                    check=True,
                    text=True,
                    capture_output=True,
                )
                log.append(result.stdout + result.stderr)
    results = [compare(args.output, mode, elapsed) for mode in args.modes for elapsed in TIMES]
    (args.output / "pixel-metrics.json").write_text(json.dumps(results, indent=2) + "\n")
    (args.output / "capture.log").write_text("".join(log))
    sheet = Image.new("RGB", (233 * len(args.modes), 3 * 253))
    labels = ImageDraw.Draw(sheet)
    for row, surface in enumerate(("face", "list", "card")):
        for column, mode in enumerate(args.modes):
            capture = Image.open(args.output / f"gate-{surface}-{mode}.png").convert("RGB")
            sheet.paste(
                capture.resize((233, 233), Image.Resampling.LANCZOS), (233 * column, 253 * row + 20)
            )
            labels.text((233 * column + 8, 253 * row + 4), f"{surface} | {mode}", fill="white")
    sheet.save(args.output / "gate-contact-sheet.png")
    for row in results:
        print(row["mode"], row["elapsed_ms"], row["visible_circle"])


if __name__ == "__main__":
    main()
