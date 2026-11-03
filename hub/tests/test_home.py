"""HomeRelay contract tests use only a throwaway HTTP/SSE server on loopback."""

import asyncio
import json
from datetime import datetime

import pytest
import pytest_asyncio
from aiohttp import ClientTimeout, web
from fake_device import paired_device

from orb_hub.envelope import LinkError
from orb_hub.home import HomeBridge, alias
from orb_hub.sessions import SessionHub

TOKEN = "hr_TEST_ONLY_home_bridge_credential"
DEVICE = "orb-001122334455"
SECOND = "orb-aabbccddeeff"


def characteristic(kind, value, writable=True, **limits):
    return {
        "id": f"characteristic-{kind}",
        "type": kind,
        "label": kind,
        "value": value,
        "format": "bool" if isinstance(value, bool) else "float",
        "readable": True,
        "writable": writable,
        "notifies": True,
        **limits,
    }


def accessory(key, kind="lightbulb", room="Bedroom", state=None, writable=None, name=None):
    state = {"on": False, "brightness": 50} if state is None else state
    writable = set(state) if writable is None else set(writable)
    return {
        "id": key,
        "name": name or key,
        "home": "Demo Home",
        "room": room,
        "category": kind,
        "reachable": True,
        "bridged": True,
        "state": state,
        "services": [
            {
                "id": f"service-{key}",
                "type": kind,
                "name": name or key,
                "primary": True,
                "characteristics": [characteristic(k, v, k in writable) for k, v in state.items()],
            }
        ],
    }


def inventory():
    accessories = [
        accessory(
            "lamp",
            state={
                "on": False,
                "brightness": 50,
                "hue": 30,
                "saturation": 80,
                "colorTemperature": 250,
                "custom.1234": 1,
            },
        ),
        accessory("switch", "switch", "Living Room", {"on": True}),
        accessory("outlet", "outlet", "Kitchen", {"on": True, "inUse": True}, ["on"]),
        accessory("fan", "fan", "Office", {"active": 1, "rotationSpeed": 60}),
        accessory(
            "thermostat",
            "thermostat",
            "Hallway",
            {"currentTemperature": 21.5, "targetTemperature": 22, "targetHeatingCooling": 1},
            ["targetTemperature", "targetHeatingCooling"],
        ),
        accessory("lock", "doorLock", "Entrance", {"lockState": 1, "lockTarget": 1}, ["lockTarget"]),
        accessory(
            "sensor",
            "sensor",
            "Bedroom",
            {
                "currentTemperature": 20,
                "humidity": 48,
                "motion": True,
                "contact": 0,
                "batteryLevel": 75,
                "lowBattery": 0,
            },
            [],
        ),
        accessory("admin", "switch", "Admin", {"on": True}),
        accessory("server", "switch", "Server Room", {"on": True}),
        accessory("bridge", "bridge", "Bedroom", {}, []),
        accessory("camera", "camera", "Entrance", {}, []),
        accessory("doorbell", "doorbell", "Entrance", {}, []),
    ]
    rooms = [
        {
            "id": f"room-{name}",
            "name": name,
            "home": "Demo Home",
            "accessoryIds": [a["id"] for a in accessories if a["room"] == name],
        }
        for name in sorted({a["room"] for a in accessories})
    ]
    return [
        {
            "id": "home-id",
            "name": "Demo Home",
            "rooms": rooms,
            "accessories": accessories,
            "scenes": [
                {"id": "morning", "name": "Good morning", "home": "Demo Home", "kind": "wakeUp"},
                {"id": "evening", "name": "Evening", "home": "Demo Home", "kind": "user"},
                {"id": "movie", "name": "Movie", "home": "Demo Home", "kind": "user"},
                {"id": "trigger", "name": "Trigger202512012359", "home": "Demo Home", "kind": "user"},
            ],
        }
    ]


