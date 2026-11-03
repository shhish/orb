"""Run the production sound worker and producers against a deterministic scheduler."""

import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
root = here.parents[3]
source = (
    Path(sys.argv[1]) if len(sys.argv) > 1 else root / "components/orb_sound/orb_sound.c"
).read_text()
regions = [
    source[
        source.index("static bool audio_open(") : source.index(
            "\n#endif", source.index("static void sound_task(")
        )
    ],
    source[source.index("static bool play(") : source.index("\nvoid orb_sound_stop(")],
    source[
        source.index("void orb_sound_set_speech_active(") : source.index(
            "\nbool orb_sound_speech_allowed("
        )
    ],
    source[source.index("bool orb_sound_audio_acquire(") : source.index("\nvoid orb_sound_stats(")],
]
with tempfile.TemporaryDirectory(prefix="orb-sound-idle-") as temporary:
    work = Path(temporary)
    (work / "sound_idle.inc").write_text("\n".join(regions))
    binary = work / "test_sound_idle"
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
            str(here / "test_sound_idle.c"),
            "-lm",
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
