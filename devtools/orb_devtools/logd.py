"""Single serial owner, persistent console recorder and local Unix socket broker.

Protocol v1 is NDJSON, one operation per connection. Raw serial bytes travel in
base64 `data` frames. `done` and `error` terminate a response. A `lease` is held
by its socket until release/EOF, so crashed flash clients cannot strand pause.
"""

import argparse
import asyncio
import base64
import fcntl
import glob
import json
import logging
import math
import os
import re
import signal
import stat
import subprocess
import sys
import time
import uuid
from collections import deque
from datetime import datetime, timezone
from logging.handlers import RotatingFileHandler
from pathlib import Path

from . import client
from .serial_io import open_port

PANIC = re.compile(
    r"Guru Meditation|\bpanic(?:ked)?\b|Backtrace:|abort\(\)|assert(?:ion)? failed|Stack smashing",
    re.I,
)
BOOT = re.compile(r"ESP-ROM:|^ets \w{3}\s+\d|(?:^|\s)rst:0x")
MAX_REQUEST = 16 * 1024
MAX_RESPONSE = 4 * 1024 * 1024


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


GOTO_SURFACES = (
    "face",
    "stack",
    "apps",
    "agents",
    "sessions",
    "usage",
    "dot",
    "home",
    "time",
    "play",
    "notifications",
    "control",
    "settings",
    "sound",
    "battery",
    "updates",
    "onboarding",
)


def validate_command(command):
    if not isinstance(command, str) or len(command) > 90:
        raise ValueError("invalid command")
    args = command.split()
    arity = {
        "shot": 0,
        "top": 0,
        "heap": 0,
        "power": 0,
        "buttons": 0,
        "senses": 0,
        "wake": 0,
        "soundstatus": 0,
        "soundstop": 0,
        "micrecord": 0,
        "soundplay": 1,
        "soundvolume": 1,
        "soundsilent": 1,
        "soundalwayson": 1,
        "press": 2,
        "goto": 1,
        "tap": 2,
        "hold": 3,
        "swipe": 5,
        "arc": 4,
        "tilt": 3,
        "profile": 1,
        "bench": 1,
        "bench_transition": 1,
        "bench_quiet": 1,
        "bench_open": 1,
        "bench_game": 2,
        "bench_nav": 2,
        "bench_scroll": 2,
    }
    if not args or args[0] not in arity or len(args) != arity[args[0]] + 1:
        raise ValueError("unsupported command or wrong argument count")
    if command != " ".join(args):
        raise ValueError("command must use single spaces and no newlines")
    if args[0] == "tilt":
        x, y = map(float, args[1:3])
        if not all(math.isfinite(v) and -1 <= v <= 1 for v in (x, y)) or x * x + y * y > 1:
            raise ValueError("tilt requires finite X/Y -1..1g with X*X+Y*Y<=1")
        if not args[3].isdigit() or not 1 <= int(args[3]) <= 5000:
            raise ValueError("tilt duration must be 1..5000 ms")
        return command
    if args[0] == "bench_open":
        if args[1] not in ("play", "dot", "agents", "apps", "back"):
            raise ValueError("open surface must be play|dot|agents|apps|back")
        return command
    if args[0] in ("soundplay", "soundvolume", "soundsilent", "soundalwayson"):
        maximum = 18 if args[0] == "soundplay" else 100 if args[0] == "soundvolume" else 1
        if not args[1].isdigit() or not 0 <= int(args[1]) <= maximum:
            raise ValueError("sound argument outside supported range")
        return command
    if args[0] == "press":
        if args[1] not in ("pwr", "boot") or args[2] not in ("short", "double", "hold"):
            raise ValueError("press requires pwr|boot short|double|hold")
        return command
    if args[0] == "goto":
        if args[1] not in GOTO_SURFACES:
            raise ValueError("goto surface must be " + "|".join(GOTO_SURFACES))
        return command
    if args[0] == "arc":
        radius, a0, a1, duration = map(int, args[1:])
        if not 1 <= radius <= 232 or any(not -3600 <= angle <= 3600 for angle in (a0, a1)):
            raise ValueError("arc requires radius 1..232 and angles -3600..3600")
        if not 1 <= duration <= 5000:
            raise ValueError("arc duration must be 1..5000 ms")
        return command
    if args[0] == "bench_scroll":
        if args[1] not in ("agents", "rooms", "settings", "notifications", "favourites"):
            raise ValueError(
                "scroll surface must be agents|rooms|settings|notifications|favourites"
            )
        if not 1000 <= int(args[2]) <= 60000:
            raise ValueError("benchmark must last 1000..60000 ms")
        return command
    if args[0] == "bench_game":
        if args[1] not in ("marble", "orbit", "ring-snake", "pop"):
            raise ValueError("game must be marble|orbit|ring-snake|pop")
        if not 1000 <= int(args[2]) <= 60000:
            raise ValueError("benchmark must last 1000..60000 ms")
        return command
    if args[0] == "bench_nav":
        if args[1] not in (
            "slide-left",
            "slide-right",
            "stack-up",
            "shade-down",
            "back",
            "iris",
            "rotary",
        ):
            raise ValueError("unsupported navigation gesture")
        if not 1000 <= int(args[2]) <= 60000:
            raise ValueError("benchmark must last 1000..60000 ms")
        return command
    values = [int(value) for value in args[1:]]
    coords = values[:-1] if args[0] in ("hold", "swipe") else values
    if args[0] in ("profile", "bench", "bench_transition", "bench_quiet"):
        coords = []
    if any(not 0 <= value <= 465 for value in coords):
        raise ValueError("coordinates must be 0..465")
    if (
        args[0] in ("hold", "swipe", "profile", "bench", "bench_transition", "bench_quiet")
        and not 1 <= values[-1] <= 60000
    ):
        raise ValueError("duration must be 1..60000 ms")
    if args[0].startswith("bench") and values[-1] < 1000:
        raise ValueError("benchmark must last 1000..60000 ms")
    if args[0] == "profile" and values[-1] > 5000:
        raise ValueError("profile duration must be 1..5000 ms")
    return command


