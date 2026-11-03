"""POSIX TZ rules for zones the firmware's built-in table doesn't know.

A TZif v2+ file (RFC 8536) ends with a newline-delimited POSIX TZ footer, which
newlib's tzset() understands. The hub has full tzdata; the device has a table.
"""

import re
import zoneinfo
from importlib import resources
from pathlib import Path

ZONE = re.compile(r"[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+)*")


def posix_tz_from_bytes(data: bytes) -> str | None:
    """The footer rule if it is printable ASCII under 64 characters, else None."""
    if not data.startswith(b"TZif") or not data.endswith(b"\n"):
        return None
    line = data[:-1].rsplit(b"\n", 1)
    if len(line) != 2:
        return None
    try:
        rule = line[1].decode("ascii")
    except UnicodeDecodeError:
        return None
    return rule if rule and len(rule) < 64 and rule.isprintable() else None


def _zone_bytes(zone: str) -> bytes | None:
    for base in [*zoneinfo.TZPATH, "/usr/share/zoneinfo"]:
        path = Path(base, zone)
        if path.is_file():
            return path.read_bytes()
    try:
        return resources.files("tzdata.zoneinfo").joinpath(zone).read_bytes()
    except (ModuleNotFoundError, OSError):
        return None


def posix_tz_for_zone(zone: str | None) -> str | None:
    """The POSIX TZ rule for an IANA zone name, or None if it can't be derived."""
    if not isinstance(zone, str) or not ZONE.fullmatch(zone):
        return None
    try:
        data = _zone_bytes(zone)
    except OSError:
        return None
    return posix_tz_from_bytes(data) if data else None
