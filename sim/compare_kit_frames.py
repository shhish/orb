#!/usr/bin/env python3
"""Compare HomeRoom's yellow rim title ink to the exported HTML screenshot.

Uses only the standard library. This checks device-pixel bounding geometry,
not browser/LVGL antialiasing or the independently coloured suffix.
"""

import argparse
import json
import struct
import zlib
from pathlib import Path


def paeth(left: int, above: int, corner: int) -> int:
    estimate = left + above - corner
    distances = [abs(estimate - value) for value in (left, above, corner)]
    return (left, above, corner)[distances.index(min(distances))]


def yellow_bounds(path: Path) -> tuple[int, int, int, int]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    position, compressed = 8, bytearray()
    width = height = depth = colour = 0
    while position < len(data):
        length = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4 : position + 8]
        payload = data[position + 8 : position + 8 + length]
        position += length + 12
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            if interlace:
                raise ValueError("interlaced PNG is unsupported")
        elif kind == b"IDAT":
            compressed.extend(payload)
    if depth != 8 or colour not in (2, 6) or (width, height) != (466, 466):
        raise ValueError("expected a 466×466 RGB/RGBA 8-bit PNG")
    channels = 3 if colour == 2 else 4
    stride = width * channels
    raw = zlib.decompress(compressed)
    previous = bytearray(stride)
    points = []
    for y in range(140):
        start = y * (stride + 1)
        filter_type = raw[start]
        row = bytearray(raw[start + 1 : start + 1 + stride])
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            above = previous[i]
            corner = previous[i - channels] if i >= channels else 0
            predictors = (0, left, above, (left + above) // 2, paeth(left, above, corner))
            row[i] = (row[i] + predictors[filter_type]) & 255
        for x in range(width):
            red, green, blue = row[x * channels : x * channels + 3]
            if red > 200 and green > 175 and blue < 80:
                points.append((x, y))
        previous = row
    if not points:
        raise ValueError(f"{path}: no yellow title ink")
    xs, ys = zip(*points, strict=True)
    return min(xs), min(ys), max(xs), max(ys)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("actual", type=Path)
    parser.add_argument("--max-error", type=int, default=2)
    args = parser.parse_args()
    reference, actual = yellow_bounds(args.reference), yellow_bounds(args.actual)
    error = max(abs(a - b) for a, b in zip(reference, actual, strict=True))
    print(json.dumps({"reference": reference, "actual": actual, "max_edge_error_px": error}))
    return int(error > args.max_error)


if __name__ == "__main__":
    raise SystemExit(main())
