"""Exercise the real ESP shared-cache callbacks without a board or threads."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
SOURCE = ROOT / "firmware/third_party/lvgl__lvgl/src/draw/sw/lv_draw_sw_blur_cache.c"


def function(source, signature):
    begin = source.index(signature)
    offset = source.index("{", begin) + 1
    depth = 1
    while depth:
        depth += (source[offset] == "{") - (source[offset] == "}")
        offset += 1
    return source[begin:offset] + "\n"


def main():
    source = SOURCE.read_text()
    fixture = Path(__file__).with_suffix(".c").read_text()
    callbacks = "\n".join(
        function(source, signature)
        for signature in (
            "static lv_blur_cache_entry_t * oldest_idle(",
            "static uint64_t cache_oldest(",
            "static bool cache_evict(",
        )
    )
    with tempfile.TemporaryDirectory(prefix="orb-blur-reclaim-") as temporary:
        directory = Path(temporary)
        path = directory / "reclaim.c"
        path.write_text(fixture.replace("/* PRODUCTION_CALLBACKS */", callbacks))
        binary = directory / "reclaim"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "clang")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
