"""Byte-exact HomeBridge receipts shared with the firmware's host parser tests.

Regenerate intentionally with ORB_UPDATE_HOME_WIRE_FIXTURES=1; ordinary test
runs only compare the checked-in receipts. The existing fake HomeRelay serves
the inventory and accepts writes over loopback HTTP, never the owner's relay.
"""

import asyncio
import os
from pathlib import Path
from types import SimpleNamespace

import aiohttp
import test_home
from test_home import (
    DEVICE,
    TOKEN,
    accessory,
)

from orb_hub import envelope
from orb_hub import home as home_module
from orb_hub.envelope import LinkError
from orb_hub.home import HomeBridge, alias, valid_value
from orb_hub.sessions import SessionHub

relay = test_home.relay

FIXTURES = Path(__file__).resolve().parents[2] / "firmware/components/orb_home/tests/fixtures"
FIXTURE_TIME = 1793145600.0
ROOMS = (
    "Living Room",
    "Bedroom",
    "Kitchen",
    "Office",
    "Hallway",
    "Entrance",
    "Dining Room",
    "Bathroom",
    "Balcon éclairé",
)


def real_shaped_inventory():
    """45 eligible objects, nine rooms and 12 scenes, with real relay key names."""
    values = []

    def add(key, kind, state, writable=None):
        index = len(values)
        values.append(
            accessory(
                key,
                kind,
                ROOMS[index % len(ROOMS)],
                state,
                writable,
                name=f"{ROOMS[index % len(ROOMS)]} {kind} {index:02d} – Demo Home",
            )
        )

    for i in range(16):
        add(
            f"fixture-light-{i:02d}",
            "lightbulb",
            {"on": i % 2 == 0, "brightness": 50, "hue": i * 20, "saturation": 80, "colorTemperature": 250},
        )
    for i in range(16):
        add(f"fixture-switch-{i:02d}", "switch", {"on": i % 3 == 0})
    for i in range(2):
        add(f"fixture-outlet-{i:02d}", "outlet", {"on": True, "inUse": True}, ["on"])
    add("fixture-fan", "fan", {"active": 1, "rotationSpeed": 60})
    add(
        "fixture-thermostat",
        "thermostat",
        {"currentTemperature": 21.5, "targetTemperature": 22, "targetHeatingCooling": 1},
        ["targetTemperature", "targetHeatingCooling"],
    )
    add("fixture-lock", "doorLock", {"lockState": 1, "lockTarget": 1}, ["lockTarget"])
    for i in range(5):
        add(
            f"fixture-sensor-{i:02d}",
            "sensor",
            {
                "currentTemperature": -2.5 if i == 0 else 20 + i / 2,
                "humidity": 48,
                "motion": i == 1,
                "contact": int(i == 2),
                "batteryLevel": 10 if i == 3 else 75,
                "lowBattery": int(i == 3),
            },
            [],
        )
    add("fixture-doorbell", "doorbell", {}, [])
    add("fixture-button", "button", {}, [])
    add("fixture-cover", "windowCovering", {"position": 40, "targetPosition": 40}, ["targetPosition"])
    scene_names = ["Good morning", "Evening", "Good Night", "I'm leaving", "I'm home", "Movie"]
    scene_names += [f"Éclairage chaleureux du salon {i:02d}" for i in range(6)]
    return [
        {
            "id": "fixture-home",
            "name": "Demo Home",
            "rooms": [
                {
                    "id": f"fixture-room-{index:02d}",
                    "name": name,
                    "home": "Demo Home",
                    "accessoryIds": [a["id"] for a in values if a["room"] == name],
                }
                for index, name in enumerate(ROOMS)
            ],
            "accessories": values,
            "scenes": [
                {"id": f"fixture-scene-{i:02d}", "name": name, "home": "Demo Home", "kind": "user"}
                for i, name in enumerate(scene_names)
            ],
        }
    ]


class CaptureLink:
    device_id = DEVICE
    caps = frozenset({"home", "cards"})

    def __init__(self):
        self.frames = []

    async def notify(self, method, params):
        self.frames.append(envelope.event(method, params))

    async def close(self, code, reason):
        raise AssertionError(f"fixture delivery unexpectedly closed: {code}")


async def drain(sessions, link):
    await asyncio.wait_for(sessions._subscribers[link].queue.join(), 1)
    frames, link.frames = link.frames, []
    return frames


