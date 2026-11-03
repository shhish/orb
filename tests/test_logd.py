"""Every serial integration test uses a local PTY, never a USB device."""

import asyncio
import base64
import importlib.util
import json
import os
import plistlib
import pty
import select
import stat
import tempfile
import termios
from contextlib import asynccontextmanager
from pathlib import Path

import pytest
from orb_devtools import client
from orb_devtools.logd import LogDaemon, Recorder, validate_command
from orb_devtools.serial_io import open_port


async def eventually(predicate):
    async with asyncio.timeout(3):
        for _ in range(300):
            if predicate():
                return
            await asyncio.sleep(0.01)
        raise AssertionError("condition not reached")


@asynccontextmanager
async def running_daemon(**options):
    # Darwin Unix-domain socket path length is only 104 bytes.
    with tempfile.TemporaryDirectory(prefix="orb-", dir="/tmp") as directory:
        path = Path(directory)
        master, slave = pty.openpty()
        port = os.ttyname(slave)
        daemon = LogDaemon(port, path / "s", path / "logs", reconnect=0.02, **options)
        try:
            await daemon.start()
            await eventually(lambda: daemon.port is not None)
            yield daemon, master, slave, path
        finally:
            await daemon.stop()
            for fd in [master, slave]:
                try:
                    os.close(fd)
                except OSError:
                    pass


async def connect(daemon, value):
    reader, writer = await asyncio.open_unix_connection(str(daemon.socket_path))
    writer.write(json.dumps(value).encode() + b"\n")
    await writer.drain()
    return reader, writer


async def frames(reader):
    result = []
    while True:
        raw = await asyncio.wait_for(reader.readline(), 3)
        assert raw, "missing terminal frame"
        frame = json.loads(raw)
        result.append(frame)
        if frame["type"] in ("done", "error"):
            return result


async def close(writer):
    writer.close()
    await writer.wait_closed()


async def read_master(master):
    def read():
        assert select.select([master], [], [], 3)[0]
        return os.read(master, 4096)

    return await asyncio.to_thread(read)


@pytest.mark.parametrize(
    "value",
    [
        "shot",
        "top",
        "heap",
        "tap 0 465",
        "hold 233 233 100",
        "swipe 0 1 2 3 60000",
        "arc 216 350 370 400",
        "arc 232 10 -10 5000",
        "tilt -0.25 0 5000",
        "tilt 0 1 1",
        "profile 200",
    ],
)
def test_valid_commands(value):
    assert validate_command(value) == value


@pytest.mark.parametrize(
    "value",
    [
        "",
        None,
        [],
        "tap",
        "tap 1 2 3",
        "tap 1 -1",
        "tap 466 3",
        "hold 1 2 0",
        "arc 216 0 90",
        "arc 216 0 90 400 1",
        "arc 0 0 90 400",
        "arc 233 0 90 400",
        "arc 216 -3601 90 400",
        "arc 216 0 3601 400",
        "arc 216 0 90 0",
        "arc 216 0 90 5001",
        "arc 216 0.5 90 400",
        "tilt 0 0",
        "tilt 0 0 100 extra",
        "tilt nan 0 100",
        "tilt 0 inf 100",
        "tilt 0.8 0.8 100",
        "tilt 1.001 0 100",
        "tilt 0 0 0",
        "tilt 0 0 5001",
        "tilt 0 0 1.5",
        "tilt 0 0 -1",
        "profile 60001",
        "profile 5001",
        "tap 1 2\nheap",
        "tap  1 2",
        "tap x 2",
        "reboot",
        "shot\r",
    ],
)
def test_invalid_commands(value):
    with pytest.raises((ValueError, TypeError)):
        validate_command(value)


def test_serial_policy():
    master, slave = pty.openpty()
    try:
        with open_port(os.ttyname(slave)) as connection:
            # DTR/RTS stay as opened: lowering DTR before RTS resets the ESP32-S3.
            assert connection.dtr is True
            assert connection.rts is True
            assert connection.exclusive is True
            assert not termios.tcgetattr(connection.fileno())[2] & termios.HUPCL
    finally:
        os.close(master)
        os.close(slave)


