"""Replay recorded responses; clocks and HTTPS transports are always injected."""

import asyncio
import copy
import json
from pathlib import Path

import pytest

from orb_hub.weather import (
    FORECAST_URL,
    GEOCODING_URL,
    MAX_RESPONSE_BYTES,
    REFRESH_SECONDS,
    STALE_SECONDS,
    WMO_CONDITIONS,
    WeatherLocation,
    WeatherProvider,
    _get_json,
    forecast_params,
    geocode,
    map_wmo,
    parse_forecast,
)

FIXTURES = Path(__file__).parent / "fixtures" / "weather"
BERLIN = WeatherLocation("Berlin", 52.52, 13.41)


def recorded(name="berlin_forecast_c.json"):
    return json.loads((FIXTURES / name).read_text())


class Clock:
    def __init__(self, now):
        self.wall = now
        self.mono = 100.0

    def time(self):
        return self.wall

    def monotonic(self):
        return self.mono

    def advance(self, seconds):
        self.wall += seconds
        self.mono += seconds


@pytest.fixture
def raw():
    return recorded()


@pytest.fixture
def clock(raw):
    return Clock(raw["current"]["time"])


async def test_geocode_replays_recording():
    calls = []

    async def fetch(url, params):
        calls.append((url, params))
        return recorded("berlin_geocode.json")

    location = await geocode(" Berlin ", fetch_json=fetch)
    assert location == WeatherLocation("Berlin, State of Berlin, DE", 52.52437, 13.41053)
    assert calls == [(GEOCODING_URL, {"name": "Berlin", "count": 1, "language": "en", "format": "json"})]


@pytest.mark.parametrize(
    "payload",
    [{}, {"results": []}, {"results": [{}]}, {"results": [{"name": "Berlin", "latitude": 91, "longitude": 0}]}],
)
async def test_geocode_missing_or_invalid_location(payload):
    async def fetch(url, params):
        return payload

    with pytest.raises(ValueError, match="not found"):
        await geocode("Berlin", fetch_json=fetch)


@pytest.mark.parametrize("city", [None, "", "x", "a\nb", "a" * 129])
async def test_geocode_rejects_bad_query_without_network(city):
    async def fetch(url, params):
        pytest.fail("invalid input reached network")

    with pytest.raises(ValueError):
        await geocode(city, fetch_json=fetch)


COLUMBIA = {
    "results": [
        {
            "name": "Columbia",
            "admin1": "South Carolina",
            "country_code": "US",
            "latitude": 34.0007,
            "longitude": -81.0348,
            "postcodes": ["29201"],
        },
        {
            "name": "Columbia",
            "admin1": "Missouri",
            "country_code": "US",
            "latitude": 38.9517,
            "longitude": -92.3341,
            "postcodes": ["65201"],
        },
        {
            "name": "Columbia",
            "admin1": "Maryland",
            "country_code": "US",
            "latitude": 39.2404,
            "longitude": -76.8394,
            "postcodes": ["21044", "21045", "21046"],
        },
    ]
}


@pytest.mark.parametrize("city", ["Columbia, MD 21044", "Columbia, MD", "Columbia, Maryland", "Columbia 21044"])
async def test_geocode_prefers_the_typed_state_or_postcode(city):
    calls = []

    async def fetch(url, params):
        calls.append(params)
        return COLUMBIA

    location = await geocode(city, fetch_json=fetch)
    assert location == WeatherLocation("Columbia, Maryland, US", 39.2404, -76.8394)
    assert calls[0]["name"] == "Columbia" and calls[0]["count"] == 10


async def test_geocode_never_falls_back_to_another_state():
    async def fetch(url, params):
        return COLUMBIA

    with pytest.raises(ValueError, match="not found"):
        await geocode("Columbia, OR", fetch_json=fetch)


@pytest.mark.parametrize("code", list(WMO_CONDITIONS))
def test_all_documented_wmo_codes_and_day_night(code):
    for is_day in (False, True):
        condition, label = map_wmo(code, is_day)
        assert condition == WMO_CONDITIONS[code]
        assert label
        assert len(label.encode()) <= 32
    assert map_wmo(0, True) == ("clear", "Sunny")
    assert map_wmo(0, False) == ("clear", "Clear")


@pytest.mark.parametrize("code,day", [(True, True), (0.0, True), (100, True), (0, 1), (None, False)])
def test_invalid_condition_never_invented(code, day):
    with pytest.raises(ValueError):
        map_wmo(code, day)


