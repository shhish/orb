#!/usr/bin/env python3
"""Deterministic Orb visual/semantic capture and strict, dependency-free comparison."""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

PROFILES = ("default", "ax3", "ax3-bold-rt", "reduce-motion")
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
FORMAT_VERSION = 1


def read_json(path: Path):
    def invalid(value):
        raise ValueError(f"non-finite JSON value: {value}")

    return json.loads(path.read_text(encoding="utf-8"), parse_constant=invalid)


def write_json(path: Path, value) -> None:
    path.write_text(
        json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    """RGB8, filter-none PNG with no clock, encoder version or other metadata."""
    if len(rgb) != width * height * 3:
        raise ValueError("incorrect RGB image size")
    stride = width * 3
    rows = b"".join(b"\0" + rgb[y * stride : (y + 1) * stride] for y in range(height))
    path.write_bytes(
        PNG_MAGIC
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(rows, 9))
        + _chunk(b"IEND", b"")
    )


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def read_png(path: Path, *, decode: bool = True) -> tuple[int, int, bytes]:
    """Decode noninterlaced 8-bit PNG; validate CRCs rather than trusting IHDR."""
    data = path.read_bytes()
    if data[:8] != PNG_MAGIC:
        raise ValueError(f"{path.name}: invalid PNG signature")
    offset, compressed, palette = 8, bytearray(), None
    width = height = channels = 0
    color = -1
    ended = False
    while offset < len(data):
        if offset + 12 > len(data):
            raise ValueError(f"{path.name}: truncated PNG chunk")
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        end = offset + 8 + length
        if end + 4 > len(data):
            raise ValueError(f"{path.name}: truncated PNG payload")
        payload = data[offset + 8 : end]
        crc = struct.unpack_from(">I", data, end)[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != crc:
            raise ValueError(f"{path.name}: invalid {kind!r} CRC")
        if kind == b"IHDR":
            if width or offset != 8 or length != 13:
                raise ValueError("invalid PNG header order")
            width, height, depth, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if not width or not height or width * height > 16_777_216:
                raise ValueError("invalid or excessive PNG dimensions")
            if depth != 8 or compression or filtering or interlace:
                raise ValueError("parity expects noninterlaced 8-bit PNG")
            channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color, 0)
            if not channels:
                raise ValueError("unsupported PNG color type")
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"PLTE":
            palette = payload
        elif kind == b"IEND":
            if length or end + 4 != len(data):
                raise ValueError("invalid PNG end/trailing bytes")
            ended = True
            break
        elif kind[0] & 32 == 0:
            raise ValueError(f"unsupported critical PNG chunk {kind!r}")
        offset = end + 4
    if not width or not ended:
        raise ValueError("incomplete PNG")
    stride = width * channels
    expected = height * (stride + 1)
    decompressor = zlib.decompressobj()
    raw = decompressor.decompress(compressed, expected + 1)
    if len(raw) != expected or not decompressor.eof or decompressor.unused_data:
        raise ValueError("invalid PNG raster size/stream")
    if any(raw[y * (stride + 1)] > 4 for y in range(height)):
        raise ValueError("invalid PNG row filter")
    # Even identical evidence must contain a valid compressed raster. Avoid
    # only the costly pixel reconstruction when hashing/validating captures.
    if not decode:
        return width, height, b""
    previous = bytearray(stride)
    rgb = bytearray(width * height * 3)
    for y in range(height):
        at = y * (stride + 1)
        filter_type, row = raw[at], bytearray(raw[at + 1 : at + 1 + stride])
        if filter_type > 4:
            raise ValueError("invalid PNG row filter")
        for x in range(stride):
            a = row[x - channels] if x >= channels else 0
            b = previous[x]
            c = previous[x - channels] if x >= channels else 0
            prediction = (0, a, b, (a + b) // 2, _paeth(a, b, c))[filter_type]
            row[x] = (row[x] + prediction) & 255
        target = y * width * 3
        if color == 2:
            rgb[target : target + width * 3] = row
        else:
            for x in range(width):
                source = x * channels
                if color == 3:
                    index = row[source] * 3
                    if palette is None or index + 3 > len(palette):
                        raise ValueError("invalid PNG palette index")
                    value = palette[index : index + 3]
                elif color in (0, 4):
                    value = bytes([row[source]]) * 3
                else:
                    value = row[source : source + 3]
                if color in (4, 6) and row[source + channels - 1] != 255:
                    raise ValueError("transparent PNG unsupported: compare panel RGB8 output")
                rgb[target + x * 3 : target + x * 3 + 3] = value
        previous = row
    return width, height, bytes(rgb)


def _safe_relative(value: str) -> Path:
    if not isinstance(value, str):
        raise ValueError("manifest paths must be strings")
    path = Path(value)
    if path.is_absolute() or not path.parts or any(p in (".", "..") for p in path.parts):
        raise ValueError(f"unsafe artifact path {value!r}")
    return path


def _files(root: Path) -> set[str]:
    files = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"symlink is not a parity artifact: {path}")
        if path.is_file():
            files.add(path.relative_to(root).as_posix())
    return files


