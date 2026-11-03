"""Capture production GPIO ISR edges without a board or RTOS."""

import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
source = here.parents[1] / "board_buttons.c"
with tempfile.TemporaryDirectory(prefix="orb-button-edges-") as tmp:
    work = Path(tmp)
    (work / "button_edges.inc").write_text(
        re.sub(r"^#include[^\n]*\n", "", source.read_text(), flags=re.M)
    )
    binary = work / "test_button_edges"
    compiler = shlex.split(os.environ.get("CC", "clang"))
    flags = shlex.split(
        os.environ.get(
            "CFLAGS", "-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
        )
    )
    subprocess.run(
        [
            *compiler,
            *flags,
            "-I",
            str(work),
            "-I",
            str(here.parents[2] / "orb_buttons/include"),
            str(here / "test_button_edges.c"),
            str(here.parents[2] / "orb_buttons/orb_buttons.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
