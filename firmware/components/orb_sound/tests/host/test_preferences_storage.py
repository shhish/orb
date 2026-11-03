"""Run the production preference load/save against a disposable in-memory NVS."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
component = here.parents[1]
with tempfile.TemporaryDirectory(prefix="orb-sound-preferences-") as tmp:
    work = Path(tmp)
    (work / "nvs.h").write_text("/* NVS is supplied by the deterministic test store. */\n")
    binary = work / "test_preferences_storage"
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
            "-DORB_SOUND_TEST_STORAGE=1",
            "-I",
            str(work),
            "-I",
            str(component),
            "-I",
            str(component / "include"),
            str(component / "orb_sound_core.c"),
            str(component / "orb_sound_pcm.c"),
            str(here / "test_preferences_storage.c"),
            "-pthread",
            "-lm",
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
