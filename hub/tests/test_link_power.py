"""Negotiated radio policy, with fake monotonic time and isolated link state."""

import asyncio
from types import SimpleNamespace

import pytest
from fake_device import paired_device

from orb_hub import envelope
from orb_hub.link_power import PendingState, PowerDelivery
from orb_hub.session_wire import encoded


def test_latest_state_and_home_patch_merge():
    pending = PendingState()
    for n in range(240):
        pending.put("agent.stream", {"session": "s", "seq": n, "activity": str(n)})
    pending.put("home.update", {"id": "lamp", "state": {"on": True, "brightness": 40}})
    pending.put("home.update", {"id": "lamp", "state": {"brightness": 70}, "reachable": False})
    assert pending.count == 2
    events = pending.take_batch()
    assert events == [
        {"m": "agent.stream", "p": {"session": "s", "seq": 239, "activity": "239"}},
        {"m": "home.update", "p": {"id": "lamp", "state": {"on": True, "brightness": 70}, "reachable": False}},
    ]


def test_snapshot_groups_replace_atomically_and_wait_for_completion():
    pending = PendingState()
    pending.put("session.update", {"session": {"id": "old"}})
    pending.put("sessions.snapshot", {"sessions": [{"id": "s"}], "reset": True, "more": True})
    assert pending.take_batch() == []
    pending.put("sessions.snapshot", {"sessions": [], "reset": False, "more": False})
    pending.put("session.update", {"session": {"id": "s", "title": "latest"}})
    pending.put("sessions.snapshot", {"sessions": [{"id": "new"}], "reset": True, "more": False})
    assert pending.take_batch() == [
        {"m": "sessions.snapshot", "p": {"sessions": [{"id": "new"}], "reset": True, "more": False}}
    ]


def test_oversized_groups_wait_for_wake_without_blocking_small_updates():
    pending = PendingState()
    for i in range(2):
        pending.put("home.snapshot", {"reset": i == 0, "more": i == 0, "accessories": ["x" * 2500]})
    pending.put("usage.snapshot", {"windows": [], "unknown": ["claude"]})
    assert pending.take_batch() == [{"m": "usage.snapshot", "p": {"windows": [], "unknown": ["claude"]}}]
    assert pending.count == 2
    assert pending.take_batch() == []
    assert len(pending.drain()) == 2


def test_batch_budget_fairness_and_overflow():
    pending = PendingState()
    for i in range(8):
        pending.put("agent.stream", {"session": str(i), "activity": "x" * 1000})
    first = pending.take_batch()
    assert len(encoded(envelope.event("power.batch", {"events": first})).encode()) <= 4096
    assert [e["p"]["session"] for e in first] == ["0", "1", "2"]
    assert pending.take_batch()[0]["p"]["session"] == "3"
    with pytest.raises(ConnectionError, match="queue full"):
        for i in range(300):
            pending.put("home.update", {"id": str(i), "state": {"on": True}})


class FakeLink:
    def __init__(self):
        self.caps = {"power_hint"}
        self.state = "online"
        self.sent = []
        self.services = SimpleNamespace(cfg=SimpleNamespace(idle_timeout=45))

    async def send(self, value):
        self.sent.append(value)


async def test_background_deadlines_and_immediate_wake_flush():
    now = [0.0]
    link = FakeLink()
    power = PowerDelivery(link, clock=lambda: now[0])
    assert power.configure({"power_hint": "background", "ping_interval_s": 15})
    power.defer("usage.snapshot", {"n": 1})
    await power.flush()
    assert link.sent == []
    now[0] = 60
    await power.flush()
    assert link.sent[0]["m"] == "power.batch"
    for n in range(240):
        power.defer("usage.snapshot", {"n": n})
    now[0] = 119.999
    await power.flush()
    assert len(link.sent) == 1
    assert power.configure({"power_hint": "interactive", "ping_interval_s": 15})
    await power.flush()
    assert link.sent[-1] == envelope.event("usage.snapshot", {"n": 239})
    assert power.pending.count == 0
    # Voice, approvals, OTA, alarms and unknown future events never wait here.
    for method in ("card.show", "card.cancel", "voice.result", "voice.speak", "ota.available", "alarm.ring", "future"):
        assert not power.defer(method, {})