def _run(binary: Path, *args: str) -> str:
    environment = dict(os.environ, TZ="UTC", LC_ALL="C", PYTHONHASHSEED="0")
    result = subprocess.run(
        [str(binary), *args],
        env=environment,
        check=False,
        capture_output=True,
        text=True,
        timeout=180,
    )
    if result.returncode:
        raise RuntimeError(
            f"orb_parity {' '.join(args[:2])} failed ({result.returncode}):\n"
            + result.stdout[-8000:]
            + result.stderr[-8000:]
        )
    return result.stdout


def compiler_metadata(binary: Path, repository: Path) -> dict:
    """Read generated compiler facts and effective per-target Ninja settings."""
    build = binary.parent
    cache_path = build / "CMakeCache.txt"
    cache = {}
    if cache_path.is_file():
        for line in cache_path.read_text().splitlines():
            match = re.fullmatch(r"([A-Z0-9_]+):[A-Z]+=(.*)", line)
            if match:
                cache[match[1]] = match[2]

    def normalized(value):
        # Build locations are not renderer inputs. Preserve flags and SDK identity.
        value = value.replace(str(build), "$BUILD").replace(str(repository), "$REPO")
        sdk = cache.get("CMAKE_OSX_SYSROOT", "")
        if sdk and Path(sdk).is_absolute():
            value = value.replace(sdk, "$SDK/" + Path(sdk).name)
        return " ".join(value.split())

    compiler = {}
    files = sorted((build / "CMakeFiles").glob("*/CMakeCCompiler.cmake"))
    if files:
        # Prefer the active cache version, even after a CMake downgrade leaves
        # a newer compiler-identification directory behind.
        def version(path):
            return tuple(
                int(part) if part.isdecimal() else 0 for part in path.parent.name.split(".")
            )

        active_version = ".".join(
            cache.get("CMAKE_CACHE_" + part + "_VERSION", "")
            for part in ("MAJOR", "MINOR", "PATCH")
        )
        active_file = build / "CMakeFiles" / active_version / "CMakeCCompiler.cmake"
        compiler_file = active_file if active_file in files else max(files, key=version)
        for key, value in re.findall(
            r'^set\((CMAKE_C_[A-Z0-9_]+) "?([^"\n)]*)"?\)', compiler_file.read_text(), re.M
        ):
            if key in {
                "CMAKE_C_COMPILER_ID",
                "CMAKE_C_COMPILER_VERSION",
                "CMAKE_C_COMPILER_ARCHITECTURE_ID",
                "CMAKE_C_COMPILER_TARGET",
                "CMAKE_C_PLATFORM_ID",
                "CMAKE_C_BYTE_ORDER",
                "CMAKE_C_SIZEOF_DATA_PTR",
            }:
                compiler[key.removeprefix("CMAKE_C_").lower()] = value
        system = compiler_file.parent / "CMakeSystem.cmake"
        if system.is_file():
            for key, value in re.findall(
                r'^set\((CMAKE_[A-Z0-9_]+) "?([^"\n)]*)"?\)', system.read_text(), re.M
            ):
                if key in {"CMAKE_SYSTEM_PROCESSOR", "CMAKE_SYSTEM_NAME"}:
                    compiler[key.removeprefix("CMAKE_").lower()] = value
    if cache.get("CMAKE_C_COMPILER"):
        compiler["executable"] = Path(cache["CMAKE_C_COMPILER"]).name
    ninja = build / "build.ninja"
    effective = {}
    configuration = cache.get("CMAKE_BUILD_TYPE", "")
    if ninja.is_file():
        content = ninja.read_text()
        match = re.search(r"^CONFIGURATION = (.*)$", content, re.M)
        if match:
            configuration = match[1]
        pattern = (
            r"^build CMakeFiles/(lvgl|orb|orb_games_view|orb_parity|parity_observed)\.dir/"
            r"[^\n]*: C_COMPILER__[^\n]*\n((?:  [^\n]*\n)*)"
        )
        unique = {}
        for target, variables in re.findall(pattern, content, re.M):
            values = {
                key.lower(): normalized(value)
                for key, value in re.findall(r"^  (CONFIG|DEFINES|FLAGS) = (.*)$", variables, re.M)
            }
            unique.setdefault(target, {})[json.dumps(values, sort_keys=True)] = values
        effective = {
            target: [values[key] for key in sorted(values)] for target, values in unique.items()
        }
    flags = [
        cache.get("CMAKE_C_FLAGS", ""),
        cache.get("CMAKE_C_FLAGS_" + configuration.upper(), ""),
    ]
    return {
        "compiler": compiler,
        "configuration": configuration,
        "effective_global_c_flags": normalized(" ".join(flags)),
        "target_settings": effective,
        "architecture_request": cache.get("CMAKE_OSX_ARCHITECTURES", ""),
        "deployment_target": cache.get("CMAKE_OSX_DEPLOYMENT_TARGET", ""),
    }