class Relay:
    def __init__(self):
        self.homes = inventory()
        self.requests = []
        self.streams = []
        self.transports = {}
        self.active = 0
        self.max_active = 0
        self.opened = 0
        self.failure = None
        self.write_delay = 0
        self.silent = False
        self.url = ""

    def find(self, key):
        return next(a for h in self.homes for a in h["accessories"] if a["id"] == key)

    async def handle(self, request):
        assert request.remote == "127.0.0.1"
        path = request.path.removeprefix("/api/v1")
        if path != "/health":
            assert request.headers.get("Authorization") == f"Bearer {TOKEN}"
        body = await request.json() if request.can_read_body else None
        self.requests.append((request.method, path, body))
        if path == "/health":
            return web.json_response({"status": "ok", "homekit": "ready", "service": "homerelay", "version": "test"})
        if self.failure:
            return web.json_response({"error": {"code": "private", "message": TOKEN}}, status=self.failure)
        if path == "/snapshot":
            # APIRouter encodes [HomeDTO], not an enclosing object. Full services
            # are necessary to determine which controls truly support writes.
            return web.json_response(self.homes)
        if path == "/events":
            response = web.StreamResponse(headers={"Content-Type": "text/event-stream"})
            await response.prepare(request)
            queue = asyncio.Queue()
            self.streams.append(queue)
            self.transports[queue] = request.transport
            self.active += 1
            self.opened += 1
            self.max_active = max(self.max_active, sum(not t.is_closing() for t in self.transports.values()))
            try:
                await response.write(b": homerelay events\nretry: 3000\n\n")
                while True:
                    try:
                        payload = await asyncio.wait_for(queue.get(), 0.025)
                    except TimeoutError:
                        if not self.silent:
                            await response.write(b": keepalive\n\n")
                        continue
                    if payload is None:
                        break
                    await response.write(payload)
            except (ConnectionError, asyncio.CancelledError):
                pass
            finally:
                self.active -= 1
                self.streams.remove(queue)
                self.transports.pop(queue, None)
            return response
        if request.method == "POST":
            await asyncio.sleep(self.write_delay)
            if path.startswith("/scenes/"):
                key = path.split("/")[2]
                return web.json_response(next(s for h in self.homes for s in h["scenes"] if s["id"] == key))
            key = path.split("/")[2]
            value = self.find(key)
            if path.endswith("/toggle"):
                power = "on" if "on" in value["state"] else "active"
                value["state"][power] = not value["state"][power]
            else:
                for field, accepted in body.items():
                    if field == "brightness":
                        accepted = min(100, max(0, round(accepted / 10) * 10))
                    value["state"][field] = accepted
            return web.json_response(value)
        raise web.HTTPNotFound()

    async def emit(self, kind="characteristic", **fields):
        # HomeRelay stamps events with the shared Mac clock, after any snapshot it served.
        value = {"type": kind, "home": "Demo Home", "ts": datetime.now().astimezone().isoformat(), **fields}
        data = f"event: {kind}\ndata: {json.dumps(value)}\n\n".encode()
        for queue in list(self.streams):
            await queue.put(data)

    async def disconnect(self):
        for queue in list(self.streams):
            await queue.put(None)


@pytest_asyncio.fixture
async def relay():
    relay = Relay()
    app = web.Application()
    app.router.add_route("*", "/api/v1/{rest:.*}", relay.handle)
    runner = web.AppRunner(app, access_log=None, shutdown_timeout=0.05)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    relay.url = f"http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}/api/v1"
    try:
        yield relay
    finally:
        await relay.disconnect()
        await runner.cleanup()


async def wait_until(predicate):
    ready = asyncio.Event()
    handle = None

    def check():
        nonlocal handle
        if predicate():
            ready.set()
        else:
            handle = asyncio.get_running_loop().call_later(0.01, check)

    check()
    try:
        async with asyncio.timeout(3):
            await ready.wait()
    finally:
        if handle is not None:
            handle.cancel()


class Link:
    def __init__(self, device_id=DEVICE, caps=("home", "cards")):
        self.device_id = device_id
        self.caps = set(caps)
        self.events = asyncio.Queue()
        self.closed = asyncio.Event()

    async def notify(self, method, params):
        await self.events.put((method, params))

    async def close(self, code, reason):
        self.closed.set()


async def next_event(link, wanted):
    async with asyncio.timeout(3):
        while True:
            method, params = await link.events.get()
            if method == wanted:
                return params


async def home_parts(link):
    parts = []
    while True:
        part = await next_event(link, "home.snapshot")
        if part["reset"]:
            parts.clear()
        parts.append(part)
        if not part["more"]:
            return parts