class PrivateRotatingFileHandler(RotatingFileHandler):
    def _open(self):
        stream = super()._open()
        os.fchmod(stream.fileno(), 0o600)
        return stream


class Recorder:
    """Rotating JSONL console and self-contained incident files with prelude."""

    def __init__(self, directory, max_bytes=5 * 1024 * 1024, backups=5, incident_idle=2):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.incidents = self.directory / "incidents"
        self.incidents.mkdir(mode=0o700, exist_ok=True)
        self.path = self.directory / "console.jsonl"
        self.handler = PrivateRotatingFileHandler(
            self.path, maxBytes=max_bytes, backupCount=backups, encoding="utf-8"
        )
        os.chmod(self.path, 0o600)
        self.prelude = deque(maxlen=100)
        self.boot = uuid.uuid4().hex[:12]
        self.last_boot = -100.0
        self.incident = None
        self.incident_bytes = 0
        self.incident_last = 0.0
        self.incident_idle = incident_idle
        self.identity_record = None

    def record(self, text):
        now = time.monotonic()
        primary_boot = "ESP-ROM:" in text or bool(re.match(r"^ets \w{3}\s+\d", text))
        if BOOT.search(text) and (primary_boot or now - self.last_boot > 1):
            self.finish_incident()
            self.prelude.clear()
            self.identity_record = None
            self.boot = uuid.uuid4().hex[:12]
            self.last_boot = now
            kind = "boot"
        else:
            kind = "console"
        record = {"time": utc_now(), "boot": self.boot, "kind": kind, "text": text}
        if "ELF file SHA256:" in text:
            self.identity_record = record
        encoded = json.dumps(record, ensure_ascii=True)
        self.handler.emit(logging.LogRecord("orb-logd", logging.INFO, "", 0, encoded, (), None))
        if PANIC.search(text) and self.incident is None:
            name = (
                datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
                + "-"
                + self.boot
                + ".jsonl"
            )
            self.incident = (self.incidents / name).open("x", encoding="utf-8")
            os.chmod(self.incident.name, 0o600)
            self.incident_bytes = 0
            if self.identity_record and self.identity_record not in self.prelude:
                self._incident_write(self.identity_record)
            for old in self.prelude:
                self._incident_write(old)
        if self.incident:
            self._incident_write(record)
            self.incident_last = now
            if self.incident_bytes >= MAX_RESPONSE:
                self.finish_incident()
        self.prelude.append(record)
        return record

    def _incident_write(self, record):
        line = json.dumps(record, ensure_ascii=True) + "\n"
        self.incident.write(line)
        self.incident.flush()
        self.incident_bytes += len(line)

    def tick(self):
        if self.incident and time.monotonic() - self.incident_last >= self.incident_idle:
            self.finish_incident()

    def finish_incident(self):
        if self.incident:
            self.incident.close()
            self.incident = None

    def tail(self, count, filters):
        result = deque(maxlen=count)
        paths = sorted(
            self.directory.glob("console.jsonl.*"),
            key=lambda p: int(p.suffix[1:]) if p.suffix[1:].isdigit() else -1,
            reverse=True,
        )
        for path in [*paths, self.path]:
            try:
                with path.open(encoding="utf-8") as stream:
                    for line in stream:
                        try:
                            record = json.loads(line)
                            if not filters or any(key in record["text"] for key in filters):
                                result.append(record)
                        except (ValueError, KeyError):
                            continue
            except FileNotFoundError:
                continue
        return list(result)

    def close(self):
        self.finish_incident()
        self.handler.close()