def test_recorder_rotation_timestamp_and_incident(tmp_path):
    recorder = Recorder(tmp_path, max_bytes=700, backups=2, incident_idle=0)
    try:
        recorder.record("ESP-ROM:esp32s3")
        boot = recorder.boot
        recorder.record("rst:0xc")
        assert recorder.boot == boot  # One boot, two boot-banner lines.
        recorder.record("ELF file SHA256: " + "ab" * 32)
        recorder.record("Guru Meditation Error: Core 0 panic'ed")
        recorder.record("Backtrace: 0x123:0x456")
        recorder.tick()
        incidents = list(recorder.incidents.glob("*.jsonl"))
        assert len(incidents) == 1
        text = incidents[0].read_text()
        assert "ELF file SHA256" in text and "Backtrace:" in text
        assert stat.S_IMODE(incidents[0].stat().st_mode) == 0o600
        for index in range(30):
            recorder.record(f"console item {index}")
        rows = recorder.tail(3, ["item"])
        assert [row["text"] for row in rows] == [
            "console item 27",
            "console item 28",
            "console item 29",
        ]
        assert all(row["time"].endswith("+00:00") for row in rows)
        assert len(list(tmp_path.glob("console.jsonl.*"))) == 2
    finally:
        recorder.close()


def test_boot_ends_incident(tmp_path):
    recorder = Recorder(tmp_path)
    try:
        recorder.record("panic: failed")
        old = recorder.boot
        recorder.record("ESP-ROM:esp32s3")
        assert recorder.incident is None
        assert recorder.boot != old
    finally:
        recorder.close()


def test_pty_record_tail_follow_and_permissions():
    async def run():
        async with running_daemon() as (daemon, master, _, path):
            assert stat.S_IMODE(daemon.socket_path.stat().st_mode) == 0o600
            reader, writer = await connect(
                daemon,
                {"op": "logs", "tail": 0, "follow": True, "seconds": 0.2, "filters": ["main:"]},
            )
            await eventually(lambda: bool(daemon.followers))
            os.write(master, b"ignored\nI (1) main: split")
            await asyncio.sleep(0.03)
            os.write(master, b" message\r\nGuru Meditation Error\nBacktrace: 0x1:0x2\n")
            rows = await frames(reader)
            await close(writer)
            assert [row["record"]["text"] for row in rows[:-1]] == ["I (1) main: split message"]
            reader, writer = await connect(daemon, {"op": "logs", "tail": 2})
            rows = await frames(reader)
            await close(writer)
            assert [row["record"]["text"] for row in rows[:-1]] == [
                "Guru Meditation Error",
                "Backtrace: 0x1:0x2",
            ]
            assert list((path / "logs/incidents").glob("*.jsonl"))

    asyncio.run(run())


def test_pty_command_frames_and_busy():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            reader, writer = await connect(daemon, {"op": "command", "command": "shot"})
            assert await read_master(master) == b"shot\n"
            other, other_writer = await connect(daemon, {"op": "command", "command": "heap"})
            assert (await frames(other))[-1]["type"] == "error"
            await close(other_writer)
            raw = b"@@ORBSHOT BEGIN 1 1 rgb565le 192 2 00000000\n@@00000 AAA=\nI (3) main: interleaved\n@@ORBSHOT END\n@@ORBDEV OK shot\n"
            for part in [raw[:12], raw[12:77], raw[77:]]:
                os.write(master, part)
                await asyncio.sleep(0.015)
            result = await frames(reader)
            await close(writer)
            assert result[-1]["type"] == "done"
            assert b"".join(base64.b64decode(frame["data"]) for frame in result[:-1]) == raw

    asyncio.run(run())


def test_failed_screenshot_drains_ack():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            reader, writer = await connect(daemon, {"op": "command", "command": "shot"})
            await read_master(master)
            os.write(master, b"@@ORBSHOT FAIL no memory\n@@ORBDEV OK shot\n")
            result = await frames(reader)
            assert result[-1]["type"] == "error"
            assert daemon.awaiting_ack is None
            await close(writer)

    asyncio.run(run())


