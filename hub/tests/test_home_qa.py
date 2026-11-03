"""Home QA regressions use fixture inventories and in-process events only."""

import asyncio
import json
from datetime import UTC, datetime, timedelta

import pytest
from test_home import DEVICE, accessory, inventory

from orb_hub import home as home_module
from orb_hub.envelope import LinkError
from orb_hub.home import HomeBridge, alias
from orb_hub.sessions import SessionHub


@pytest.fixture
async def home(short_tmp):
    sessions = SessionHub(short_tmp)
    bridge = HomeBridge(sessions, short_tmp, url="http://127.0.0.1:1/api/v1")
    bridge._load(inventory())
    bridge.available = True
    try:
        yield bridge
    finally:
        await bridge.stop()
        await sessions.close()


async def test_secondary_service_cannot_replace_primary_temperature(home):
    homes = inventory()
    thermostat = next(a for a in homes[0]["accessories"] if a["id"] == "thermostat")
    primary = thermostat["services"][0]
    primary["characteristics"][0]["id"] = "primary-temperature"
    secondary = accessory("secondary", "sensor", state={"currentTemperature": 40})["services"][0]
    secondary["primary"] = False
    secondary["characteristics"][0]["id"] = "secondary-temperature"
    thermostat["services"] = [secondary, primary]
    home._load(homes)
    key = alias("a", "thermostat")
    await home._event(
        "characteristic",
        {
            "accessoryId": "thermostat",
            "serviceId": secondary["id"],
            "characteristicId": "secondary-temperature",
            "characteristic": "currentTemperature",
            "value": 41,
        },
    )
    assert home.accessories[key]["state"]["temp"] == 21.5
    await home._event(
        "characteristic",
        {
            "accessoryId": "thermostat",
            "serviceId": primary["id"],
            "characteristicId": "primary-temperature",
            "characteristic": "currentTemperature",
            "value": None,
        },
    )
    assert home.accessories[key]["state"]["temp"] == 41
    await home._event(
        "characteristic",
        {
            "accessoryId": "thermostat",
            "serviceId": primary["id"],
            "characteristicId": "primary-temperature",
            "characteristic": "currentTemperature",
            "value": 22,
        },
    )
    assert home.accessories[key]["state"]["temp"] == 22


async def test_secondary_service_event_does_not_override_accepted_primary_write(home, monkeypatch):
    homes = inventory()
    lamp = next(a for a in homes[0]["accessories"] if a["id"] == "lamp")
    secondary = accessory("secondary", state={"brightness": 1})["services"][0]
    secondary["primary"] = False
    secondary["characteristics"][0]["id"] = "secondary-brightness"
    lamp["services"].append(secondary)
    home._load(homes)
    key = alias("a", "lamp")

    async def secondary_event(value):
        await home._event(
            "characteristic",
            {
                "accessoryId": "lamp",
                "serviceId": secondary["id"],
                "characteristicId": "secondary-brightness",
                "characteristic": "brightness",
                "value": value,
            },
        )

    async def accepted(method, path, *, values=None):
        await secondary_event(41)
        return {"id": "lamp", "state": {"on": False, "brightness": 70}}

    monkeypatch.setattr(home, "_json", accepted)
    result = await home.request(DEVICE, "home.set", {"id": key, "values": {"brightness": 67}})
    assert result["state"]["brightness"] == 70
    assert home.accessories[key]["state"]["brightness"] == 70
    await secondary_event(42)
    assert home.accessories[key]["state"]["brightness"] == 70


@pytest.mark.parametrize(
    "raw_id,kind,primary,secondary,field",
    [("fan", "fan", "active", "on", "on"), ("sensor", "sensor", "occupancy", "motion", "motion")],
)
async def test_primary_service_wins_across_relay_aliases(home, monkeypatch, raw_id, kind, primary, secondary, field):
    homes = inventory()
    item = accessory(raw_id, kind, state={primary: True})
    extra = accessory("secondary", "switch", state={secondary: False})["services"][0]
    extra["primary"] = False
    item["services"].append(extra)
    item["state"][secondary] = False
    homes[0]["accessories"] = [item]
    home._load(homes)
    key = alias("a", raw_id)
    assert home.accessories[key]["state"][field] is True
    await home._event("characteristic", {"accessoryId": raw_id, "characteristic": secondary, "value": False})
    assert home.accessories[key]["state"][field] is True

    async def accepted(method, path, *, values=None):
        await home._event("characteristic", {"accessoryId": raw_id, "characteristic": secondary, "value": True})
        return {"id": raw_id, "state": {primary: False, secondary: True}}

    monkeypatch.setattr(home, "_json", accepted)
    if field == "on":
        assert (await home.request(DEVICE, "home.set", {"id": key, "values": {"on": False}}))["state"][field] is False
        assert home.accessories[key]["state"][field] is False


