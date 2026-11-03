"""Bounded Open-Meteo observations; no network work until a location is configured."""

import asyncio
import copy
import json
import math
import re
import time
from collections.abc import Awaitable, Callable, Iterable
from dataclasses import dataclass, field
from datetime import datetime
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import aiohttp


class WeatherDataError(ValueError):
    """Weather input or provider data that cannot be used."""


GEOCODING_URL = "https://geocoding-api.open-meteo.com/v1/search"
FORECAST_URL = "https://api.open-meteo.com/v1/forecast"
REFRESH_SECONDS = 20 * 60
STALE_SECONDS = 40 * 60
MAX_RESPONSE_BYTES = 128 * 1024
MAX_TIMESTAMP = 4294967295
FetchJSON = Callable[[str, dict], Awaitable[dict]]

WMO_CONDITIONS = {
    0: "clear",
    1: "partly_cloudy",
    2: "partly_cloudy",
    3: "cloudy",
    45: "fog",
    48: "fog",
    51: "drizzle",
    53: "drizzle",
    55: "drizzle",
    56: "drizzle",
    57: "drizzle",
    61: "rain",
    63: "rain",
    65: "rain",
    66: "rain",
    67: "rain",
    71: "snow",
    73: "snow",
    75: "snow",
    77: "snow",
    80: "rain",
    81: "rain",
    82: "rain",
    85: "snow",
    86: "snow",
    95: "thunder",
    96: "thunder",
    99: "thunder",
}
LABELS = {
    "partly_cloudy": "Partly cloudy",
    "cloudy": "Cloudy",
    "fog": "Fog",
    "drizzle": "Drizzle",
    "rain": "Rain",
    "snow": "Snow",
    "thunder": "Thunder",
}


def _number(value, low, high):
    if type(value) not in (int, float) or not low <= value <= high or not math.isfinite(value):
        raise WeatherDataError("invalid weather number")
    return value


def _timestamp(value):
    if type(value) is not int or not 0 <= value <= MAX_TIMESTAMP:
        raise WeatherDataError("invalid weather timestamp")
    return value


def _clip(text, limit):
    return text.encode("utf-8")[:limit].decode("utf-8", errors="ignore")


@dataclass(frozen=True)
class WeatherLocation:
    name: str
    latitude: float
    longitude: float

    def __post_init__(self):
        if (
            not isinstance(self.name, str)
            or not self.name.strip()
            or len(self.name.encode("utf-8")) > 64
            or any(ord(char) < 32 or ord(char) == 127 for char in self.name)
        ):
            raise WeatherDataError("invalid weather location")
        _number(self.latitude, -90, 90)
        _number(self.longitude, -180, 180)


def map_wmo(code: int, is_day: bool) -> tuple[str, str]:
    if type(code) is not int or code not in WMO_CONDITIONS or type(is_day) is not bool:
        raise WeatherDataError("invalid weather condition")
    condition = WMO_CONDITIONS[code]
    return condition, ("Sunny" if is_day else "Clear") if condition == "clear" else LABELS[condition]


def _day(value):
    if type(value) is not int or value not in (0, 1):
        raise WeatherDataError("invalid weather day flag")
    return bool(value)


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise WeatherDataError("duplicate weather field")
        result[key] = value
    return result


async def _get_json(session, url, params):
    # Fixed HTTPS endpoints, normal certificate verification, no redirects or env proxies.
    async with session.get(url, params=params, allow_redirects=False) as response:
        if response.status != 200:
            raise WeatherDataError("weather service unavailable")
        body = bytearray()
        async for chunk in response.content.iter_chunked(8192):
            body.extend(chunk)
            if len(body) > MAX_RESPONSE_BYTES:
                raise WeatherDataError("weather response too large")
    try:
        value = json.loads(body, object_pairs_hook=_object)
    except (RecursionError, UnicodeError) as error:
        raise WeatherDataError("invalid weather response") from error
    if not isinstance(value, dict):
        raise WeatherDataError("invalid weather response")
    return value


def _session():
    return aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=12, connect=5), trust_env=False)


