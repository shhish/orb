"""Build/inspect the bounded, versioned payload signed as Orb assets (not a trust check).

Secure Boot v2 signs the complete 4 KiB-padded envelope. The trailing signature sector
is supplied by espsecure/sign-app.sh; only cryptographic verification grants trust.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

MAGIC = b"ORBASSET"
HEADER = struct.Struct("<8sIIIIII32s")
SECTOR = 4096
LIMIT = 8 * 1024 * 1024
BUNDLE = struct.Struct("<8sIIIIII")


def bundle(fonts: bytes, sounds: bytes) -> bytes:
    """V2 has explicit bounded sections; font offsets remain section-relative."""
    font_offset = BUNDLE.size
    sound_offset = (font_offset + len(fonts) + 3) & ~3
    result = BUNDLE.pack(
        b"ORBBNDL2",
        2,
        font_offset,
        len(fonts),
        sound_offset,
        len(sounds),
        sound_offset + len(sounds),
    )
    return result + fonts + bytes(sound_offset - font_offset - len(fonts)) + sounds


def sections(payload: bytes, kind: int) -> dict:
    if kind == 1:
        fonts, sounds = payload, b""
    elif kind == 2 and len(payload) >= BUNDLE.size:
        magic, schema, fo, fs, so, ss, total = BUNDLE.unpack_from(payload)
        if (
            magic != b"ORBBNDL2"
            or schema != 2
            or total != len(payload)
            or fo != BUNDLE.size
            or fs < 48
            or so < fo + fs
            or so % 4
            or ss < 24
            or so > total
            or ss != total - so
        ):
            raise ValueError("invalid assets section bounds")
        fonts, sounds = payload[fo : fo + fs], payload[so : so + ss]
        if (
            sounds[:8] != b"ORBSOUND"
            or struct.unpack_from("<I", sounds, 8)[0] != 1
            or struct.unpack_from("<I", sounds, 20)[0] != ss
        ):
            raise ValueError("invalid sound section")
    else:
        raise ValueError("unsupported assets sections")
    if fonts[:8] != b"ORBFONTS" or struct.unpack_from("<I", fonts, 32)[0] != len(fonts):
        raise ValueError("assets payload is not a complete font pack")
    return {"font_bytes": len(fonts), "sound_bytes": len(sounds)}


def inspect(data: bytes, *, signed: bool = False) -> dict:
    if len(data) < HEADER.size:
        raise ValueError("truncated assets header")
    magic, schema, header_size, size, signed_size, version, kind, digest = HEADER.unpack_from(data)
    if magic != MAGIC or schema not in (1, 2) or header_size != HEADER.size or kind != schema:
        raise ValueError("unsupported assets header")
    if not 1 <= version <= 0xFFFFFFFF or not 48 <= size <= LIMIT - 2 * SECTOR:
        raise ValueError("assets version or payload length out of range")
    padded = (HEADER.size + size + SECTOR - 1) // SECTOR * SECTOR
    if signed_size != padded or signed_size + SECTOR > LIMIT:
        raise ValueError("assets signed length out of range")
    expected = signed_size + (SECTOR if signed else 0)
    if len(data) != expected:
        raise ValueError(f"assets image must contain exactly {expected} bytes")
    payload = data[HEADER.size : HEADER.size + size]
    section_info = sections(payload, kind)
    if hashlib.sha256(payload).digest() != digest:
        raise ValueError("assets payload SHA-256 mismatch")
    if data[HEADER.size + size : signed_size] != b"\xff" * (signed_size - HEADER.size - size):
        raise ValueError("assets padding is not erased flash")
    return {
        **section_info,
        "schema": schema,
        "content_version": version,
        "payload_size": size,
        "signed_size": signed_size,
        "image_size": signed_size + SECTOR,
        "payload_sha256": digest.hex(),
    }


def envelope(pack: bytes, version: int, sounds: bytes | None = None) -> bytes:
    if not 1 <= version <= 0xFFFFFFFF:
        raise ValueError("content version must be 1..4294967295")
    schema = 2 if sounds is not None else 1
    if sounds is not None:
        pack = bundle(pack, sounds)
    padded = (HEADER.size + len(pack) + SECTOR - 1) // SECTOR * SECTOR
    header = HEADER.pack(
        MAGIC,
        schema,
        HEADER.size,
        len(pack),
        padded,
        version,
        schema,
        hashlib.sha256(pack).digest(),
    )
    result = header + pack + b"\xff" * (padded - HEADER.size - len(pack))
    inspect(result)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    build = sub.add_parser("envelope")
    build.add_argument("pack", type=Path)
    build.add_argument("--sounds", type=Path)
    build.add_argument("--version", type=int, required=True)
    build.add_argument("--output", "-o", type=Path, required=True)
    check = sub.add_parser("inspect")
    check.add_argument("image", type=Path)
    check.add_argument("--signed", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "envelope":
            image = envelope(
                args.pack.read_bytes(),
                args.version,
                args.sounds.read_bytes() if args.sounds else None,
            )
            args.output.write_bytes(image)
            result = inspect(image)
        else:
            result = inspect(args.image.read_bytes(), signed=args.signed)
    except (OSError, ValueError) as error:
        parser.exit(1, f"assets: {error}\n")
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
