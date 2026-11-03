"""Per-device session focus, bounded idle browsing and source preferences."""

import asyncio
import json
from types import SimpleNamespace

import pytest
from fake_device import paired_device
from test_sessions import VoiceSocket, card, session

from orb_hub import envelope
from orb_hub.envelope import LinkError
from orb_hub.link import Link
from orb_hub.sessions import SessionHub

DEFAULT_PREFS = {
    "sources": {"claude": True, "codex": True, "dot": True},
    "idle_minutes": 10,
    "units": {"temperature": "C", "source": "auto"},
}


class CaptureLink:
    def __init__(self, device_id="orb-first"):
        self.device_id = device_id
        self.caps = {"cards"}
        self.events = []

    async def notify(self, method, params):
        self.events.append((method, params))

    async def close(self, _code, _reason):
        pass


async def flush(coordinator, link, *, tick=False):
    if tick:
        await asyncio.sleep(0.3)
    await asyncio.wait_for(coordinator._subscribers[link].queue.join(), 1)
    events, link.events = link.events, []
    return events


def snapshot_ids(events):
    return [s["id"] for name, params in events if name == "sessions.snapshot" for s in params["sessions"]]


def summaries(events):
    return [params for name, params in events if name == "sessions.summary"]


@pytest.fixture
async def focus_hub(short_tmp):
    now = [10000.0]
    coordinator = SessionHub(short_tmp, clock=lambda: now[0])
    await coordinator.start()
    yield coordinator, now
    await coordinator.close()


async def test_focus_priority_cap_and_counts_account_for_every_session(focus_hub):
    coordinator, _ = focus_hub
    values = [
        session("wait-new", state="waiting", updated=9900),
        session("wait-old", state="waiting", updated=9800),
        session("busy-new", state="busy", updated=9999),
        session("busy-old", state="busy", updated=9700),
        session("done-new", state="done", updated=9990),
        session("error", state="error", updated=9980),
        session("idle-new", state="idle", updated=9970),
        session("done-old", state="done", updated=9960),
        session("overflow", state="done", updated=9950),
    ] + [session(f"old-{i}") for i in range(15)]
    for value in reversed(values):
        await coordinator.upsert_session(value)
    link = CaptureLink()
    await coordinator.subscribe(link)
    events = await flush(coordinator, link)
    assert snapshot_ids(events) == [
        "wait-old",
        "wait-new",
        "busy-new",
        "busy-old",
        "done-new",
        "error",
        "idle-new",
        "done-old",
    ]
    assert summaries(events) == [{"idle": 16, "hidden_by_source": {"claude": 0, "codex": 0, "dot": 0}}]


async def test_state_age_is_independent_of_activity_and_expires_without_upserts(focus_hub):
    coordinator, now = focus_hub
    await coordinator.upsert_session(session())
    link = CaptureLink()
    await coordinator.subscribe(link)
    assert snapshot_ids(await flush(coordinator, link)) == []
    await coordinator.upsert_session(session(updated=now[0], summary="Still idle"))
    assert not snapshot_ids(await flush(coordinator, link, tick=True))
    await coordinator.upsert_session(session(state="done", updated=1))
    assert snapshot_ids(await flush(coordinator, link, tick=True)) == ["s"]
    now[0] += 599
    await coordinator.upsert_session(session(state="done", updated=now[0], summary="Metadata only"))
    await flush(coordinator, link, tick=True)
    now[0] += 2
    events = await flush(coordinator, link, tick=True)
    assert ("session.remove", {"id": "s"}) in events
    assert snapshot_ids(events) == []
    assert summaries(events)[-1]["idle"] == 1
    assert "s" in coordinator.sessions


async def test_waiting_age_stays_stable_busy_activity_reorders_and_ties_use_id(focus_hub):
    coordinator, _ = focus_hub
    for key, state, updated in [
        ("w-b", "waiting", 9000),
        ("w-a", "waiting", 9000),
        ("b-b", "busy", 9000),
        ("b-a", "busy", 9000),
    ]:
        await coordinator.upsert_session(session(key, state=state, updated=updated))
    link = CaptureLink()
    await coordinator.subscribe(link)
    assert snapshot_ids(await flush(coordinator, link)) == ["w-a", "w-b", "b-a", "b-b"]
    await coordinator.upsert_session(session("w-a", state="waiting", updated=10000, summary="Waiting again"))
    events = await flush(coordinator, link, tick=True)
    assert not any(name == "sessions.snapshot" for name, _ in events)
    assert any(name == "session.update" and params["session"]["id"] == "w-a" for name, params in events)
    await coordinator.upsert_session(session("b-b", state="busy", updated=10000))
    assert snapshot_ids(await flush(coordinator, link, tick=True)) == ["w-a", "w-b", "b-b", "b-a"]


