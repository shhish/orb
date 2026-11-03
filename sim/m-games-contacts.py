#!/usr/bin/env python3
"""Pair every game matrix render with its supplied frame; no network access.

Run after CTest: python3 sim/m-games-contacts.py
There are no exported pause/tutorial/help frames in Design 2.0. Those pairs
explicitly show the corresponding Design 1.0 arena as context, not a purported
pixel-exact target. Each full-resolution pair has the frame on the left.
"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).parent / "build")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = args.build / "m-games-contacts"
    destination.mkdir(parents=True, exist_ok=True)
    for manifest in sorted((args.build / "m-games-layout").glob("*/manifest.tsv")):
        mode = manifest.parent.name
        mode_directory = destination / mode
        mode_directory.mkdir(exist_ok=True)
        pairs: list[tuple[str, Image.Image]] = []
        for row in manifest.read_text().splitlines():
            name, reference = row.split("\t")
            version, frame = reference.split("/")
            target = root / "docs" / "design" / version / "png" / f"{frame}.png"
            capture = manifest.parent / f"{name}.png"
            pair = Image.new("RGB", (932, 506), "#161616")
            draw = ImageDraw.Draw(pair)
            context = " (arena context; no exported chrome frame)" if version == "1.0" else ""
            draw.text((10, 5), f"Frame {reference}{context}", fill="white")
            draw.text((476, 5), f"Simulator: {name} / {mode}", fill="white")
            pair.paste(Image.open(target).convert("RGB").resize((466, 466)), (0, 40))
            pair.paste(Image.open(capture).convert("RGB"), (466, 40))
            pair.save(mode_directory / f"{name}.png")
            pairs.append((name, pair))
        # Bounded overview pages retain readable ordering; full pairs provide
        # device-pixel inspection without fitting hundreds of states to a page.
        for page in range(0, len(pairs), 12):
            selection = pairs[page : page + 12]
            overview = Image.new("RGB", (932, 253 * ((len(selection) + 1) // 2)), "#161616")
            for index, (_, pair) in enumerate(selection):
                overview.paste(pair.resize((466, 253)), ((index % 2) * 466, (index // 2) * 253))
            overview.save(destination / f"{mode}-{page // 12 + 1:02}.png")
        print(f"{mode}: {len(pairs)} inspected-size pairs in {mode_directory}")


if __name__ == "__main__":
    main()
