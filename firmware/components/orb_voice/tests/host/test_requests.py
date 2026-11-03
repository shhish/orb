"""Exercise production request ownership and PSRAM OOM without ESP headers."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
component = here.parents[1]
source = (component / "orb_voice.c").read_text()
start = source.index("static bool request_send(")
end = source.index("\nstatic bool answer_send(", start)
with tempfile.TemporaryDirectory(prefix="orb-voice-requests-") as tmp:
    work = Path(tmp)
    (work / "voice_requests.inc").write_text(source[start:end])
    binary = work / "test_requests"
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
            str(component),
            "-I",
            str(component.parent / "orb_model" / "include"),
            str(here / "test_requests.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