@pytest.mark.parametrize("command", ["bench_open apps", "bench_nav slide-left 1000"])
def test_benchmark_receipt_and_ack_survive_usb_sized_fragments_with_logs(command):
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            reader, writer = await connect(
                daemon, {"op": "command", "command": command, "timeout": 1}
            )
            assert await read_master(master) == f"{command}\n".encode()
            receipt = (
                b"NAV gesture=slide-left "
                if command.startswith("bench_nav")
                else b"OPEN surface=apps "
            )
            raw = (
                b"I (1) main: restore complete\n@@ORBDEV "
                + receipt
                + b"timing=1234567890 " * 96
                + f"\r\nI (2) net: online\n@@ORBDEV OK {command}\r\n".encode()
            )
            for offset in range(0, len(raw), 64):
                await asyncio.to_thread(os.write, master, raw[offset : offset + 64])
            result = await frames(reader)
            assert result[-1]["type"] == "done"
            assert b"".join(base64.b64decode(frame["data"]) for frame in result[:-1]) == raw
            assert daemon.awaiting_ack is None
            await close(writer)

    asyncio.run(run())


def test_timeout_quarantines_until_late_ack():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            reader, writer = await connect(
                daemon, {"op": "command", "command": "heap", "timeout": 0.05}
            )
            await read_master(master)
            assert (await frames(reader))[-1]["type"] == "error"
            assert daemon.awaiting_ack == "heap"
            await close(writer)
            reader, writer = await connect(daemon, {"op": "command", "command": "heap"})
            assert (await frames(reader))[-1]["type"] == "error"
            await close(writer)
            os.write(master, b"@@ORBDEV OK heap\n")
            await eventually(lambda: daemon.awaiting_ack is None)

    asyncio.run(run())


@pytest.mark.parametrize("explicit_release", [True, False])
def test_lease_releases_and_reclaims(explicit_release):
    async def run():
        async with running_daemon() as (daemon, _, slave, _):
            reader, writer = await connect(daemon, {"op": "lease"})
            leased = json.loads(await reader.readline())
            assert leased == {"type": "leased", "port": os.ttyname(slave)}
            assert daemon.paused and daemon.port is None
            with open_port(leased["port"]):
                pass
            status, status_writer = await connect(daemon, {"op": "status"})
            assert (await frames(status))[0]["paused"] is True
            await close(status_writer)
            if explicit_release:
                writer.write(b'{"op":"release"}\n')
                await writer.drain()
                assert (await frames(reader))[-1]["type"] == "done"
            await close(writer)
            await eventually(lambda: not daemon.paused and daemon.port is not None)

    asyncio.run(run())


def test_reenumeration_using_symlink():
    async def run():
        with tempfile.TemporaryDirectory(prefix="orb-", dir="/tmp") as directory:
            path = Path(directory)
            master1, slave1 = pty.openpty()
            master2, slave2 = pty.openpty()
            link = path / "device"
            link.symlink_to(os.ttyname(slave1))
            daemon = LogDaemon(str(link), path / "s", path / "logs", reconnect=0.02)
            try:
                await daemon.start()
                await eventually(lambda: daemon.port is not None)
                os.write(master1, b"first device\n")
                await eventually(
                    lambda: any(x["text"] == "first device" for x in daemon.recorder.prelude)
                )
                os.close(master1)
                master1 = None
                link.unlink()
                await eventually(lambda: daemon.port is None)
                link.symlink_to(os.ttyname(slave2))
                await eventually(lambda: daemon.port is not None)
                os.write(master2, b"second device\n")
                await eventually(
                    lambda: any(x["text"] == "second device" for x in daemon.recorder.prelude)
                )
            finally:
                await daemon.stop()
                for fd in [master1, slave1, master2, slave2]:
                    if fd is not None:
                        os.close(fd)

    asyncio.run(run())


@pytest.mark.parametrize(
    "payload",
    [
        {},
        [],
        {"op": "unknown"},
        {"op": "command", "command": "heap", "timeout": None},
        {"op": "logs", "tail": -1},
        {"op": "logs", "tail": True},
        {"op": "logs", "filters": "main"},
        {"op": "logs", "seconds": -1},
        {"op": "logs", "follow": "yes"},
    ],
)
def test_malformed_requests_are_errors(payload):
    async def run():
        async with running_daemon() as (daemon, _, _, _):
            reader, writer = await connect(daemon, payload)
            assert (await frames(reader))[-1]["type"] == "error"
            await close(writer)

    asyncio.run(run())