@pytest_asyncio.fixture
async def bridge(short_tmp, relay):
    home = short_tmp / "home"
    home.mkdir(mode=0o700)
    token = home / "homerelay.token"
    token.write_text(TOKEN)
    token.chmod(0o600)
    sessions = SessionHub(short_tmp)
    await sessions.start()
    bridge = HomeBridge(sessions, short_tmp, url=relay.url, reconnect_delay=0.02, stream_timeout=0.15)
    await bridge.start()
    try:
        await wait_until(lambda: bridge.status()["available"] and relay.active == 1)
        yield bridge, sessions
    finally:
        await bridge.stop()
        await sessions.close()


def keyed(snapshot):
    return {value["id"]: value for value in snapshot["accessories"]}


async def test_mapping_uses_compact_state_and_actual_writable_metadata(bridge):
    home, _ = bridge
    snapshot = home.snapshot(DEVICE)
    values = keyed(snapshot)
    lamp = values[alias("a", "lamp")]
    assert lamp["kind"] == "light" and lamp["name"] == "lamp"
    assert lamp["state"] == {"on": False, "brightness": 50, "hue": 30, "sat": 80, "ct": 250}
    assert set(lamp["writable"]) == {"on", "brightness", "hue", "sat", "ct"}
    assert values[alias("a", "fan")]["state"] == {"on": True, "speed": 60}
    assert values[alias("a", "thermostat")]["state"] == {"temp": 21.5, "target": 22, "mode": 1}
    assert set(values[alias("a", "thermostat")]["writable"]) == {"target", "mode"}
    assert values[alias("a", "lock")]["state"] == {"locked": True}
    assert values[alias("a", "sensor")]["state"] == {
        "temp": 20,
        "humidity": 48,
        "motion": True,
        "contact": False,
        "battery": 75,
        "low_battery": False,
    }
    assert values[alias("a", "sensor")]["writable"] == []
    assert all(len(a["id"].encode()) <= 16 and len(a["name"].encode()) <= 32 for a in values.values())
    assert all(a["room"] in {r["id"] for r in snapshot["rooms"]} for a in values.values())
    assert "custom." not in json.dumps(snapshot)
    assert TOKEN not in json.dumps(home.status())


async def test_default_favourites_and_hiding_rules_are_per_device(bridge):
    home, _ = bridge
    snapshot = home.snapshot(DEVICE)
    values = keyed(snapshot)
    assert {a["name"] for a in values.values() if a["favourite"]} == {"lamp", "switch", "fan", "thermostat", "lock"}
    assert not {"admin", "server", "bridge", "camera"} & {a["name"] for a in values.values()}
    assert {s["name"] for s in snapshot["scenes"] if s["favourite"]} == {"Good morning", "Evening"}
    assert "Trigger202512012359" not in {s["name"] for s in snapshot["scenes"]}
    await home.set_favourite(DEVICE, alias("a", "admin"), True)
    await home.set_favourite(DEVICE, alias("a", "lamp"), False)
    assert keyed(home.snapshot(DEVICE))[alias("a", "admin")]["favourite"]
    assert not keyed(home.snapshot(DEVICE))[alias("a", "lamp")]["favourite"]
    assert alias("a", "admin") not in keyed(home.snapshot(SECOND))
    assert keyed(home.snapshot(SECOND))[alias("a", "lamp")]["favourite"]
    for key in ("bridge", "camera"):
        with pytest.raises(LinkError):
            await home.set_favourite(DEVICE, alias("a", key), True)


async def test_favourites_persist_without_renaming_aliases(bridge, short_tmp, relay):
    home, sessions = bridge
    await home.set_favourite(DEVICE, alias("a", "lamp"), False)
    await home.set_favourite(DEVICE, alias("a", "server"), True)
    await home.set_favourite(DEVICE, alias("s", "movie"), True)
    await home.stop()
    replacement = HomeBridge(sessions, short_tmp, url=relay.url, reconnect_delay=0.02)
    await replacement.start()
    try:
        await wait_until(lambda: replacement.status()["available"])
        values = keyed(replacement.snapshot(DEVICE))
        assert not values[alias("a", "lamp")]["favourite"]
        assert values[alias("a", "server")]["favourite"]
        assert next(s for s in replacement.snapshot(DEVICE)["scenes"] if s["id"] == alias("s", "movie"))["favourite"]
    finally:
        await replacement.stop()


