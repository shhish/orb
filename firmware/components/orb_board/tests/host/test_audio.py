"""Exercise the production audio constructor without hardware or IDF headers."""

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
source = Path(sys.argv[1]) if len(sys.argv) > 1 else here.parents[1] / "board_audio.c"
text = source.read_text()
start = text.index("/* Audio has one owner.")
end = text.index('\n#include "orb_audio_levels.h"', start)
with tempfile.TemporaryDirectory(prefix="orb-audio-test-") as tmp:
    work = Path(tmp)
    (work / "orb_board_audio_impl.inc").write_text(
        text[start:end]
        + '\n#include "orb_audio_levels.h"\n'
        + text[text.index("esp_err_t orb_board_audio_set_volume(") :]
    )
    binary = work / "test_audio"
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
            str(here.parents[1]),
            str(here / "test_audio.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
