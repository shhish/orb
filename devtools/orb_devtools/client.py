"""Small synchronous client for orb-logd's newline-delimited JSON protocol."""

import base64
import json
import os
import socket
from contextlib import contextmanager
from pathlib import Path


class DaemonError(RuntimeError):
    """The daemon replied with an operational or protocol failure."""


def default_socket() -> Path:
    return Path(os.environ.get("ORB_LOGD_SOCKET", "~/.local/share/orb-logd/orb.sock")).expanduser()


def send(stream, value: dict) -> None:
    stream.write(json.dumps(value).encode() + b"\n")
    stream.flush()


def receive(stream) -> dict:
    raw = stream.readline(1024 * 1024)
    if not raw:
        raise DaemonError("daemon disconnected before completion")
    if not raw.endswith(b"\n"):
        raise DaemonError("oversized daemon response")
    frame = json.loads(raw)
    if frame.get("type") == "error":
        raise DaemonError(frame.get("error", "daemon error"))
    return frame


@contextmanager
def connection(path=None, timeout=130):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect(str(path or default_socket()))
        with sock.makefile("rwb") as stream:
            stream.socket_fd = sock.fileno()
            yield stream
    finally:
        sock.close()


def request(value: dict, path=None):
    """Yield frames until done. Socket absence/refusal remains an OSError."""
    with connection(path, timeout=None if value.get("follow") else 130) as stream:
        send(stream, value)
        while (frame := receive(stream))["type"] != "done":
            yield frame


def command(text: str, path=None, timeout=35):
    """Yield exact serial bytes, including device framing and interleaved logs."""
    for frame in request({"op": "command", "command": text, "timeout": timeout}, path):
        if frame["type"] == "data":
            yield base64.b64decode(frame["data"], validate=True)


@contextmanager
def lease(path=None, *, with_fd=False):
    """Keep open until the external serial owner exits.

    With ``with_fd=True``, yield (port, socket_fd); pass that FD to a child via
    subprocess pass_fds so even SIGKILL of the wrapper cannot reclaim early.
    """
    with connection(path, timeout=None) as stream:
        send(stream, {"op": "lease"})
        frame = receive(stream)
        if frame["type"] != "leased":
            raise DaemonError("expected a serial lease")
        try:
            yield (frame["port"], stream.socket_fd) if with_fd else frame["port"]
        finally:
            if not with_fd:
                send(stream, {"op": "release"})
                if receive(stream)["type"] != "done":
                    raise DaemonError("expected lease release acknowledgement")
            # With an inherited descriptor, EOF is the ownership boundary: an
            # explicit release could reclaim while a surviving child still runs.
