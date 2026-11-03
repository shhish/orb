"""Read the Mac's temperature preference without changing its global settings."""

import plistlib
import re
import sys
from pathlib import Path

FAHRENHEIT_REGIONS = {"US", "LR", "MM", "BS", "BZ", "KY", "PW", "FM", "MH"}


def temperature_unit(preferences: dict) -> str:
    explicit = preferences.get("AppleTemperatureUnit")
    if explicit in ("Fahrenheit", "Celsius"):
        return "F" if explicit == "Fahrenheit" else "C"
    region = re.search(r"[_-]([A-Z]{2})(?:$|[@._-])", str(preferences.get("AppleLocale", "")))
    return "F" if region and region[1] in FAHRENHEIT_REGIONS else "C"


def read_temperature_unit() -> str:
    """Called in a worker thread: a preferences plist can be on slow storage."""
    if sys.platform != "darwin":
        return "C"
    try:
        with (Path.home() / "Library/Preferences/.GlobalPreferences.plist").open("rb") as handle:
            preferences = plistlib.load(handle)
        return temperature_unit(preferences if isinstance(preferences, dict) else {})
    except (OSError, ValueError, plistlib.InvalidFileException):
        return "C"
