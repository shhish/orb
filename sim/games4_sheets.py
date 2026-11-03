"""Make labelled contact sheets from unmodified production simulator PNGs."""

import sys
from pathlib import Path

from png_tools import check, encode_rgb


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "sim/build/games4")
    sequences = {
        "marble": ["calibration", "rolling", "rewind", "release"],
        "beads": ["left", "crossing", "right", "clock"],
        "orbit": ["start", "switch", "switch-again", "playing"],
        "snake": ["start", "straight", "rim-warning", "rim-loss", "touch", "turn"],
        "pop": ["start", "hit", "combo", "playing"],
    }
    for game, scenes in sequences.items():
        width = 466 * len(scenes)
        height = 466 * 2 + 8
        sheet = bytearray(width * height * 3)
        for row, mode in enumerate(("default", "ax3-bold-rt")):
            for col, scene in enumerate(scenes):
                frame_width, frame_height, pixels = check(root / mode / f"{game}-{scene}.png")
                if (frame_width, frame_height) != (466, 466):
                    raise ValueError("unexpected simulator frame dimensions")
                for y in range(466):
                    start = ((row * 474 + y) * width + col * 466) * 3
                    sheet[start : start + 466 * 3] = pixels[y * 466 * 3 : (y + 1) * 466 * 3]
        path = root / f"{game}-contact.png"
        scanlines = b"".join(
            b"\0" + sheet[y * width * 3 : (y + 1) * width * 3] for y in range(height)
        )
        path.write_bytes(encode_rgb(width, height, scanlines))
        print(f"{path}: top Default, bottom AX3/Bold/opaque; " + ", ".join(scenes))


if __name__ == "__main__":
    main()
