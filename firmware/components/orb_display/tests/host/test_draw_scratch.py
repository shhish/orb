"""Compile the real scratch allocator against a capability-aware host heap."""

import itertools
import os
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
root = here.parents[4]
lvgl = root / "firmware/third_party/lvgl__lvgl"
options = (
    "ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH",
    "ORB_DISPLAY_INTERNAL_BLUR_SCRATCH",
    "ORB_DISPLAY_INTERNAL_DRAW_MASKS",
)
compiler = shlex.split(os.environ.get("CC", "clang"))
flags = shlex.split(
    os.environ.get("CFLAGS", "-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined")
)
with tempfile.TemporaryDirectory(prefix="orb-draw-scratch-") as temp:
    work = Path(temp)
    (work / "sdkconfig.h").write_text("")
    (work / "esp_attr.h").write_text("")
    (work / "esp_heap_caps.h").write_text(
        """#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_DMA 4
#define MALLOC_CAP_SPIRAM 8
void *heap_caps_malloc(size_t bytes, uint32_t caps);
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, uint32_t caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
"""
    )
    for esp, enabled in [(False, (1, 1, 1))] + [
        (True, flags) for flags in itertools.product((0, 1), repeat=3)
    ]:
        binary = work / (f"scratch_{int(esp)}_" + "".join(map(str, enabled)))
        subprocess.run(
            [
                *compiler,
                *flags,
                *(f"-DCONFIG_{name}={value}" for name, value in zip(options, enabled)),
                *(["-DESP_PLATFORM"] if esp else []),
                "-DLV_CONF_INCLUDE_SIMPLE",
                "-DLV_KCONFIG_IGNORE",
                "-I",
                str(root / "sim"),
                "-I",
                str(lvgl / "include"),
                "-I",
                str(work),
                str(lvgl / "src/stdlib/lv_mem_draw.c"),
                str(here / "test_draw_scratch.c"),
                "-pthread",
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