async def test_source_counts_and_focus_eviction_backfill_are_per_device(focus_hub):
    coordinator, _ = focus_hub
    for i in range(10):
        await coordinator.upsert_session(session(str(i), state="busy", updated=10000 - i))
    await coordinator.upsert_session(session("claude", agent="claude", state="waiting", updated=9999))
    await coordinator.upsert_session(session("dot", agent="dot"))
    await coordinator.set_prefs("orb-first", {"sources": {"claude": False, "dot": False}})
    first, second = CaptureLink(), CaptureLink("orb-second")
    await coordinator.subscribe(first)
    await coordinator.subscribe(second)
    first_events, second_events = await flush(coordinator, first), await flush(coordinator, second)
    assert snapshot_ids(first_events) == [str(i) for i in range(8)]
    assert snapshot_ids(second_events)[0] == "claude"
    assert summaries(first_events)[-1] == {"idle": 2, "hidden_by_source": {"claude": 1, "codex": 0, "dot": 1}}
    page = coordinator.list_sessions(first.device_id, {"include": "idle"})
    assert [value["id"] for value in page["sessions"]] == ["8", "9"]
    assert page["cursor"] is None
    await coordinator.remove_session("0")
    events = await flush(coordinator, first, tick=True)
    assert ("session.remove", {"id": "0"}) in events
    assert snapshot_ids(events) == [str(i) for i in range(1, 9)]


async def test_idle_pages_are_bounded_complete_and_do_not_subscribe(focus_hub):
    coordinator, _ = focus_hub
    for i in range(24):
        await coordinator.upsert_session(session(f"idle-{i:02}", updated=i))
    link = CaptureLink()
    await coordinator.subscribe(link)
    await flush(coordinator, link, tick=True)
    ids, cursor = [], None
    while True:
        params = {"include": "idle", "limit": 8}
        if cursor is not None:
            params["cursor"] = cursor
        page = coordinator.list_sessions(link.device_id, params)
        assert 0 < len(page["sessions"]) <= 8
        ids.extend(s["id"] for s in page["sessions"])
        cursor = page["cursor"]
        if cursor is None:
            break
        assert len(cursor.encode()) <= 80
    assert ids == [f"idle-{i:02}" for i in reversed(range(24))]
    await coordinator.upsert_session(session("idle-23", updated=23, summary="A changed summary"))
    events = await flush(coordinator, link, tick=True)
    assert not any(name in {"session.update", "sessions.snapshot"} for name, _ in events)


async def test_idle_response_budget_counts_json_escaping_and_actual_request_id(focus_hub):
    coordinator, _ = focus_hub
    for i in range(8):
        await coordinator.upsert_session(
            session(str(i), title="\x01" * 64, project="\x01" * 32, reason="\x01" * 64, summary="\x01" * 160)
        )
    request_id = 999999999999999
    page = coordinator.list_sessions("orb-first", {"include": "idle", "limit": 8}, request_id=request_id)
    assert 0 < len(page["sessions"]) < 8 and page["cursor"] is not None
    payload = envelope.encode(envelope.success(request_id, page)).encode()
    assert len(payload) <= 4096


async def test_idle_cursors_bind_device_preferences_and_ordered_remainder(focus_hub):
    coordinator, _ = focus_hub
    for i in range(4):
        await coordinator.upsert_session(session(str(i), updated=i))
    first = coordinator.list_sessions("orb-first", {"include": "idle", "limit": 1})
    cursor = first["cursor"]
    with pytest.raises(LinkError, match="conflict"):
        coordinator.list_sessions("orb-second", {"include": "idle", "cursor": cursor})
    await coordinator.set_prefs("orb-first", {"idle_minutes": 0})
    with pytest.raises(LinkError, match="conflict"):
        coordinator.list_sessions("orb-first", {"include": "idle", "cursor": cursor})
    cursor = coordinator.list_sessions("orb-first", {"include": "idle", "limit": 1})["cursor"]
    await coordinator.upsert_session(session("0", updated=100))
    with pytest.raises(LinkError, match="conflict"):
        coordinator.list_sessions("orb-first", {"include": "idle", "cursor": cursor})
    for bad in ["malformed", "x" * 81, "", 1, False]:
        with pytest.raises(LinkError, match="bad_request"):
            coordinator.list_sessions("orb-first", {"include": "idle", "cursor": bad})