async def test_successful_set_toggle_scene_reply_with_relay_accepted_state(bridge, relay):
    home, _ = bridge
    lamp = alias("a", "lamp")
    result = await home.request(DEVICE, "home.set", {"id": lamp, "values": {"on": True, "brightness": 67, "sat": 20}})
    assert result["id"] == lamp
    assert result["state"]["on"] is True and result["state"]["brightness"] == 70
    assert result["state"]["sat"] == 20
    assert relay.requests[-1] == ("POST", "/accessories/lamp", {"on": True, "brightness": 67, "saturation": 20})
    assert (await home.request(DEVICE, "home.toggle", {"id": lamp}))["state"]["on"] is False
    scene = alias("s", "movie")
    assert await home.request(DEVICE, "home.scene", {"id": scene}) == {"id": scene, "accepted": True}
    assert relay.requests[-1][:2] == ("POST", "/scenes/movie/run")
    # The write response is authoritative: no immediate fresh GET can overwrite
    # the accepted value with a temporarily stale bridged-device value.
    assert not any(method == "GET" and path == "/accessories/lamp" for method, path, _ in relay.requests)


@pytest.mark.parametrize(
    "raw, values, characteristic, value, expected",
    [
        ("lamp", {"brightness": 67}, "brightness", 23, {"brightness": 23}),
        ("lamp", {"on": True}, "on", False, {"on": False}),
        ("lock", {"locked": False}, "lockState", 2, {}),
    ],
)
async def test_newer_sse_survives_delayed_write_reply_in_shared_cache(
    bridge, relay, monkeypatch, raw, values, characteristic, value, expected
):
    home, _ = bridge
    key = alias("a", raw)
    original = home._json
    ready, release = asyncio.Event(), asyncio.Event()

    async def delayed(method, path, *, values=None):
        result = await original(method, path, values=values)
        if method == "POST":
            ready.set()
            await release.wait()
        return result

    monkeypatch.setattr(home, "_json", delayed)
    request = asyncio.create_task(home.request(DEVICE, "home.set", {"id": key, "values": values}))
    try:
        await ready.wait()
        await relay.emit(accessoryId=raw, characteristic=characteristic, value=value)
        # The event can repeat the pre-write value, so wait for its provenance
        # marker rather than mistaking an unchanged cache value for delivery.
        await wait_until(lambda: bool(home._write_events.get(key)))
        assert home.accessories[key]["_relay_state"].get(characteristic) == value
        release.set()
        result = await request
        state = keyed(home.snapshot(DEVICE))[key]["state"]
        if raw == "lock":
            assert result["state"] == {"locked": True} and "locked" not in state
        else:
            field = next(iter(expected))
            assert result["state"][field] != expected[field]
            assert state[field] == expected[field]
        assert not home._write_events
    finally:
        release.set()
        await asyncio.gather(request, return_exceptions=True)


async def test_replacement_snapshot_supersedes_delayed_write_cache(bridge, relay, monkeypatch):
    home, _ = bridge
    key = alias("a", "lamp")
    original = home._json
    ready, release = asyncio.Event(), asyncio.Event()

    async def delayed(method, path, *, values=None):
        result = await original(method, path, values=values)
        if method == "POST":
            ready.set()
            await release.wait()
        return result

    monkeypatch.setattr(home, "_json", delayed)
    request = asyncio.create_task(home.request(DEVICE, "home.set", {"id": key, "values": {"brightness": 67}}))
    try:
        await ready.wait()
        previous = home.accessories[key]
        relay.find("lamp")["state"]["brightness"] = 23
        await relay.emit("homes")
        await wait_until(lambda: home.accessories[key] is not previous)
        release.set()
        assert (await request)["state"]["brightness"] == 70
        assert keyed(home.snapshot(DEVICE))[key]["state"]["brightness"] == 23
        assert not home._write_events
    finally:
        release.set()
        await asyncio.gather(request, return_exceptions=True)