async def test_doorbells_without_event_timestamp_are_separate_presses(home):
    calls = []

    async def capture(value, **kwargs):
        calls.append(value)

    home.sessions.show_card = capture
    for _ in range(2):
        await home._event(
            "characteristic",
            {
                "accessoryId": "doorbell",
                "characteristicId": "button",
                "characteristic": "buttonEvent",
                "value": 0,
            },
        )
    assert len(calls) == 2


async def test_removed_doorbells_do_not_accumulate_dedupe_memory(home):
    async def capture(*args, **kwargs):
        pass

    home.sessions.show_card = capture
    for index in range(20):
        homes = inventory()
        bell = next(a for a in homes[0]["accessories"] if a["id"] == "doorbell")
        bell["id"] = f"doorbell-{index}"
        home._load(homes)
        await home._event(
            "characteristic",
            {
                "accessoryId": bell["id"],
                "characteristic": "buttonEvent",
                "value": 0,
                "ts": f"2026-10-27T03:{index:02d}:00+00:00",
            },
        )
        assert len(home._last_doorbell) == 1


async def test_automatic_discovery_retries_after_relay_address_changes(short_tmp, monkeypatch):
    discovered = []
    attempts = []
    recovered = asyncio.Event()

    async def discover():
        url = f"http://127.0.0.1:{23456 + len(discovered)}/api/v1"
        discovered.append(url)
        return url

    class NoNetworkClient:
        def get(self, url, **kwargs):
            attempts.append(url)
            if len(attempts) == 2:
                recovered.set()
            raise LinkError("unsupported", "fixture disconnect")

        async def close(self):
            pass

    monkeypatch.delenv("HOMERELAY_URL", raising=False)
    monkeypatch.setattr(home_module, "discover_url", discover)
    bridge = HomeBridge(None, short_tmp, reconnect_delay=0.001)
    monkeypatch.setattr(bridge, "_token", lambda: "hr_TEST_ONLY_qa")
    bridge._client = NoNetworkClient()
    bridge._tasks = [asyncio.create_task(bridge._run())]
    try:
        async with asyncio.timeout(1):
            await recovered.wait()
        assert len(discovered) == 2
        assert attempts == [url + "/events" for url in discovered]
    finally:
        await bridge.stop()


async def test_known_unlock_scene_is_hidden_and_cannot_be_requested(home, monkeypatch):
    homes = inventory()
    scene = homes[0]["scenes"][0]
    scene["contains_unlock"] = True
    home._load(homes)
    key = alias("s", scene["id"])
    assert key not in {s["id"] for s in home.snapshot(DEVICE)["scenes"]}

    async def never_write(*args, **kwargs):
        pytest.fail("known unsafe scene reached HomeRelay")

    monkeypatch.setattr(home, "_json", never_write)
    with pytest.raises(LinkError, match="not_found"):
        await home.request(DEVICE, "home.scene", {"id": key})


async def test_doorbell_time_uses_event_observation_in_local_timezone(home):
    from datetime import datetime

    calls = []

    async def capture(value, **kwargs):
        calls.append(value)

    home.sessions.show_card = capture
    timestamp = "2026-10-27T03:17:00+09:00"
    await home._event(
        "characteristic",
        {"accessoryId": "doorbell", "characteristic": "buttonEvent", "value": 0, "ts": timestamp},
    )
    local = datetime.fromisoformat(timestamp).astimezone().strftime("%H:%M")
    assert calls[0]["body"] == "Doorbell rang at " + local


@pytest.mark.parametrize("timestamp", [None, "bad-date", 999, "2026-10-27T03:17:00"])
async def test_doorbell_invalid_or_naive_timestamp_uses_receipt_time(home, monkeypatch, timestamp):
    calls = []

    async def capture(value, **kwargs):
        calls.append(value)

    home.sessions.show_card = capture
    monkeypatch.setattr(home_module.time, "strftime", lambda pattern: "12:34")
    await home._event(
        "characteristic",
        {"accessoryId": "doorbell", "characteristic": "buttonEvent", "value": 0, "ts": timestamp},
    )
    assert calls[0]["body"] == "Doorbell rang at 12:34"