US_STATES = {
    "AL": "Alabama",
    "AK": "Alaska",
    "AZ": "Arizona",
    "AR": "Arkansas",
    "CA": "California",
    "CO": "Colorado",
    "CT": "Connecticut",
    "DE": "Delaware",
    "DC": "District of Columbia",
    "FL": "Florida",
    "GA": "Georgia",
    "HI": "Hawaii",
    "ID": "Idaho",
    "IL": "Illinois",
    "IN": "Indiana",
    "IA": "Iowa",
    "KS": "Kansas",
    "KY": "Kentucky",
    "LA": "Louisiana",
    "ME": "Maine",
    "MD": "Maryland",
    "MA": "Massachusetts",
    "MI": "Michigan",
    "MN": "Minnesota",
    "MS": "Mississippi",
    "MO": "Missouri",
    "MT": "Montana",
    "NE": "Nebraska",
    "NV": "Nevada",
    "NH": "New Hampshire",
    "NJ": "New Jersey",
    "NM": "New Mexico",
    "NY": "New York",
    "NC": "North Carolina",
    "ND": "North Dakota",
    "OH": "Ohio",
    "OK": "Oklahoma",
    "OR": "Oregon",
    "PA": "Pennsylvania",
    "RI": "Rhode Island",
    "SC": "South Carolina",
    "SD": "South Dakota",
    "TN": "Tennessee",
    "TX": "Texas",
    "UT": "Utah",
    "VT": "Vermont",
    "VA": "Virginia",
    "WA": "Washington",
    "WV": "West Virginia",
    "WI": "Wisconsin",
    "WY": "Wyoming",
}


def _query(city: str) -> tuple[str, str | None, str | None]:
    """Split "Columbia, MD 21044" into a name, a region and a postal code.

    Open-Meteo searches names only, and its top result for a common name is
    often the wrong place (Columbia resolves to South Carolina first)."""
    postcode = re.search(r"\b\d{5}(?:-\d{4})?\b", city)
    rest = re.sub(r"\b\d{5}(?:-\d{4})?\b", "", city)
    name, _, region = rest.partition(",")
    region = region.strip(" ,") or None
    return name.strip(), region, postcode.group(0)[:5] if postcode else None


def _match(found: dict, region: str | None, postcode: str | None) -> int:
    # Every qualifier is an owner constraint, not a weighted hint. A ZIP in
    # another state must never defeat the explicitly supplied state.
    postcodes = found.get("postcodes")
    if postcode and (not isinstance(postcodes, list) or postcode not in postcodes):
        return 0
    if region:
        wanted = US_STATES.get(region.upper(), region).casefold()
        for key in ("admin1", "country", "country_code"):
            value = found.get(key)
            if isinstance(value, str) and value.casefold() == wanted:
                return 2 if postcode else 1
        return 0
    return 2 if postcode else 0


async def geocode(city: str, *, fetch_json: FetchJSON | None = None) -> WeatherLocation:
    if (
        not isinstance(city, str)
        or not 2 <= len(city.strip().encode("utf-8")) <= 128
        or any(ord(char) < 32 or ord(char) == 127 for char in city)
    ):
        raise WeatherDataError("enter a city or use --weather-latlon LAT,LON")
    name, region, postcode = _query(city)
    if len(name.encode("utf-8")) < 2:
        raise WeatherDataError("enter a city or use --weather-latlon LAT,LON")
    qualified = bool(region or postcode)
    params = {"name": name, "count": 10 if qualified else 1, "language": "en", "format": "json"}
    if fetch_json is None:
        async with _session() as session:
            raw = await _get_json(session, GEOCODING_URL, params)
    else:
        raw = await fetch_json(GEOCODING_URL, params)
    try:
        results = [item for item in raw["results"] if isinstance(item, dict)]
        found = results[0]
        if qualified:
            # A region or postal code the owner typed must match: never fall
            # back to a same-named city in another state.
            best = max(results, key=lambda item: _match(item, region, postcode))
            if not _match(best, region, postcode):
                raise WeatherDataError("weather location not found; use a more specific city or --weather-latlon")
            found = best
        name = found["name"]
        if not isinstance(name, str):
            raise WeatherDataError("invalid location name")
        # Keep administrative context when supplied: names such as Springfield recur.
        parts = [name]
        for key in ("admin1", "country_code"):
            part = found.get(key)
            if isinstance(part, str) and part and part not in parts:
                parts.append(part)
        return WeatherLocation(_clip(", ".join(parts), 64), found["latitude"], found["longitude"])
    except (KeyError, IndexError, TypeError, ValueError) as error:
        raise WeatherDataError("weather location not found; use a more specific city or --weather-latlon") from error