async def test_waiting_for_shared_slots_preserves_per_accessory_serialization(bridge, monkeypatch):
    home, _ = bridge
    key = alias("a", "lamp")
    home._write_slots = asyncio.Semaphore(0)
    original = home._json
    second_entered, third_entered, release = asyncio.Event(), asyncio.Event(), asyncio.Event()
    calls = 0

    async def gated(method, path, *, values=None):
        nonlocal calls
        if method == "POST":
            calls += 1
            if calls == 2:
                second_entered.set()
                await release.wait()
            elif calls == 3:
                third_entered.set()
        return await original(method, path, values=values)

    monkeypatch.setattr(home, "_json", gated)
    requests = [asyncio.create_task(home.request(DEVICE, "home.toggle", {"id": key})) for _ in range(2)]
    try:
        await asyncio.sleep(0.02)
        home._write_slots.release()
        await requests[0]
        await second_entered.wait()
        requests.append(asyncio.create_task(home.request(DEVICE, "home.toggle", {"id": key})))
        home._write_slots.release()
        await asyncio.sleep(0.02)
        assert not third_entered.is_set()
        release.set()
        await asyncio.gather(*requests)
        assert third_entered.is_set() and not home._locks and not home._write_events
    finally:
        release.set()
        for request in requests:
            if not request.done():
                request.cancel()
        await asyncio.gather(*requests, return_exceptions=True)


@pytest.mark.parametrize(
    "params",
    [
        {"id": "missing", "values": {"on": True}},
        {"id": alias("a", "lamp"), "values": {}},
        {"id": alias("a", "lamp"), "values": {"on": "yes"}},
        {"id": alias("a", "lamp"), "values": {"brightness": True}},
        {"id": alias("a", "lamp"), "values": {"brightness": 150}},
        {"id": alias("a", "lamp"), "values": {"brightness": float("nan")}},
        {"id": alias("a", "lamp"), "values": {"custom.1234": 1}},
        {"id": alias("a", "lamp"), "values": {"temp": 22}},
        {"id": alias("a", "admin"), "values": {"on": False}},
        {"id": alias("a", "lamp"), "values": {"on": True}, "extra": True},
    ],
)
async def test_invalid_or_hidden_writes_fail_before_http(bridge, relay, params):
    home, _ = bridge
    before = len([r for r in relay.requests if r[0] == "POST"])
    with pytest.raises(LinkError):
        await home.request(DEVICE, "home.set", params)
    assert len([r for r in relay.requests if r[0] == "POST"]) == before


async def test_lock_toggle_and_read_only_sensor_are_never_sent(bridge, relay):
    home, _ = bridge
    for method, params in [
        ("home.toggle", {"id": alias("a", "lock")}),
        ("home.toggle", {"id": alias("a", "sensor")}),
        ("home.set", {"id": alias("a", "sensor"), "values": {"temp": 22}}),
    ]:
        with pytest.raises(LinkError, match="unsupported"):
            await home.request(DEVICE, method, params)
    assert not [r for r in relay.requests if r[0] == "POST"]


@pytest.mark.parametrize(
    "status, code",
    [
        (401, "unauthorized"),
        (403, "unauthorized"),
        (404, "not_found"),
        (409, "conflict"),
        (422, "bad_request"),
        (429, "busy"),
        (500, "internal"),
        (503, "unsupported"),
        (504, "timeout"),
    ],
)
async def test_relay_failures_never_claim_success_or_disclose_payload(bridge, relay, caplog, status, code):
    home, _ = bridge
    relay.failure = status
    with pytest.raises(LinkError) as caught:
        await home.request(DEVICE, "home.set", {"id": alias("a", "lamp"), "values": {"on": True}})
    assert caught.value.code == code
    assert TOKEN not in str(caught.value) + caplog.text + json.dumps(home.status())
    assert not keyed(home.snapshot(DEVICE))[alias("a", "lamp")]["state"]["on"]


async def test_sse_coalesces_latest_fields_and_reachability_over_one_stream(bridge, relay):
    home, _ = bridge
    first, second = Link(), Link(SECOND)
    await home.subscribe(first)
    await home.subscribe(second)
    await home_parts(first)
    await home_parts(second)
    for brightness in range(100):
        await relay.emit(accessoryId="lamp", characteristic="brightness", value=brightness)
    await relay.emit(accessoryId="lamp", characteristic="on", value=True)
    await relay.emit("reachability", accessoryId="lamp", reachable=False)
    update = await next_event(first, "home.update")
    assert update["id"] == alias("a", "lamp")
    assert update["state"]["brightness"] == 99 and update["state"]["on"] is True
    assert update["reachable"] is False
    assert await next_event(second, "home.update") == update
    assert relay.max_active == 1 and relay.opened == 1
    assert first.events.empty() and second.events.empty()
    with pytest.raises(LinkError):
        await home.request(DEVICE, "home.toggle", {"id": alias("a", "lamp")})


