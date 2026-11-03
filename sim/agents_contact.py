#!/usr/bin/env python3
"""Render Design 2.0 Agents fixtures and compare every state with its frame."""

import argparse
import json
import os
import re
import subprocess
from pathlib import Path

from png_tools import decode, encode_rgb

MODES = ("default", "ax3", "ax3-bold-rt")
BASES = (
    "Agents",
    "SessionLive",
    "Talk",
    "TalkSend",
    "Usage",
    "Sessions",
    "Dot",
    "Approval",
    "Question",
    "TimeSave",
    "SendQueued",
    "SendDelivered",
    "SendFailed",
    "VoiceIdle",
    "VoiceTranscribing",
    "VoiceNoSpeech",
    "VoiceTouchCapture",
)


def checked_image(path: Path) -> tuple[bytes, tuple]:
    width, height, pixels = decode(path)
    extrema = tuple((min(pixels[c::3]), max(pixels[c::3])) for c in range(3))
    if (width, height) != (466, 466):
        raise ValueError(f"Unexpected size for {path}: {(width, height)}")
    if not any(high > 0 and low != high for low, high in extrema):
        raise ValueError(f"Blank image: {path}, extrema={extrema}")
    return pixels, extrema


def frame_for(root: Path, name: str) -> Path:
    base = next((b for b in sorted(BASES, key=len, reverse=True) if name.startswith(b)), "TalkSend")
    if base in {"Agents", "Usage"}:
        return root / "docs/design/2.0/png" / f"{base}.png"
    # The 2.0 export defines only Agents and Usage in this stream. Preserve the
    # approved 1.0 content frame for the remaining screens, then apply Main's
    # new kit geometry; record this limitation instead of inventing a frame.
    if base.startswith("Voice"):
        base = "Talk"
    if base in {"Question", "TimeSave"}:
        base = "Approval" if base == "Question" else "TalkSend"
    reference = root / "docs/design/1.0/png" / f"{base}.png"
    return reference if reference.exists() else root / "docs/design/1.0/png/TalkSend.png"


def pair_pixels(left: bytes, right: bytes) -> bytes:
    pixels = bytearray()
    for row in range(466):
        start = row * 466 * 3
        pixels += (
            b"\0" + left[start : start + 466 * 3] + b"\x70\x70\x70" + right[start : start + 466 * 3]
        )
    return bytes(pixels)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("sim/build/orb_agents_sim"))
    parser.add_argument("--out", type=Path, default=Path("sim/build/agents-design2"))
    parser.add_argument("--skip-render", action="store_true")
    parser.add_argument("--mode", choices=MODES, action="append")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = args.out.resolve()
    binary = args.binary.resolve()
    manifest = {}
    failed = []
    status = {}
    memory = {}
    for mode in args.mode or MODES:
        directory = output / mode
        directory.mkdir(parents=True, exist_ok=True)
        if not args.skip_render:
            for stale in directory.glob("*.png"):
                stale.unlink()
            result = subprocess.run(
                ["nice", "-n", "10", str(binary), str(directory), mode],
                check=False,
                cwd=root,
                env={**os.environ, "SDL_VIDEODRIVER": "dummy"},
                capture_output=True,
                text=True,
            )
            (output / f"{mode}.log").write_text(result.stdout + result.stderr)
            if result.returncode:
                failed.append(f"{mode}: simulator exit {result.returncode}")
        required = list(BASES) + [
            f"{base}{state}" for base in BASES for state in ("Long", "Empty", "Offline", "Error")
        ]
        for scene in required:
            if not (directory / f"{scene}.png").exists():
                failed.append(f"{mode}: missing {scene}.png")
        log = (output / f"{mode}.log").read_text() if (output / f"{mode}.log").exists() else ""
        status[mode] = {
            "layout_failures": [line for line in log.splitlines() if line.startswith("LAYOUT ")],
            "behaviour_blockers": [
                line for line in log.splitlines() if line.startswith("BEHAVIOUR ")
            ],
            "check_summary": [
                line for line in log.splitlines() if "wire/render/input checks passed" in line
            ],
            "scroll_summary": [line for line in log.splitlines() if line.startswith("SCROLL ")],
        }
        pattern = r"^MEMORY (\w+) retained=(\d+) shared-glass=(\d+) private=(\d+)$"
        current = {
            name: dict(zip(("retained", "shared_glass", "private"), map(int, values)))
            for name, *values in re.findall(pattern, log, re.MULTILINE)
        }
        baseline_path = root / "sim/build/agents-baseline" / f"{mode}.log"
        baseline = (
            {
                name: dict(zip(("retained", "shared_glass", "private"), map(int, values)))
                for name, *values in re.findall(pattern, baseline_path.read_text(), re.MULTILINE)
            }
            if baseline_path.exists()
            else {}
        )
        memory[mode] = {
            name: {
                "after": values,
                "before": baseline.get(name),
                "private_delta": values["private"] - baseline[name]["private"]
                if name in baseline
                else None,
            }
            for name, values in current.items()
        }
        overview = bytearray()
        page = 0
        row_count = 0
        for path in sorted(directory.glob("*.png")):
            if path.stem.endswith("-pair"):
                continue
            render, extrema = checked_image(path)
            reference = frame_for(root, path.stem)
            design, design_extrema = checked_image(reference)
            pair = pair_pixels(design, render)
            (directory / f"{path.stem}-pair.png").write_bytes(encode_rgb(933, 466, pair))
            if row_count:
                overview += (b"\0" + b"\x11" * (933 * 3)) * 8
            overview += pair
            manifest[f"{mode}/{path.stem}"] = {
                "sheet": f"contact-{mode}-{page:02}.png",
                "row": row_count,
                "size": [466, 466],
                "render_getextrema": extrema,
                "design_getextrema": design_extrema,
                "reference": str(reference.relative_to(root)),
            }
            row_count += 1
            if row_count == 8:
                (output / f"contact-{mode}-{page:02}.png").write_bytes(
                    encode_rgb(933, row_count * 474 - 8, bytes(overview))
                )
                page += 1
                row_count = 0
                overview = bytearray()
        if row_count:
            (output / f"contact-{mode}-{page:02}.png").write_bytes(
                encode_rgb(933, row_count * 474 - 8, bytes(overview))
            )
    (output / "image-validation.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "validation-status.json").write_text(json.dumps(status, indent=2) + "\n")
    (output / "memory-comparison.json").write_text(json.dumps(memory, indent=2) + "\n")
    print(
        f"Verified {len(manifest)} nonblank 466×466 renders and lossless frame-left pairs: {output}"
    )
    if failed:
        raise SystemExit("\n".join(failed))


if __name__ == "__main__":
    main()
