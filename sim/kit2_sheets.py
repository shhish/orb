#!/usr/bin/env python3
"""Pair every kit state with its local design-family frame, without downloads.

References are family comparisons for primitive regression pages, not a claim
that the gallery recreates an application. Each pair preserves the exact frame
pixels on the left; its simulator frame is on the right. manifest.tsv labels
every pair and the four-pair review pages.
"""

import argparse
import csv
from pathlib import Path

import png_tools


def reference(name: str) -> Path:
    if name.startswith(("stopwatch", "laps16")):
        return Path("docs/design/1.0/png/Stopwatch.png")
    if name.startswith("hero") or name.startswith("disc"):
        return Path("docs/design/2.0/png/HomeRooms.png")
    if name.startswith(("rows", "switch")):
        return Path("docs/design/2.0/png/Settings.png")
    if name.startswith(("glyph", "rings", "rim-inset")):
        return Path("docs/design/2.0/png/Timer.png")
    if name.startswith(("rim-slider", "control-slider")):
        return Path("docs/design/2.0/png/HomeLight.png")
    if name == "buttons":
        return Path("docs/design/2.0/png/Play.png")
    return Path("docs/design/2.0/png/HomeRoom.png")


def write_rgb(path: Path, width: int, height: int, rgb: bytearray) -> None:
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        scanlines.extend(rgb[y * width * 3 : (y + 1) * width * 3])
    path.write_bytes(png_tools.encode_rgb(width, height, scanlines))


def pair(frame: Path, actual: Path, output: Path) -> None:
    fw, fh, left = png_tools.decode(frame)
    aw, ah, right = png_tools.decode(actual)
    assert (fw, fh, aw, ah) == (466, 466, 466, 466)
    rgb = bytearray(933 * 466 * 3)
    for y in range(466):
        start = y * 933 * 3
        rgb[start : start + 1398] = left[y * 1398 : (y + 1) * 1398]
        rgb[start + 1398 : start + 1401] = b"\x70\x70\x70"
        rgb[start + 1401 : start + 2799] = right[y * 1398 : (y + 1) * 1398]
    write_rgb(output, 933, 466, rgb)
    # Readback proves exact left/right identity, not just successful encoding.
    _, _, pixels = png_tools.decode(output)
    for y in range(466):
        assert pixels[y * 2799 : y * 2799 + 1398] == left[y * 1398 : (y + 1) * 1398]
        assert pixels[y * 2799 + 1401 : (y + 1) * 2799] == right[y * 1398 : (y + 1) * 1398]


def page(paths: list[Path], output: Path) -> None:
    width, height = 1866, 932
    rgb = bytearray(width * height * 3)
    for slot, path in enumerate(paths):
        w, h, pixels = png_tools.decode(path)
        for y in range(h):
            start = ((slot // 2 * 466 + y) * width + slot % 2 * 933) * 3
            rgb[start : start + w * 3] = pixels[y * w * 3 : (y + 1) * w * 3]
    write_rgb(output, width, height, rgb)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("sim/build/kit-gallery"))
    parser.add_argument("--out", type=Path, default=Path("sim/build/kit2-sheets"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for mode in ("default", "ax3", "ax3-bold-rt"):
        output = args.out / mode
        output.mkdir(exist_ok=True)
        pairs = []
        for actual in sorted((args.source / mode).glob("*.png")):
            name = actual.name.removesuffix(f"-{mode}.png")
            frame = reference(name)
            dest = output / f"{name}-compare.png"
            pair(frame, actual, dest)
            pairs.append(dest)
            rows.append(
                [
                    mode,
                    name,
                    str(frame),
                    str(actual),
                    str(dest),
                    str(output / f"contact-{(len(pairs) - 1) // 4 + 1:02}.png"),
                    str((len(pairs) - 1) % 4 + 1),
                    "family reference / kit regression",
                ]
            )
        for offset in range(0, len(pairs), 4):
            page(pairs[offset : offset + 4], output / f"contact-{offset // 4 + 1:02}.png")
    with (args.out / "manifest.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t")
        writer.writerow(
            [
                "mode",
                "state",
                "reference-left",
                "simulator-right",
                "pair",
                "page",
                "slot",
                "comparison",
            ]
        )
        writer.writerows(rows)
    print(f"Verified {len(rows)} exact frame-left/simulator-right pairs in {args.out}")


if __name__ == "__main__":
    main()