@pytest.mark.parametrize("unit,name", [("C", "berlin_forecast_c.json"), ("F", "berlin_forecast_f.json")])
def test_recorded_forecast_units_current_high_low_and_six_future_hours(unit, name):
    raw = recorded(name)
    now = raw["current"]["time"]
    value = parse_forecast(raw, BERLIN, unit, now=now)
    assert value["configured"] and value["available"] and not value["stale"]
    assert value["temperature"] == raw["current"]["temperature_2m"]
    assert value["high"] == raw["daily"]["temperature_2m_max"][0]
    assert value["low"] == raw["daily"]["temperature_2m_min"][0]
    assert value["unit"] == unit
    assert value["updated_at"] == now
    assert value["is_day"] is bool(raw["current"]["is_day"])
    assert value["hourly"][0]["hour"] > now
    assert len(value["hourly"]) == 6
    assert value["hourly"][-1]["hour"] - value["hourly"][0]["hour"] == 5 * 3600
    assert len(json.dumps({"t": "evt", "m": "weather.state", "p": value}).encode()) < 4096


def test_daily_range_uses_location_date_at_midnight(raw):
    now = raw["hourly"]["time"][-8]
    raw["current"]["time"] = now
    value = parse_forecast(raw, BERLIN, "C", now=now)
    assert value["high"] == raw["daily"]["temperature_2m_max"][1]
    assert value["low"] == raw["daily"]["temperature_2m_min"][1]


@pytest.mark.parametrize(
    "case",
    [
        "nan",
        "bool",
        "null",
        "unknown_code",
        "day",
        "timestamp",
        "future",
        "units",
        "array",
        "short",
        "duplicate_hour",
        "range",
    ],
)
def test_invalid_forecast_rejected_atomically(raw, case):
    now = raw["current"]["time"]
    if case == "nan":
        raw["current"]["temperature_2m"] = float("nan")
    elif case == "bool":
        raw["current"]["temperature_2m"] = True
    elif case == "null":
        raw["current"]["temperature_2m"] = None
    elif case == "unknown_code":
        raw["current"]["weather_code"] = 100
    elif case == "day":
        raw["current"]["is_day"] = True
    elif case == "timestamp":
        raw["current"]["time"] = -1
    elif case == "future":
        raw["current"]["time"] = now + 301
    elif case == "units":
        raw["hourly_units"]["temperature_2m"] = "°F"
    elif case == "array":
        raw["hourly"]["temperature_2m"].pop()
    elif case == "short":
        now = raw["hourly"]["time"][-5]
    elif case == "duplicate_hour":
        raw["hourly"]["time"][1] = raw["hourly"]["time"][0]
    elif case == "range":
        raw["daily"]["temperature_2m_min"] = [350, 350]
    with pytest.raises(ValueError):
        parse_forecast(raw, BERLIN, "C", now=now)


def test_forecast_request_is_explicit_bounded_and_in_device_units():
    for unit, name in (("F", "fahrenheit"), ("C", "celsius")):
        params = forecast_params(BERLIN, unit)
        assert params["temperature_unit"] == name
        assert params["current"] == "temperature_2m,weather_code,is_day"
        assert params["hourly"] == "temperature_2m,weather_code,is_day"
        assert params["daily"] == "temperature_2m_max,temperature_2m_min"
        assert params["timezone"] == "auto"
        assert params["timeformat"] == "unixtime"
        assert params["forecast_days"] == 2


