"""Bounded trace transfer through the real shell entry point and 115200/8N1 PTY."""

import asyncio
import os
import pty
import select
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest
from orb_dev_remote import command_timeout
from orb_devtools.logd import LogDaemon
from scroll_trace import analyze_trace
from test_scroll_trace_firmware import emitter as emitter
from test_speed_ab import accounting_text, surface_text
from test_speed_profile import profile_text

ROOT = Path(__file__).resolve().parents[1]
USB_BYTES_PER_SECOND = 115200 / 10


async def _trace_roundtrip(use_daemon, duration_ms, trace):
    with tempfile.TemporaryDirectory(prefix="orb-trace-pty-", dir="/tmp") as temp:
        path = Path(temp)
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        daemon = LogDaemon(port, path / "s", path / "logs", reconnect=0.01) if use_daemon else None
        process = None
        command = f"bench_scroll agents {duration_ms}"
        progress = b"@@ORBDEV SCROLL_PROGRESS phase=setup\n"
        # Use the compiled production C emitter's full-capacity output, including
        # its histogram, plus representative normal benchmark/accounting/profile
        # receipts from their existing host fixtures.
        auxiliary = surface_text() + accounting_text(phases=("agents",)) + profile_text()
        auxiliary = "\n".join(line for line in auxiliary.splitlines() if "@@ORBDEV OK " not in line)
        payload = trace + auxiliary.encode() + f"\n@@ORBDEV OK {command}\n".encode()
        try:
            if daemon:
                await daemon.start()
                for _ in range(200):
                    if daemon.port:
                        break
                    await asyncio.sleep(0.01)
                assert daemon.port
            process = await asyncio.create_subprocess_exec(
                str(ROOT / "scripts/orb-dev.sh"),
                *command.split(),
                env={
                    **os.environ,
                    "ORB_DEV_LOCAL": "1",
                    "ORB_PYTHON": sys.executable,
                    "ESP_REMOTE_PORT": port,
                    "ORB_LOGD_SOCKET": str(path / "s"),
                },
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )

            def read_command():
                data = b""
                while b"\n" not in data:
                    assert select.select([master], [], [], 5)[0], "helper never sent command"
                    data += os.read(master, 4096)
                return data

            assert (await asyncio.to_thread(read_command)).strip() == command.encode()
            os.write(master, progress)
            # A live receipt must arrive before either measurement or ACK. The
            # former buffered helper withheld this forever on a failed capture.
            first = await asyncio.wait_for(process.stdout.readexactly(len(progress)), 2)
            assert first == progress
            await asyncio.sleep(duration_ms / 1000)
            started = time.monotonic()
            for offset in range(0, len(payload), 64):
                block = payload[offset : offset + 64]
                os.write(master, block)
                # PTYs ignore their baud setting. Explicit wall-clock pacing
                # models all ten 8N1 bits per byte at the configured 115200 baud.
                deadline = started + (offset + len(block)) / USB_BYTES_PER_SECOND
                await asyncio.sleep(max(0, deadline - time.monotonic()))
            emission_seconds = time.monotonic() - started
            rest, stderr = await asyncio.wait_for(process.communicate(), 5)
            assert process.returncode == 0, stderr
            output = first + rest
            assert b"@@ORBDEV SCROLLTRACE version=2" in output
            result = analyze_trace(output.decode())
            assert len(result["events"]) == 2048
            assert result["metadata"]["dropped"] == 17
            assert result["completion_intervals"]["count"] == 127
            for marker in (b"SCROLL surface=agents", b"ACCOUNT_META ", b"DRAWPROFILE_META "):
                assert marker in output
            assert emission_seconds >= len(payload) / USB_BYTES_PER_SECOND
            assert emission_seconds < 10
            assert duration_ms / 1000 + 10 + emission_seconds < command_timeout(command)
            return len(payload), emission_seconds
        finally:
            if process and process.returncode is None:
                process.kill()
                await process.wait()
            if daemon:
                await daemon.stop()
            os.close(master)
            os.close(slave)


@pytest.mark.parametrize(("use_daemon", "duration_ms"), [(False, 1000), (True, 2000)])
def test_maximum_trace_emits_within_budget_at_usb_serial_jtag_host_rate(
    emitter, use_daemon, duration_ms
):
    trace = subprocess.run([str(emitter), "max"], check=True, capture_output=True).stdout
    size, seconds = asyncio.run(_trace_roundtrip(use_daemon, duration_ms, trace))
    print(
        f"PTY {'daemon' if use_daemon else 'direct'}: {size} bytes, {seconds:.3f}s emission, "
        f"{duration_ms}ms sample, 115200 baud 8N1"
    )