def forecast_params(location: WeatherLocation, unit: str) -> dict:
    if unit not in ("C", "F"):
        raise WeatherDataError("invalid weather unit")
    return {
        "latitude": location.latitude,
        "longitude": location.longitude,
        "current": "temperature_2m,weather_code,is_day",
        "hourly": "temperature_2m,weather_code,is_day",
        "daily": "temperature_2m_max,temperature_2m_min",
        "temperature_unit": "fahrenheit" if unit == "F" else "celsius",
        "timezone": "auto",
        "timeformat": "unixtime",
        # Two local days preserve six future entries even near midnight.
        "forecast_days": 2,
    }


def parse_forecast(raw: dict, location: WeatherLocation, unit: str, *, now: float) -> dict:
    """Reject incomplete observations atomically; never replace a good reading with zeros."""
    forecast_params(location, unit)
    try:
        expected_unit = "°F" if unit == "F" else "°C"
        if any(
            raw[key][field] != expected_unit
            for key, field in (
                ("current_units", "temperature_2m"),
                ("hourly_units", "temperature_2m"),
                ("daily_units", "temperature_2m_max"),
                ("daily_units", "temperature_2m_min"),
            )
        ):
            raise WeatherDataError("weather unit mismatch")
        current = raw["current"]
        updated_at = _timestamp(current["time"])
        if updated_at == 0 or updated_at > now + 300:
            raise WeatherDataError("invalid weather observation time")
        is_day = _day(current["is_day"])
        condition, label = map_wmo(current["weather_code"], is_day)
        temperature = _number(current["temperature_2m"], -250, 350)
        _number(raw["utc_offset_seconds"], -86400, 86400)
        # The current offset can differ from midnight's offset on a DST day.
        # Compare calendar dates in the provider's zone, not a fixed offset.
        zone = ZoneInfo(raw["timezone"])
        daily = raw["daily"]
        if not all(isinstance(daily[key], list) for key in ("time", "temperature_2m_max", "temperature_2m_min")):
            raise WeatherDataError("invalid daily forecast")
        if not len(daily["time"]) == len(daily["temperature_2m_max"]) == len(daily["temperature_2m_min"]):
            raise WeatherDataError("incomplete daily forecast")
        today = datetime.fromtimestamp(now, zone).date()
        day = next(
            i
            for i, stamp in enumerate(daily["time"])
            if datetime.fromtimestamp(_timestamp(stamp), zone).date() == today
        )
        high = _number(daily["temperature_2m_max"][day], -250, 350)
        low = _number(daily["temperature_2m_min"][day], -250, 350)
        if low > high:
            raise WeatherDataError("invalid daily range")
        hourly = raw["hourly"]
        keys = ("time", "temperature_2m", "weather_code", "is_day")
        if not all(isinstance(hourly[key], list) for key in keys):
            raise WeatherDataError("invalid hourly forecast")
        if len({len(hourly[key]) for key in keys}) != 1:
            raise WeatherDataError("incomplete hourly forecast")
        hours, previous = [], -1
        for i, stamp in enumerate(hourly["time"]):
            stamp = _timestamp(stamp)
            if stamp <= previous:
                raise WeatherDataError("unordered hourly forecast")
            previous = stamp
            if stamp <= now:
                continue
            day_flag = _day(hourly["is_day"][i])
            weather, _ = map_wmo(hourly["weather_code"][i], day_flag)
            hours.append(
                {
                    "hour": stamp,
                    "temperature": _number(hourly["temperature_2m"][i], -250, 350),
                    "condition": weather,
                    "is_day": day_flag,
                }
            )
            if len(hours) == 6:
                break
        if len(hours) != 6:
            raise WeatherDataError("six future weather hours unavailable")
    except (KeyError, IndexError, TypeError, StopIteration, OverflowError, OSError, ZoneInfoNotFoundError) as error:
        raise WeatherDataError("invalid weather forecast") from error
    return {
        "configured": True,
        "available": True,
        "place": location.name,
        "unit": unit,
        "temperature": temperature,
        "condition": condition,
        "is_day": is_day,
        "label": label,
        "high": high,
        "low": low,
        "hourly": hours,
        "updated_at": updated_at,
        "stale": now - updated_at >= STALE_SECONDS,
    }


