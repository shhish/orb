#!/usr/bin/env python3
"""Offline Design 2.0 Home receipts; frame on left, firmware on right.

Only Python's standard library is used. The PNG reader intentionally accepts
8-bit non-interlaced RGB/RGBA (the checked-in design frames and simulator format).
The script never contacts HomeRelay and all inputs are synthetic.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import zlib
from functools import lru_cache
from pathlib import Path


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not PNG: {path}")
    offset, payload = 8, bytearray()
    width = height = channels = 0
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        block = data[offset + 8 : offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", block
            )
            if depth != 8 or colour not in (2, 6) or compression or filtering or interlace:
                raise ValueError(f"unsupported PNG format: {path}")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            payload.extend(block)
        offset += length + 12
    raw = zlib.decompress(payload)
    stride = width * channels
    prior, pixels = bytearray(stride), bytearray()
    for y in range(height):
        start = y * (stride + 1)
        mode = raw[start]
        row = bytearray(raw[start + 1 : start + 1 + stride])
        for x in range(stride) if mode else ():
            left = row[x - channels] if x >= channels else 0
            above = prior[x]
            diagonal = prior[x - channels] if x >= channels else 0
            if mode == 1:
                predictor = left
            elif mode == 2:
                predictor = above
            elif mode == 3:
                predictor = (left + above) // 2
            elif mode == 4:
                p = left + above - diagonal
                distances = (abs(p - left), abs(p - above), abs(p - diagonal))
                predictor = (left, above, diagonal)[distances.index(min(distances))]
            elif mode == 0:
                predictor = 0
            else:
                raise ValueError(f"unsupported PNG filter {mode}: {path}")
            row[x] = (row[x] + predictor) & 255
        if channels == 3:
            pixels.extend(row)
        else:
            for x in range(0, stride, channels):
                alpha = row[x + 3]
                pixels.extend((value * alpha // 255 for value in row[x : x + 3]))
        prior = row
    return width, height, bytes(pixels)


def write_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload))
        )

    stride = width * 3
    rows = b"".join(b"\0" + pixels[y * stride : (y + 1) * stride] for y in range(height))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


@lru_cache(maxsize=4)
def reference_pixels(path: Path) -> tuple[int, int, bytes]:
    """Decode the four immutable local frames once per rendering run."""
    return read_png(path)


def contact_sheet(reference: Path, actual: Path, output: Path) -> None:
    left_w, left_h, left = reference_pixels(reference)
    right_w, right_h, right = read_png(actual)
    gap = 16
    width, height = left_w + right_w + gap, max(left_h, right_h)
    pixels = bytearray(width * height * 3)
    for y in range(height):
        if y < left_h:
            pixels[y * width * 3 : (y * width + left_w) * 3] = left[
                y * left_w * 3 : (y + 1) * left_w * 3
            ]
        if y < right_h:
            start = (y * width + left_w + gap) * 3
            pixels[start : start + right_w * 3] = right[y * right_w * 3 : (y + 1) * right_w * 3]
    write_png(output, width, height, pixels)
    # A saved PNG is evidence only if neither half was resized or replaced.
    saved_w, saved_h, saved = read_png(output)
    if (saved_w, saved_h) != (width, height):
        raise ValueError(f"incorrect contact-sheet dimensions: {output}")
    for y in range(left_h):
        if (
            saved[y * width * 3 : (y * width + left_w) * 3]
            != left[y * left_w * 3 : (y + 1) * left_w * 3]
        ):
            raise ValueError(f"reference pixels changed: {output}")
    for y in range(right_h):
        start = (y * width + left_w + gap) * 3
        if saved[start : start + right_w * 3] != right[y * right_w * 3 : (y + 1) * right_w * 3]:
            raise ValueError(f"simulator pixels changed: {output}")


def review_galleries(captures: list[dict], out: Path) -> None:
    """Inspect every exact RGB tile; JPEG avoids preview-client PNG omissions."""
    index = []
    for profile in PROFILES:
        group = [capture for capture in captures if capture["profile"] == profile]
        for start in range(0, len(group), 12):
            page = group[start : start + 12]
            width, height = 4 * 466, 3 * 466
            pixels = bytearray(width * height * 3)
            for slot, capture in enumerate(page):
                tile_width, tile_height, tile = read_png(Path(capture["actual"]))
                x, y = slot % 4 * 466, slot // 4 * 466
                for row in range(tile_height):
                    offset = ((y + row) * width + x) * 3
                    pixels[offset : offset + tile_width * 3] = tile[
                        row * tile_width * 3 : (row + 1) * tile_width * 3
                    ]
            path = out / profile / f"gallery-{start // 12 + 1}.png"
            write_png(path, width, height, pixels)
            if shutil.which("sips"):
                subprocess.run(
                    [
                        "sips",
                        "-s",
                        "format",
                        "jpeg",
                        str(path),
                        "--out",
                        str(path.with_suffix(".jpg")),
                    ],
                    stdout=subprocess.DEVNULL,
                    check=True,
                )
            index.append(
                {
                    "gallery": str(path),
                    "row_major": [capture["scene"] for capture in page],
                    "contact_sheets": [capture["contact_sheet"] for capture in page],
                }
            )
    (out / "gallery-index.json").write_text(json.dumps(index, indent=2) + "\n")


# There are no separate thermostat/sensor/empty frames in Design 2.0. Those
# compositions use the HomeLight or HomeRooms family reference, explicitly
# labelled in the manifest; do not imply those are pixel-equivalent designs.
SCENES = {
    "HomeRooms": (
        "rooms-list-top",
        "rooms-list-off",
        "rooms-list-offline",
        "rooms-list-pending",
        "rooms-list-failed",
        "rooms-list-stale",
        "rooms-list-end",
        "design2-rooms-long",
        "design2-rooms-wide",
        "empty-home",
        "loading-home",
        "climate",
        "offline-climate",
        "design2-climate-empty",
    ),
    "HomeRoom": (
        "room-living",
        "room-office",
        "room-long-names",
        "busy-room",
        "busy-room-scrolled",
        "busy-room-last",
        "empty-room",
        "design2-room-offline",
        "climate-sensors",
        "offline-sensors",
        "design2-sensors-empty",
        "room-actions",
        "all-off-pending",
        "all-off-accepted",
        "pending-room",
        "all-off-failed",
    ),
    "HomeScenes": (
        "home-scenes",
        "scene-pending",
        "scene-failed",
        "scene-accepted",
        "design2-scenes-empty",
        "design2-scenes-offline",
        "design2-scenes-long",
    ),
    "HomeLight": (
        "light",
        "light-off",
        "colour",
        "white",
        "thermostat-f-heat",
        "thermostat-f-modes",
        "fan",
        "lock",
        "unlocked",
        "switch",
        "outlet",
        "temperature",
        "humidity",
        "motion",
        "contact",
        "pending-light",
        "pending-switch",
        "pending-lock",
        "offline-lock",
        "unknown-lock",
        "no-response",
        "error",
        "long-error",
        "wide-error",
        "long-scene",
        "wide-scene",
        "unavailable",
    ),
}
SCENES["HomeLight"] += tuple(
    f"detail-{kind}-{state}"
    for kind in (
        "light",
        "colour",
        "thermostat",
        "modes",
        "fan",
        "lock",
        "switch",
        "outlet",
        "temperature",
        "humidity",
        "motion",
        "contact",
    )
    for state in ("long", "roomlong", "empty", "offline", "error")
)

PROFILES = {"default": [], "ax3": ["ax3"], "ax3-bold-rt": ["ax3", "bold", "rt"]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sim = Path(__file__).resolve().parent
    parser.add_argument("--binary", type=Path, default=sim / "build/orb_home_sim")
    parser.add_argument("--out", type=Path, default=sim / "home_design2")
    parser.add_argument("--sheets-only", action="store_true")
    args = parser.parse_args()
    out = args.out.resolve()
    manifest = []
    failures = []
    for profile, flags in PROFILES.items():
        directory = out / profile
        directory.mkdir(parents=True, exist_ok=True)
        for reference, scenes in SCENES.items():
            frame = sim.parent / "docs/design/2.0/png" / f"{reference}.png"
            for scene in scenes:
                actual = directory / f"{scene}.png"
                log = directory / f"{scene}.log"
                if not args.sheets_only:
                    with log.open("w") as stream:
                        result = subprocess.run(
                            [
                                "nice",
                                "-n",
                                "10",
                                str(args.binary.resolve()),
                                scene,
                                str(actual),
                                *flags,
                            ],
                            stdout=stream,
                            stderr=subprocess.STDOUT,
                            check=False,
                        )
                    if result.returncode:
                        failures.append({"scene": scene, "profile": profile, "log": str(log)})
                        continue
                sheet = directory / f"{scene}-compare.png"
                if not actual.exists():
                    failures.append({"scene": scene, "profile": profile, "missing": str(actual)})
                    continue
                contact_sheet(frame, actual, sheet)
                manifest.append(
                    {
                        "scene": scene,
                        "profile": profile,
                        "reference": str(frame),
                        "comparison": "Design 2.0 family reference; not pixel-equivalent content",
                        "actual": str(actual),
                        "contact_sheet": str(sheet),
                        "log": str(log),
                    }
                )
    (out / "manifest.json").write_text(
        json.dumps(
            {
                "layout": "reference left, simulator right",
                "captures": manifest,
                "failures": failures,
            },
            indent=2,
        )
        + "\n"
    )
    review_galleries(manifest, out)
    print(f"Home Design 2.0: {len(manifest)} contact sheets, {len(failures)} failures: {out}")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
