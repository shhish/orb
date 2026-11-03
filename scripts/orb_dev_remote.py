"""Single-file USB transport, sent over SSH stdin; no orb package required remotely.

Prefer orb-logd's v1 NDJSON socket. Fall back only if no daemon is listening;
protocol, busy and permission errors never authorize opening its serial port.
"""

import base64
import contextlib
import errno
import hashlib
import json
import math
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

import serial

LINE = re.compile(rb"@@([0-9a-f]{5}) ([A-Za-z0-9+/=]+)")
BEGIN = re.compile(rb"@@ORBSHOT BEGIN (\d+) (\d+) rgb565le (\d+) (\d+) ([0-9a-f]{8})")
MAX_REPLY = 8 * 1024 * 1024


class AbsentDaemon(Exception):
    """The daemon socket does not exist or has no listener."""


@contextlib.contextmanager
def daemon(request):
    path = os.environ.get("ORB_LOGD_SOCKET", "~/.local/share/orb-logd/orb.sock")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        try:
            sock.connect(os.path.expanduser(path))
        except OSError as exc:
            if exc.errno in (errno.ENOENT, errno.ECONNREFUSED):
                raise AbsentDaemon from exc
            raise
        sock.sendall(json.dumps(request).encode() + b"\n")
        sock.settimeout(max(15, float(request.get("timeout", 35)) + 10))
        with sock.makefile("rb") as reader:
            yield sock, reader
    finally:
        sock.close()


def frames(reader):
    while True:
        line = reader.readline(2 * 1024 * 1024)
        if not line or not line.endswith(b"\n"):
            raise RuntimeError("orb-logd disconnected or sent an oversized frame")
        frame = json.loads(line)
        if frame["type"] == "error":
            raise RuntimeError(frame["error"])
        if frame["type"] == "done":
            return
        yield frame


def open_port(port: str) -> serial.Serial:
    """Open without resetting the chip: leave DTR/RTS as macOS opens them, clear HUPCL.

    The USB-Serial-JTAG resets the chip while RTS is asserted and DTR isn't; lowering
    DTR before RTS (dtr=False, rts=False) passes through exactly that state.
    """
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.exclusive = True
    s.open()
    try:
        import termios

        attrs = termios.tcgetattr(s.fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(s.fd, termios.TCSANOW, attrs)
    except BaseException:
        s.close()
        raise
    return s


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
    "weather",
)


