"""Bounded developer commands on a single authenticated, paired TLS connection."""

import asyncio
import base64
import binascii
import math
import re
import struct
import zlib
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from orb_hub.control import ControlError
from orb_hub.envelope import LinkError

if TYPE_CHECKING:
    from orb_hub.link import Link
    from orb_hub.server import Hub

MAX_OUTPUT = 2 * 1024 * 1024
MAX_EVENTS = 8192
DEADLINE = 90.0
MAX_STREAM = 2147483647
OPEN_SURFACES = ("play", "dot", "agents", "apps", "back")
SCROLL_SURFACES = ("agents", "rooms", "settings", "notifications", "favourites")
GAMES = ("marble", "beads", "orbit", "ring-snake", "pop")
# Tuple entries are inclusive integer/float bounds or a string allowlist.
COMMANDS = {
    "shot": (),
    "tap": ((0, 465), (0, 465)),
    "hold": ((0, 465), (0, 465), (1, 5000)),
    "swipe": ((0, 465), (0, 465), (0, 465), (0, 465), (1, 5000)),
    "arc": ((1, 232), (-3600, 3600), (-3600, 3600), (1, 5000)),
    "tilt": ((-1.0, 1.0), (-1.0, 1.0), (1, 5000)),
    "bench": ((1000, 60000),),
    "bench_open": (OPEN_SURFACES,),
    "bench_nav": (OPEN_SURFACES,),
    "bench_scroll": (SCROLL_SURFACES, (1000, 60000)),
    "bench_game": (GAMES, (1000, 60000)),
    "heap": (),
    "top": (),
    "power": (),
    "lightsleep": (("usb",), ("on", "off")),
    "buttons": (),
    "wake": (),
    "sleep": (),
    "crash": (),
    "usbreattach": (),
    "senses": (),
    "soundstatus": (),
    "soundstop": (),
    "micrecord": (),
    "soundplay": ((0, 18),),
    "soundvolume": ((0, 100),),
    "soundsilent": ((0, 1),),
    "soundalwayson": ((0, 1),),
    "press": (("pwr", "boot"), ("short", "double", "hold")),
    "goto": (
        (
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
        ),
    ),
}
HEADER = re.compile(r"@@ORBSHOT BEGIN 466 466 rgb565le 192 434312 ([0-9a-fA-F]{8})")
CHUNK = re.compile(r"@@([0-9a-f]{5}) ([A-Za-z0-9+/]+={0,2})")
ERROR_RECEIPT = re.compile(r"@@(?:ORBDEV ERROR|ORBSHOT FAIL|TOP ERROR) [A-Za-z0-9 @_.:=,%+|();-]{1,1000}")
ERROR_CODES = {"bad_request", "unauthorized", "unsupported", "busy", "internal", "timeout", "too_large"}


def validate(command: str, args: list) -> None:
    specification = COMMANDS.get(command) if isinstance(command, str) else None
    if specification is None or not isinstance(args, list) or len(args) != len(specification):
        raise ControlError("invalid developer command or argument count")
    for value, permitted in zip(args, specification, strict=True):
        if isinstance(permitted[0], int):
            valid = type(value) is int and permitted[0] <= value <= permitted[1]
        elif isinstance(permitted[0], float):
            valid = type(value) in (int, float) and permitted[0] <= value <= permitted[1] and math.isfinite(value)
        else:
            valid = isinstance(value, str) and value in permitted
        if not valid:
            raise ControlError("developer argument is outside its allowed values")
    if command == "tilt" and args[0] * args[0] + args[1] * args[1] > 1:
        raise ControlError("tilt specific force must lie within the unit disc")


def screenshot_png(text: str) -> bytes:
    """Validate the USB-compatible ordered CRC transfer before encoding any PNG."""
    raw = bytearray()
    crc = None
    ended = False
    index = 0
    for line in text.splitlines():
        if line.startswith("@@ORBSHOT BEGIN"):
            match = HEADER.fullmatch(line)
            if crc is not None or not match:
                raise ControlError("invalid screenshot header")
            crc = int(match[1], 16)
        elif line == "@@ORBSHOT END":
            if crc is None or ended or len(raw) != 434312:
                raise ControlError("incomplete screenshot")
            ended = True
        elif line.startswith("@@ORBSHOT"):
            raise ControlError("screenshot capture failed")
        elif match := CHUNK.fullmatch(line):
            if crc is None or ended or int(match[1], 16) != index:
                raise ControlError("screenshot chunks are out of order")
            try:
                chunk = base64.b64decode(match[2], validate=True)
            except binascii.Error:
                raise ControlError("invalid screenshot encoding") from None
            expected = min(192, 434312 - len(raw))
            if expected <= 0 or len(chunk) != expected or base64.b64encode(chunk).decode() != match[2]:
                raise ControlError("invalid screenshot chunk size or encoding")
            raw.extend(chunk)
            index += 1
        elif line.startswith("@@") and crc is not None and not ended:
            raise ControlError("invalid screenshot chunk")
    if not ended or zlib.crc32(raw) != crc:
        raise ControlError("screenshot is incomplete or its CRC does not match")
    pixels = bytearray()
    for y in range(466):
        pixels.append(0)  # PNG filter: none.
        for x in range(466):
            offset = (y * 466 + x) * 2
            value = raw[offset] | raw[offset + 1] << 8
            r, g, b = value >> 11, value >> 5 & 63, value & 31
            alpha = 255 if (x - 232.5) ** 2 + (y - 232.5) ** 2 <= 233**2 else 0
            pixels.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), alpha))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack("!I", len(data)) + kind + data + struct.pack("!I", zlib.crc32(kind + data))

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack("!IIBBBBB", 466, 466, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(pixels))
        + chunk(b"IEND", b"")
    )


