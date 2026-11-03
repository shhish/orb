#!/usr/bin/env python3
"""Design 2.0 frame-left / simulator-right sheets for the system matrix.

Run with the locally installed Pillow Python, e.g. /opt/homebrew/bin/python3.
References absent from 2.0 use Settings as a *kit reference*, explicitly named
in the image and manifest. No exact missing-screen design is implied.
"""

import csv
import hashlib
import html
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    root = Path(__file__).resolve().parents[1]
    source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "sim/build/m-system-layout"
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else root / "sim/build/m-system-contact"
    output.mkdir(parents=True, exist_ok=True)
    frames = root / "docs/design/2.0/png"
    font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 13)
    small_font = ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 11)
    manifest = []
    for mode_dir in sorted(path for path in source.iterdir() if path.is_dir()):
        mode_output = output / mode_dir.name
        mode_output.mkdir(exist_ok=True)
        for old in mode_output.glob("*.png"):
            old.unlink()
        unique = {}
        # Prior iterations can have more scroll pages. The current receipt,
        # rather than a directory glob, is authoritative for this capture run.
        with (mode_dir / f"metrics-{mode_dir.name}.csv").open() as handle:
            shots = [
                mode_dir / f"{r['scene']}-{r['state']}-{r['mode']}-{r['page']}.png"
                for r in csv.DictReader(handle)
            ]
        entries = [(shot, shot.name) for shot in shots]
        supplemental = root / "sim/build/system-sheets" / mode_dir.name
        # The sheet harness owns OTA progress/error and modal Power snapshots.
        # Retain its independent geometry checks while presenting its renders
        # through the same frame-left review workflow.
        for shot in sorted(supplemental.glob("*.png")):
            if shot.name.startswith(("Updates-", "PowerSheet")):
                entries.append((shot, "Sheet-" + shot.name))
        spoken = root / "sim/build/sound-ui" / mode_dir.name / "SpokenReply.png"
        if spoken.exists():
            entries.append((spoken, "Sound-SpokenReply.png"))
        for shot, target_name in sorted(entries):
            scene = shot.stem.split("-", 1)[0]
            reference = frames / f"{scene}.png"
            exact = reference.exists()
            if not exact:
                reference = frames / "Settings.png"
            left = Image.open(reference).convert("RGB").resize((466, 466))
            right = Image.open(shot).convert("RGB")
            if right.size != (466, 466):
                raise ValueError(f"bad simulator dimensions: {shot}")
            pair = Image.new("RGB", (933, 506), "#202020")
            pair.paste(left, (0, 40))
            pair.paste(right, (467, 40))
            draw = ImageDraw.Draw(pair)
            draw.line((466, 0, 466, 505), fill="white")
            draw.text(
                (8, 4),
                f"Design 2.0: {scene}" if exact else "Design 2.0 Settings: kit reference",
                font=font,
                fill="white",
            )
            draw.text((475, 4), shot.stem, font=font, fill="white")
            if not exact:
                draw.text(
                    (8, 21),
                    "No screen-specific 2.0 frame supplied",
                    font=small_font,
                    fill="#CCCCCC",
                )
            target = mode_output / target_name
            pair.save(target)
            digest = hashlib.sha256(right.tobytes()).hexdigest()
            manifest.append(
                (str(target.relative_to(output)), str(reference.relative_to(root)), exact, digest)
            )
            unique.setdefault(digest, (shot.stem, pair))
        # Every byte-distinct PNG is included once in an indexed review sheet.
        # Duplicate states are still emitted individually above and share hashes.
        tiles = list(unique.values())
        for start in range(0, len(tiles), 12):
            batch = tiles[start : start + 12]
            overview = Image.new("RGB", (2 * 483, ((len(batch) + 1) // 2) * 273), "#303030")
            draw = ImageDraw.Draw(overview)
            for index, (name, pair) in enumerate(batch):
                x, y = (index % 2) * 483, (index // 2) * 273
                overview.paste(pair.resize((467, 253), Image.Resampling.LANCZOS), (x, y + 20))
                draw.text((x + 3, y + 3), name, font=small_font, fill="white")
            overview.save(mode_output / f"review-{start // 12 + 1:02d}.png")
        print(f"{mode_dir.name}: {len(entries)} pairs; {len(tiles)} unique simulator frames")
    with (output / "manifest.csv").open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            ("contact_sheet", "reference", "exact_screen_frame", "simulator_pixel_sha256")
        )
        writer.writerows(manifest)
    links = "\n".join(
        f'<li><a href="{html.escape(row[0])}">{html.escape(row[0])}</a></li>' for row in manifest
    )
    (output / "index.html").write_text(
        f"<!doctype html><meta charset='utf-8'><title>Orb system review</title><h1>Frame left / simulator right</h1><ul>{links}</ul>"
    )


if __name__ == "__main__":
    main()
