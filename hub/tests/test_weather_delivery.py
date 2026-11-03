"""Recorded forecast data travels over the real paired link, with no public network."""

import asyncio
import json
from dataclasses import asdict
from pathlib import Path

import pytest
from fake_device import paired_device
from test_home_delivery import CaptureLink, flush, reconnect

from orb_hub import weather
from orb_hub.control import ControlError
from orb_hub.sessions import SessionHub
from orb_hub.weather_delivery import WeatherBridge
from orb_hub.weather_prefs import load_location

FIXTURES = Path(__file__).parent / "fixtures/weather"
LOCATION = weather.WeatherLocation("Berlin", 52.52437, 13.41053)


@pytest.fixture
def provider():
    recordings = {
        unit: json.loads((FIXTURES / f"berlin_forecast_{unit.lower()}.json").read_text()) for unit in ("C", "F")
    }
    now = max(value["current"]["time"] for value in recordings.values())

    async def fetch(url, params):
        assert url == weather.FORECAST_URL
        return recordings["F" if params["temperature_unit"] == "fahrenheit" else "C"]

    return weather.WeatherProvider(LOCATION, fetch_json=fetch, clock=lambda: now)


async def test_weather_only_negotiation_idle_and_preference_request(hub):
    device = await paired_device(hub)
    try:
        result = await reconnect(device, ["weather"])
        assert result["caps"] == ["weather"]
        assert result["prefs"]["units"] == {"temperature": "C", "source": "auto"}
        method, state = await device.event()
        assert method == "weather.state"
        assert not state["configured"] and not state["available"]
        assert state["temperature"] is None and state["condition"] is None
        assert state["label"] == "Set a location in orb-hub"
        subscriber = hub.sessions._subscribers[hub.online[device.device_id]]
        assert not subscriber.sessions_enabled
        response = await device.request("prefs.set", {"units": {"temperature": "F"}})
        assert response["ok"] and response["r"]["units"]["temperature"] == "F"
        assert (await device.event())[0] == "prefs.update"
        assert (await device.event())[1]["unit"] == "F"
        assert (await device.request("sessions.subscribe"))["e"]["code"] == "unsupported"
    finally:
        await device.aclose()


async def test_recorded_forecast_over_pinned_tls_then_off_discards_value(hub, provider):
    # Replace the idle provider; its real transport is never used by this test.
    await hub.weather.provider.stop()
    hub.weather.provider = provider
    await provider.tick({"C"})
    device = await paired_device(hub)
    try:
        await reconnect(device, ["weather"])
        method, state = await device.event()
        assert method == "weather.state" and state == provider.snapshot("C")
        assert state["available"] and len(state["hourly"]) == 6
        assert all(item["hour"] > state["updated_at"] for item in state["hourly"])
        assert await hub.handle_command("weather.prefs.set", {"weather": "off"}) == {"location": None}
        method, cleared = await device.event()
        assert method == "weather.state" and not cleared["available"] and not cleared["stale"]
        assert cleared["updated_at"] == 0 and cleared["hourly"] == []
    finally:
        await device.aclose()


@pytest.mark.parametrize("caps", [[], ["cards"], ["usage"]])
async def test_weather_never_reaches_legacy_or_other_subscribers(hub, caps):
    device = await paired_device(hub)
    try:
        result = await reconnect(device, caps)
        assert "weather" not in result.get("caps", [])
        await hub.handle_command("weather.prefs.set", {"latlon": [40, -74]})
        await asyncio.sleep(0.3)
        assert all(method != "weather.state" for method, _ in list(device.events._queue))
        assert hub.weather.provider._session is None
    finally:
        await device.aclose()


async def test_weather_only_rejects_oversized_hello_and_messages(hub):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        result = await device.hello(caps=["weather"], fw="x" * 4096)
        assert result["e"]["code"] == "too_large"
        assert await device.closed() == 1009
        await device.connect()
        assert (await device.hello(caps=["weather"]))["ok"]
        await device.event()
        await device.send({"t": "evt", "m": "unknown", "p": {"text": "x" * 4096}})
        assert await device.closed() == 1009
    finally:
        await device.aclose()