@dataclass
class Transfer:
    stream: int
    finished: asyncio.Future
    fragments: list[str] = field(default_factory=list)
    size: int = 0
    sequence: int = 0

    def fail(self, reason: str) -> None:
        if not self.finished.done():
            self.finished.set_exception(ControlError(reason))


class Devtools:
    def __init__(self, hub: "Hub"):
        self.hub = hub
        self.active: dict[Link, Transfer] = {}

    def allowed(self, link: "Link") -> bool:
        device = self.hub.store.get(link.device_id) if link.device_id else None
        return bool(
            device
            and device.channel == "dev"
            and link.state == "online"
            and "devtools" in link.caps
            and self.hub.online.get(link.device_id) is link
        )

    def revoke(self, link: "Link") -> None:
        link.caps.discard("devtools")
        self.disconnect(link, "developer access was revoked; reconnect after changing channel")

    def disconnect(self, link: "Link", reason: str = "the developer connection closed") -> None:
        if transfer := self.active.get(link):
            transfer.fail(reason)

    def event(self, link: "Link", method: str, params: dict) -> None:
        transfer = self.active.get(link)
        if transfer is None or transfer.finished.done():
            return
        if not self.allowed(link):
            transfer.fail("developer access was revoked")
            return
        if type(params.get("stream")) is not int or params["stream"] != transfer.stream:
            return  # Late output can never satisfy a newer stream or connection.
        if method == "devtools.output":
            text = params.get("text")
            try:
                size = len(text.encode()) if isinstance(text, str) else 0
            except UnicodeEncodeError:
                size = 0
            if (
                set(params) != {"stream", "seq", "text"}
                or type(params.get("seq")) is not int
                or params["seq"] != transfer.sequence
                or not isinstance(text, str)
                or not 1 <= size <= 2048
            ):
                transfer.fail("invalid developer output sequence or fragment")
                return
            transfer.size += size
            transfer.sequence += 1
            if transfer.size > MAX_OUTPUT or transfer.sequence > MAX_EVENTS:
                transfer.fail("developer output exceeded its budget")
                return
            transfer.fragments.append(text)
        elif method == "devtools.done":
            if params.get("ok") is True and set(params) == {"stream", "ok"}:
                transfer.finished.set_result("".join(transfer.fragments))
            elif (
                params.get("ok") is False
                and set(params) == {"stream", "ok", "code"}
                and isinstance(params.get("code"), str)
                and params["code"] in ERROR_CODES
            ):
                # Preserve bounded handler reasons for board diagnosis, never
                # arbitrary interleaved logs or path/URL-bearing provider text.
                receipts = "\n".join(
                    line for line in "".join(transfer.fragments).splitlines() if ERROR_RECEIPT.fullmatch(line)
                )[:2048]
                detail = f"\n{receipts}" if receipts else ""
                transfer.fail(f"developer command failed: {params['code']}{detail}")
            else:
                transfer.fail("invalid developer completion")

    async def run(self, link: "Link", command: str, args: list) -> dict:
        validate(command, args)
        if not self.allowed(link):
            raise ControlError("wireless devtools requires an online dev-channel device with CONFIG_ORB_DEVTOOLS")
        if link in self.active or len(self.active) >= 4:
            raise ControlError("developer tools are busy")
        stream = link.allocate_devtools_stream(MAX_STREAM)
        if stream is None:
            raise ControlError("developer stream IDs exhausted; reconnect the device")
        transfer = Transfer(stream, asyncio.get_running_loop().create_future())
        self.active[link] = transfer
        try:
            async with asyncio.timeout(DEADLINE):
                result = await link.request(f"devtools.{command}", {"stream": stream, "args": args}, timeout=10)
                if result != {"accepted": True} or result.get("accepted") is not True:
                    raise ControlError("the device did not accept the developer command")
                text = await transfer.finished
                if not self.allowed(link):
                    raise ControlError("developer access was revoked")
                if command == "shot":
                    png = await asyncio.to_thread(screenshot_png, text)
                    if not self.allowed(link):
                        raise ControlError("developer access was revoked")
                    return {"png": base64.b64encode(png).decode()}
                return {"text": text}
        except TimeoutError:
            raise ControlError("developer command exceeded its 90 s deadline") from None
        except LinkError as exc:
            raise ControlError(exc.msg or exc.code) from None
        finally:
            self.active.pop(link, None)
            if not transfer.finished.done():
                transfer.finished.cancel()
            elif not transfer.finished.cancelled():
                transfer.finished.exception()