@pytest.mark.parametrize(
    "params",
    [
        {},
        {"include": "all"},
        {"include": "idle", "limit": 0},
        {"include": "idle", "limit": 9},
        {"include": "idle", "limit": True},
        {"include": "idle", "limit": 1.5},
        {"include": "idle", "other": True},
    ],
)
async def test_idle_paging_rejects_invalid_requests(focus_hub, params):
    coordinator, _ = focus_hub
    with pytest.raises(LinkError, match="bad_request"):
        coordinator.list_sessions("orb-first", params)


@pytest.mark.parametrize(
    "patch",
    [
        {},
        {"sources": {}},
        {"sources": {"other": False}},
        {"sources": {"codex": 0}},
        {"idle_minutes": True},
        {"idle_minutes": -1},
        {"idle_minutes": 1441},
        {"idle_minutes": 1.5},
        {"unknown": True},
        {"sources": {"codex": False}, "idle_minutes": -1},
    ],
)
async def test_invalid_preferences_are_rejected_atomically(focus_hub, patch):
    coordinator, _ = focus_hub
    with pytest.raises(LinkError, match="bad_request"):
        await coordinator.set_prefs("orb-first", patch)
    assert coordinator.get_prefs("orb-first") == DEFAULT_PREFS


async def test_preferences_persist_partial_patches_and_return_defensive_values(short_tmp):
    coordinator = SessionHub(short_tmp)
    assert coordinator.get_prefs("orb-first") == DEFAULT_PREFS
    prefs = await coordinator.set_prefs("orb-first", {"sources": {"codex": False}, "idle_minutes": 0})
    assert prefs == {**DEFAULT_PREFS, "sources": {"claude": True, "codex": False, "dot": True}, "idle_minutes": 0}
    prefs["sources"]["claude"] = False
    assert coordinator.get_prefs("orb-first")["sources"]["claude"] is True
    await coordinator.set_prefs("orb-first", {"idle_minutes": 1440})
    await coordinator.close()
    coordinator = SessionHub(short_tmp)
    try:
        assert coordinator.get_prefs("orb-first") == {
            **DEFAULT_PREFS,
            "sources": {"claude": True, "codex": False, "dot": True},
            "idle_minutes": 1440,
            "units": {"temperature": "C", "source": "auto"},
        }
        assert coordinator.get_prefs("orb-second") == DEFAULT_PREFS
    finally:
        await coordinator.close()


@pytest.mark.parametrize(
    "saved_units,expected_source",
    [
        ({"temperature": "auto"}, "auto"),
        ({"temperature": "C", "source": "auto"}, "auto"),
        ({"temperature": "F", "source": "auto"}, "auto"),
        ({"temperature": "F"}, "owner"),
        ({"temperature": "F", "source": "owner"}, "owner"),
    ],
)
async def test_both_saved_temperature_layouts_survive_partial_patch_and_restart(
    short_tmp, saved_units, expected_source
):
    coordinator = SessionHub(short_tmp, temperature="F")
    saved = {**DEFAULT_PREFS, "units": saved_units}
    with coordinator.db:
        coordinator.db.execute("INSERT INTO device_prefs VALUES (?,?)", ("orb-first", json.dumps(saved)))
    try:
        assert coordinator.get_prefs("orb-first")["units"] == {"temperature": "F", "source": expected_source}
        await coordinator.set_prefs("orb-first", {"idle_minutes": 5})
    finally:
        await coordinator.close()
    coordinator = SessionHub(short_tmp, temperature="C")
    try:
        assert coordinator.get_prefs("orb-first")["idle_minutes"] == 5
        assert coordinator.get_prefs("orb-first")["units"] == {
            "temperature": "C" if expected_source == "auto" else "F",
            "source": expected_source,
        }
    finally:
        await coordinator.close()


