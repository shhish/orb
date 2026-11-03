"""Resolve display units from the hub's region without changing Home wire values."""

import re
import subprocess

FAHRENHEIT_REGIONS = frozenset({"US", "LR", "MM", "BS", "BZ", "KY", "PW", "FM", "MH"})


def temperature_unit(explicit: str | None, locale: str | None) -> str:
    if explicit in {"Fahrenheit", "Celsius"}:
        return "F" if explicit == "Fahrenheit" else "C"
    # AppleLocale uses ICU identifiers, e.g. en_US or zh_Hant_US@calendar=gregorian.
    parts = re.split("[_-]", (locale or "").split("@", 1)[0])
    region = next((part.upper() for part in parts[1:] if len(part) == 2 and part.isalpha()), None)
    return "F" if region in FAHRENHEIT_REGIONS else "C"


def _read_default(key: str) -> str | None:
    try:
        result = subprocess.run(
            ["/usr/bin/defaults", "read", "-g", key],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            timeout=2,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired, UnicodeError):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def read_temperature_unit() -> str:
    """Blocking, bounded read; callers must run this off the event loop."""
    explicit = _read_default("AppleTemperatureUnit")
    return temperature_unit(explicit, None if explicit in {"Fahrenheit", "Celsius"} else _read_default("AppleLocale"))