def capture_metadata(binary: Path) -> dict:
    """Record inputs to drawing, without fingerprinting refactorable production code."""
    source = Path(__file__).resolve().parent
    repository = source.parent
    fonts = repository / "firmware/components/orb_fonts"
    assets = [fonts / "pack/orb_fonts.pack", repository / "tools/sounds/out/palette.orbs"]
    assets.extend(fonts.glob("fonts/*.c"))
    assets.extend((repository / "firmware/components/orb_shell/fonts").glob("*.c"))
    resources = {}
    for path in sorted(assets):
        name = path.relative_to(repository).as_posix()
        resources[name] = (
            {"sha256": sha256(path), "bytes": path.stat().st_size}
            if path.is_file()
            else {"missing": True}
        )
    aggregate = hashlib.sha256(
        json.dumps(resources, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()
    configuration = re.sub(
        r"/\*.*?\*/|//[^\n]*", "", (source / "lv_conf.h").read_text(), flags=re.S
    )
    # Whitespace/comments cannot change this record; drawing/animation switches can.
    defines = {}
    for name, value in re.findall(r"^\s*#define\s+(\w+)[ \t]+([^\n]+)", configuration, re.M):
        if name.startswith(("LV_COLOR_", "LV_DRAW_", "LV_FONT_")) or name in {
            "LV_DEF_REFR_PERIOD",
            "LV_USE_OS",
            "LV_USE_FLOAT",
            "LV_USE_SNAPSHOT",
            "LV_USE_DRAW_SW",
            "LV_USE_DRAW_SW_ASM",
            "LV_USE_FONT_COMPRESSED",
            "LV_USE_ARCLABEL",
            "LV_USE_LODEPNG",
        }:
            defines[name] = " ".join(value.split())
    cache = binary.parent / "CMakeCache.txt"
    cmake = {}
    if cache.is_file():
        for line in cache.read_text().splitlines():
            match = re.fullmatch(
                r"(ORB_(?:RENDER_|DISPLAY_|UI_)[A-Z0-9_]+|ORB_OPEN_LATENCY_FIXES):BOOL=(.*)", line
            )
            if match:
                cmake[match[1]] = match[2]
    return {
        "resources": resources,
        "resources_sha256": aggregate,
        "renderer": {
            "lv_conf_defines": defines,
            "cmake_switches": cmake,
            "compilation": compiler_metadata(binary, repository),
        },
    }


def capture(args) -> int:
    started = time.monotonic()
    if args.out_dir.is_symlink():
        raise ValueError("capture output cannot be a symlink")
    output = args.out_dir.resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError("capture output must be absent or empty; use a new build directory")
    binary = args.binary.resolve()
    binary_fingerprint = sha256(binary)
    metadata = capture_metadata(binary)
    catalogue = json.loads(_run(binary, "--list"))
    if not isinstance(catalogue, list) or not catalogue:
        raise ValueError("orb_parity --list must return a nonempty JSON array")
    identifiers = [item["id"] for item in catalogue]
    if len(set(identifiers)) != len(identifiers):
        raise ValueError("duplicate scenario IDs")
    for identifier in identifiers:
        if "/" in identifier or "\\" in identifier or identifier in (".", "..", ""):
            raise ValueError(f"unsafe scenario ID {identifier!r}")
    catalogue.sort(key=lambda item: item["id"])
    selected = [item for item in catalogue if fnmatch.fnmatchcase(item["id"], args.filter)]
    if not selected:
        raise ValueError(f"no scenarios match {args.filter!r}")
    index, total = args.shard
    tasks = [(item["id"], profile) for item in selected for profile in PROFILES]
    tasks = [task for number, task in enumerate(tasks) if number % total == index - 1]
    if not tasks:
        raise ValueError("shard is empty")
    output.mkdir(parents=True, exist_ok=True)

    def execute(task):
        scenario, profile = task
        destination = output / scenario / profile
        destination.mkdir(parents=True)
        _run(binary, scenario, profile, str(destination))
        files = _files(destination)
        pngs = sorted(path for path in files if path.endswith(".png"))
        if not pngs:
            raise ValueError(f"{scenario}/{profile}: no frames produced")
        expected = set(pngs) | {str(Path(path).with_suffix(".json")) for path in pngs}
        if files != expected:
            raise ValueError(f"{scenario}/{profile}: unmatched files {sorted(files ^ expected)}")
        frames = []
        for name in pngs:
            png = destination / name
            trace = png.with_suffix(".json")
            read_json(trace)
            width, height, _ = read_png(png, decode=False)
            frames.append(
                {
                    "scenario": scenario,
                    "profile": profile,
                    "frame": Path(name).with_suffix("").as_posix(),
                    "png": png.relative_to(output).as_posix(),
                    "trace": trace.relative_to(output).as_posix(),
                    "sha256": sha256(png),
                    "trace_sha256": sha256(trace),
                    "width": width,
                    "height": height,
                }
            )
        return frames

    frames = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        # executor.map preserves catalogue ordering independently of completion order.
        for result in pool.map(execute, tasks):
            frames.extend(result)
    frames.sort(key=lambda item: (item["scenario"], item["profile"], item["frame"]))
    if metadata != capture_metadata(binary):
        raise ValueError(
            "capture assets or renderer configuration changed while scenarios were running"
        )
    if binary_fingerprint != sha256(binary):
        raise ValueError("orb_parity executable changed while scenarios were running")
    manifest = {
        "format_version": FORMAT_VERSION,
        "metadata": metadata,
        "profiles": list(PROFILES),
        "catalogue": catalogue,
        "selection": {"filter": args.filter, "shard": [index, total]},
        "frames": frames,
    }
    write_json(output / "manifest.json", manifest)
    print(
        f"Captured {len(tasks)} scenario/profile cases, {len(frames)} frames "
        f"in {time.monotonic() - started:.2f}s ({args.jobs} workers)."
    )
    return 0


def json_differences(old, new, path: str = "$") -> list[dict]:
    """Every changed leaf, including type, length and key-set changes."""
    if type(old) is not type(new):
        return [{"path": path, "base": old, "new": new}]
    if isinstance(old, dict):
        differences = []
        for key in sorted(old.keys() | new.keys()):
            child = f"{path}[{json.dumps(key, ensure_ascii=False)}]"
            if key not in old:
                differences.append({"path": child, "change": "added", "new": new[key]})
            elif key not in new:
                differences.append({"path": child, "change": "removed", "base": old[key]})
            else:
                differences.extend(json_differences(old[key], new[key], child))
        return differences
    if isinstance(old, list):
        differences = []
        for index in range(max(len(old), len(new))):
            child = f"{path}[{index}]"
            if index >= len(old):
                differences.append({"path": child, "change": "added", "new": new[index]})
            elif index >= len(new):
                differences.append({"path": child, "change": "removed", "base": old[index]})
            else:
                differences.extend(json_differences(old[index], new[index], child))
        return differences
    return [] if old == new else [{"path": path, "base": old, "new": new}]


def read_manifest(path: Path) -> dict:
    manifest = read_json(path)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("frames"), list):
        raise ValueError(f"{path}: manifest must be an object with a frames array")
    for frame in manifest["frames"]:
        if not isinstance(frame, dict):
            raise ValueError(f"{path}: frame must be an object")
        for field in ("scenario", "profile", "frame", "png", "trace", "sha256", "trace_sha256"):
            if not isinstance(frame.get(field), str) or not frame[field]:
                raise ValueError(f"{path}: frame {field} must be a nonempty string")
        for field in ("width", "height"):
            if type(frame.get(field)) is not int or frame[field] < 1:
                raise ValueError(f"{path}: frame {field} must be a positive integer")
        for field in ("sha256", "trace_sha256"):
            if not re.fullmatch("[0-9a-f]{64}", frame[field]):
                raise ValueError(f"{path}: invalid {field}")
    return manifest


def _validate_artifact(root: Path, manifest: dict) -> list[str]:
    errors = []
    if manifest.get("format_version") != FORMAT_VERSION:
        errors.append("unsupported format_version")
    expected = {"manifest.json"}
    seen = set()
    for frame in manifest.get("frames", []):
        key = (frame["scenario"], frame["profile"], frame["frame"])
        if key in seen:
            errors.append(f"duplicate frame {key}")
        seen.add(key)
        for field, digest in (("png", "sha256"), ("trace", "trace_sha256")):
            path = root / _safe_relative(frame[field])
            if frame[field] in expected:
                errors.append(f"duplicate artifact path {frame[field]}")
            expected.add(frame[field])
            if not path.is_file():
                errors.append(f"missing {frame[field]}")
            elif sha256(path) != frame[digest]:
                errors.append(f"hash mismatch {frame[field]}")
        if (root / frame["png"]).is_file():
            try:
                width, height, _ = read_png(root / frame["png"], decode=False)
                if (width, height) != (frame["width"], frame["height"]):
                    errors.append(f"dimension mismatch {frame['png']}")
            except (ValueError, zlib.error) as error:
                errors.append(f"invalid PNG {frame['png']}: {error}")
        if (root / frame["trace"]).is_file():
            try:
                read_json(root / frame["trace"])
            except (ValueError, UnicodeError) as error:
                errors.append(f"invalid trace {frame['trace']}: {error}")
    actual = _files(root)
    errors.extend(f"unlisted file {name}" for name in sorted(actual - expected))
    errors.extend(f"missing file {name}" for name in sorted(expected - actual))
    if not manifest.get("frames"):
        errors.append("empty capture")
    return errors


def _pixel_difference(base: Path, new: Path, destination: Path) -> dict:
    bw, bh, old = read_png(base)
    nw, nh, current = read_png(new)
    width, height = max(bw, nw), max(bh, nh)
    difference = bytearray(width * height * 3)
    count, x1, y1, x2, y2 = 0, width, height, -1, -1
    for y in range(height):
        for x in range(width):
            old_pixel = old[(y * bw + x) * 3 : (y * bw + x + 1) * 3] if x < bw and y < bh else None
            new_pixel = (
                current[(y * nw + x) * 3 : (y * nw + x + 1) * 3] if x < nw and y < nh else None
            )
            if old_pixel == new_pixel:
                continue
            count += 1
            x1, y1, x2, y2 = min(x1, x), min(y1, y), max(x2, x), max(y2, y)
            at = (y * width + x) * 3
            # Full-bright red makes even a one-channel, one-level change visible.
            difference[at : at + 3] = b"\xff\x00\x00"
    if count:
        destination.parent.mkdir(parents=True, exist_ok=True)
        write_png(destination, width, height, bytes(difference))
    return {
        "count": count,
        "bbox": [x1, y1, x2, y2] if count else None,
        "base_dimensions": [bw, bh],
        "new_dimensions": [nw, nh],
    }


def compare(args) -> int:
    started = time.monotonic()
    base, new = args.base_dir.resolve(), args.new_dir.resolve()
    output = args.out.resolve() if args.out else new.parent / f"{base.name}-vs-{new.name}-diff"
    if output == base or output == new or base in output.parents or new in output.parents:
        raise ValueError("diff output must be outside both capture directories")
    if output.exists() and any(output.iterdir()):
        raise ValueError("diff output must be absent or empty")
    old_manifest, new_manifest = (
        read_manifest(base / "manifest.json"),
        read_manifest(new / "manifest.json"),
    )
    report = {
        "format_version": FORMAT_VERSION,
        "artifact_errors": {
            "base": _validate_artifact(base, old_manifest),
            "new": _validate_artifact(new, new_manifest),
        },
        "manifest_differences": json_differences(old_manifest, new_manifest),
        "pixel_differences": [],
        "trace_differences": [],
        "file_differences": [],
    }
    old_files, new_files = _files(base), _files(new)
    for missing in sorted(old_files - new_files):
        report["file_differences"].append({"path": missing, "change": "removed"})
    for added in sorted(new_files - old_files):
        report["file_differences"].append({"path": added, "change": "added"})
    for name in sorted(old_files & new_files):
        if (base / name).read_bytes() == (new / name).read_bytes():
            continue
        report["file_differences"].append({"path": name, "change": "bytes"})
        if name.endswith(".png"):
            destination = output / Path(name).with_suffix(".diff.png")
            try:
                pixels = _pixel_difference(base / name, new / name, destination)
                pixels.update(
                    {
                        "path": name,
                        "diff": destination.relative_to(output).as_posix()
                        if pixels["count"]
                        else None,
                    }
                )
            except (ValueError, zlib.error) as error:
                pixels = {"path": name, "error": str(error)}
            report["pixel_differences"].append(pixels)
        elif name.endswith(".json") and name != "manifest.json":
            try:
                differences = json_differences(read_json(base / name), read_json(new / name))
                report["trace_differences"].extend({"file": name, **item} for item in differences)
            except (ValueError, UnicodeError) as error:
                report["trace_differences"].append({"file": name, "error": str(error)})
    failed = bool(
        report["file_differences"]
        or report["manifest_differences"]
        or any(report["artifact_errors"].values())
    )
    output.mkdir(parents=True, exist_ok=True)
    write_json(output / "report.json", report)
    for side, errors in report["artifact_errors"].items():
        for error in errors:
            print(f"ARTIFACT {side}: {error}")
    for item in report["pixel_differences"]:
        print(f"PIXELS {item['path']}: {json.dumps(item, sort_keys=True)}")
    for item in report["trace_differences"]:
        print(f"TRACE {json.dumps(item, ensure_ascii=False, sort_keys=True)}")
    print(
        f"{'FAIL' if failed else 'PASS'}: {len(old_manifest.get('frames', []))} baseline frames; "
        f"{len(report['file_differences'])} file, "
        f"{len(report['manifest_differences'])} manifest, "
        f"{len(report['trace_differences'])} trace differences; "
        f"{time.monotonic() - started:.2f}s. Report: {output / 'report.json'}"
    )
    return int(failed)


def _shard(value: str) -> tuple[int, int]:
    try:
        index, count = (int(part) for part in value.split("/"))
    except ValueError as error:
        raise argparse.ArgumentTypeError("shard must be INDEX/COUNT (one-based)") from error
    if count < 1 or index < 1 or index > count:
        raise argparse.ArgumentTypeError("shard requires 1 <= INDEX <= COUNT")
    return index, count


def _workers(value: str) -> int:
    workers = int(value)
    if workers < 1 or workers > 4:
        raise argparse.ArgumentTypeError("use 1 to 4 workers on this Mac")
    return workers


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    capture_parser = commands.add_parser(
        "parity-capture", help="capture isolated deterministic cases"
    )
    capture_parser.add_argument("out_dir", type=Path)
    capture_parser.add_argument(
        "--binary", type=Path, default=Path(__file__).parent / "build" / "orb_parity"
    )
    capture_parser.add_argument("--jobs", type=_workers, default=4)
    capture_parser.add_argument("--shard", type=_shard, default=(1, 1), metavar="INDEX/COUNT")
    capture_parser.add_argument("--filter", default="*", help="scenario ID glob")
    capture_parser.set_defaults(run=capture)
    compare_parser = commands.add_parser(
        "parity-compare", help="strictly compare pixels, traces and inventory"
    )
    compare_parser.add_argument("base_dir", type=Path)
    compare_parser.add_argument("new_dir", type=Path)
    compare_parser.add_argument("--out", type=Path, help="diff artifacts outside captures")
    compare_parser.set_defaults(run=compare)
    args = parser.parse_args(argv)
    try:
        return args.run(args)
    except (
        OSError,
        ValueError,
        KeyError,
        TypeError,
        RuntimeError,
        subprocess.TimeoutExpired,
        zlib.error,
    ) as error:
        print(f"parity: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
