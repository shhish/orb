#!/usr/bin/env python3
"""Contact-sheet PNG helpers for the simulator.

The Design 1.0 sheet used to go through BMP: sips wrote 32-bit, top-down BMPs
that bmp_concat could not read, so the "PNG" outputs were actually BMP data
(or a multi-gigabyte sparse file from its negative-height size calculation).
This reads the rendered PNGs directly and writes a real RGB PNG, avoiding the
BMP round trip. The check mode fails on a malformed or entirely black image.
"""

import os
import struct
import sys
import zlib
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOR_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _chunks(data):
    if not data.startswith(PNG_SIGNATURE):
        raise ValueError("not a PNG")
    pos = len(PNG_SIGNATURE)
    while pos < len(data):
        if pos + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        end = pos + 8 + length
        if end + 4 > len(data):
            raise ValueError("truncated PNG chunk")
        payload = data[pos + 8 : end]
        crc = struct.unpack(">I", data[end : end + 4])[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != crc:
            raise ValueError(f"bad CRC in {kind!r} chunk")
        yield kind, payload
        pos = end + 4


def _unfilter(raw, width, height, channels):
    stride = width * channels
    if len(raw) != height * (stride + 1):
        raise ValueError("unexpected decompressed PNG size")
    rows = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        row = bytearray(raw[pos : pos + stride])
        pos += stride
        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                row[i] = (row[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                up = previous[i]
                upper_left = previous[i - channels] if i >= channels else 0
                estimate = left + up - upper_left
                pa = abs(estimate - left)
                pb = abs(estimate - up)
                pc = abs(estimate - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
                row[i] = (row[i] + predictor) & 0xFF
        else:
            raise ValueError(f"unsupported PNG filter {filter_type}")
        rows.append(row)
        previous = row
    return rows


def _row_to_rgb(row, color_type, width, palette):
    if color_type == 2:
        return bytes(row)
    rgb = bytearray(width * 3)
    if color_type == 6:
        for x in range(width):
            r, g, b, a = row[4 * x : 4 * x + 4]
            rgb[3 * x] = (r * a + 127) // 255
            rgb[3 * x + 1] = (g * a + 127) // 255
            rgb[3 * x + 2] = (b * a + 127) // 255
    elif color_type == 0:
        for x in range(width):
            value = row[x]
            rgb[3 * x : 3 * x + 3] = bytes((value, value, value))
    elif color_type == 4:
        for x in range(width):
            value, alpha = row[2 * x : 2 * x + 2]
            grey = (value * alpha + 127) // 255
            rgb[3 * x : 3 * x + 3] = bytes((grey, grey, grey))
    elif color_type == 3:
        for x in range(width):
            index = row[x]
            if 3 * index + 3 > len(palette):
                raise ValueError("PNG palette index out of range")
            rgb[3 * x : 3 * x + 3] = palette[3 * index : 3 * index + 3]
    else:
        raise ValueError(f"unsupported PNG colour type {color_type}")
    return bytes(rgb)


def decode(path):
    """Return (width, height, RGB888 bytes), compositing alpha over black."""
    data = Path(path).read_bytes()
    width = height = depth = color_type = interlace = None
    palette = b""
    idat = []
    for kind, payload in _chunks(data):
        if kind == b"IHDR":
            if len(payload) != 13:
                raise ValueError("bad IHDR")
            width, height, depth, color_type, compression, filter_method, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if compression != 0 or filter_method != 0:
                raise ValueError("unsupported PNG compression or filter method")
            if interlace != 0:
                raise ValueError("interlaced PNGs are not supported")
            if depth != 8:
                raise ValueError(f"unsupported PNG bit depth {depth}")
            if color_type not in COLOR_CHANNELS:
                raise ValueError(f"unsupported PNG colour type {color_type}")
        elif kind == b"PLTE":
            palette = payload
        elif kind == b"IDAT":
            idat.append(payload)
        elif kind == b"IEND":
            break
    if width is None or height is None:
        raise ValueError("PNG has no IHDR")
    if not idat:
        raise ValueError("PNG has no IDAT")
    if color_type == 3 and not palette:
        raise ValueError("indexed PNG has no palette")
    if width <= 0 or height <= 0:
        raise ValueError("PNG has no pixels")

    rows = _unfilter(zlib.decompress(b"".join(idat)), width, height, COLOR_CHANNELS[color_type])
    rgb = bytearray(width * height * 3)
    pos = 0
    for row in rows:
        converted = _row_to_rgb(row, color_type, width, palette)
        rgb[pos : pos + len(converted)] = converted
        pos += len(converted)
    return width, height, bytes(rgb)


def _chunk(kind, payload):
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def encode_rgb(width, height, scanlines):
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", ihdr)
        + _chunk(b"IDAT", zlib.compress(scanlines, 6))
        + _chunk(b"IEND", b"")
    )


def check(path):
    width, height, rgb = decode(path)
    if not any(rgb):
        raise ValueError("image is all black")
    return width, height, rgb


def concat(left_path, right_path, out_path):
    left_width, left_height, left = check(left_path)
    right_width, right_height, right = check(right_path)
    if (left_width, left_height) != (right_width, right_height):
        raise ValueError(
            f"size mismatch {left_width}x{left_height} vs {right_width}x{right_height}"
        )
    row_bytes = left_width * 3
    divider = b"\xff\xff\xff"
    scanlines = bytearray()
    for y in range(left_height):
        start = y * row_bytes
        scanlines.append(0)
        scanlines += left[start : start + row_bytes]
        scanlines += divider
        scanlines += right[start : start + row_bytes]
    png = encode_rgb(left_width * 2 + 1, left_height, bytes(scanlines))
    temporary = out_path + ".tmp"
    try:
        Path(temporary).write_bytes(png)
        os.replace(temporary, out_path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main(argv):
    if len(argv) >= 3 and argv[1] == "check":
        failed = False
        for path in argv[2:]:
            try:
                check(path)
            except (OSError, ValueError, zlib.error) as error:
                print(f"{path}: {error}", file=sys.stderr)
                failed = True
        return 1 if failed else 0
    if len(argv) == 5 and argv[1] == "concat":
        try:
            concat(argv[2], argv[3], argv[4])
        except (OSError, ValueError, zlib.error) as error:
            print(f"{argv[4]}: {error}", file=sys.stderr)
            return 1
        return 0
    print(
        "usage: png_tools.py check FILE...\n       png_tools.py concat LEFT.png RIGHT.png OUT.png",
        file=sys.stderr,
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