def command_timeout(command):
    words = command.split()
    if command != " ".join(words):
        raise ValueError("command must use single spaces and no newlines")
    sizes = {
        "shot": 0,
        "top": 0,
        "heap": 0,
        "power": 0,
        "lightsleep": 2,
        "buttons": 0,
        "wake": 0,
        "sleep": 0,
        "crash": 0,
        "usbreattach": 0,
        "senses": 0,
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
    if not words or words[0] not in sizes or len(words) != sizes[words[0]] + 1:
        raise ValueError(
            "expected shot/top/heap/power/tap X Y/hold X Y MS/swipe X1 Y1 X2 Y2 MS/arc R A0 A1 MS/profile MS/bench[_transition|_quiet] MS"
        )
    if words[0] == "bench_open":
        if words[1] not in ("play", "dot", "agents", "apps", "back"):
            raise ValueError("open surface must be play|dot|agents|apps|back")
        return 20
    if words[0] == "micrecord":
        return 90  # Five seconds capture, then bounded base64 stereo output.
    if words[0] == "lightsleep":
        if words[1] != "usb" or words[2] not in ("on", "off"):
            raise ValueError("lightsleep requires usb on|off")
        return 10
    if words[0] == "tilt":
        x, y = map(float, words[1:3])
        if not all(math.isfinite(v) and -1 <= v <= 1 for v in (x, y)) or x * x + y * y > 1:
            raise ValueError("tilt requires finite X/Y -1..1g with X*X+Y*Y<=1")
        if not words[3].isdigit() or not 1 <= int(words[3]) <= 5000:
            raise ValueError("tilt duration must be 1..5000 ms")
        return 10
    if words[0] in ("soundplay", "soundvolume", "soundsilent", "soundalwayson"):
        maximum = 18 if words[0] == "soundplay" else 100 if words[0] == "soundvolume" else 1
        if not words[1].isdigit() or not 0 <= int(words[1]) <= maximum:
            raise ValueError("sound argument outside supported range")
        return 10
    if words[0] == "press":
        if words[1] not in ("pwr", "boot") or words[2] not in ("short", "double", "hold"):
            raise ValueError("press requires pwr|boot short|double|hold")
        return 10
    if words[0] == "goto":
        if words[1] not in GOTO_SURFACES:
            raise ValueError("goto surface must be " + "|".join(GOTO_SURFACES))
        return 10
    if words[0] == "arc":
        radius, a0, a1, duration = map(int, words[1:])
        if not 1 <= radius <= 232 or any(not -3600 <= angle <= 3600 for angle in (a0, a1)):
            raise ValueError("arc requires radius 1..232 and angles -3600..3600")
        if not 1 <= duration <= 5000:
            raise ValueError("arc duration must be 1..5000 ms")
        return 10 + duration / 1000
    if words[0] == "bench_scroll":
        if words[1] not in ("agents", "rooms", "settings", "notifications", "favourites"):
            raise ValueError(
                "scroll surface must be agents|rooms|settings|notifications|favourites"
            )
        duration = int(words[2])
        if not 1000 <= duration <= 60000:
            raise ValueError("benchmark must last 1000..60000 ms")
        # At most 2048 packed records emit about 48 KiB including framing.
        # Allow 10s setup + 10s backpressured USB output + 5s margin.
        return 25 + duration / 1000
    if words[0] == "bench_game":
        if words[1] not in ("marble", "beads", "orbit", "ring-snake", "pop"):
            raise ValueError("game must be marble|beads|orbit|ring-snake|pop")
        duration = int(words[2])
        if not 1000 <= duration <= 60000:
            raise ValueError("benchmark must last 1000..60000 ms")
        return 10 + duration / 1000
    if words[0] == "bench_nav":
        if words[1] not in (
            "slide-left",
            "slide-right",
            "stack-up",
            "shade-down",
            "back",
            "iris",
            "rotary",
        ):
            raise ValueError("unsupported navigation gesture")
        duration = int(words[2])
        if not 1000 <= duration <= 60000:
            raise ValueError("benchmark must last 1000..60000 ms")
        return 10 + duration / 1000
    values = [int(x) for x in words[1:]]
    timed = words[0] in ("hold", "swipe", "profile", "bench", "bench_transition", "bench_quiet")
    coords = values[:-1] if timed else values
    if any(x < 0 or x > 465 for x in coords):
        raise ValueError("coordinates must be 0..465")
    if timed and not 1 <= values[-1] <= (5000 if words[0] == "profile" else 60000):
        raise ValueError("duration outside supported range")
    if words[0].startswith("bench") and values[-1] < 1000:
        raise ValueError("benchmark must last 1000..60000 ms")
    return 40 if words[0] == "shot" else 10 + (values[-1] / 1000 if timed else 0)


def complete(buf, command):
    lines = buf.split(b"\n")[:-1]
    if any(b"@@ORBDEV ERROR" in line or b"@@ORBSHOT FAIL" in line for line in lines):
        raise RuntimeError("device rejected command: " + buf.decode(errors="replace")[-500:])
    if any(b"@@ORBDEV ?" in line for line in lines):
        raise RuntimeError("command unsupported by this firmware")
    marker = b"@@ORBDEV OK " + command.encode()
    return any(line.rstrip(b"\r").endswith(marker) for line in lines)


def run_command(port, command, *, on_data=None):
    timeout = command_timeout(command)
    buf = bytearray()

    def receive(data):
        buf.extend(data)
        if len(buf) > MAX_REPLY:
            raise RuntimeError("reply exceeds 8 MiB")
        if on_data is not None:
            on_data(data)

    try:
        with daemon({"op": "command", "command": command, "timeout": timeout}) as (_, reader):
            for frame in frames(reader):
                if frame["type"] == "data":
                    receive(base64.b64decode(frame["data"], validate=True))
    except AbsentDaemon:
        with open_port(port) as s:
            s.reset_input_buffer()
            s.write(command.encode() + b"\n")
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                receive(s.read(s.in_waiting or 1))
                if complete(buf, command):
                    break
            else:
                raise TimeoutError("device command timed out")
    if not complete(buf, command):
        raise RuntimeError("incomplete device reply")
    return bytes(buf)


def png(w, h, rgb565):
    """RGBA PNG, transparent outside the round panel."""
    r2, cx, cy = (min(w, h) / 2) ** 2, (w - 1) / 2, (h - 1) / 2
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            v = struct.unpack_from("<H", rgb565, (y * w + x) * 2)[0]
            inside = (x - cx) ** 2 + (y - cy) ** 2 <= r2
            r, g, b = (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3
            row += bytes([r | r >> 5, g | g >> 6, b | b >> 5, 255 if inside else 0])
        rows.append(bytes(row))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
        + chunk(b"IEND", b"")
    )


def decode_shot(buf):
    match = BEGIN.search(buf)
    if not match or b"@@ORBSHOT END" not in buf[match.end() :]:
        raise ValueError("missing screenshot frame")
    w, h, per, length = (int(match[i]) for i in range(1, 5))
    crc = int(match[5], 16)
    if not (1 <= w <= 466 and 1 <= h <= 466 and 1 <= per <= 4096 and length == w * h * 2):
        raise ValueError("invalid screenshot dimensions/length")
    parts = {}
    for idx, data in LINE.findall(buf[match.end() :]):
        i = int(idx, 16)
        try:
            block = base64.b64decode(data, validate=True)
        except ValueError:
            continue
        if 0 <= i < math.ceil(length / per) and len(block) == min(per, length - i * per):
            parts.setdefault(i, block)
    raw = b"".join(parts.get(i, b"") for i in range(math.ceil(length / per)))
    if len(raw) != length or zlib.crc32(raw) != crc:
        raise ValueError("screenshot length/CRC mismatch")
    return png(w, h, raw)


@contextlib.contextmanager
def lease(port):
    try:
        manager = daemon({"op": "lease"})
        sock, reader = manager.__enter__()
    except AbsentDaemon:
        yield port, ()
        return
    try:
        frame = next(frames(reader))
        if frame["type"] != "leased":
            raise RuntimeError("orb-logd did not grant serial lease")
        # Let a surviving child retain the lease even if this wrapper is SIGKILLed.
        yield frame["port"], (sock.fileno(),)
    finally:
        # Closing the connection releases even if the child fails or is interrupted.
        manager.__exit__(None, None, None)


def execute(port, args, cwd=None):
    with lease(port) as (actual_port, inherited_fds):
        argv = [os.path.expanduser(x.replace("{port}", actual_port)) for x in args]
        return subprocess.call(
            argv, cwd=os.path.expanduser(cwd) if cwd else None, pass_fds=inherited_fds
        )


def read_flash(port, offset, size):
    if offset < 0 or size <= 0 or offset + size > 32 * 1024 * 1024:
        raise ValueError("flash range outside board's 32 MiB")
    with tempfile.TemporaryDirectory(prefix="orb-crash-") as temp, lease(port) as lease_info:
        actual_port, inherited_fds = lease_info
        path = Path(temp) / "flash.bin"
        subprocess.run(
            [
                sys.executable,
                "-m",
                "esptool",
                "--chip",
                "esp32s3",
                "--port",
                actual_port,
                "read-flash",
                hex(offset),
                hex(size),
                str(path),
            ],
            check=True,
            stdout=sys.stderr,
            pass_fds=inherited_fds,
        )
        sys.stdout.buffer.write(path.read_bytes())


def core_partition(table):
    """Minimal partition reader kept in this standalone SSH helper."""
    found = []
    for pos in range(0, len(table) - 31, 32):
        magic, kind, subtype, offset, size, _name, flags = struct.unpack_from(
            "<HBBII16sI", table, pos
        )
        if magic == 0xEBEB:
            checksum = hashlib.md5(table[:pos], usedforsecurity=False).digest()
            if checksum != table[pos + 16 : pos + 32]:
                raise ValueError("partition checksum mismatch")
            break
        if magic != 0x50AA:
            raise ValueError("invalid partition table or missing checksum")
        if kind == 1 and subtype == 3:
            if flags & 1 or not size or offset < 0x9000 or offset + size > 32 * 1024 * 1024:
                raise ValueError("unsupported encrypted core partition or invalid bounds")
            found.append((offset, size))
    else:
        raise ValueError("partition table missing checksum")
    if len(found) != 1:
        raise ValueError("device needs the coredump partition table deployed first")
    return found[0]


def read_core(port):
    # Public esptool 5.6 API: one bootloader session and one daemon lease.
    # The application never runs between the table read and the core read.
    from esptool.cmds import attach_flash, detect_chip, read_flash, reset_chip, run_stub

    with lease(port) as (actual_port, _), contextlib.redirect_stdout(sys.stderr):
        with detect_chip(port=actual_port) as device:
            try:
                if device.CHIP_NAME != "ESP32-S3":
                    raise ValueError("expected ESP32-S3")
                device = run_stub(device)
                attach_flash(device)
                table = read_flash(device, 0x8000, 0x1000, no_progress=True)
                offset, size = core_partition(table)
                raw = read_flash(device, offset, size, no_progress=True)
                if len(table) != 0x1000 or len(raw) != size:
                    raise ValueError("short flash read")
            finally:
                reset_chip(device, "hard-reset")
    sys.stdout.buffer.write(b"ORBCORE1" + struct.pack("<II", len(table), len(raw)) + table + raw)


def logs(port, seconds, pattern, *, tail=100):
    if not math.isfinite(seconds) or seconds < 0:
        raise ValueError("seconds must be nonnegative and finite (0 means forever)")
    keys = [x for x in pattern.split("|") if x]
    try:
        with daemon(
            {"op": "logs", "tail": tail, "follow": True, "seconds": seconds, "filters": keys}
        ) as (sock, reader):
            sock.settimeout(None)
            for frame in frames(reader):
                if frame["type"] == "log":
                    record = frame["record"]
                    print(f"{record['time']} {record['text']}", flush=True)
    except AbsentDaemon:
        with open_port(port) as s:
            deadline = time.monotonic() + seconds if seconds else float("inf")
            buf = bytearray()
            while time.monotonic() < deadline:
                buf.extend(s.read(s.in_waiting or 1))
                while b"\n" in buf:
                    line, _, buf = buf.partition(b"\n")
                    line = line.decode(errors="replace").rstrip("\r")
                    if not keys or any(key in line for key in keys):
                        print(line, flush=True)
                if len(buf) > MAX_REPLY:
                    raise RuntimeError("console line too long")


def main():
    if len(sys.argv) < 3:
        raise ValueError("usage: orb_dev_remote.py PORT COMMAND [ARGS]")
    port, args = sys.argv[1], sys.argv[2:]
    if args[0] == "--exec":
        args = args[1:]
        cwd = None
        if args[:1] == ["--cwd"]:
            cwd, args = args[1], args[2:]
        if not args:
            raise ValueError("--exec needs a program")
        return execute(port, args, cwd)
    if args[0] == "--read-core":
        read_core(port)
    elif args[0] == "--read-flash":
        read_flash(port, int(args[1], 0), int(args[2], 0))
    elif args[0] in ("--logs", "--logs-new"):
        logs(
            port,
            float(args[1]),
            args[2] if len(args) > 2 else "",
            tail=0 if args[0] == "--logs-new" else 100,
        )
    else:
        command = " ".join(args)
        if args[0] == "bench_scroll":
            # Persist progress and partial traces even when the terminal ACK is
            # lost. Keep protocol prefixes intact for scroll_trace.py.
            def emit(data):
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()

            run_command(port, command, on_data=emit)
            return 0
        for attempt in range(3):
            buf = run_command(port, command)
            if command != "shot":
                if args[0] in ("profile", "power", "senses", "micrecord") or args[0].startswith(
                    "sound"
                ):
                    sys.stdout.buffer.write(buf)
                else:
                    for line in buf.decode(errors="replace").splitlines():
                        if line.startswith(("@@TOP", "@@HEAP", "@@ORBDEV")):
                            print(line[2:])
                break
            try:
                sys.stdout.buffer.write(decode_shot(buf))
                break
            except ValueError:
                if attempt == 2:
                    raise
                print("screenshot damaged; retrying", file=sys.stderr)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130) from None
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"orb transport: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
