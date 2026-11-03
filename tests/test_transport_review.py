"""Wire and shell integration tests; every endpoint and executable is a fake."""

import base64
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import types
from contextlib import contextmanager
from pathlib import Path

import orb_dev_remote as transport
import pytest

ROOT = Path(__file__).parents[1]


@pytest.mark.parametrize("command", ["tilt -0.25 0 5000", "tilt 0 1 1", "tilt 0 0 600"])
def test_tilt_transport_accepts_bounded_specific_force(command):
    assert transport.command_timeout(command) == 10


@pytest.mark.parametrize(
    "command",
    [
        "tilt 0 0",
        "tilt 0 0 100 extra",
        "tilt nan 0 100",
        "tilt 0 inf 100",
        "tilt 1.01 0 100",
        "tilt 0.8 0.8 100",
        "tilt 0 0 0",
        "tilt 0 0 5001",
        "tilt 0 0 -1",
        "tilt 0 0 1.5",
        "tilt 0 0 100\nheap",
    ],
)
def test_tilt_transport_rejects_invalid_force_and_duration(command):
    with pytest.raises(ValueError):
        transport.command_timeout(command)


class LeaseSocket:
    def fileno(self):
        return 9001


def stream(*frames):
    return io.BytesIO(b"".join(json.dumps(frame).encode() + b"\n" for frame in frames))


def fake_daemon(monkeypatch, *responses):
    requests = []

    @contextmanager
    def connect(request):
        requests.append(request)
        yield object(), stream(*responses)

    monkeypatch.setattr(transport, "daemon", connect)
    return requests


def prohibit_serial(monkeypatch):
    def forbidden(*args, **kwargs):
        pytest.fail("daemon reply must not open direct serial")

    monkeypatch.setattr(transport, "open_port", forbidden)


def test_helper_reassembles_wire_command_fragments(monkeypatch):
    chunks = [b"I (1) log\n@@HEAP free 3\n@@ORB", b"DEV OK heap\r", b"\n"]
    requests = fake_daemon(
        monkeypatch,
        *[{"type": "data", "data": base64.b64encode(chunk).decode()} for chunk in chunks],
        {"type": "done"},
    )
    prohibit_serial(monkeypatch)
    assert transport.run_command("/not-a-real-port", "heap") == b"".join(chunks)
    assert requests == [{"op": "command", "command": "heap", "timeout": 10}]


@pytest.mark.parametrize(
    "reply",
    [
        {"type": "error", "error": "serial busy"},
        {"type": "done"},
    ],
)
def test_error_or_incomplete_reply_never_falls_back(monkeypatch, reply):
    fake_daemon(monkeypatch, reply)
    prohibit_serial(monkeypatch)
    with pytest.raises(RuntimeError):
        transport.run_command("/not-a-real-port", "heap")


def test_absent_daemon_falls_back_once(monkeypatch):
    class Serial:
        in_waiting = 999

        def __init__(self):
            self.written = []
            self.reset = False
            self.closed = False

        def __enter__(self):
            return self

        def __exit__(self, *args):
            self.closed = True

        def reset_input_buffer(self):
            self.reset = True

        def write(self, value):
            self.written.append(value)

        def read(self, _size):
            return b"@@HEAP x\n@@ORBDEV OK heap\n"

    @contextmanager
    def absent(_request):
        raise transport.AbsentDaemon
        yield  # pragma: no cover

    device = Serial()
    calls = []
    monkeypatch.setattr(transport, "daemon", absent)
    monkeypatch.setattr(transport, "open_port", lambda port: calls.append(port) or device)
    assert b"@@HEAP" in transport.run_command("/fake", "heap")
    assert calls == ["/fake"]
    assert device.written == [b"heap\n"]
    assert device.reset and device.closed


def test_lease_remains_open_until_child_finishes(monkeypatch):
    events = []

    @contextmanager
    def connect(request):
        assert request == {"op": "lease"}
        events.append("open")
        try:
            yield LeaseSocket(), stream({"type": "leased", "port": "/fake/re-enumerated"})
        finally:
            events.append("close")

    def child(argv, cwd, pass_fds):
        assert pass_fds == (9001,)
        assert events == ["open"]
        assert argv == ["program", "/fake/re-enumerated", "literal;$(payload)"]
        assert cwd == "/tmp"
        events.append("child finished")
        return 7

    monkeypatch.setattr(transport, "daemon", connect)
    monkeypatch.setattr(transport.subprocess, "call", child)
    assert transport.execute("/old", ["program", "{port}", "literal;$(payload)"], "/tmp") == 7
    assert events == ["open", "child finished", "close"]


