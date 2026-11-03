"""Owner-selected public weather location; never infer it from the network."""

import json
import logging
import math
import os
import tempfile
from pathlib import Path

log = logging.getLogger(__name__)
MAX_LOCATION_BYTES = 1024
LOCATION_FILE = "weather.json"


def validate_location(value: dict | None) -> dict | None:
    """Validate both saved data and control input before replacing the location."""
    if value is None:
        return None
    if not isinstance(value, dict) or value.keys() != {"name", "latitude", "longitude"}:
        raise ValueError("weather location must contain name, latitude and longitude")
    name = value["name"]
    if (
        not isinstance(name, str)
        or not name.strip()
        or len(name.encode("utf-8")) > 64
        or any(ord(char) < 32 or ord(char) == 127 for char in name)
    ):
        raise ValueError("weather place name must be 1-64 UTF-8 bytes without control characters")
    coordinates = {}
    for key, limit in (("latitude", 90), ("longitude", 180)):
        number = value[key]
        if type(number) not in (int, float) or not -limit <= number <= limit or not math.isfinite(number):
            raise ValueError(f"weather {key} must be a finite number in {-limit}..{limit}")
        coordinates[key] = float(number)
    return {"name": name.strip(), **coordinates}


def parse_latlon(value: str) -> dict:
    """Parse an explicit coordinate pair, preserving a readable place name."""
    if not isinstance(value, str) or len(value) > 128:
        raise ValueError("weather coordinates must be LAT,LON")
    try:
        latitude, longitude = (float(part.strip()) for part in value.split(","))
    except (TypeError, ValueError) as exc:
        raise ValueError("weather coordinates must be LAT,LON") from exc
    return validate_location(
        {
            "name": f"{latitude:.4f}, {longitude:.4f}",
            "latitude": latitude,
            "longitude": longitude,
        }
    )


def load_location(data_dir: Path) -> dict | None:
    """Corrupt or absent preferences leave weather idle, without network guessing."""
    try:
        with (data_dir / LOCATION_FILE).open("rb") as file:
            raw = file.read(MAX_LOCATION_BYTES + 1)
        if len(raw) > MAX_LOCATION_BYTES:
            raise ValueError("oversized location file")
        value = json.loads(raw)
        if not isinstance(value, dict) or value.keys() != {"location"}:
            raise ValueError("invalid location file")
        return validate_location(value["location"])
    except FileNotFoundError:
        return None
    except (OSError, UnicodeError, ValueError):
        log.warning("Weather location preferences are unreadable; weather is idle")
        return None


def save_location(data_dir: Path, location: dict | None) -> None:
    """Atomically persist only the location, with no credentials or fetched data."""
    location = validate_location(location)
    data_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=data_dir, prefix=".weather-", delete=False) as file:
            temporary = Path(file.name)
            os.fchmod(file.fileno(), 0o600)
            file.write(json.dumps({"location": location}, ensure_ascii=False).encode("utf-8"))
        os.replace(temporary, data_dir / LOCATION_FILE)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
