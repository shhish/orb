#!/usr/bin/env python3
"""Compare actual snapshot band code with IDF's no-builtin-memcpy policy.

Host SIMD is represented by a contiguous memcpy; results reproduce software
overhead, not device timing. Historical sources are read using git show only.
"""

import argparse
import json
import os
import platform
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "firmware/components/orb_nav"


def run(arguments):
    return subprocess.run(arguments, check=True, capture_output=True, text=True).stdout


def benchmark(directory, revision, scalar_flags):
    source_directory = directory / revision.replace("/", "_")
    source_directory.mkdir()

    def source_text(name):
        return (
            (COMPONENT / name).read_text()
            if revision == "working-tree"
            else run(
                [
                    "git",
                    "-c",
                    "maintenance.auto=false",
                    "-C",
                    str(ROOT),
                    "show",
                    f"{revision}:firmware/components/orb_nav/{name}",
                ]
            )
        )

    compositor = source_text("orb_nav_compositor.c")
    names = ["orb_nav_compositor.c", "include/orb_nav_compositor.h"]
    if '#include "orb_nav_compositor_private.h"' in compositor:
        names.extend(["orb_nav_compositor_private.h", "orb_nav_compositor_slots.c"])
    for name in names:
        target = source_directory / Path(name).name
        target.write_text(compositor if name == "orb_nav_compositor.c" else source_text(name))
    results = {}
    for mode in ("timing", "count"):
        binary = source_directory / mode
        run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-D_POSIX_C_SOURCE=200809L",
                "-D__XTENSA__",
                "-D_FORTIFY_SOURCE=0",
                "-fno-builtin-memcpy",
                *scalar_flags,
                *(
                    ["-DORB_BENCH_COUNT", "-Dmemcpy=orb_nav_bench_memcpy"]
                    if mode == "count"
                    else []
                ),
                f"-I{source_directory}",
                f"-I{ROOT / 'firmware/components/orb_lv_simd/include'}",
                *(str(source_directory / Path(name).name) for name in names if name.endswith(".c")),
                str(COMPONENT / "tests/bench_compositor.c"),
                "-lm",
                "-o",
                str(binary),
            ]
        )
        results[mode] = json.loads(run([str(binary)]))
    for timing, count in zip(results["timing"]["cases"], results["count"]["cases"], strict=True):
        timing["tiny_memcpy_calls"] = count["tiny_memcpy_calls"]
        timing["span_calls"] = count["span_calls"]
    return results["timing"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revisions", nargs="+", default=["0131817", "7f7d893", "working-tree"])
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--scalar-c",
        action="store_true",
        help="disable host C-loop vectorization; memcpy still models the explicit S3 SIMD transfer",
    )
    options = parser.parse_args()
    compiler = run([os.environ.get("CC", "cc"), "--version"]).splitlines()[0]
    scalar_flags = (
        (
            ["-fno-vectorize", "-fno-slp-vectorize"]
            if "clang" in compiler
            else ["-fno-tree-vectorize"]
        )
        if options.scalar_c
        else []
    )
    results = {
        "host": platform.platform(),
        "compiler": compiler,
        "flags": " ".join(
            ["-O2", "-D__XTENSA__", "-D_FORTIFY_SOURCE=0", "-fno-builtin-memcpy", *scalar_flags]
        ),
        "model": "S3 SIMD transfer substituted by host memcpy; no PSRAM latency emulation",
        "revisions": {},
    }
    with tempfile.TemporaryDirectory(prefix="orb-compositor-bench-") as temporary:
        for revision in options.revisions:
            results["revisions"][revision] = benchmark(Path(temporary), revision, scalar_flags)
    encoded = json.dumps(results, indent=2) + "\n"
    if options.output:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(encoded)
    print(encoded, end="")


if __name__ == "__main__":
    main()