def test_lease_closes_when_child_cannot_launch(monkeypatch):
    events = []

    @contextmanager
    def connect(_request):
        try:
            yield LeaseSocket(), stream({"type": "leased", "port": "/fake"})
        finally:
            events.append("released")

    def child(*_args, **_kwargs):
        raise FileNotFoundError("throwaway fake executable")

    monkeypatch.setattr(transport, "daemon", connect)
    monkeypatch.setattr(transport.subprocess, "call", child)
    with pytest.raises(FileNotFoundError):
        transport.execute("/old", ["missing"])
    assert events == ["released"]


def test_wire_logs_filter_and_timestamp(monkeypatch, capsys):
    class Socket:
        def settimeout(self, timeout):
            assert timeout is None

    requests = []

    @contextmanager
    def connect(request):
        requests.append(request)
        yield (
            Socket(),
            stream(
                {"type": "log", "record": {"time": "2026-10-26Z", "text": "main: ready"}},
                {"type": "done"},
            ),
        )

    monkeypatch.setattr(transport, "daemon", connect)
    prohibit_serial(monkeypatch)
    transport.logs("/fake", 0.2, "main:|panic")
    assert capsys.readouterr().out == "2026-10-26Z main: ready\n"
    assert requests[0]["filters"] == ["main:", "panic"]
    assert requests[0]["seconds"] == 0.2


@pytest.mark.parametrize("raw", [b"", b'{"type":"done"}', b"x" * (2 * 1024 * 1024)])
def test_wire_truncation_and_oversize_rejected(raw):
    with pytest.raises(RuntimeError, match="disconnected or sent an oversized frame"):
        list(transport.frames(io.BytesIO(raw)))


