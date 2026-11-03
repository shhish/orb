"""The control socket: how CLI commands reach the running hub.

A unix socket in the data directory (mode 600, in a mode-700 directory). One JSON request per connection,
{"cmd": "...", "args": {...}}, answered by one JSON line: {"ok": true, "result": ...} or
{"ok": false, "error": "..."}.
"""

import asyncio
import json
import logging
import os
import socket
from collections.abc import Awaitable, Callable
from pathlib import Path
from typing import Any

MAX_REQUEST = 64 * 1024
# Developer output is at most 2 MiB of text; JSON escaping can expand it 6x.
MAX_RESPONSE = 16 * 1024 * 1024
# macOS limits a unix socket path to 104 bytes.
MAX_PATH = 103

log = logging.getLogger(__name__)

Handler = Callable[[str, dict], Awaitable[Any]]


class ControlError(Exception):
    """A command failed; the message is meant for the user."""


class NotRunning(ControlError):
    pass


class ControlServer:
    def __init__(self, path: Path, handler: Handler):
        self.path = path
        self.handler = handler
        self._server: asyncio.Server | None = None

    async def start(self) -> None:
        if len(str(self.path).encode()) > MAX_PATH:
            raise ControlError(f"control socket path is too long for macOS: {self.path}")
        if self.path.exists():
            if _answers(self.path):
                raise ControlError(f"another orb-hub is already running ({self.path})")
            self.path.unlink()  # left behind by a hub that didn't shut down cleanly
        self._server = await asyncio.start_unix_server(self._serve, path=str(self.path), limit=MAX_REQUEST)
        os.chmod(self.path, 0o600)

    async def stop(self) -> None:
        if self._server:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
            self.path.unlink(missing_ok=True)

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            line = await reader.readline()
            try:
                request = json.loads(line)
                cmd = request["cmd"]
                args = request.get("args", {})
                if not isinstance(cmd, str) or not isinstance(args, dict):
                    raise TypeError
            except (ValueError, KeyError, TypeError):
                reply = {"ok": False, "error": "malformed request"}
            else:
                try:
                    reply = {"ok": True, "result": await self.handler(cmd, args)}
                except ControlError as exc:
                    reply = {"ok": False, "error": str(exc)}
                except Exception:
                    log.exception("control command %s failed", cmd)
                    reply = {"ok": False, "error": f"{cmd} failed inside the hub; see its log"}
            writer.write(json.dumps(reply).encode() + b"\n")
            await writer.drain()
        except (ConnectionError, asyncio.LimitOverrunError, ValueError):
            pass
        finally:
            writer.close()


def _answers(path: Path) -> bool:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        try:
            s.connect(str(path))
        except OSError:
            return False
    return True


def call(path: Path, cmd: str, args: dict | None = None, timeout: float = 30.0) -> Any:
    """Runs a command in the hub. NotRunning when no hub listens on path; ControlError when the command fails."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        try:
            s.connect(str(path))
        except (FileNotFoundError, ConnectionRefusedError):
            raise NotRunning(f"orb-hub isn't running (no control socket at {path})") from None
        s.sendall(json.dumps({"cmd": cmd, "args": args or {}}).encode() + b"\n")
        data = b""
        try:
            while not data.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                data += chunk
                if len(data) > MAX_RESPONSE:
                    raise ControlError("the hub control response exceeded its budget")
        except TimeoutError:
            raise ControlError(f"the hub didn't answer {cmd} within {timeout:.0f} s") from None
    try:
        reply = json.loads(data)
    except ValueError:
        raise ControlError("the hub closed the control connection without answering") from None
    if not reply.get("ok"):
        raise ControlError(reply.get("error", "unknown error"))
    return reply.get("result")