def test_policy_validation_and_legacy_idle_timeout():
    link = FakeLink()
    power = PowerDelivery(link)
    assert power.idle_timeout == 45
    assert power.configure({"power_hint": "background", "ping_interval_s": 45})
    assert power.idle_timeout == 135
    for params in (
        {"power_hint": "interactive", "ping_interval_s": 45},
        {"power_hint": "background", "ping_interval_s": True},
        {"power_hint": "invalid"},
        {"ping_interval_s": 45},
    ):
        assert not power.configure(params)
        assert power.idle_timeout == 135
    link.caps.clear()
    assert power.idle_timeout == 45
    assert power.configure({"power_hint": "invalid", "ping_interval_s": True})


def test_actual_wire_stream_sequences_are_contiguous():
    link = FakeLink()
    power = PowerDelivery(link)
    direct = envelope.event("agent.stream", {"session": "s", "seq": 7})
    first = power.prepare(direct, lambda method, params: True)
    second = power.prepare(
        envelope.event("power.batch", {"events": [{"m": "agent.stream", "p": {"session": "s", "seq": 240}}]}),
        lambda method, params: True,
    )
    assert first["p"]["seq"] == 1
    assert second["p"]["events"][0]["p"]["seq"] == 2
    assert direct["p"]["seq"] == 7
    assert (
        power.prepare(
            envelope.event("power.batch", {"events": [{"m": "weather.state", "p": {}}]}), lambda method, params: False
        )
        is None
    )


async def test_hello_negotiation_legacy_unknown_fields_and_state_transition(hub):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        result = await device.hello(caps=["power_hint", "usage"], power_hint="background", ping_interval_s=15)
        assert result["r"]["caps"] == ["power_hint", "usage"]
        link = hub.online[device.device_id]
        assert link.power.background and link.power.idle_timeout == 45
        await device.send(envelope.event("state.report", {"power_hint": "background", "ping_interval_s": 45}))
        # Request response orders the preceding report without sleeping for time.
        assert (await device.request("ota.check"))["ok"]
        assert link.power.idle_timeout == 135
        await link.notify("usage.snapshot", {"windows": [], "unknown": []})
        assert device.events.empty()
        await device.send(envelope.event("state.report", {"power_hint": "interactive", "ping_interval_s": 15}))
        assert (await device.request("ota.check"))["ok"]
        assert (await device.event())[0] == "usage.snapshot"
        assert link.power.idle_timeout == 45
        await device.disconnect()
        await device.connect()
        result = await device.hello(power_hint="background", ping_interval_s=45, future_field={"ignored": True})
        assert result["ok"] and "caps" not in result["r"]
        assert not hub.online[device.device_id].power.background
        assert hub.online[device.device_id].power.idle_timeout == 45
    finally:
        await device.aclose()


async def test_four_busy_sessions_one_minute_traffic():
    """Synthetic ceiling: four independently changing 4 Hz progress streams."""
    now = [0.0]
    link = FakeLink()
    power = PowerDelivery(link, clock=lambda: now[0])
    assert power.configure({"power_hint": "background", "ping_interval_s": 45})
    raw_events = 0
    for tick in range(240):
        now[0] = tick / 4
        for session in range(4):
            value = {"session": str(session), "seq": tick + 1, "activity": f"step {tick}"}
            assert power.defer("agent.stream", value)
            raw_events += 1
        await power.flush()
    assert raw_events == 960 and link.sent == []
    now[0] = 60
    await power.flush()
    assert len(link.sent) == 1
    events = link.sent[0]["p"]["events"]
    assert len(events) == 4 and all(event["p"]["activity"] == "step 239" for event in events)
    # Unchanged connected sessions produce no heartbeat-like application push.
    now[0] = 120
    await power.flush()
    assert len(link.sent) == 1