async def test_sse_homes_change_refreshes_snapshot_and_drops_deleted_accessory(bridge, relay):
    home, _ = bridge
    link = Link()
    await home.subscribe(link)
    await home_parts(link)
    relay.homes[0]["accessories"] = [a for a in relay.homes[0]["accessories"] if a["id"] != "lamp"]
    relay.homes[0]["accessories"].append(accessory("new-light", name="New lamp"))
    await relay.emit("homes")
    parts = await home_parts(link)
    names = {a["name"] for p in parts for a in p["accessories"]}
    assert "lamp" not in names and "New lamp" in names
    assert relay.max_active == 1


async def test_stream_disconnect_marks_cache_unavailable_then_refreshes_on_reconnect(bridge, relay):
    home, _ = bridge
    link = Link()
    await home.subscribe(link)
    await home_parts(link)
    relay.failure = 503
    await relay.disconnect()
    await wait_until(lambda: not home.status()["available"])
    assert all(not a["reachable"] for a in home.snapshot(DEVICE)["accessories"])
    with pytest.raises(LinkError):
        await home.request(DEVICE, "home.toggle", {"id": alias("a", "lamp")})
    relay.find("lamp")["state"]["brightness"] = 23
    relay.failure = None
    await wait_until(lambda: home.status()["available"] and relay.opened >= 2)
    assert keyed(home.snapshot(DEVICE))[alias("a", "lamp")]["state"]["brightness"] == 23
    assert relay.max_active == 1


async def test_keepalive_prevents_timeout_and_silence_forces_single_reconnect(bridge, relay):
    home, _ = bridge
    await asyncio.sleep(0.35)
    assert relay.opened == 1 and home.status()["available"]
    relay.silent = True
    await wait_until(lambda: relay.opened >= 2)
    assert relay.max_active == 1


async def test_doorbell_is_attention_card_independent_of_agent_source_preferences(bridge, relay):
    home, sessions = bridge
    link = Link()
    await sessions.set_prefs(DEVICE, {"sources": {"claude": False, "codex": False, "dot": False}})
    await sessions.subscribe(link)
    await home.subscribe(link)
    await home_parts(link)
    await relay.emit(accessoryId="doorbell", characteristic="buttonEvent", value=0)
    value = await next_event(link, "card.show")
    assert value["kind"] == "attention" and value["title"] == "Front Door"
    assert value["reply"] is False and value["priority"] == "high"
    assert value["body"] and TOKEN not in json.dumps(value)
    assert value["expires"] > 0
    assert value["options"] == [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}]


async def test_snapshot_bounds_utf8_chunks_and_favourite_priority(bridge, relay):
    home, _ = bridge
    relay.homes[0]["accessories"] = [accessory(f"extra-{n:03d}", name="💡" * 20) for n in range(80)]
    await relay.emit("homes")
    await wait_until(lambda: any(a["name"].startswith("💡") for a in home.snapshot(DEVICE)["accessories"]))
    await home.set_favourite(DEVICE, alias("a", "extra-079"), True)
    link = Link()
    await home.subscribe(link)
    parts = await home_parts(link)
    accessories = [a for part in parts for a in part["accessories"]]
    assert len(accessories) == 64 and len({a["id"] for a in accessories}) == 64
    assert alias("a", "extra-079") in {a["id"] for a in accessories}
    assert parts[0]["reset"] is True and all(not p["reset"] for p in parts[1:])
    assert len(parts) > 1 and parts[-1]["more"] is False
    assert all(len(a["name"].encode()) <= 32 for a in accessories)
    for part in parts:
        frame = {"t": "evt", "m": "home.snapshot", "p": part}
        assert len(json.dumps(frame, ensure_ascii=False, separators=(",", ":")).encode()) <= 4096


async def test_missing_token_stays_offline_without_network(short_tmp, relay, monkeypatch):
    sessions = SessionHub(short_tmp)
    home = HomeBridge(sessions, short_tmp, url=relay.url, reconnect_delay=0.02)
    await home.start()
    try:
        await asyncio.sleep(0.05)
        assert home.status()["available"] is False
        assert not relay.requests
    finally:
        await home.stop()
        await sessions.close()


