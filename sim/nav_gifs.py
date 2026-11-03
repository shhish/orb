#!/usr/bin/env python3
"""Assemble captured real-input frames without inventing intermediate images."""

import argparse
import re
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        from PIL import Image
    except ImportError:
        print("Pillow unavailable; the complete PNG sequences remain in", args.directory)
        return

    groups = defaultdict(list)
    for path in args.directory.rglob("*.png"):
        match = re.fullmatch(r"(.+)-(\d+)\.png", path.name)
        if match:
            groups[path.parent / match[1]].append((int(match[2]), path))
    for destination, sequence in sorted(groups.items()):
        sequence.sort()
        frames = []
        for _, path in sequence:
            with Image.open(path) as source:
                frames.append(source.convert("RGB"))
        # One palette from the complete sequence prevents per-frame palette
        # changes from making stationary Inter text appear to shimmer.
        sample = Image.new("RGB", (116 * len(frames), 116), "black")
        for index, frame in enumerate(frames):
            sample.paste(frame.resize((116, 116)), (116 * index, 0))
        palette = sample.quantize(colors=256)
        indexed = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
        # GIF stores 10 ms units. 30/30/40 approximates the captured 33 ms cadence.
        durations = [40 if index % 3 == 2 else 30 for index in range(len(indexed))]
        output = destination.with_suffix(".gif")
        indexed[0].save(
            output,
            save_all=True,
            append_images=indexed[1:],
            duration=durations,
            loop=0,
            disposal=2,
            optimize=False,
        )
        print(f"{output}: {len(frames)} captured frames")


if __name__ == "__main__":
    main()