@pytest.mark.parametrize("socket_override", [False, True])
def test_shell_remote_preserves_arguments_without_expanding_payload(tmp_path, socket_override):
    """Fake SSH evaluates only a substituted collector, never connects anywhere."""
    tools = tmp_path / "tools"
    tools.mkdir()
    result_file = tmp_path / "args.json"
    marker = tmp_path / "must-not-exist"
    collector = tools / "collector.py"
    collector.write_text(
        "import json, os, sys\n"
        "from pathlib import Path\n"
        "Path(os.environ['TEST_ARGS']).write_text(json.dumps({'args':sys.argv[1:], 'input':sys.stdin.read(), 'socket':os.environ.get('ORB_LOGD_SOCKET')}))\n"
    )
    fake_ssh = tools / "ssh"
    fake_ssh.write_text(
        f"#!{sys.executable}\n"
        "import os, shlex, subprocess, sys\n"
        "prefix = '$HOME/.venvs/esptool/bin/python - '\n"
        "command = sys.argv[-1]\n"
        "assert command.count(prefix) == 1\n"
        "replacement = shlex.quote(sys.executable) + ' ' + shlex.quote(os.environ['TEST_COLLECTOR']) + ' '\n"
        "raise SystemExit(subprocess.run(['/bin/bash', '-c', command.replace(prefix, replacement, 1)], input=sys.stdin.buffer.read()).returncode)\n"
    )
    fake_ssh.chmod(0o755)
    values = ["--logs", "0.2", f"main:|$(touch {marker})|'quote'\nnext"]
    env = {
        **os.environ,
        "PATH": str(tools) + os.pathsep + os.environ["PATH"],
        "ORB_DEV_LOCAL": "0",
        "ESP_REMOTE_PORT": "/fake/port with spaces;literal",
        "ESP_REMOTE_HOST": "throwaway-host",
        "TEST_ARGS": str(result_file),
        "TEST_COLLECTOR": str(collector),
    }
    if socket_override:
        env["ORB_LOGD_SOCKET"] = f"/tmp/socket with spaces;$(touch {marker})\nquoted"
    else:
        env.pop("ORB_LOGD_SOCKET", None)
    result = subprocess.run(
        [str(ROOT / "scripts/orb-transport.sh"), *values], env=env, capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    captured = json.loads(result_file.read_text())
    assert captured["args"] == [env["ESP_REMOTE_PORT"], *values]
    assert captured["socket"] == env.get("ORB_LOGD_SOCKET")
    assert "Single-file USB transport" in captured["input"]
    assert not marker.exists()


def test_shell_local_mode_calls_helper_with_exact_args(tmp_path):
    result_file = tmp_path / "args.json"
    fake_python = tmp_path / "python"
    fake_python.write_text(
        f"#!{sys.executable}\n"
        "import json, os, sys\n"
        "from pathlib import Path\n"
        "Path(os.environ['TEST_ARGS']).write_text(json.dumps(sys.argv[1:]))\n"
    )
    fake_python.chmod(0o755)
    env = {
        **os.environ,
        "ORB_DEV_LOCAL": "1",
        "ORB_PYTHON": str(fake_python),
        "ESP_REMOTE_PORT": "/fake/port",
        "TEST_ARGS": str(result_file),
    }
    result = subprocess.run(
        [str(ROOT / "scripts/orb-transport.sh"), "tap", "1", "2"],
        env=env,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result_file.read_text()) == [
        str(ROOT / "scripts/orb_dev_remote.py"),
        "/fake/port",
        "tap",
        "1",
        "2",
    ]


@pytest.mark.parametrize("command", ["tap\n1 2", "tap  1 2", "tap 1 2\r", " tap 1 2"])
def test_noncanonical_command_cannot_reach_either_transport(command):
    with pytest.raises(ValueError):
        transport.command_timeout(command)


@pytest.mark.parametrize("command", ["arc 216 350 370 400", "arc 232 10 -10 400"])
def test_arc_transport_accepts_unwrapped_angles(command, monkeypatch):
    requests = fake_daemon(
        monkeypatch,
        {"type": "data", "data": base64.b64encode(f"@@ORBDEV OK {command}\n".encode()).decode()},
        {"type": "done"},
    )
    prohibit_serial(monkeypatch)
    assert transport.run_command("/fake", command) == f"@@ORBDEV OK {command}\n".encode()
    assert requests == [{"op": "command", "command": command, "timeout": 10.4}]


@pytest.mark.parametrize(
    "command",
    [
        "arc 216 0 90",
        "arc 216 0 90 400 1",
        "arc 0 0 90 400",
        "arc 233 0 90 400",
        "arc 216 -3601 90 400",
        "arc 216 0 3601 400",
        "arc 216 0 90 0",
        "arc 216 0 90 5001",
        "arc 216 0.5 90 400",
        "arc 216 0 90 400\nheap",
    ],
)
def test_arc_transport_rejects_invalid_arguments(command):
    with pytest.raises(ValueError):
        transport.command_timeout(command)


def flash_table(subtype=3):
    entry = struct.pack("<HBBII16sI", 0x50AA, 1, subtype, 0x1000000, 4096, b"coredump", 0)
    footer = b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(entry, usedforsecurity=False).digest()
    return (entry + footer).ljust(4096, b"\xff")


def fake_esptool(monkeypatch, *, table=None, raw=None, fail_read=None, chip="ESP32-S3"):
    """Inject the API module so no installed esptool or physical port is used."""
    events = []
    table = flash_table() if table is None else table
    raw = bytes(range(256)) * 16 if raw is None else raw

    class Device:
        CHIP_NAME = chip

        def __enter__(self):
            events.append("device enter")
            return self

        def __exit__(self, *_args):
            events.append("device close")

    device = Device()
    stub = Device()

    def detect_chip(*, port):
        events.append(("detect", port))
        print("detect: fake esptool stdout")
        return device

    def run_stub(actual):
        assert actual is device
        events.append("stub")
        print("stub: fake esptool stdout")
        return stub

    def attach_flash(actual):
        assert actual is stub
        events.append("attach")
        print("attach: fake esptool stdout")

    def read_flash(actual, offset, size, *, no_progress):
        assert actual is stub and no_progress
        events.append(("read", offset, size))
        print("read: fake esptool stdout")
        if fail_read == offset:
            raise OSError("fake flash read failed")
        if offset == 0x8000:
            assert size == 0x1000
            return table
        assert offset == 0x1000000 and size == 4096
        return raw

    def reset_chip(actual, mode):
        assert actual is (device if chip != "ESP32-S3" else stub)
        assert mode == "hard-reset"
        events.append("reset")
        print("reset: fake esptool stdout")

    @contextmanager
    def fake_lease(port):
        assert port == "/fake/original"
        events.append("lease acquired")
        try:
            yield "/fake/re-enumerated", ()
        finally:
            events.append("lease released")

    module = types.ModuleType("esptool.cmds")
    for name, fn in {
        "detect_chip": detect_chip,
        "run_stub": run_stub,
        "attach_flash": attach_flash,
        "read_flash": read_flash,
        "reset_chip": reset_chip,
    }.items():
        setattr(module, name, fn)
    monkeypatch.setitem(sys.modules, "esptool.cmds", module)
    monkeypatch.setattr(transport, "lease", fake_lease)
    return events, table, raw


def test_read_core_uses_one_session_and_keeps_stdout_binary(monkeypatch, capsysbinary):
    events, table, raw = fake_esptool(monkeypatch)
    transport.read_core("/fake/original")
    captured = capsysbinary.readouterr()
    assert captured.out == b"ORBCORE1" + struct.pack("<II", len(table), len(raw)) + table + raw
    assert captured.err.count(b"fake esptool stdout") == 6
    assert events == [
        "lease acquired",
        ("detect", "/fake/re-enumerated"),
        "device enter",
        "stub",
        "attach",
        ("read", 0x8000, 0x1000),
        ("read", 0x1000000, 4096),
        "reset",
        "device close",
        "lease released",
    ]


def test_read_core_missing_partition_resets_then_releases(monkeypatch, capsysbinary):
    events, _, _ = fake_esptool(monkeypatch, table=flash_table(subtype=2))
    with pytest.raises(ValueError, match="partition table deployed"):
        transport.read_core("/fake/original")
    captured = capsysbinary.readouterr()
    assert captured.out == b""
    assert ("read", 0x1000000, 4096) not in events
    assert events[-3:] == ["reset", "device close", "lease released"]


@pytest.mark.parametrize("offset", [0x8000, 0x1000000])
def test_read_core_flash_failure_still_resets_and_releases(monkeypatch, capsysbinary, offset):
    events, _, _ = fake_esptool(monkeypatch, fail_read=offset)
    with pytest.raises(OSError, match="fake flash read failed"):
        transport.read_core("/fake/original")
    assert capsysbinary.readouterr().out == b""
    assert events[-3:] == ["reset", "device close", "lease released"]


def test_read_core_rejects_short_read_without_partial_stdout(monkeypatch, capsysbinary):
    events, _, _ = fake_esptool(monkeypatch, raw=b"too short")
    with pytest.raises(ValueError, match="short flash read"):
        transport.read_core("/fake/original")
    assert capsysbinary.readouterr().out == b""
    assert events[-3:] == ["reset", "device close", "lease released"]


def test_read_core_rejects_wrong_chip_and_releases(monkeypatch, capsysbinary):
    events, _, _ = fake_esptool(monkeypatch, chip="ESP32-C6")
    with pytest.raises(ValueError, match="expected ESP32-S3"):
        transport.read_core("/fake/original")
    assert capsysbinary.readouterr().out == b""
    assert "stub" not in events
    assert events[-3:] == ["reset", "device close", "lease released"]


def test_streamed_receipt_arrives_before_daemon_done_and_survives_error(monkeypatch):
    prefix = b"@@ORBDEV SCROLLTRACE_PROGRESS phase=running\n"
    fake_daemon(
        monkeypatch,
        {"type": "data", "data": base64.b64encode(prefix).decode()},
        {"type": "error", "error": "device command timed out"},
    )
    prohibit_serial(monkeypatch)
    received = []
    with pytest.raises(RuntimeError, match="timed out"):
        transport.run_command("/fake", "bench_scroll agents 1000", on_data=received.append)
    assert b"".join(received) == prefix


def test_streaming_direct_serial_keeps_partial_output_on_timeout(monkeypatch):
    @contextmanager
    def absent(_request):
        raise transport.AbsentDaemon
        yield

    class Serial:
        in_waiting = 999

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            pass

        def reset_input_buffer(self):
            pass

        def write(self, _data):
            pass

        def read(self, _size):
            clock[0] += 1
            return b"@@ORBDEV SCROLLTRACE_PROGRESS phase=running\n"

    clock = [0]
    monkeypatch.setattr(transport, "daemon", absent)
    monkeypatch.setattr(transport, "open_port", lambda _port: Serial())
    monkeypatch.setattr(transport.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(transport, "command_timeout", lambda _cmd: 1)
    received = []
    with pytest.raises(TimeoutError, match="timed out"):
        transport.run_command("/fake", "bench_scroll agents 1000", on_data=received.append)
    assert received == [b"@@ORBDEV SCROLLTRACE_PROGRESS phase=running\n"]