async def test_unconfigured_provider_is_idle_and_truthful(clock):
    async def fetch(url, params):
        pytest.fail("unconfigured weather reached network")

    provider = WeatherProvider(fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    await provider.start()
    await provider.tick(["F", "C"])
    value = provider.snapshot("F")
    assert value == {
        "configured": False,
        "available": False,
        "place": "",
        "unit": "F",
        "temperature": None,
        "condition": None,
        "is_day": False,
        "label": "Set a location in orb-hub",
        "high": None,
        "low": None,
        "hourly": [],
        "updated_at": 0,
        "stale": False,
    }
    assert not await provider.tick(["F", "C"])
    await provider.stop()


async def test_provider_refresh_schedule_and_independent_units(raw, clock):
    calls = []

    async def fetch(url, params):
        calls.append((url, params.copy()))
        value = recorded("berlin_forecast_f.json") if params["temperature_unit"] == "fahrenheit" else copy.deepcopy(raw)
        value["current"]["time"] = int(clock.wall)
        return value

    provider = WeatherProvider(BERLIN, fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    assert provider.snapshot("C")["label"] == "Loading weather"
    assert await provider.tick(["C"])
    assert not await provider.tick(["C"])
    clock.advance(REFRESH_SECONDS - 1)
    assert not await provider.tick(["C"])
    assert len(calls) == 1
    clock.advance(1)
    assert await provider.tick(["C"])
    assert len(calls) == 2
    assert await provider.tick(["C", "F"])
    assert len(calls) == 3
    assert all(url == FORECAST_URL and url.startswith("https://") for url, _ in calls)
    assert provider.snapshot("F")["unit"] == "F"
    assert provider.snapshot("F")["temperature"] == recorded("berlin_forecast_f.json")["current"]["temperature_2m"]


async def test_backoff_retains_good_reading_and_marks_stale(raw, clock):
    fail, calls = False, []

    async def fetch(url, params):
        calls.append(clock.mono)
        if fail:
            raise TimeoutError
        value = copy.deepcopy(raw)
        value["current"]["time"] = int(clock.wall)
        return value

    provider = WeatherProvider(BERLIN, fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    await provider.tick(["C"])
    good = provider.snapshot("C")
    fail = True
    clock.advance(REFRESH_SECONDS)
    await provider.tick(["C"])
    assert provider.snapshot("C") == {**good, "stale": True}
    for delay in (60, 120, 240, 480, 960, 1200, 1200):
        before = len(calls)
        clock.advance(delay - 1)
        await provider.tick(["C"])
        assert len(calls) == before
        clock.advance(1)
        await provider.tick(["C"])
        assert len(calls) == before + 1
    fail = False
    clock.advance(1200)
    await provider.tick(["C"])
    assert not provider.snapshot("C")["stale"]
    assert provider.snapshot("C")["updated_at"] == int(clock.wall)


async def test_observation_age_and_snapshot_isolation(raw, clock):
    async def fetch(url, params):
        return raw

    provider = WeatherProvider(BERLIN, fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    await provider.tick(["C"])
    external = provider.snapshot("C")
    external["hourly"][0]["temperature"] = 999
    assert provider.snapshot("C")["hourly"][0]["temperature"] != 999
    # Wall-clock observation age is independent of the monotonic polling deadline.
    clock.wall += STALE_SECONDS - 1
    assert not provider.snapshot("C")["stale"]
    clock.wall += 1
    assert await provider.tick(["C"])
    assert provider.snapshot("C")["stale"]


async def test_first_failure_and_location_off_discard_previous_data(clock):
    async def fetch(url, params):
        raise OSError("test outage")

    provider = WeatherProvider(BERLIN, fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    await provider.tick(["C"])
    value = provider.snapshot("C")
    assert value["configured"] and not value["available"] and value["stale"]
    assert value["label"] == "Weather unavailable"
    assert value["temperature"] is None and value["updated_at"] == 0
    provider.set_location(None)
    assert await provider.tick(["C"])
    assert not provider.snapshot("C")["configured"]
    assert not provider.snapshot("C")["stale"]


async def test_location_change_drops_inflight_result(raw, clock):
    entered, release = asyncio.Event(), asyncio.Event()

    async def fetch(url, params):
        entered.set()
        await release.wait()
        return raw

    provider = WeatherProvider(BERLIN, fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    pending = asyncio.create_task(provider.refresh("C"))
    await entered.wait()
    provider.set_location(WeatherLocation("New York", 40.71, -74.01))
    release.set()
    assert not await pending
    assert provider.snapshot("C")["place"] == "New York"
    assert not provider.snapshot("C")["available"]


async def test_cancelled_fetch_propagates_and_does_not_fake_failure(raw, clock):
    entered = asyncio.Event()

    async def fetch(url, params):
        entered.set()
        await asyncio.Event().wait()

    provider = WeatherProvider(BERLIN, fetch_json=fetch, clock=clock.time, monotonic=clock.monotonic)
    pending = asyncio.create_task(provider.refresh("C"))
    await entered.wait()
    pending.cancel()
    with pytest.raises(asyncio.CancelledError):
        await pending
    assert provider.snapshot("C")["label"] == "Loading weather"
    assert not provider.snapshot("C")["stale"]
    await provider.stop()


class Response:
    def __init__(self, chunks, status=200):
        self.status = status
        self.chunks = chunks
        self.content = self

    async def __aenter__(self):
        return self

    async def __aexit__(self, *args):
        return False

    async def iter_chunked(self, size):
        for chunk in self.chunks:
            yield chunk


class Session:
    def __init__(self, response):
        self.response = response

    def get(self, url, **kwargs):
        assert url.startswith("https://")
        assert kwargs["allow_redirects"] is False
        return self.response


@pytest.mark.parametrize(
    "response",
    [
        Response([b"x" * (MAX_RESPONSE_BYTES + 1)]),
        Response([b"{}"], 302),
        Response([b'{"a":1,"a":2}']),
        Response([b"[]"]),
    ],
)
async def test_https_response_is_bounded_and_redirects_rejected(response):
    with pytest.raises(ValueError):
        await _get_json(Session(response), FORECAST_URL, {})


@pytest.mark.parametrize(
    "name,lat,lon",
    [
        ("", 0, 0),
        ("a\nb", 0, 0),
        ("é" * 33, 0, 0),
        ("Test", True, 0),
        ("Test", 91, 0),
        ("Test", 0, -181),
        ("Test", float("nan"), 0),
        ("Test", 10**1000, 0),
    ],
)
def test_location_validation(name, lat, lon):
    with pytest.raises(ValueError):
        WeatherLocation(name, lat, lon)
