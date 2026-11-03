"""Run the production ES7210 profile against a failure-injecting register bus."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
component = here.parents[1]
source = (component / "board_audio.c").read_text()
start = source.index("esp_err_t orb_board_audio_set_mic_gain(")
end = source.index("\nesp_err_t orb_board_audio_write(", start)
with tempfile.TemporaryDirectory(prefix="orb-mic-gain-") as tmp:
    work = Path(tmp)
    (work / "mic_gain.inc").write_text(source[start:end])
    compiler = shlex.split(os.environ.get("CC", "clang"))
    flags = shlex.split(
        os.environ.get(
            "CFLAGS", "-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
        )
    )
    binary = work / "test_mic_gain"
    subprocess.run(
        [
            *compiler,
            *flags,
            "-I",
            str(work),
            "-I",
            str(component / "include"),
            str(here / "test_mic_gain.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