@pytest.mark.parametrize("timestamp", [None, "bad-date", 999, "2026-10-27T03:17:00"])
async def test_unusable_doorbell_timestamps_cannot_deduplicate_distinct_presses(home, timestamp):
    calls = []

    async def capture(value, **kwargs):
        calls.append(value)

    home.sessions.show_card = capture
    for _ in range(2):
        await home._event(
            "characteristic",
            {"accessoryId": "doorbell", "characteristic": "buttonEvent", "value": 0, "ts": timestamp},
        )
    assert len(calls) == 2


async def test_buffered_old_sse_cannot_replace_newer_reconnect_snapshot(home, monkeypatch):
    consumed = asyncio.Event()
    old = {
        "type": "characteristic",
        "accessoryId": "lamp",
        "characteristic": "brightness",
        "value": 10,
        "ts": "2026-10-27T10:00:00-04:00",
    }

    class Stream:
        def __init__(self):
            self.lines = iter([b"event: characteristic\n", b"data: " + json.dumps(old).encode() + b"\n", b"\n"])

        async def readline(self):
            line = next(self.lines, None)
            if line is not None:
                return line
            consumed.set()
            await asyncio.Future()

    class Response:
        status = 200
        content_type = "text/event-stream"
        content = Stream()

        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            pass

    class Client:
        def get(self, *args, **kwargs):
            return Response()

        async def close(self):
            pass

    async def newer_snapshot(method, path, **kwargs):
        # /events already buffered brightness 10 before /snapshot sampled 50.
        assert (method, path) == ("GET", "/snapshot")
        homes = inventory()
        next(a for a in homes[0]["accessories"] if a["id"] == "lamp")["state"]["brightness"] = 50
        return homes

    monkeypatch.setattr(home, "_token", lambda: "hr_TEST_ONLY_ordering")
    monkeypatch.setattr(home, "_json", newer_snapshot)
    home._client = Client()
    home._tasks = [asyncio.create_task(home._run())]
    async with asyncio.timeout(1):
        await consumed.wait()
    assert home.accessories[alias("a", "lamp")]["state"]["brightness"] == 50


async def test_newer_event_after_reconnect_snapshot_applies(home, monkeypatch):
    consumed = asyncio.Event()
    newer_ts = (datetime.now(UTC) + timedelta(minutes=5)).isoformat()
    newer = {
        "type": "characteristic",
        "accessoryId": "lamp",
        "characteristic": "brightness",
        "value": 75,
        "ts": newer_ts,
    }

    class Stream:
        def __init__(self):
            self.lines = iter([b"event: characteristic\n", b"data: " + json.dumps(newer).encode() + b"\n", b"\n"])

        async def readline(self):
            line = next(self.lines, None)
            if line is not None:
                return line
            consumed.set()
            await asyncio.Future()

    class Response:
        status = 200
        content_type = "text/event-stream"
        content = Stream()

        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            pass

    class Client:
        def get(self, *args, **kwargs):
            return Response()

        async def close(self):
            pass

    async def snapshot(method, path, **kwargs):
        assert (method, path) == ("GET", "/snapshot")
        homes = inventory()
        next(a for a in homes[0]["accessories"] if a["id"] == "lamp")["state"]["brightness"] = 50
        return homes

    monkeypatch.setattr(home, "_token", lambda: "hr_TEST_ONLY_ordering")
    monkeypatch.setattr(home, "_json", snapshot)
    home._client = Client()
    home._tasks = [asyncio.create_task(home._run())]
    async with asyncio.timeout(1):
        await consumed.wait()
    assert home.accessories[alias("a", "lamp")]["state"]["brightness"] == 75


async def test_doors_and_windows_are_read_only_but_blinds_stay_writable(home):
    homes = inventory()
    position = {"position": 40, "targetPosition": 40}
    homes[0]["accessories"] += [
        accessory("blinds", "windowCovering", state=dict(position)),
        accessory("garage", "door", state=dict(position)),
        accessory("skylight", "window", state=dict(position)),
    ]
    home._load(homes)
    writable = {item["name"]: item["writable"] for item in home.accessories.values()}
    assert "position" in writable["blinds"]
    assert "position" not in writable["garage"]
    assert "position" not in writable["skylight"]
    with pytest.raises(LinkError, match="read-only"):
        await home.request(DEVICE, "home.set", {"id": alias("a", "garage"), "values": {"position": 100}})
