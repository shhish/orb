"""Exercise the firmware receipt writer against a backpressured 64-byte USB FIFO."""

import os
import subprocess
from pathlib import Path


def test_command_receipt_survives_backpressure_and_terminates_usb_packets(tmp_path):
    component = Path(__file__).parents[1] / "firmware/components/orb_devtools"
    executable = tmp_path / "console-reply"
    subprocess.run(
        [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            f"-I{component}",
            str(component / "tests/host/test_console_reply.c"),
            "-o",
            str(executable),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
    assert result.stdout == "console reply: 44 checks passed\n"
