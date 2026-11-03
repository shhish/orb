"""Exercise production adaptive-buffer admission and DMA fences without IDF."""

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
source = Path(sys.argv[1]) if len(sys.argv) > 1 else here.parents[1] / "orb_display.c"
text = source.read_text()


def function(name: str) -> str:
    start = text.index(name)
    start = text.rfind("\n", 0, start) + 1
    end = text.index("\n}", start) + len("\n}")
    return text[start:end]


with tempfile.TemporaryDirectory(prefix="orb-double-buffer-test-") as tmp:
    work = Path(tmp)
    (work / "orb_double_buffer_impl.inc").write_text(
        function("try_double_buffer(void)")
        + "\n\n"
        + function("orb_display_request_double_buffer(void)")
        + "\n#if CONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER\n"
        + function("install_draw_buffers(bool use_halves)")
        + "\n\n"
        + function("select_draw_buffers(void)")
        + "\n#endif\n"
        + function("orb_display_set_snapshot_transition(bool active)")
        + "\n"
    )
    (work / "orb_render_accounting_impl.inc").write_text(
        function("on_render(lv_event_t *e)") + "\n"
    )
    compiler = shlex.split(os.environ.get("CC", "clang"))
    flags = shlex.split(
        os.environ.get(
            "CFLAGS", "-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
        )
    )
    for rows, enabled, live in (
        (84, 0, 0),
        (84, 1, 0),
        (84, 1, 1),
        (104, 0, 0),
        (104, 1, 0),
        (104, 1, 1),
    ):
        binary = work / f"test_double_buffer_{rows}_{enabled}_{live}"
        subprocess.run(
            [
                *compiler,
                *flags,
                f"-DBAND_ROWS={rows}",
                f"-DCONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER={enabled}",
                f"-DCONFIG_ORB_DISPLAY_LIVE_DOUBLE_BUFFER={live}",
                "-I",
                str(work),
                str(here / "test_double_buffer.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
    for enabled, accounting in ((0, 0), (1, 0), (0, 1), (1, 1)):
        binary = work / f"test_render_accounting_{enabled}_{accounting}"
        subprocess.run(
            [
                *compiler,
                *flags,
                f"-DCONFIG_ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER={enabled}",
                f"-DCONFIG_ORB_DISPLAY_RENDER_ACCOUNTING={accounting}",
                "-I",
                str(here.parents[1] / "include"),
                "-I",
                str(work),
                str(here / "test_render_accounting.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)