@dataclass
class _Reading:
    value: dict | None = None
    failed: bool = False
    failures: int = 0
    next_due: float = 0
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)


class WeatherProvider:
    def __init__(
        self,
        location: WeatherLocation | None = None,
        *,
        fetch_json: FetchJSON | None = None,
        clock=time.time,
        monotonic=time.monotonic,
    ):
        if location is not None and not isinstance(location, WeatherLocation):
            raise WeatherDataError("invalid weather location")
        self.location = location
        self._fetch_json = fetch_json
        self._clock = clock
        self._monotonic = monotonic
        self._session = None
        self._generation = 0
        self._readings: dict[str, _Reading] = {}
        self._published: dict[str, dict] = {}

    async def start(self):
        # Session creation is lazy: an unconfigured hub does no weather network work.
        await asyncio.sleep(0)

    async def stop(self):
        self._generation += 1
        if self._session is not None:
            await self._session.close()
            self._session = None

    def set_location(self, location: WeatherLocation | None):
        if location is not None and not isinstance(location, WeatherLocation):
            raise WeatherDataError("invalid weather location")
        if location != self.location:
            self.location = location
            self._generation += 1
            self._readings.clear()

    def snapshot(self, unit: str) -> dict:
        if unit not in ("C", "F"):
            raise WeatherDataError("invalid weather unit")
        reading = self._readings.get(unit)
        if self.location is not None and reading is not None and reading.value is not None:
            result = copy.deepcopy(reading.value)
            result["stale"] = reading.failed or self._clock() - result["updated_at"] >= STALE_SECONDS
            return result
        configured = self.location is not None
        failed = configured and reading is not None and reading.failed
        return {
            "configured": configured,
            "available": False,
            "place": self.location.name if configured else "",
            "unit": unit,
            "temperature": None,
            "condition": None,
            "is_day": False,
            "label": "Weather unavailable"
            if failed
            else "Loading weather"
            if configured
            else "Set a location in orb-hub",
            "high": None,
            "low": None,
            "hourly": [],
            "updated_at": 0,
            "stale": bool(failed),
        }

    async def refresh(self, unit: str) -> bool:
        if unit not in ("C", "F"):
            raise WeatherDataError("invalid weather unit")
        if self.location is None:
            return False
        reading = self._readings.setdefault(unit, _Reading())
        async with reading.lock:
            if self.location is None or self._readings.get(unit) is not reading or self._monotonic() < reading.next_due:
                return False
            location, generation = self.location, self._generation
            try:
                params = forecast_params(location, unit)
                if self._fetch_json is not None:
                    raw = await self._fetch_json(FORECAST_URL, params)
                else:
                    if self._session is None:
                        self._session = _session()
                    raw = await _get_json(self._session, FORECAST_URL, params)
                value = parse_forecast(raw, location, unit, now=self._clock())
            except (aiohttp.ClientError, OSError, TimeoutError, ValueError, TypeError):
                if generation != self._generation:
                    return False
                reading.failed = True
                reading.failures = min(reading.failures + 1, 6)
                reading.next_due = self._monotonic() + min(60 * 2 ** (reading.failures - 1), REFRESH_SECONDS)
                return True
            if generation != self._generation:
                return False
            reading.value = value
            reading.failed = False
            reading.failures = 0
            reading.next_due = self._monotonic() + REFRESH_SECONDS
            return True

    async def tick(self, units: Iterable[str]) -> bool:
        units = set(units)
        for unit in units:
            await self.refresh(unit)
        snapshots = {unit: self.snapshot(unit) for unit in units}
        changed = snapshots != self._published
        self._published = snapshots
        return changed