async def test_home_capability_is_independent_legacy_receives_no_home_frames(make_hub, short_tmp, relay, monkeypatch):
    token_dir = short_tmp / "data" / "home"
    token_dir.mkdir(parents=True, mode=0o700)
    token = token_dir / "homerelay.token"
    token.write_text(TOKEN)
    token.chmod(0o600)
    monkeypatch.setenv("HOMERELAY_URL", relay.url)
    hub = await make_hub(home_enabled=True)
    await wait_until(lambda: hub.home.status()["available"])
    legacy = await paired_device(hub)
    modern = await paired_device(hub, device_id=SECOND)
    try:
        await modern.disconnect()
        await modern.connect()
        hello = await modern.hello(caps=["home"])
        assert hello["r"]["caps"] == ["home"] and hello["r"]["prefs"]["units"] == {"temperature": "C", "source": "auto"}
        parts = await home_parts(modern)
        assert any(p["accessories"] for p in parts)
        assert legacy.events.empty()
        assert (await legacy.request("home.toggle", {"id": alias("a", "lamp")}))["e"]["code"] == "unsupported"
        result = await modern.request("home.toggle", {"id": alias("a", "lamp")})
        assert result["ok"] and result["r"]["state"]["on"] is True
        assert all(method.startswith("home.") for method, _ in list(modern.events._queue))
    finally:
        await legacy.aclose()
        await modern.aclose()


async def test_lock_write_uses_explicit_target_without_inventing_physical_state(bridge, relay):
    home, _ = bridge
    result = await home.request(DEVICE, "home.set", {"id": alias("a", "lock"), "values": {"locked": False}})
    assert relay.requests[-1] == ("POST", "/accessories/lock", {"lockTarget": 0})
    # HomeKit still reports locked while its motor is moving; a target is not
    # confirmation that the door has physically unlocked.
    assert result["state"] == {"locked": True}


async def test_fahrenheit_preferences_keep_home_writes_and_accepted_state_in_celsius(bridge, relay):
    home, sessions = bridge
    await sessions.set_prefs(DEVICE, {"units": {"temperature": "F"}})
    result = await home.request(DEVICE, "home.set", {"id": alias("a", "thermostat"), "values": {"target": 23}})
    assert relay.requests[-1] == ("POST", "/accessories/thermostat", {"targetTemperature": 23})
    assert result["state"] == {"temp": 21.5, "target": 23, "mode": 1}


@pytest.mark.parametrize("payload", [b"[]", b'"not-an-object"', b"{malformed"])
async def test_malformed_sse_object_reconnects_and_repairs_state(bridge, relay, payload):
    home, _ = bridge
    link = Link()
    await home.subscribe(link)
    await home_parts(link)
    opened = relay.opened
    relay.find("lamp")["state"]["brightness"] = 20
    for queue in list(relay.streams):
        await queue.put(b"event: characteristic\ndata: " + payload + b"\n\n")
    await wait_until(lambda: relay.opened > opened and home.status()["available"])
    assert keyed(home.snapshot(DEVICE))[alias("a", "lamp")]["state"]["brightness"] == 20
    await relay.emit(accessoryId="lamp", characteristic="brightness", value=30)
    update = await next_event(link, "home.update")
    assert update["state"]["brightness"] == 30


@pytest.mark.parametrize("mode, symlink", [(0o644, False), (0o600, True)])
async def test_insecure_token_file_is_rejected_without_network(short_tmp, relay, mode, symlink):
    directory = short_tmp / "home"
    directory.mkdir(mode=0o700)
    path = directory / "homerelay.token"
    if symlink:
        target = short_tmp / "another-token"
        target.write_text(TOKEN)
        target.chmod(mode)
        path.symlink_to(target)
    else:
        path.write_text(TOKEN)
        path.chmod(mode)
    sessions = SessionHub(short_tmp)
    original_mode = path.lstat().st_mode
    home = HomeBridge(sessions, short_tmp, url=relay.url, reconnect_delay=0.01)
    await home.start()
    try:
        await wait_until(lambda: home.status()["error"] == "unauthorized")
        assert not home.status()["available"] and not relay.requests
        assert path.lstat().st_mode == original_mode
    finally:
        await home.stop()
        await sessions.close()


