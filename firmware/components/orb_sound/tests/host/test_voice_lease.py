"""Exercise the production voice audio lease failure paths without ESP headers."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
component = here.parents[1]
source = (component.parent / "orb_voice" / "orb_voice.c").read_text()
start = source.index("static bool audio_open(")
end = source.index("\nstatic size_t audio_read(", start)
with tempfile.TemporaryDirectory(prefix="orb-sound-voice-") as tmp:
    work = Path(tmp)
    (work / "voice_audio.inc").write_text(source[start:end])
    binary = work / "test_voice_lease"
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
            str(component / "include"),
            str(component / "orb_sound.c"),
            str(component / "orb_sound_preferences.c"),
            str(component / "orb_sound_core.c"),
            str(component / "orb_sound_pcm.c"),
            str(here / "test_voice_lease.c"),
            "-lm",
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