async def test_source_disable_cancels_presentation_without_answering_other_devices(focus_hub):
    coordinator, _ = focus_hub
    first, second = CaptureLink(), CaptureLink("orb-second")
    for link in [first, second]:
        await coordinator.subscribe(link)
        await flush(coordinator, link)
    calls = []

    async def answer(value):
        calls.append(value)
        return True

    await coordinator.show_card(card(kind="approval"), source="codex", answer_handler=answer)
    assert ("card.show", coordinator.cards["c"]) in await flush(coordinator, first)
    await flush(coordinator, second)
    await coordinator.set_prefs(first.device_id, {"sources": {"codex": False}})
    assert ("card.cancel", {"card": "c"}) in await flush(coordinator, first)
    assert not any(name == "card.cancel" for name, _ in await flush(coordinator, second))
    assert not calls and "c" in coordinator.cards
    with pytest.raises(LinkError, match="unsupported"):
        await coordinator.answer(first.device_id, {"card": "c", "option": "yes"})
    result = await coordinator.answer(second.device_id, {"card": "c", "option": "yes"})
    assert result["delivered"] and len(calls) == 1
    assert await coordinator.answer(first.device_id, {"card": "c", "option": "no"}) == result


async def test_disabled_source_is_suppressed_on_reconnect_and_reenabled_cards_replay(focus_hub):
    coordinator, _ = focus_hub
    first = CaptureLink()
    await coordinator.set_prefs(first.device_id, {"sources": {"dot": False}})
    await coordinator.show_card(card(), source="dot", device_id=first.device_id)
    await coordinator.subscribe(first)
    assert not any(name == "card.show" for name, _ in await flush(coordinator, first))
    await coordinator.set_prefs(first.device_id, {"sources": {"dot": True}})
    events = await flush(coordinator, first)
    assert [params["id"] for name, params in events if name == "card.show"] == ["c"]


async def test_unbound_card_source_ownership_survives_restart(short_tmp):
    coordinator = SessionHub(short_tmp)
    await coordinator.set_prefs("orb-first", {"sources": {"codex": False}})
    await coordinator.show_card(card(), source="codex", device_id="orb-first")
    await coordinator.close()
    coordinator = SessionHub(short_tmp)
    link = CaptureLink()
    try:
        await coordinator.subscribe(link)
        assert not any(name == "card.show" for name, _ in await flush(coordinator, link))
        await coordinator.set_prefs(link.device_id, {"sources": {"codex": True}})
        events = await flush(coordinator, link)
        assert [params["id"] for name, params in events if name == "card.show"] == ["c"]
    finally:
        await coordinator.close()


async def test_disabled_source_rejects_messages_and_new_recordings_before_provider(focus_hub):
    coordinator, _ = focus_hub
    await coordinator.upsert_session(session(can_message=True))
    calls = []

    async def provider(*args, **kwargs):
        calls.append((args, kwargs))

    coordinator.register_message_handler("s", provider)
    coordinator.register_message_handler("dot", provider)
    await coordinator.set_prefs("orb-first", {"sources": {"codex": False, "dot": False}})
    hub = SimpleNamespace(sessions=coordinator, voice=SimpleNamespace(begin=provider))
    link = Link(hub, VoiceSocket(), "test-only", "127.0.0.1")
    link.state, link.device_id, link.caps = "online", "orb-first", {"cards", "voice"}
    for target in ["s", "dot"]:
        with pytest.raises(LinkError, match="unsupported"):
            await coordinator.message(link.device_id, {"target": target, "text": "Try this"})
        with pytest.raises(LinkError, match="unsupported"):
            await link._extension_request("voice.begin", {"stream": 1, "target": target})
    assert calls == []


async def test_time_transcription_is_local_and_independent_of_agent_sources(focus_hub):
    coordinator, _ = focus_hub
    await coordinator.set_prefs("orb-first", {"sources": {"codex": False, "dot": False}})
    calls = []

    async def begin(*args):
        calls.append(args)
        return {}

    hub = SimpleNamespace(sessions=coordinator, voice=SimpleNamespace(begin=begin))
    link = Link(hub, VoiceSocket(), "test-only", "127.0.0.1")
    link.state, link.device_id, link.caps = "online", "orb-first", {"cards", "voice"}
    assert await link._extension_request("voice.begin", {"stream": 1, "target": "time"}) == {}
    assert len(calls) == 1 and calls[0][1] == {"stream": 1, "target": "time"}
    with pytest.raises(LinkError, match="bad_request"):
        await link._extension_request("message.send", {"stream": 1, "target": "time", "text": "private reminder"})
    assert len(calls) == 1


