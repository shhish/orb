"""Home capability and delivery isolation over the hub's existing device path."""

import asyncio
import json

import pytest
from fake_device import paired_device
from test_home import inventory
from test_sessions import card, session

from orb_hub.control import ControlError
from orb_hub.home import HomeBridge, alias
from orb_hub.sessions import MAX_DELIVERY_QUEUE, SessionHub

DEVICE = "orb-001122334455"


@pytest.fixture
async def loaded_home(hub):
    # Pure inventory injection deliberately never starts the HTTP client or SSE
    # task. All network traffic in these delivery tests is local pinned TLS.
    home = HomeBridge(hub.sessions, hub.cfg.data_dir, url="http://127.0.0.1:1/api/v1")
    home._load(inventory())
    home.available = True
    hub.home = home
    return home


async def reconnect(device, caps, **hello):
    await device.disconnect()
    await device.connect()
    result = await device.hello(caps=caps, **hello)
    assert result["ok"]
    return result["r"]


async def snapshot(device):
    while True:
        method, params = await device.event()
        assert method == "home.snapshot"
        if not params["more"]:
            return params


async def test_home_only_negotiates_units_without_sessions_or_cards(hub, loaded_home):
    await hub.sessions.upsert_session(session(state="busy"))
    await hub.sessions.show_card(card(), source="codex")
    device = await paired_device(hub)
    try:
        result = await reconnect(device, ["home"])
        assert result["caps"] == ["home"] and result["prefs"]["units"] == {"temperature": "C", "source": "auto"}
        await snapshot(device)
        link = hub.online[device.device_id]
        subscriber = hub.sessions._subscribers[link]
        assert subscriber.sessions_enabled is False
        await hub.sessions.upsert_session(session(state="waiting"))
        await hub.sessions.show_card(card("second"), source="claude")
        await asyncio.sleep(0.3)
        assert device.events.empty()
        assert (await device.request("sessions.subscribe"))["e"]["code"] == "unsupported"
    finally:
        await device.aclose()


@pytest.mark.parametrize("binary", [False, True])
async def test_home_only_enforces_4096_byte_frame_ceiling(hub, loaded_home, binary):
    device = await paired_device(hub)
    try:
        await reconnect(device, ["home"])
        await snapshot(device)
        if binary:
            await device.ws.send_bytes(b"x" * 4097)
        else:
            await device.ws.send_str(json.dumps({"t": "evt", "m": "unknown", "p": {"text": "x" * 4096}}))
        assert await device.closed() == 1009
    finally:
        await device.aclose()


async def test_home_oversize_hello_fails_before_negotiation(hub, loaded_home):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        result = await device.hello(caps=["home"], fw="x" * 4096)
        assert result["e"]["code"] == "too_large"
        assert await device.closed() == 1009
        assert device.events.empty()
    finally:
        await device.aclose()


@pytest.mark.parametrize("caps", [[], ["cards"]])
async def test_legacy_and_cards_only_never_receive_home_events_or_write(hub, loaded_home, caps):
    device = await paired_device(hub)
    try:
        result = await reconnect(device, caps)
        assert "home" not in result["caps"]
        if caps:
            assert (await device.event())[0] == "sessions.snapshot"
            assert (await device.event())[0] == "sessions.summary"
        await loaded_home.set_favourite(device.device_id, alias("a", "admin"), True)
        await asyncio.sleep(0.03)
        assert device.events.empty()
        for method, params in (
            ("home.set", {"id": alias("a", "lamp"), "values": {"on": True}}),
            ("home.toggle", {"id": alias("a", "lamp")}),
            ("home.scene", {"id": alias("s", "movie")}),
        ):
            assert (await device.request(method, params))["e"]["code"] == "unsupported"
    finally:
        await device.aclose()


async def test_home_request_without_enabled_bridge_is_unsupported(hub):
    device = await paired_device(hub)
    try:
        result = await reconnect(device, ["home"])
        assert result["caps"] == []
        assert (await device.request("home.toggle", {"id": "a123"}))["e"]["code"] == "unsupported"
    finally:
        await device.aclose()


class CaptureLink:
    def __init__(self, device_id, caps):
        self.device_id = device_id
        self.caps = set(caps)
        self.events = []
        self.close_code = None
        self.closed = asyncio.Event()
        self.blocked = None
        self.writing = asyncio.Event()

    async def notify(self, method, params):
        self.writing.set()
        if self.blocked:
            await self.blocked.wait()
        self.events.append((method, params))

    async def close(self, code, reason):
        self.close_code = code
        self.closed.set()


async def flush(sessions, link):
    await asyncio.wait_for(sessions._subscribers[link].queue.join(), 1)
    events, link.events = link.events, []
    return events