async def capture_wire(short_tmp, relay, monkeypatch):
    relay.homes = real_shaped_inventory()
    sessions = SessionHub(short_tmp, clock=lambda: FIXTURE_TIME)
    home = HomeBridge(sessions, short_tmp, url=relay.url)
    home.token_path.write_text(TOKEN)
    home.token_path.chmod(0o600)
    home._client = aiohttp.ClientSession(trust_env=False)
    link = CaptureLink()
    result = {}
    try:
        # Exercise HomeBridge's actual HTTP decoder and snapshot mapper, rather
        # than constructing compact firmware-facing fields in this test.
        await home._reload()
        home.available = True
        await sessions.subscribe(link)
        await drain(sessions, link)
        await home.subscribe(link)
        result["snapshot.jsonl"] = await drain(sessions, link)

        await home._event("characteristic", {"accessoryId": "fixture-light-00", "characteristic": "hue", "value": 120})
        await home._event("reachability", {"accessoryId": "fixture-sensor-00", "reachable": False})
        await home._refresh(link)
        result["updates.jsonl"] = await drain(sessions, link)

        lamp = alias("a", "fixture-light-00")
        requests = [
            envelope.request(101, "home.set", {"id": lamp, "values": {"on": True, "brightness": 67, "sat": 20}}),
            envelope.request(102, "home.toggle", {"id": lamp}),
            envelope.request(103, "home.scene", {"id": alias("s", "fixture-scene-05")}),
            envelope.request(104, "home.toggle", {"id": lamp}),
        ]
        replies = []
        for request in requests:
            if request["id"] == 104:
                relay.failure = 429
            try:
                accepted = await home.request(DEVICE, request["m"], request["p"])
                replies.append(envelope.success(request["id"], accepted))
            except LinkError as exc:
                replies.append(envelope.failure(request["id"], exc.code, exc.msg))
        relay.failure = None
        result["requests.jsonl"], result["replies.jsonl"] = requests, replies

        # Freeze only the adapter's clock/UUID dependencies. The SessionHub
        # clock is injected separately; event loops retain their real clocks.
        monkeypatch.setattr(
            home_module, "time", SimpleNamespace(time=lambda: FIXTURE_TIME, strftime=lambda fmt: "19:30")
        )
        monkeypatch.setattr(home_module, "uuid", SimpleNamespace(uuid4=lambda: SimpleNamespace(hex="0" * 32)))
        await home._event(
            "characteristic",
            {"accessoryId": "fixture-doorbell", "characteristic": "buttonEvent", "value": 0, "ts": "fixture-press"},
        )
        result["doorbell.jsonl"] = await drain(sessions, link)

        home.available = False
        await home._refresh(link, force=True)
        result["offline.jsonl"] = await drain(sessions, link)
        home._load([])
        await home._refresh(link, force=True)
        result["empty.jsonl"] = await drain(sessions, link)
        return result
    finally:
        await home.stop()
        await sessions.close()


async def test_home_bridge_byte_exact_firmware_fixtures(short_tmp, relay, monkeypatch):
    generated = await capture_wire(short_tmp, relay, monkeypatch)
    for name, frames in generated.items():
        data = "".join(envelope.encode(frame) + "\n" for frame in frames).encode()
        assert TOKEN.encode() not in data and b"fixture-light-" not in data
        assert b"127.0.0.1" not in data
        for line in data.splitlines():
            assert len(line) <= 4096
            assert envelope.parse(line.decode()).t in {"evt", "req", "res"}
        if os.environ.get("ORB_UPDATE_HOME_WIRE_FIXTURES") == "1":
            FIXTURES.mkdir(parents=True, exist_ok=True)
            (FIXTURES / name).write_bytes(data)
        assert (FIXTURES / name).read_bytes() == data, f"regenerate {name} only after reviewing the wire change"

    parts = [frame["p"] for frame in generated["snapshot.jsonl"]]
    assert len(parts) >= 3 and parts[0]["reset"] is True
    assert all(p["reset"] is False for p in parts[1:])
    assert all(p["more"] is True for p in parts[:-1]) and parts[-1]["more"] is False
    assert all(set(p) == {"rooms", "accessories", "scenes", "available", "reset", "more"} for p in parts)
    assert all(p["available"] is True for p in parts)
    assert max(len(envelope.encode(f).encode()) for f in generated["snapshot.jsonl"]) >= 3900
    snapshot = {key: [item for p in parts for item in p[key]] for key in ("rooms", "accessories", "scenes")}
    assert {key: len(values) for key, values in snapshot.items()} == {"rooms": 9, "accessories": 45, "scenes": 12}
    assert any(len(a["name"].encode()) == 32 for a in snapshot["accessories"])
    for a in snapshot["accessories"]:
        assert a["room"] in {r["id"] for r in snapshot["rooms"]}
        assert all(valid_value(key, value) for key, value in a["state"].items())
        assert len(a["id"].encode()) <= 16 and a["id"].isascii()
        assert len(a["name"].encode()) <= 32
    assert len(generated["updates.jsonl"]) == 2
    replies = generated["replies.jsonl"]
    assert replies[0]["r"]["state"]["brightness"] == 70  # Relay normalization, not the requested 67.
    assert replies[1]["r"]["state"]["on"] is False
    assert replies[2]["r"]["accepted"] is True and replies[3]["e"]["code"] == "busy"
    assert len(generated["doorbell.jsonl"]) == 1
    card = generated["doorbell.jsonl"][0]["p"]
    assert card["id"].startswith("home-doorbell-") and card["kind"] == "attention"
    assert card["priority"] == "high" and card["reply"] is False and card["title"] == "Front Door"
    assert card["options"] == [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}]
    assert all(p["p"]["available"] is False for p in generated["offline.jsonl"])
    assert all(a["reachable"] is False for p in generated["offline.jsonl"] for a in p["p"]["accessories"])
    assert generated["empty.jsonl"][0]["p"] == {
        "rooms": [],
        "accessories": [],
        "scenes": [],
        "available": False,
        "reset": True,
        "more": False,
    }