async def test_tls_hello_defaults_prefs_requests_and_reconnect(hub):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        hello = await device.hello(caps=["cards"])
        assert hello["r"]["prefs"] == DEFAULT_PREFS
        response = await device.request("prefs.set", {"sources": {"codex": False}, "idle_minutes": 0})
        assert response["ok"]
        prefs = response["r"]
        assert prefs == {**DEFAULT_PREFS, "sources": {"claude": True, "codex": False, "dot": True}, "idle_minutes": 0}
        assert (await device.request("sessions.list", {"include": "idle", "limit": 8}))["r"] == {
            "sessions": [],
            "cursor": None,
        }
        await device.disconnect()
        await device.connect()
        assert (await device.hello(caps=["cards"]))["r"]["prefs"] == prefs
    finally:
        await device.aclose()


async def test_source_disable_suppresses_card_already_waiting_for_send_lock(short_tmp):
    coordinator = SessionHub(short_tmp)
    hub = SimpleNamespace(sessions=coordinator, cfg=SimpleNamespace(request_timeout=2))
    link = Link(hub, VoiceSocket(), "test-only", "127.0.0.1")
    link.state, link.device_id, link.caps = "online", "orb-first", {"cards"}
    try:
        await coordinator.subscribe(link)
        await coordinator._subscribers[link].queue.join()
        while not link.ws.messages.empty():
            await link.ws.receive()
        await link._send_lock.acquire()
        try:
            await coordinator.show_card(card(), source="codex")
            await asyncio.sleep(0)
            assert coordinator._subscribers[link].queue.empty()
            await coordinator.set_prefs(link.device_id, {"sources": {"codex": False}})
        finally:
            link._send_lock.release()
        await asyncio.wait_for(coordinator._subscribers[link].queue.join(), 1)
        messages = []
        while not link.ws.messages.empty():
            messages.append(await link.ws.receive())
        assert not any(message["m"] == "card.show" for message in messages)
        assert any(message["m"] == "card.cancel" for message in messages)
    finally:
        await coordinator.close()


@pytest.fixture
async def page_link(short_tmp):
    coordinator = SessionHub(short_tmp)
    hub = SimpleNamespace(sessions=coordinator, cfg=SimpleNamespace(request_timeout=2))
    link = Link(hub, VoiceSocket(), "test-only", "127.0.0.1")
    link.state, link.device_id, link.caps = "online", "orb-first", {"cards"}
    for i in range(4):
        await coordinator.upsert_session(session(str(i), updated=i))
    yield link
    await coordinator.close()


async def test_idle_page_waiting_for_send_lock_uses_current_source_preferences(page_link):
    link = page_link
    await link._send_lock.acquire()
    request = envelope.Envelope("req", id=17, m="sessions.list", p={"include": "idle"})
    pending = asyncio.create_task(link._on_request(request))
    try:
        await asyncio.sleep(0)
        assert not pending.done()
        await link.hub.sessions.set_prefs(link.device_id, {"sources": {"codex": False}})
    finally:
        link._send_lock.release()
    await asyncio.wait_for(pending, 1)
    response = await link.ws.receive()
    assert response == envelope.success(17, {"sessions": [], "cursor": None})


async def test_idle_cursor_waiting_for_send_lock_conflicts_after_preferences_change(page_link):
    link = page_link
    cursor = link.hub.sessions.list_sessions(link.device_id, {"include": "idle", "limit": 1})["cursor"]
    await link._send_lock.acquire()
    request = envelope.Envelope("req", id=18, m="sessions.list", p={"include": "idle", "cursor": cursor})
    pending = asyncio.create_task(link._on_request(request))
    try:
        await asyncio.sleep(0)
        assert not pending.done()
        await link.hub.sessions.set_prefs(link.device_id, {"sources": {"codex": False}})
    finally:
        link._send_lock.release()
    await asyncio.wait_for(pending, 1)
    response = await link.ws.receive()
    assert response["id"] == 18 and response["ok"] is False
    assert response["e"]["code"] == "conflict"


async def test_idle_page_with_large_valid_request_id_keeps_error_within_wire_budget(page_link):
    link = page_link
    request_id = 10**3999
    params = {"include": "idle"}
    assert len(envelope.encode(envelope.request(request_id, "sessions.list", params)).encode()) <= 4096
    await link._on_request(envelope.Envelope("req", id=request_id, m="sessions.list", p=params))
    response = await link.ws.receive()
    assert response["id"] == request_id and response["ok"] is False
    assert response["e"]["code"] == "too_large"
    assert len(envelope.encode(response).encode()) <= 4096