async def test_home_and_cards_share_one_bounded_queue_and_disconnect_slow_consumer(short_tmp):
    sessions = SessionHub(short_tmp)
    home = HomeBridge(sessions, short_tmp, url="http://127.0.0.1:1/api/v1")
    home._load(inventory())
    link = CaptureLink(DEVICE, ["home", "cards"])
    link.blocked = asyncio.Event()
    try:
        await sessions.subscribe(link)
        original = sessions._subscribers[link]
        await home.subscribe(link)
        assert sessions._subscribers[link] is original
        assert len(sessions._subscribers) == 1
        assert original.queue.maxsize == MAX_DELIVERY_QUEUE == 64
        await asyncio.wait_for(link.writing.wait(), 1)
        # Both Home and session events consume the remaining slots in this
        # same queue; overflow requires reconnect and a fresh snapshot.
        for index in range(MAX_DELIVERY_QUEUE):
            method, params = (
                ("home.update", {"id": alias("a", "lamp"), "state": {"on": True}})
                if index % 2
                else ("sessions.summary", original.summary)
            )
            await sessions.emit(link, method, params)
        await asyncio.wait_for(link.closed.wait(), 1)
        assert link.close_code == 1013 and original.failed
        assert original.queue.qsize() == 64
    finally:
        await home.stop()
        await sessions.close()


async def test_home_doorbell_requires_both_caps_and_ignores_agent_source_switches(short_tmp):
    sessions = SessionHub(short_tmp)
    home = HomeBridge(sessions, short_tmp, url="http://127.0.0.1:1/api/v1")
    home._load(inventory())
    links = [
        CaptureLink("orb-home", ["home"]),
        CaptureLink("orb-cards", ["cards"]),
        CaptureLink("orb-both", ["home", "cards"]),
    ]
    try:
        for link in links:
            await sessions.set_prefs(link.device_id, {"sources": {"claude": False, "codex": False, "dot": False}})
            if "cards" in link.caps:
                await sessions.subscribe(link)
            if "home" in link.caps:
                await home.subscribe(link)
            await flush(sessions, link)
        await home._event(
            "characteristic",
            {"accessoryId": "doorbell", "characteristic": "buttonEvent", "value": 0, "ts": "2026-10-27T20:00:00Z"},
        )
        first, second, combined = [await flush(sessions, link) for link in links]
        assert first == [] and second == []
        assert len(combined) == 1 and combined[0][0] == "card.show"
        shown = combined[0][1]
        assert shown["kind"] == "attention" and shown["title"] == "Front Door"
        assert shown["reply"] is False and shown["priority"] == "high"
        assert "Doorbell rang at " in shown["body"]
        assert set(shown) <= {"id", "kind", "title", "body", "priority", "reply", "options", "expires"}
    finally:
        await home.stop()
        await sessions.close()


async def test_home_control_reports_disabled_bridge_and_unknown_paired_device(hub):
    assert await hub.handle_command("home.status", {}) == {"enabled": False, "available": False}
    hub.store.pair(DEVICE, "Orb", "test", "1.1", "ab" * 32, "dev")
    for command in ("list", "add", "remove"):
        with pytest.raises(ControlError, match="start the hub with --home first"):
            await hub.handle_command(f"home.favourites.{command}", {"device_id": DEVICE, "id": "a123"})
    with pytest.raises(ControlError, match="no paired device"):
        await hub.handle_command("home.favourites.list", {"device_id": "orb-aabbccddeeff"})


async def test_home_control_mutates_only_target_offline_device_favourites(hub, loaded_home):
    other = "orb-aabbccddeeff"
    for index, key in enumerate((DEVICE, other)):
        hub.store.pair(key, "Orb", "test", "1.1", f"{index:02x}" * 32, "dev")
    assert DEVICE not in hub.online
    admin = alias("a", "admin")
    lamp = alias("a", "lamp")
    await hub.handle_command("home.favourites.add", {"device_id": DEVICE, "id": admin})
    await hub.handle_command("home.favourites.remove", {"device_id": DEVICE, "id": lamp})
    first = await hub.handle_command("home.favourites.list", {"device_id": DEVICE})
    second = await hub.handle_command("home.favourites.list", {"device_id": other})
    assert admin in {item["id"] for item in first["favourites"]}
    assert lamp not in {item["id"] for item in first["favourites"]}
    assert admin not in {item["id"] for item in second["favourites"]}
    assert lamp in {item["id"] for item in second["favourites"]}
    assert (await hub.handle_command("home.status", {}))["enabled"] is True
    with pytest.raises(ControlError, match="unknown Home object"):
        await hub.handle_command("home.favourites.add", {"device_id": DEVICE, "id": "a-invalid"})