def test_duplicate_daemon_does_not_unlink_active_socket():
    async def run():
        async with running_daemon() as (daemon, _, _, path):
            other = LogDaemon(daemon.port_pattern, daemon.socket_path, path / "other")
            try:
                with pytest.raises(RuntimeError, match="another"):
                    await other.start()
                assert daemon.socket_path.exists()
            finally:
                await other.stop()
            reader, writer = await connect(daemon, {"op": "status"})
            assert (await frames(reader))[0]["connected"]
            await close(writer)

    asyncio.run(run())


def test_client_command_and_lease_helpers():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            task = asyncio.create_task(
                asyncio.to_thread(lambda: b"".join(client.command("heap", daemon.socket_path)))
            )
            await read_master(master)
            os.write(master, b"@@HEAP free 123\n@@ORBDEV OK heap\n")
            assert await task == b"@@HEAP free 123\n@@ORBDEV OK heap\n"

            def lease():
                with client.lease(daemon.socket_path) as port:
                    assert port == daemon.port_name
                    assert daemon.paused

            await asyncio.to_thread(lease)
            await eventually(lambda: daemon.port is not None)

    asyncio.run(run())


def test_installer_template_escapes_paths():
    path = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location(
        "orb_logd_install", path / "scripts/orb_logd_install.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    result = module.render_template(
        (path / "deploy/com.orb.logd.plist.in").read_bytes(),
        {
            "PYTHON": "/tmp/A & B/python",
            "SOCKET": "/tmp/s",
            "PORT": "/dev/fake",
            "LOGDIR": "/tmp/logs",
        },
    )
    plist = plistlib.loads(result)
    assert plist["ProgramArguments"][0] == "/tmp/A & B/python"
    assert plist["ProgramArguments"][5] == "serve"
    assert "@" not in result.decode()


def test_disconnected_requester_holds_command_until_ack():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            _, writer = await connect(daemon, {"op": "command", "command": "top"})
            await read_master(master)
            await close(writer)
            os.write(master, b"@@TOP task core cpu\n")
            await asyncio.sleep(0.04)
            assert daemon.command_lock.locked()
            reader, other = await connect(daemon, {"op": "lease"})
            assert (await frames(reader))[-1]["type"] == "error"
            await close(other)
            os.write(master, b"@@ORBDEV OK top\n")
            await eventually(lambda: not daemon.command_lock.locked())

    asyncio.run(run())


def test_profile_error_completes_without_quarantine():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            reader, writer = await connect(daemon, {"op": "command", "command": "profile 200"})
            await read_master(master)
            os.write(master, b"@@ORBDEV ERROR profile disabled\n")
            assert (await frames(reader))[-1]["type"] == "error"
            assert daemon.awaiting_ack is None
            await close(writer)

    asyncio.run(run())


def test_persistent_tail_after_restart(tmp_path):
    first = Recorder(tmp_path)
    first.record("last boot line")
    first.close()
    second = Recorder(tmp_path)
    try:
        assert second.tail(1, ["boot"])[0]["text"] == "last boot line"
    finally:
        second.close()


def test_malformed_json_and_oversized_request():
    async def run():
        async with running_daemon() as (daemon, _, _, _):
            for data in [b"not JSON\n", b"[" + b"x" * 20000 + b"]\n"]:
                reader, writer = await asyncio.open_unix_connection(str(daemon.socket_path))
                writer.write(data)
                await writer.drain()
                assert (await frames(reader))[-1]["type"] == "error"
                await close(writer)

    asyncio.run(run())


def test_partial_console_line_flushed_on_stop():
    async def run():
        async with running_daemon() as (daemon, master, _, _):
            os.write(master, b"unterminated last line")
            await eventually(lambda: bool(daemon.pending))
            daemon.close_port()
            assert daemon.recorder.tail(1, [])[0]["text"] == "unterminated last line"

    asyncio.run(run())