@pytest.mark.parametrize("invalidation", ["hidden", "unreachable", "read_only"])
async def test_queued_write_revalidates_current_visibility_and_controls(bridge, relay, invalidation):
    home, _ = bridge
    raw = "admin" if invalidation == "hidden" else "lamp"
    key = alias("a", raw)
    if invalidation == "hidden":
        await home.set_favourite(DEVICE, key, True)
    lock = asyncio.Lock()
    home._locks[key] = lock
    await lock.acquire()
    request = asyncio.create_task(home.request(DEVICE, "home.toggle", {"id": key}))
    try:
        await asyncio.sleep(0.02)
        if invalidation == "hidden":
            await home.set_favourite(DEVICE, key, False)
        elif invalidation == "unreachable":
            await relay.emit("reachability", accessoryId=raw, reachable=False)
            await wait_until(lambda: not keyed(home.snapshot(DEVICE))[key]["reachable"])
        else:
            for service in relay.find(raw)["services"]:
                for field in service["characteristics"]:
                    field["writable"] = False
            await relay.emit("homes")
            await wait_until(lambda: not keyed(home.snapshot(DEVICE))[key]["writable"])
        lock.release()
        with pytest.raises(LinkError):
            await request
        assert not [r for r in relay.requests if r[0] == "POST"]
    finally:
        if lock.locked():
            lock.release()
        if not request.done():
            request.cancel()
        await asyncio.gather(request, return_exceptions=True)


async def test_write_deadline_reports_timeout_without_retrying_uncertain_operation(bridge, relay):
    home, _ = bridge
    relay.write_delay = 0.1
    home._client._timeout = ClientTimeout(total=0.02)
    with pytest.raises(LinkError, match="timeout"):
        await home.request(DEVICE, "home.toggle", {"id": alias("a", "lamp")})
    assert keyed(home.snapshot(DEVICE))[alias("a", "lamp")]["state"]["on"] is False
    await asyncio.sleep(0.15)
    assert len([r for r in relay.requests if r[0] == "POST"]) == 1


async def test_doorbell_requires_both_capabilities_and_deduplicates_one_press(bridge, relay):
    home, sessions = bridge
    both = Link()
    cards_only = Link(SECOND, ("cards",))
    home_only = Link("orb-000000000001", ("home",))
    await sessions.subscribe(both)
    await sessions.subscribe(cards_only)
    await home.subscribe(both)
    await home.subscribe(home_only)
    await home_parts(both)
    await home_parts(home_only)
    while not cards_only.events.empty():
        cards_only.events.get_nowait()
    pressed = datetime.now().astimezone().isoformat()  # one press delivered twice
    for _ in range(2):
        await relay.emit(
            accessoryId="doorbell",
            characteristic="buttonEvent",
            characteristicId="doorbell-button",
            value=0,
            ts=pressed,
        )
    value = await next_event(both, "card.show")
    assert value["id"].startswith("home-doorbell-")
    await asyncio.sleep(0.05)
    assert len(sessions.cards) == 1
    assert cards_only.events.empty() and home_only.events.empty()


async def test_excluded_room_doorbell_card_only_reaches_device_that_pinned_it(bridge, relay):
    home, sessions = bridge
    relay.find("doorbell")["room"] = "Admin"
    await relay.emit("homes")
    key = alias("a", "doorbell")
    await wait_until(lambda: key not in keyed(home.snapshot(DEVICE)))
    await home.set_favourite(DEVICE, key, True)
    first, second = Link(), Link(SECOND)
    for link in (first, second):
        await sessions.subscribe(link)
        await home.subscribe(link)
        await home_parts(link)
    await relay.emit(accessoryId="doorbell", characteristic="buttonEvent", value=0)
    value = await next_event(first, "card.show")
    assert value["id"].startswith("home-doorbell-") and value["title"] == "Front Door"
    await asyncio.sleep(0.05)
    assert all(method != "card.show" for method, _ in list(second.events._queue))
    assert key not in keyed(home.snapshot(SECOND))


async def test_on_loop_reads_proceed_while_favourite_commit_holds_write_lock(bridge, short_tmp):
    import sqlite3

    home, _sessions = bridge
    assert home.db.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
    writer = sqlite3.connect(short_tmp / "home" / "home.db", timeout=0)
    try:
        writer.execute("BEGIN EXCLUSIVE")
        writer.execute("INSERT OR REPLACE INTO favourites VALUES ('x','y',1)")
        # An on-loop reader must not block or fail behind a worker commit.
        assert home.db.execute("SELECT count(*) FROM favourites").fetchone() is not None
        home.snapshot(DEVICE)
    finally:
        writer.rollback()
        writer.close()