def test_snapshot_dependency_and_privacy_recheck_are_atomic():
    pending = PendingState()
    pending.put("sessions.snapshot", {"sessions": [{"id": "s"}], "reset": True, "more": True})
    pending.put("agent.stream", {"session": "s", "seq": 1})
    assert pending.take_batch() == []
    power = PowerDelivery(FakeLink())
    batch = envelope.event(
        "power.batch",
        {
            "events": [
                {"m": "sessions.snapshot", "p": {"sessions": [{"id": "public"}], "reset": True, "more": True}},
                {"m": "sessions.snapshot", "p": {"sessions": [{"id": "private"}], "reset": False, "more": False}},
                {"m": "home.update", "p": {"id": "lamp", "state": {"on": True}}},
            ]
        },
    )
    result = power.prepare(
        batch, lambda method, params: not any(s["id"] == "private" for s in params.get("sessions", []))
    )
    assert result["p"]["events"] == [batch["p"]["events"][2]]


async def test_background_hello_initializes_sessions_before_pending_approval(hub):
    from test_sessions import card, session

    device = await paired_device(hub)
    try:
        await device.disconnect()
        for i in range(8):
            await hub.sessions.upsert_session(
                session(
                    str(i) * 48,
                    state="waiting",
                    title="x" * 64,
                    project="x" * 32,
                    summary="x" * 160,
                    reason="x" * 64,
                    updated=10**12,
                )
            )
        await hub.sessions.show_card(card(kind="approval", session="0" * 48), source="codex")
        await device.connect()
        result = await device.hello(caps=["cards", "power_hint"], power_hint="background", ping_interval_s=15)
        assert result["ok"]
        method, baseline = await device.event()
        assert method == "sessions.snapshot" and baseline["reset"] and baseline["more"]
        total = len(baseline["sessions"])
        while baseline["more"]:
            method, baseline = await device.event()
            assert method == "sessions.snapshot" and not baseline["reset"]
            total += len(baseline["sessions"])
        assert total == 8
        # The C decoder requires a completed baseline before any card event.
        assert hub.online[device.device_id].power.have_sessions
        method, approval = await device.event()
        assert method == "card.show" and approval["kind"] == "approval"
        assert device.events.empty()
        # Later state replacements remain coalesced; attention stays immediate.
        await hub.sessions.subscribe(hub.online[device.device_id])
        assert (await device.event())[1]["id"] == "c"
        await hub.sessions.show_card(card("next", kind="attention"), source="codex")
        assert (await device.event()) == ("card.show", card("next", kind="attention"))
    finally:
        await device.aclose()


async def test_session_page_fallback_stays_bounded_after_power_prepare(hub, monkeypatch):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        assert (await device.hello(caps=["cards", "power_hint"]))["ok"]
        monkeypatch.setattr(hub.sessions, "list_sessions", lambda *args, **kwargs: {"sessions": ["x" * 5000]})
        result = await device.request("sessions.list", {"include": "idle"})
        assert not result["ok"] and result["e"]["code"] == "too_large"
        assert len(encoded(result).encode()) < 4096
    finally:
        await device.aclose()


async def test_deferred_snapshot_replays_live_cards_and_never_cancelled_ones(hub):
    from test_sessions import card, session

    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        assert (await device.hello(caps=["cards", "power_hint"], power_hint="background"))["ok"]
        assert (await device.event())[0] == "sessions.snapshot"
        link = hub.online[device.device_id]
        await hub.sessions.upsert_session(session("s", state="waiting"))
        await hub.sessions.show_card(card("live", kind="approval", session="s"), source="codex")
        await hub.sessions.show_card(card("cancelled", kind="attention"), source="codex")
        assert (await device.event())[1]["id"] == "live"
        assert (await device.event())[1]["id"] == "cancelled"
        await hub.sessions.subscribe(link)
        assert (await device.event())[1]["id"] == "live"
        assert (await device.event())[1]["id"] == "cancelled"
        await hub.sessions.cancel_card("cancelled")
        assert (await device.event()) == ("card.cancel", {"card": "cancelled"})
        # Switch wakes the waiter and flushes the complete deferred snapshot.
        await device.send(envelope.event("state.report", {"power_hint": "interactive", "ping_interval_s": 15}))
        assert (await device.request("ota.check"))["ok"]
        visible = {"live"}
        async with asyncio.timeout(3):
            while True:
                method, params = await device.event()
                if method == "sessions.snapshot" and not params["more"]:
                    visible.clear()  # orb_model_sessions_chunk's commit behavior
                elif method == "card.show":
                    visible.add(params["id"])
                    break
        assert visible == {"live"}
        assert device.events.empty()
    finally:
        await device.aclose()