class LogDaemon:
    def __init__(
        self,
        port,
        socket_path,
        log_dir,
        *,
        opener=open_port,
        reconnect=0.25,
        max_bytes=5 * 1024 * 1024,
        backups=5,
    ):
        self.port_pattern = port
        self.socket_path = Path(socket_path)
        self.recorder = Recorder(log_dir, max_bytes, backups)
        self.opener = opener
        self.reconnect = reconnect
        self.port = None
        self.port_name = None
        self.paused = False
        self.command_lock = asyncio.Lock()
        self.command_queue = None
        self.followers = set()
        self.pending = bytearray()
        self.server = None
        self.reader_task = None
        self.clients = set()
        self.lock_file = None
        self.stopping = False
        self.last_error = None
        self.awaiting_ack = None

    async def start(self):
        self.socket_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        lock_path = self.socket_path.with_suffix(self.socket_path.suffix + ".lock")
        self.lock_file = lock_path.open("a")
        os.chmod(lock_path, 0o600)
        try:
            fcntl.flock(self.lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            self.lock_file.close()
            self.lock_file = None
            raise RuntimeError("another orb-logd owns this socket") from None
        if self.socket_path.exists():
            if not stat.S_ISSOCK(self.socket_path.lstat().st_mode):
                raise RuntimeError("socket path exists and is not a socket")
            self.socket_path.unlink()
        self.server = await asyncio.start_unix_server(
            self.handle_client, str(self.socket_path), limit=MAX_REQUEST
        )
        os.chmod(self.socket_path, 0o600)
        self.reader_task = asyncio.create_task(self.read_serial())
        return self

    def record(self, text):
        record = self.recorder.record(text)
        if self.awaiting_ack and (
            f"@@ORBDEV OK {self.awaiting_ack}" in text
            or text.startswith(f"@@ORBDEV ERROR {self.awaiting_ack.split()[0]} ")
            or record["kind"] == "boot"
        ):
            self.awaiting_ack = None
        for queue in tuple(self.followers):
            if queue.full():
                self.followers.discard(queue)
                while not queue.empty():
                    queue.get_nowait()
                queue.put_nowait(
                    RuntimeError("log client too slow; reconnect to recover disk tail")
                )
            else:
                queue.put_nowait(record)

    def close_port(self):
        if self.port:
            self.port.close()
            self.port = None
        if self.pending:
            self.record(bytes(self.pending).decode("utf-8", "replace"))
            self.pending.clear()

    def feed(self, data):
        if self.command_queue is not None:
            if self.command_queue.full():
                while not self.command_queue.empty():
                    self.command_queue.get_nowait()
                self.command_queue.put_nowait(RuntimeError("command output overflow"))
            else:
                self.command_queue.put_nowait(data)
        self.pending.extend(data)
        while b"\n" in self.pending:
            line, _, rest = self.pending.partition(b"\n")
            self.pending[:] = rest
            self.record(line.rstrip(b"\r").decode("utf-8", "replace"))
        if len(self.pending) > 65536:
            self.record(self.pending[:65536].decode("utf-8", "replace"))
            del self.pending[:65536]

    async def read_serial(self):
        while not self.stopping:
            self.recorder.tick()
            if self.paused:
                await asyncio.sleep(0.01)
                continue
            if self.port is None:
                matches = sorted(glob.glob(self.port_pattern))
                if len(matches) != 1:
                    self.last_error = (
                        "port missing"
                        if not matches
                        else "ambiguous port pattern; specify one device"
                    )
                    await asyncio.sleep(self.reconnect)
                    continue
                try:
                    self.port = self.opener(matches[0])
                    self.port_name = matches[0]
                    self.last_error = None
                    self.record(f"[orb-logd] connected {self.port_name}")
                except (OSError, ValueError) as error:
                    self.last_error = str(error)
                    await asyncio.sleep(self.reconnect)
                    continue
            try:
                data = self.port.read(65536)
                if data:
                    self.feed(data)
            except (OSError, ValueError) as error:
                self.last_error = str(error)
                self.record(f"[orb-logd] disconnected: {error}")
                self.close_port()
                if self.command_queue is not None:
                    if self.command_queue.full():
                        self.command_queue.get_nowait()
                    self.command_queue.put_nowait(
                        RuntimeError("serial disconnected during command")
                    )
            await asyncio.sleep(0.01)

    async def emit(self, writer, frame):
        writer.write(json.dumps(frame).encode() + b"\n")
        await asyncio.wait_for(writer.drain(), 5)

    async def read_request(self, reader):
        raw = await asyncio.wait_for(reader.readline(), 5)
        if not raw or len(raw) > MAX_REQUEST:
            raise ValueError("missing or oversized request")
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError("request must be an object")
        return value

    async def handle_client(self, reader, writer):
        task = asyncio.current_task()
        self.clients.add(task)
        try:
            request = await self.read_request(reader)
            op = request.get("op")
            if op == "status":
                await self.emit(
                    writer,
                    {
                        "type": "status",
                        "version": 1,
                        "connected": self.port is not None,
                        "paused": self.paused,
                        "port": self.port_name,
                        "boot": self.recorder.boot,
                        "error": self.last_error,
                        "awaiting_ack": self.awaiting_ack,
                    },
                )
            elif op == "command":
                await self.run_command(request, writer)
            elif op == "logs":
                await self.logs(request, reader, writer)
            elif op == "lease":
                await self.run_lease(reader, writer)
            else:
                raise ValueError("unknown operation")
            await self.emit(writer, {"type": "done"})
        except (TypeError, ValueError, OSError, RuntimeError, asyncio.TimeoutError) as error:
            try:
                await self.emit(
                    writer, {"type": "error", "error": str(error) or "operation timed out"}
                )
            except (OSError, asyncio.TimeoutError):
                pass
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass
            self.clients.discard(task)

    async def run_command(self, request, writer):
        command = validate_command(request.get("command"))
        timeout = float(request.get("timeout", 35))
        if not 0.05 <= timeout <= 120:
            raise ValueError("timeout must be 0.05..120 seconds")
        if self.paused or self.command_lock.locked() or self.awaiting_ack:
            raise RuntimeError("serial busy or awaiting completion of a timed-out command")
        async with self.command_lock:
            if not self.port:
                raise RuntimeError("serial device disconnected")
            queue = asyncio.Queue(maxsize=256)
            self.command_queue = queue
            buffer = b""
            size = 0
            deadline = time.monotonic() + timeout
            disconnected = False
            complete = False
            capture_failed = False
            try:
                self.port.write(command.encode() + b"\n")
                while True:
                    chunk = await asyncio.wait_for(
                        queue.get(), max(0.001, deadline - time.monotonic())
                    )
                    if isinstance(chunk, Exception):
                        raise chunk
                    size += len(chunk)
                    if size > MAX_RESPONSE:
                        raise RuntimeError("command response exceeds 4 MiB")
                    buffer = (buffer + chunk)[-65536:]
                    if not disconnected:
                        try:
                            await self.emit(
                                writer, {"type": "data", "data": base64.b64encode(chunk).decode()}
                            )
                        except (OSError, asyncio.TimeoutError):
                            # Finish draining the in-flight command before another
                            # client may issue one, even if its requester died.
                            disconnected = True
                    if b"@@ORBSHOT FAIL " in buffer:
                        capture_failed = True
                    if re.search(rb"@@ORBDEV (?:\?|ERROR) [^\r\n]*[\r]?\n", buffer):
                        complete = True
                        raise RuntimeError("firmware rejected command")
                    if re.search(
                        rb"@@ORBDEV OK " + re.escape(command.encode()) + rb"\r?\n", buffer
                    ):
                        complete = True
                        if capture_failed:
                            raise RuntimeError("device failed to capture screenshot")
                        return
                    if time.monotonic() >= deadline:
                        raise asyncio.TimeoutError
            finally:
                self.command_queue = None
                if not complete:
                    self.awaiting_ack = command

    async def run_lease(self, reader, writer):
        if self.paused or self.command_lock.locked():
            raise RuntimeError("serial busy")
        async with self.command_lock:
            if self.port_name is None:
                matches = sorted(glob.glob(self.port_pattern))
                if len(matches) != 1:
                    raise RuntimeError("cannot lease absent or ambiguous serial port")
                self.port_name = matches[0]
            self.paused = True
            self.close_port()
            self.awaiting_ack = None
            self.record("[orb-logd] port released for external owner")
            try:
                await self.emit(writer, {"type": "leased", "port": self.port_name})
                raw = await reader.readline()
                if raw:
                    request = json.loads(raw)
                    if not isinstance(request, dict) or request.get("op") != "release":
                        raise ValueError("lease expects release or EOF")
            finally:
                self.paused = False
                self.record("[orb-logd] lease ended; reclaiming port")

    async def logs(self, request, reader, writer):
        count = request.get("tail", 100)
        filters = request.get("filters", [])
        seconds = float(request.get("seconds", 0))
        if isinstance(count, bool) or not isinstance(count, int) or not 0 <= count <= 10000:
            raise ValueError("tail must be 0..10000")
        if (
            not isinstance(filters, list)
            or len(filters) > 32
            or any(not isinstance(key, str) or len(key) > 256 for key in filters)
        ):
            raise ValueError("filters must be up to 32 strings of at most 256 characters")
        if not 0 <= seconds <= 86400:
            raise ValueError("seconds must be 0..86400")
        queue = asyncio.Queue(maxsize=1000)
        follow = request.get("follow", False)
        if not isinstance(follow, bool):
            raise ValueError("follow must be boolean")
        if follow:
            self.followers.add(queue)
        try:
            for record in self.recorder.tail(count, filters):
                await self.emit(writer, {"type": "log", "record": record})
            if not follow:
                return
            deadline = time.monotonic() + seconds if seconds else float("inf")
            eof = asyncio.create_task(reader.read(1))
            try:
                while time.monotonic() < deadline:
                    item = asyncio.create_task(queue.get())
                    done, _ = await asyncio.wait(
                        [item, eof],
                        timeout=min(1, deadline - time.monotonic()),
                        return_when=asyncio.FIRST_COMPLETED,
                    )
                    if item not in done:
                        item.cancel()
                        await asyncio.gather(item, return_exceptions=True)
                    if eof in done:
                        return
                    if item in done:
                        record = item.result()
                        if isinstance(record, Exception):
                            raise record
                        if not filters or any(key in record["text"] for key in filters):
                            await self.emit(writer, {"type": "log", "record": record})
            finally:
                eof.cancel()
                await asyncio.gather(eof, return_exceptions=True)
        finally:
            self.followers.discard(queue)

    async def stop(self):
        self.stopping = True
        if self.server:
            self.server.close()
            await self.server.wait_closed()
        for task in list(self.clients):
            task.cancel()
        await asyncio.gather(*list(self.clients), return_exceptions=True)
        if self.reader_task:
            self.reader_task.cancel()
            await asyncio.gather(self.reader_task, return_exceptions=True)
        self.close_port()
        self.recorder.close()
        if self.server and self.socket_path.exists():
            self.socket_path.unlink()
        if self.lock_file:
            self.lock_file.close()


async def serve(args):
    daemon = LogDaemon(
        args.port, args.socket, args.log_dir, max_bytes=args.max_bytes, backups=args.backups
    )
    stopped = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stopped.set)
    try:
        await daemon.start()
        await stopped.wait()
    finally:
        await daemon.stop()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", type=Path, default=client.default_socket())
    sub = parser.add_subparsers(dest="op", required=True)
    server = sub.add_parser("serve")
    server.add_argument(
        "--port",
        default="/dev/cu.usbmodem1101",
        help="exact path or glob resolving to exactly one device",
    )
    server.add_argument(
        "--log-dir", type=Path, default=Path("~/Library/Logs/orb-logd").expanduser()
    )
    server.add_argument("--max-bytes", type=int, default=5 * 1024 * 1024)
    server.add_argument("--backups", type=int, default=5)
    sub.add_parser("status")
    logs = sub.add_parser("logs")
    logs.add_argument("--tail", type=int, default=100)
    logs.add_argument("--follow", action="store_true")
    logs.add_argument("--seconds", type=float, default=0)
    logs.add_argument("--filter", action="append", default=[])
    logs.add_argument("--json", action="store_true")
    command = sub.add_parser("command")
    command.add_argument("--timeout", type=float, default=35)
    command.add_argument("command", nargs=argparse.REMAINDER)
    execute = sub.add_parser("exec")
    execute.add_argument("argv", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        if args.op == "serve":
            if args.max_bytes < 1024 or not 1 <= args.backups <= 100:
                parser.error("max-bytes must be >=1024 and backups 1..100")
            os.umask(0o077)
            asyncio.run(serve(args))
        elif args.op == "command":
            for chunk in client.command(" ".join(args.command), args.socket, args.timeout):
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
        elif args.op == "exec":
            argv = args.argv[1:] if args.argv[:1] == ["--"] else args.argv
            if not argv:
                parser.error("exec requires an executable after --")
            with client.lease(args.socket, with_fd=True) as (port, lease_fd):
                # Child exits before the lease closes, including Ctrl-C.
                process = subprocess.Popen(
                    argv, env={**os.environ, "ORB_SERIAL_PORT": port}, pass_fds=(lease_fd,)
                )
                try:
                    return process.wait()
                except BaseException:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    raise
        else:
            request = {"op": args.op}
            if args.op == "logs":
                request.update(
                    tail=args.tail, follow=args.follow, seconds=args.seconds, filters=args.filter
                )
            for frame in client.request(request, args.socket):
                if frame["type"] == "log" and not args.json:
                    row = frame["record"]
                    print(f"{row['time']} [{row['boot']}] {row['text']}", flush=True)
                else:
                    print(json.dumps(frame), flush=True)
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"orb-logd: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
