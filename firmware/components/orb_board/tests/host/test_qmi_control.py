"""Exercise production IMU register transitions with a deterministic fake bus."""

import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
component = here.parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else component / "qmi8658.c"
with tempfile.TemporaryDirectory(prefix="orb-qmi-control-") as tmp:
    work = Path(tmp)
    (work / "qmi_control.inc").write_text(
        re.sub(r"^#include[^\n]*\n", "", source.read_text(), flags=re.M)
    )
    compiler = shlex.split(os.environ.get("CC", "clang"))
    flags = shlex.split(
        os.environ.get(
            "CFLAGS", "-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
        )
    )
    binary = work / "test_qmi_control"
    subprocess.run(
        [
            *compiler,
            *flags,
            "-I",
            str(work),
            "-I",
            str(component),
            str(here / "test_qmi_control.c"),
            str(component / "orb_imu.c"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