async def test_weather_control_geocodes_and_persists_only_explicit_location(hub, monkeypatch):
    calls = []

    async def geocode(city):
        calls.append(city)
        return LOCATION

    monkeypatch.setattr(weather, "geocode", geocode)
    assert await hub.handle_command("weather.prefs.show", {}) == {"location": None}
    result = await hub.handle_command("weather.prefs.set", {"weather": " Berlin "})
    assert result == {"location": asdict(LOCATION)} and calls == ["Berlin"]
    assert load_location(hub.cfg.data_dir) == asdict(LOCATION)
    for args in (
        {},
        {"weather": ""},
        {"weather": True},
        {"latlon": [True, 1]},
        {"latlon": [91, 1]},
        {"latlon": [1]},
        {"weather": "off", "latlon": [1, 2]},
    ):
        with pytest.raises(ControlError):
            await hub.handle_command("weather.prefs.set", args)
        assert load_location(hub.cfg.data_dir) == asdict(LOCATION)
    assert hub.weather.provider._session is None


async def test_per_device_units_and_pending_location_change_share_bounded_worker(short_tmp, provider):
    sessions = SessionHub(short_tmp)
    bridge = WeatherBridge(sessions, provider=provider)
    # Drive the same bridge/model without a timer task for exact backpressure ordering.
    sessions.weather_snapshot = bridge.snapshot
    await provider.tick({"C", "F"})
    links = [CaptureLink("orb-celsius", ["weather"]), CaptureLink("orb-fahrenheit", ["weather", "cards"])]
    try:
        await sessions.set_prefs(links[1].device_id, {"units": {"temperature": "F"}})
        for link in links:
            bridge.subscribe(link)
        celsius, fahrenheit = [await flush(sessions, link) for link in links]
        assert celsius[0][1] == provider.snapshot("C")
        assert fahrenheit[0][1] == provider.snapshot("F")
        assert celsius[0][1]["temperature"] != fahrenheit[0][1]["temperature"]
        subscriber = sessions._subscribers[links[1]]
        assert subscriber.queue.maxsize == 64
        await sessions.subscribe(links[1])
        assert sessions._subscribers[links[1]] is subscriber
        await flush(sessions, links[1])
        old = provider.snapshot("F")
        bridge.set_location(None)
        assert not sessions.delivery_allowed(links[1], "weather.state", old)
        assert (await flush(sessions, links[1]))[0][1]["configured"] is False
    finally:
        await provider.stop()
        await sessions.close()


async def test_weather_suppresses_old_unit_at_delivery_boundary(short_tmp, provider):
    sessions = SessionHub(short_tmp)
    bridge = WeatherBridge(sessions, provider=provider)
    sessions.weather_snapshot = bridge.snapshot
    await provider.tick({"C", "F"})
    link = CaptureLink("orb-weather", ["weather"])
    try:
        bridge.subscribe(link)
        await flush(sessions, link)
        old = bridge.snapshot(link.device_id)
        await sessions.set_prefs(link.device_id, {"units": {"temperature": "F"}})
        assert not sessions.delivery_allowed(link, "weather.state", old)
        bridge.publish()
        events = await flush(sessions, link)
        assert [name for name, _ in events] == ["prefs.update", "weather.state"]
        assert events[-1][1] == provider.snapshot("F")
    finally:
        await provider.stop()
        await sessions.close()


async def test_discarded_snapshot_is_replayed_if_units_change_back(short_tmp, provider):
    sessions = SessionHub(short_tmp)
    bridge = WeatherBridge(sessions, provider=provider)
    sessions.weather_snapshot = bridge.snapshot
    await provider.tick({"C", "F"})
    link = CaptureLink("orb-weather", ["weather"])
    try:
        bridge.subscribe(link)  # C is queued, but has not reached the worker.
        await sessions.set_prefs(link.device_id, {"units": {"temperature": "F"}})
        assert all(name != "weather.state" for name, _ in await flush(sessions, link))
        await sessions.set_prefs(link.device_id, {"units": {"temperature": "C"}})
        bridge.publish()
        events = await flush(sessions, link)
        assert events[-1] == ("weather.state", provider.snapshot("C"))
    finally:
        await provider.stop()
        await sessions.close()