def test_ambiguous_port_glob_never_opens(tmp_path):
    async def run():
        with tempfile.TemporaryDirectory(prefix="orb-", dir="/tmp") as directory:
            path = Path(directory)
            (path / "serial-one").touch()
            (path / "serial-two").touch()

            def never_open(_):
                raise AssertionError("ambiguous port must not be opened")

            daemon = LogDaemon(
                str(path / "serial-*"),
                path / "s",
                tmp_path / "logs",
                opener=never_open,
                reconnect=0.01,
            )
            try:
                await daemon.start()
                await eventually(lambda: daemon.last_error is not None)
                assert "ambiguous" in daemon.last_error
                reader, writer = await connect(daemon, {"op": "lease"})
                assert (await frames(reader))[-1]["type"] == "error"
                await close(writer)
            finally:
                await daemon.stop()

    asyncio.run(run())


def test_log_subscriber_backpressure_is_explicit():
    async def run():
        async with running_daemon() as (daemon, _, _, _):
            queue = asyncio.Queue(maxsize=1)
            daemon.followers.add(queue)
            daemon.record("first")
            daemon.record("second")
            assert queue not in daemon.followers
            assert isinstance(queue.get_nowait(), RuntimeError)

    asyncio.run(run())


def test_cli_exec_child_holds_lease_after_wrapper_killed():
    import signal
    import subprocess
    import sys

    package_path = str(Path(__file__).resolve().parents[1] / "devtools")

    async def run():
        async with running_daemon() as (daemon, _, _, path):
            child_pid = path / "child.pid"
            child_code = "import os,pathlib,time; pathlib.Path(os.environ['PID_FILE']).write_text(str(os.getpid())); time.sleep(60)"
            env = {
                **os.environ,
                "PID_FILE": str(child_pid),
                "PYTHONPATH": package_path,
            }
            process = await asyncio.to_thread(
                subprocess.Popen,
                [
                    sys.executable,
                    "-m",
                    "orb_devtools.logd",
                    "--socket",
                    str(daemon.socket_path),
                    "exec",
                    "--",
                    sys.executable,
                    "-c",
                    child_code,
                ],
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            pid = None
            try:
                await eventually(lambda: child_pid.exists())
                pid = int(child_pid.read_text())
                assert daemon.paused
                process.kill()
                await asyncio.to_thread(process.wait)
                await asyncio.sleep(0.05)
                assert daemon.paused, "child must inherit lease socket until it exits"
                os.kill(pid, signal.SIGTERM)
                pid = None
                await eventually(lambda: not daemon.paused and daemon.port is not None)
            finally:
                if pid is not None:
                    os.kill(pid, signal.SIGTERM)
                if process.poll() is None:
                    process.kill()
                await asyncio.to_thread(process.communicate)

    asyncio.run(run())


def test_incident_identity_is_current_boot_and_survives_long_uptime(tmp_path):
    recorder = Recorder(tmp_path)
    try:
        recorder.record("ELF file SHA256: " + "ab" * 32)
        recorder.record("previous boot context")
        recorder.record("ESP-ROM:esp32s3")
        recorder.record("ELF file SHA256: " + "cd" * 32)
        for index in range(150):
            recorder.record(f"long uptime console {index}")
        recorder.record("Guru Meditation Error")
        recorder.finish_incident()
        incident = next(recorder.incidents.glob("*.jsonl")).read_text()
        assert "ab" * 32 not in incident
        assert "previous boot context" not in incident
        assert "cd" * 32 in incident
        assert "long uptime console 149" in incident
    finally:
        recorder.close()


def test_fast_reboot_loop_detects_each_rom_banner(tmp_path):
    recorder = Recorder(tmp_path)
    try:
        recorder.record("ESP-ROM:esp32s3")
        recorder.record("rst:0xc")
        first = recorder.boot
        recorder.record("Guru Meditation Error")
        recorder.record("ESP-ROM:esp32s3")
        assert recorder.boot != first
        assert recorder.incident is None
        assert len(recorder.prelude) == 1
    finally:
        recorder.close()


@pytest.fixture
def fake_installer(monkeypatch):
    """No subprocess in these installer tests can invoke pip or launchctl."""
    import subprocess

    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location(
        "orb_logd_install_tests", root / "scripts/orb_logd_install.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    calls = []

    def fake_run(argv, **kwargs):
        calls.append((argv, kwargs))
        if argv[1:3] == ["-m", "venv"]:
            (Path(argv[3]) / "bin").mkdir(parents=True, exist_ok=True)
        elif argv[1:5] == ["-m", "pip", "install", "--no-cache-dir"]:
            pass
        elif argv[:2] in (["launchctl", "bootstrap"], ["launchctl", "bootout"]):
            pass
        else:
            raise AssertionError(f"unexpected subprocess: {argv}")
        return subprocess.CompletedProcess(argv, 0)

    monkeypatch.setattr(module.subprocess, "run", fake_run)
    with tempfile.TemporaryDirectory(prefix="orb-inst-", dir="/tmp") as directory:
        path = Path(directory).resolve()
        prefix = path / "prefix"
        agents = path / "agents"
        logs = path / "logs"
        args = [
            "--prefix",
            str(prefix),
            "--agents-dir",
            str(agents),
            "--log-dir",
            str(logs),
            "--port",
            "/dev/fake-orb",
        ]
        yield module, prefix, agents, logs, args, calls


def test_install_stages_agent_without_loading(fake_installer, capsys):
    module, prefix, agents, logs, args, calls = fake_installer
    assert module.main(["install", *args]) == 0
    agent = agents / "com.orb.logd.plist"
    assert stat.S_IMODE(agent.stat().st_mode) == 0o600
    config = plistlib.loads(agent.read_bytes())
    assert config["ProgramArguments"] == [
        str(prefix / "venv/bin/python"),
        "-m",
        "orb_devtools.logd",
        "--socket",
        str(prefix / "orb.sock"),
        "serve",
        "--port",
        "/dev/fake-orb",
        "--log-dir",
        str(logs),
    ]
    assert config["StandardErrorPath"] == str(logs / "service.stderr.log")
    assert (prefix / "venv/.orb-logd-owned").is_file()
    assert logs.is_dir()
    assert len(calls) == 2
    assert all(argv[0] != "launchctl" for argv, _ in calls)
    assert "Not loaded" in capsys.readouterr().out


def test_install_load_requires_explicit_flag_and_uses_fake_launchctl(fake_installer):
    module, _, agents, _, args, calls = fake_installer
    assert module.main(["install", "--load", *args]) == 0
    assert calls[-1] == (
        ["launchctl", "bootstrap", f"gui/{os.getuid()}", str(agents / "com.orb.logd.plist")],
        {"check": True},
    )


def test_uninstall_removes_owned_runtime_and_preserves_incidents(fake_installer):
    module, prefix, agents, logs, args, calls = fake_installer
    module.main(["install", *args])
    incident = logs / "incidents/test.jsonl"
    incident.parent.mkdir()
    incident.write_text("preserved evidence\n")
    (prefix / "unrelated.txt").write_text("not owned")
    calls.clear()
    assert module.main(["uninstall", *args]) == 0
    assert not (prefix / "venv").exists()
    assert not (agents / "com.orb.logd.plist").exists()
    assert incident.read_text() == "preserved evidence\n"
    assert (prefix / "unrelated.txt").read_text() == "not owned"
    assert calls == [
        (
            ["launchctl", "bootout", f"gui/{os.getuid()}/com.orb.logd"],
            {"check": False, "capture_output": True},
        )
    ]


def test_uninstall_never_removes_unowned_venv(fake_installer):
    module, prefix, _, logs, args, calls = fake_installer
    sentinel = prefix / "venv/precious.txt"
    sentinel.parent.mkdir(parents=True)
    sentinel.write_text("leave this alone")
    logs.mkdir()
    assert module.main(["uninstall", *args]) == 0
    assert sentinel.read_text() == "leave this alone"
    assert logs.is_dir()
    assert not calls


def test_install_refuses_unowned_venv(fake_installer):
    module, prefix, agents, _, args, calls = fake_installer
    (prefix / "venv").mkdir(parents=True)
    with pytest.raises(SystemExit) as raised:
        module.main(["install", *args])
    assert raised.value.code == 2
    assert not calls
    assert not (agents / "com.orb.logd.plist").exists()
    assert not (prefix / "venv/.orb-logd-owned").exists()


def test_install_refuses_existing_agent_without_overwriting(fake_installer):
    module, _, agents, _, args, calls = fake_installer
    agents.mkdir()
    agent = agents / "com.orb.logd.plist"
    agent.write_bytes(b"original agent bytes")
    with pytest.raises(SystemExit) as raised:
        module.main(["install", *args])
    assert raised.value.code == 2
    assert agent.read_bytes() == b"original agent bytes"
    assert not calls


def test_uninstall_rejects_load_flag_before_mutation(fake_installer):
    module, prefix, _, _, args, calls = fake_installer
    with pytest.raises(SystemExit) as raised:
        module.main(["uninstall", "--load", *args])
    assert raised.value.code == 2
    assert not prefix.exists()
    assert not calls


def test_install_failed_dependency_setup_does_not_load_or_stage_agent(fake_installer, monkeypatch):
    import subprocess

    module, prefix, agents, _, args, calls = fake_installer
    original_fake = module.subprocess.run

    def fail_pip(argv, **kwargs):
        if argv[1:4] == ["-m", "pip", "install"]:
            raise subprocess.CalledProcessError(1, argv)
        return original_fake(argv, **kwargs)

    monkeypatch.setattr(module.subprocess, "run", fail_pip)
    with pytest.raises(subprocess.CalledProcessError):
        module.main(["install", "--load", *args])
    assert not (agents / "com.orb.logd.plist").exists()
    assert (prefix / "venv/.orb-logd-owned").exists()
    assert all(argv[0] != "launchctl" for argv, _ in calls)


@pytest.mark.parametrize("still_loaded", [True, False])
def test_uninstall_stop_failure_preserves_runtime_if_still_loaded(
    fake_installer, monkeypatch, still_loaded
):
    import subprocess

    module, prefix, agents, _, args, calls = fake_installer
    module.main(["install", *args])
    calls.clear()

    def fake_launchctl(argv, **kwargs):
        calls.append((argv, kwargs))
        if argv[:2] == ["launchctl", "bootout"]:
            return subprocess.CompletedProcess(argv, 5)
        if argv[:2] == ["launchctl", "print"]:
            return subprocess.CompletedProcess(argv, 0 if still_loaded else 113)
        raise AssertionError(f"unexpected subprocess: {argv}")

    monkeypatch.setattr(module.subprocess, "run", fake_launchctl)
    if still_loaded:
        with pytest.raises(SystemExit) as raised:
            module.main(["uninstall", *args])
        assert raised.value.code == 2
        assert (agents / "com.orb.logd.plist").exists()
        assert (prefix / "venv/.orb-logd-owned").exists()
    else:
        assert module.main(["uninstall", *args]) == 0
        assert not (agents / "com.orb.logd.plist").exists()
        assert not (prefix / "venv").exists()
    assert [argv[1] for argv, _ in calls] == ["bootout", "print"]


def test_inherited_lease_survives_normal_context_exit_until_child_exits():
    import subprocess
    import sys

    async def run():
        async with running_daemon() as (daemon, _, _, _):

            def start_child_and_leave_context():
                with client.lease(daemon.socket_path, with_fd=True) as (port, lease_fd):
                    assert port == daemon.port_name
                    process = subprocess.Popen(
                        [sys.executable, "-c", "import sys; sys.stdin.buffer.read()"],
                        pass_fds=(lease_fd,),
                        stdin=subprocess.PIPE,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                # Normal context exit must only close our descriptor, not send
                # an explicit release while this child still owns its copy.
                return process

            process = await asyncio.to_thread(start_child_and_leave_context)
            try:
                await asyncio.sleep(0.05)
                assert process.poll() is None
                assert daemon.paused
                assert daemon.port is None
                process.stdin.close()
                assert await asyncio.to_thread(process.wait, timeout=3) == 0
                await eventually(lambda: not daemon.paused and daemon.port is not None)
            finally:
                if process.poll() is None:
                    process.kill()
                    await asyncio.to_thread(process.wait)
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()

    asyncio.run(run())
