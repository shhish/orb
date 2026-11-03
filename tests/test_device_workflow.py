"""Real shell entry points and serial framing, with a PTY standing in for Orb."""

import asyncio
import base64
import io
import os
import pty
import select
import tempfile
import zlib
from pathlib import Path

import pytest
from orb_dev_remote import command_timeout, decode_shot, png
from orb_devtools.logd import LogDaemon
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def capture_frame(raw, *, crc=None):
    crc = zlib.crc32(raw) if crc is None else crc
    count = 16
    blocks = [
        f"@@{i // count:05x} ".encode() + base64.b64encode(raw[i : i + count]) + b"\n"
        for i in range(0, len(raw), count)
    ]
    # Out-of-order indexed data and unrelated logs are part of the transport contract.
    return (
        f"@@ORBSHOT BEGIN 8 8 rgb565le {count} {len(raw)} {crc:08x}\n".encode()
        + b"I (20) main: background log\n"
        + b"".join(reversed(blocks))
        + b"@@ORBSHOT END\n\n@@ORBDEV OK shot\n"
    )


async def master_command(master):
    def read():
        data = b""
        while b"\n" not in data:
            assert select.select([master], [], [], 5)[0], "helper never sent command"
            data += os.read(master, 4096)
        return data

    return await asyncio.to_thread(read)


async def shell_workflow(use_daemon, command, responses, output=None):
    with tempfile.TemporaryDirectory(prefix="orb-e2e-", dir="/tmp") as temp:
        path = Path(temp)
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        daemon = LogDaemon(port, path / "s", path / "logs", reconnect=0.01) if use_daemon else None
        process = None
        try:
            if daemon:
                await daemon.start()
                for _ in range(200):
                    if daemon.port:
                        break
                    await asyncio.sleep(0.01)
                assert daemon.port
            args = [str(ROOT / "scripts/orb-dev.sh"), *command.split()]
            if output:
                args.append(str(output))
            process = await asyncio.create_subprocess_exec(
                *args,
                env={
                    **os.environ,
                    "ORB_DEV_LOCAL": "1",
                    "ESP_REMOTE_PORT": port,
                    "ORB_LOGD_SOCKET": str(path / "s"),
                },
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            for response in responses:
                assert (await master_command(master)).strip() == command.encode()
                # Deliberately split the marker across serial reads.
                os.write(master, response[:9])
                await asyncio.sleep(0.02)
                os.write(master, response[9:])
            stdout, stderr = await asyncio.wait_for(process.communicate(), 8)
            return process.returncode, stdout, stderr
        finally:
            if process and process.returncode is None:
                process.kill()
                await process.wait()
            if daemon:
                await daemon.stop()
            os.close(master)
            os.close(slave)


@pytest.mark.parametrize("use_daemon", [False, True])
@pytest.mark.parametrize("retry", [False, True])
def test_shell_screenshot_roundtrip_and_crc_retry(tmp_path, use_daemon, retry):
    raw = b"\x00\xf8" * 64
    replies = ([capture_frame(raw, crc=0)] if retry else []) + [capture_frame(raw)]
    destination = tmp_path / "directory with spaces" / "shot.png"
    code, stdout, stderr = asyncio.run(shell_workflow(use_daemon, "shot", replies, destination))
    assert code == 0, stderr
    assert str(destination).encode() in stdout
    assert not destination.with_suffix(".png.part").exists()
    with Image.open(destination) as image:
        assert image.size == (8, 8)
        assert image.getpixel((4, 4)) == (255, 0, 0, 255)
        assert image.getpixel((0, 0))[3] == 0
    if retry:
        assert b"retrying" in stderr


@pytest.mark.parametrize("use_daemon", [False, True])
@pytest.mark.parametrize(
    "command",
    ["tap 233 400", "hold 100 100 80", "swipe 20 30 40 50 60", "heap", "top", "profile 20"],
)
def test_shell_commands(use_daemon, command):
    payload = b""
    if command == "heap":
        payload = b"@@HEAP internal free 123\n"
    elif command == "top":
        payload = b"@@TOP task core cpu\n"
    elif command.startswith("profile"):
        payload = b"=== PROFILE BEGIN ===\nLVGL-1 [0] 1.001: tracing_mark_write: B|1|frame\n=== PROFILE END ===\n"
    payload += b"@@ORBDEV OK " + command.encode() + b"\n"
    code, stdout, stderr = asyncio.run(shell_workflow(use_daemon, command, [payload]))
    assert code == 0, stderr
    assert ("ORBDEV OK " + command).encode() in stdout
    if command.startswith("profile"):
        assert b"tracing_mark_write" in stdout


@pytest.mark.parametrize("use_daemon", [False, True])
def test_shell_failed_screenshot_does_not_publish_partial_file(tmp_path, use_daemon):
    path = tmp_path / "shot.png"
    path.write_bytes(b"previous good image")
    reply = b"@@ORBSHOT FAIL no memory\n@@ORBDEV OK shot\n"
    code, _, _ = asyncio.run(shell_workflow(use_daemon, "shot", [reply], path))
    assert code == 1
    assert path.read_bytes() == b"previous good image"
    assert not path.with_suffix(".png.part").exists()


@pytest.mark.parametrize(
    "command",
    ["tap\n1 2", " tap 1 2", "tap 1 2\n", "profile 5001", "hold 1 2 -1", "swipe 0 0 466 0 1"],
)
def test_command_rejects_injection_and_unsafe_coordinates(command):
    with pytest.raises(ValueError):
        command_timeout(command)


def test_shot_rejects_malicious_lengths_and_missing_chunks():
    with pytest.raises(ValueError, match="dimensions"):
        decode_shot(b"@@ORBSHOT BEGIN 99999 99999 rgb565le 0 0 00000000\n@@ORBSHOT END\n")
    with pytest.raises(ValueError, match="CRC"):
        decode_shot(capture_frame(b"\0\0" * 64).replace(b"@@00000", b"broken0"))
    image = Image.open(io.BytesIO(png(1, 1, b"\xff\xff")))
    assert image.getpixel((0, 0)) == (255, 255, 255, 255)
