#!/usr/bin/env python3
"""Pair each actual Faces 2.0 capture with its exported frame, offline.

Run with /opt/homebrew/bin/python3 (the locally installed Pillow interpreter).
The harness metrics receipt is authoritative, so stale PNGs are never included.
Every unique rendered image appears in an indexed review sheet at native size.
"""

import csv
import hashlib
import html
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    root = Path(__file__).resolve().parents[1]
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "sim/build/faces-layout"
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "sim/build/faces-contact"
    output.mkdir(parents=True, exist_ok=True)
    frames = root / "docs/design/2.0/png"
    font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 13)
    manifest = []
    for mode_dir in sorted(path for path in source.iterdir() if path.is_dir()):
        mode_output = output / mode_dir.name
        mode_output.mkdir(exist_ok=True)
        for stale_review in mode_output.glob("review-*.png"):
            stale_review.unlink()
        unique = {}
        with (mode_dir / "metrics.csv").open() as handle:
            rows = list(csv.DictReader(handle))
        for row in rows:
            scene, state = row["scene"], row["state"]
            reference_name = scene
            for family in ("Lock", "FaceGallery", "FaceColor", "FaceSlots"):
                if scene.startswith(family):
                    reference_name = family
                    break
            reference = frames / f"{reference_name}.png"
            shot = mode_dir / f"{scene}-{state}.png"
            left = Image.open(reference).convert("RGB")
            right = Image.open(shot).convert("RGB")
            if left.size != (466, 466) or right.size != (466, 466):
                raise ValueError(f"466px capture/reference required: {shot}")
            pair = Image.new("RGB", (933, 500), "#202020")
            pair.paste(left, (0, 34))
            pair.paste(right, (467, 34))
            draw = ImageDraw.Draw(pair)
            draw.line((466, 0, 466, 499), fill="white")
            draw.text((8, 6), f"Design 2.0: {reference_name}", font=font, fill="white")
            draw.text((475, 6), f"{scene} / {state} / {mode_dir.name}", font=font, fill="white")
            target = mode_output / shot.name
            pair.save(target)
            digest = hashlib.sha256(right.tobytes()).hexdigest()
            manifest.append((str(target.relative_to(output)), reference_name, digest))
            unique.setdefault(digest, pair)
        # Native-size pairs keep small ink legible during visual review. Each
        # unique PNG appears in exactly one four-pair sheet; all pairs remain.
        values = list(unique.values())
        for start in range(0, len(values), 4):
            batch = values[start : start + 4]
            sheet = Image.new("RGB", (1866, 1000), "#303030")
            for index, pair in enumerate(batch):
                sheet.paste(pair, ((index % 2) * 933, (index // 2) * 500))
            sheet.save(mode_output / f"review-{start // 4 + 1:02d}.png")
        print(f"{mode_dir.name}: {len(rows)} frame-left pairs, {len(values)} unique renders")
    with (output / "manifest.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(("contact_sheet", "reference_frame", "simulator_pixel_sha256"))
        writer.writerows(manifest)
    links = "\n".join(
        f'<li><a href="{html.escape(path)}">{html.escape(path)}</a></li>' for path, _, _ in manifest
    )
    (output / "index.html").write_text(
        "<!doctype html><meta charset='utf-8'><title>Orb Faces review</title>"
        f"<h1>Design 2.0 left / simulator right</h1><ul>{links}</ul>"
    )


if __name__ == "__main__":
    main()
