"""Real pinned-TLS v1.1 round trips and coordinator failure boundaries."""

import asyncio
import json
import struct

import pytest
from conftest import voice_pcm
from fake_device import paired_device

from orb_hub.envelope import LinkError
from orb_hub.sessions import MAX_DELIVERY_QUEUE, SessionHub
from orb_hub.voice import VoiceService


def session(key="s", **changes):
    return {
        "id": key,
        "agent": "codex",
        "title": "Orb",
        "project": "hub",
        "state": "idle",
        "reason": "",
        "summary": "Ready",
        "updated": 1,
        "can_message": False,
        **changes,
    }


def card(key="c", **changes):
    return {
        "id": key,
        "kind": "choice",
        "title": "Choose",
        "body": "Next check?",
        "options": [
            {"id": "yes", "label": "Continue", "role": "primary"},
            {"id": "no", "label": "Cancel", "role": "cancel"},
        ],
        "reply": False,
        "priority": "normal",
        **changes,
    }


async def capable(hub, **kwargs):
    device = await paired_device(hub, **kwargs)
    await device.disconnect()
    await device.connect()
    result = await device.hello(caps=["cards", "voice", "future-cap"])
    assert result["ok"] and "cards" in result["r"]["caps"]
    method, value = await device.event()
    assert method == "sessions.snapshot"
    while value["more"]:
        method, value = await device.event()
        assert method == "sessions.snapshot"
    return device


async def next_event(device, name):
    async with asyncio.timeout(3):
        while True:
            method, params = await device.event()
            if method == name:
                return params


async def test_tls_first_answer_wins_across_devices_and_reconnect(hub):
    first = await capable(hub)
    second = await capable(hub, device_id="orb-001122334455")
    calls = []

    async def deliver(value):
        calls.append(value)
        await asyncio.sleep(0.02)

    try:
        await hub.sessions.show_card(card(), answer_handler=deliver)
        assert (await next_event(first, "card.show"))["id"] == "c"
        assert (await next_event(second, "card.show"))["id"] == "c"
        responses = await asyncio.gather(
            first.request("card.answer", {"card": "c", "option": "yes"}),
            second.request("card.answer", {"card": "c", "option": "no"}),
        )
        assert all(r["ok"] and r["r"]["delivered"] for r in responses)
        assert responses[0]["r"] == responses[1]["r"]
        assert len(calls) == 1
        await first.disconnect()
        await first.connect()
        await first.hello(caps=["cards"])
        assert (await first.request("card.answer", {"card": "c", "text": "different"}))["r"] == responses[0]["r"]
        assert len(calls) == 1
    finally:
        await first.aclose()
        await second.aclose()


async def test_targeted_cards_and_text_answers(hub):
    first = await capable(hub)
    second = await capable(hub, device_id="orb-001122334455")
    try:
        await hub.sessions.show_card(card(reply=True), device_id=first.device_id)
        await next_event(first, "card.show")
        result = await second.request("card.answer", {"card": "c", "option": "yes"})
        assert result["e"]["code"] == "unauthorized"
        result = await first.request("card.answer", {"card": "c", "option": "missing"})
        assert result["e"]["code"] == "bad_request"
        result = await first.request("card.answer", {"card": "c", "text": "A reply"})
        assert result["r"]["answer"] == {"text": "A reply"}
        # Missing transport handlers after a restart never claim delivery.
        assert result["r"]["delivered"] is False
    finally:
        await first.aclose()
        await second.aclose()


async def test_v1_no_new_events_or_methods(hub):
    device = await paired_device(hub)
    try:
        await hub.sessions.upsert_session(session())
        await hub.sessions.show_card(card())
        await asyncio.sleep(0.08)
        assert device.events.empty()
        assert (await device.request("card.answer", {"card": "c", "option": "yes"}))["e"]["code"] == "unsupported"
        await device.ws.send_bytes(b"reserved in v1")
        assert (await device.request("ota.check"))["ok"]
    finally:
        await device.aclose()


async def test_snapshots_chunked_and_resubscribe_order(hub):
    for i in range(24):
        await hub.sessions.upsert_session(
            session(str(i), state="busy", title="\x01" * 64, project="x" * 32, reason="x" * 64, summary="x" * 160)
        )
    device = await capable(hub)
    try:
        assert (await device.request("sessions.subscribe"))["ok"]
        parts = []
        while True:
            value = await next_event(device, "sessions.snapshot")
            if value["reset"]:
                parts.clear()
            parts.append(value)
            assert (
                len(json.dumps({"t": "evt", "m": "sessions.snapshot", "p": value}, separators=(",", ":")).encode())
                <= 4096
            )
            if not value["more"]:
                break
        assert sum(len(part["sessions"]) for part in parts) == 8
        assert len(parts) > 1 and parts[0]["reset"]
    finally:
        await device.aclose()


async def test_coalesces_latest_and_never_exceeds_four_updates_per_second(hub):
    await hub.sessions.upsert_session(session(state="busy"))
    device = await capable(hub)
    try:
        await next_event(device, "sessions.summary")
        for i in range(100):
            await hub.sessions.upsert_session(session(state="busy", summary=f"step {i}"))
        assert (await next_event(device, "session.update"))["session"]["summary"] == "step 99"
        assert device.events.empty()
    finally:
        await device.aclose()


async def test_card_answer_is_durable_and_stale_approvals_cancelled(short_tmp):
    coordinator = SessionHub(short_tmp)
    calls = []

    async def delivered(answer):
        # Verify committed state is visible to a second connection inside callback.
        calls.append(coordinator.db.execute("SELECT answer FROM session_cards WHERE id='c'").fetchone()[0])

    await coordinator.show_card(card(), answer_handler=delivered)
    original = await coordinator.answer("orb-1", {"card": "c", "option": "yes"})
    await coordinator.show_card(card("p", kind="approval"))
    await coordinator.show_card(card("question:stale"))
    await coordinator.close()
    coordinator = SessionHub(short_tmp)
    try:
        with pytest.raises(LinkError, match="bad_request"):
            await coordinator.upsert_session(session(updated=10**1000))
        assert await coordinator.answer("orb-1", {"card": "c", "option": "no"}) == original
        assert calls == ['{"option":"yes"}']
        assert "p" not in coordinator.cards and "question:stale" not in coordinator.cards
    finally:
        await coordinator.close()


async def test_provider_failure_persists_without_retry(short_tmp):
    coordinator = SessionHub(short_tmp)
    calls = []

    async def failed(answer):
        calls.append(answer)
        raise OSError("private provider payload")

    try:
        await coordinator.show_card(card(), answer_handler=failed)
        result = await coordinator.answer("orb-1", {"card": "c", "option": "yes"})
        assert result["delivered"] is False
        assert await coordinator.answer("orb-1", {"card": "c", "option": "no"}) == result
        assert len(calls) == 1
    finally:
        await coordinator.close()


async def test_limits_utf8_and_encoded_budget_before_mutation(short_tmp):
    coordinator = SessionHub(short_tmp)
    try:
        with pytest.raises(LinkError, match="bad_request"):
            await coordinator.upsert_session(session(title="💡" * 17))
        with pytest.raises(LinkError, match="too_large"):
            await coordinator.show_card(
                card(
                    body="\x01" * 280,
                    detail="\x01" * 200,
                    title="\x01" * 64,
                    options=[{"id": "\x01" * 31 + str(i), "label": "\x01" * 20, "role": "default"} for i in range(4)],
                )
            )
        assert not coordinator.cards
        for i in range(24):
            await coordinator.upsert_session(session(str(i)))
        with pytest.raises(LinkError, match="busy"):
            await coordinator.upsert_session(session("25"))
        for i in range(16):
            await coordinator.show_card(card(str(i)))
        with pytest.raises(LinkError, match="busy"):
            await coordinator.show_card(card("17"))
        with pytest.raises(LinkError, match="conflict"):
            await coordinator.show_card(card("0", body="changed"))
        await coordinator.cancel_card("0")
        with pytest.raises(LinkError, match="expired"):
            await coordinator.answer("orb", {"card": "0", "option": "yes"})
        await coordinator.show_card(card("17"))
    finally:
        await coordinator.close()


async def test_slow_consumer_disconnects_bounded_queue(short_tmp):
    class SlowLink:
        device_id = "orb"

        def __init__(self):
            self.closed = asyncio.Event()

        async def notify(self, method, params):
            await self.closed.wait()

        async def close(self, code, reason):
            self.closed.set()

    coordinator = SessionHub(short_tmp, send_timeout=0.05)
    link = SlowLink()
    try:
        await coordinator.subscribe(link)
        for _ in range(MAX_DELIVERY_QUEUE + 2):
            await coordinator.emit(link, "session.remove", {"id": "s"})
        assert coordinator._subscribers[link].queue.qsize() <= MAX_DELIVERY_QUEUE
        await asyncio.wait_for(link.closed.wait(), 1)
    finally:
        await coordinator.close()


async def test_fake_voice_tls_confirmation_and_capabilities(hub):
    await hub.voice.close()
    hub.voice = VoiceService(lambda pcm: "Run the tests")
    await hub.sessions.upsert_session(session(can_message=True))
    calls = []

    async def send(device_id, text, stream=None):
        calls.append((text, stream))

    hub.sessions.register_message_handler("s", send)
    device = await capable(hub)
    try:
        assert "voice" in hub.online[device.device_id].caps
        assert (await device.request("voice.begin", {"stream": 1, "target": "s"}))["ok"]
        pcm = voice_pcm()
        for sequence, offset in enumerate(range(0, len(pcm), 640)):
            await device.ws.send_bytes(struct.pack(">BHI", 1, 1, sequence) + pcm[offset : offset + 640])
        assert (await device.request("voice.end", {"stream": 1}))["ok"]
        result = await next_event(device, "voice.result")
        assert result == {"stream": 1, "text": "Run the tests"} and not calls
        bad = await device.request("message.send", {"target": "s", "stream": 1, "text": "Changed"})
        assert bad["e"]["code"] == "conflict" and not calls
        assert (await device.request("message.send", {"target": "s", **result}))["ok"]
        assert calls == [("Run the tests", 1)]
        assert (await device.request("message.send", {"target": "s", **result}))["e"]["code"] == "not_found"
        assert (await device.request("voice.begin", {"stream": 1, "target": "s"}))["e"]["code"] == "conflict"
    finally:
        await device.aclose()


async def test_confirmed_voice_queue_failure_keeps_recording_then_returns_queued(hub, short_tmp):
    from orb_agents import AgentError, DeliveryResult, Session, StateStore

    from orb_hub.agents import AgentsBridge

    class FakeQueueManager:
        def __init__(self):
            self.store = StateStore()
            self.calls = []
            self.fail = True

        def can_message(self, _identity):
            return True

        async def send(self, identity, text):
            self.calls.append((identity, text))
            if self.fail:
                raise AgentError("delivery_failed", "synthetic private diagnostic; never forward")
            return DeliveryResult("queued", "Codex queue accepted the existing-session message")

    manager = FakeQueueManager()
    identity = "codex:synthetic-queue"
    manager.store.update(Session(identity, "codex", "desktop app", "Synthetic", str(short_tmp)))
    bridge = AgentsBridge(hub.sessions, short_tmp, manager=manager)
    await hub.voice.close()
    hub.voice = VoiceService(lambda _pcm: "Synthetic voice request")
    await hub.sessions.upsert_session(session(identity, can_message=True))
    hub.sessions.register_message_handler(identity, bridge._message_handler(identity))
    device = await capable(hub)
    try:
        assert (await device.request("voice.begin", {"stream": 7, "target": identity}))["ok"]
        pcm = voice_pcm()
        for sequence, offset in enumerate(range(0, len(pcm), 640)):
            await device.ws.send_bytes(struct.pack(">BHI", 1, 7, sequence) + pcm[offset : offset + 640])
        assert (await device.request("voice.end", {"stream": 7}))["ok"]
        transcript = await next_event(device, "voice.result")
        assert not manager.calls
        request = {"target": identity, **transcript}
        failed = await device.request("message.send", request)
        assert failed["ok"] is False and failed["e"]["status"] == "failed"
        assert failed["e"]["msg"] and "private diagnostic" not in failed["e"]["msg"]
        # Failed transport admission preserves the exact confirmed recording.
        owner = hub.online[device.device_id]
        assert hub.voice.confirmed(owner, 7, identity, transcript["text"]) > 0
        manager.fail = False
        queued = await device.request("message.send", request)
        assert queued["ok"] is True
        assert queued["r"] == {
            "accepted": True,
            "status": "queued",
            "reason": "Codex queue accepted the existing-session message",
        }
        assert manager.calls == [(identity, "Synthetic voice request")] * 2
        consumed = await device.request("message.send", request)
        assert consumed["e"]["code"] == "not_found" and consumed["e"]["status"] == "failed"
        assert len(manager.calls) == 2
    finally:
        await device.aclose()
        await manager.store.close()


async def test_demo_and_cli_status_real_hub(hub):
    device = await capable(hub)
    try:
        result = await hub.handle_command("demo-cards", {"device_id": device.device_id})
        assert len(result["cards"]) == 4 and len(result["sessions"]) == 2
        second = await hub.handle_command("demo-cards", {"device_id": device.device_id})
        assert len(hub.sessions.cards) == 4
        assert set(second["cards"]).isdisjoint(result["cards"])
        assert (await hub.handle_command("agents.status", {}))["enabled"] is False
        assert (await hub.handle_command("dot.status", {}))["enabled"] is False
    finally:
        await device.aclose()


async def test_close_interrupts_request_blocked_in_provider(make_hub):
    hub = await make_hub(request_timeout=0.05)
    await hub.sessions.upsert_session(session(can_message=True))
    started, cancelled = asyncio.Event(), asyncio.Event()

    async def send(device_id, text, stream=None):
        started.set()
        try:
            await asyncio.Event().wait()
        finally:
            cancelled.set()

    hub.sessions.register_message_handler("s", send)
    device = await capable(hub)
    try:
        await device.send({"t": "req", "id": 100, "m": "message.send", "p": {"target": "s", "text": "Hello"}})
        await asyncio.wait_for(started.wait(), 1)
        await asyncio.wait_for(hub.online[device.device_id].close(1001, "test close"), 1)
        assert cancelled.is_set()
        assert device.device_id not in hub.online
    finally:
        await device.aclose()


async def test_capability_negotiation_ignores_unknown_and_requires_cards_for_voice(hub):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        result = await device.hello(caps=["voice", "future"])
        assert result["r"]["caps"] == []
        assert (await device.request("voice.begin", {"stream": 1, "target": "dot"}))["e"]["code"] == "unsupported"
        await device.disconnect()
        await device.connect()
        result = await device.hello(caps="cards")
        assert result["e"]["code"] == "bad_request"
    finally:
        await device.aclose()


class VoiceSocket:
    """Capture writes after Link's real send lock, without opening a listener."""

    def __init__(self):
        self.messages = asyncio.Queue()

    async def send_str(self, text):
        self.messages.put_nowait(json.loads(text))

    async def receive(self):
        async with asyncio.timeout(1):
            return await self.messages.get()


@pytest.fixture
async def voice_link(short_tmp):
    from types import SimpleNamespace

    from orb_hub.link import Link

    async def transcribe(_pcm):
        return "Run the tests"

    coordinator = SessionHub(short_tmp)
    voice = VoiceService(transcribe)
    hub = SimpleNamespace(sessions=coordinator, voice=voice, cfg=SimpleNamespace(request_timeout=2))
    link = Link(hub, VoiceSocket(), "test-only", "127.0.0.1")
    link.state, link.device_id, link.caps = "online", "orb-test", {"cards", "voice"}
    await coordinator.subscribe(link)
    assert (await link.ws.receive())["m"] == "sessions.snapshot"
    assert (await link.ws.receive())["m"] == "sessions.summary"
    yield link
    await voice.close()
    await coordinator.close()


async def queued_transcript(link, stream=1):
    await link.hub.voice.begin(link, {"stream": stream, "target": "s"}, link._voice_event)
    pcm = voice_pcm()
    for sequence, offset in enumerate(range(0, len(pcm), 640)):
        await link.hub.voice.frame(link, struct.pack(">BHI", 1, stream, sequence) + pcm[offset : offset + 640])
    await link.hub.voice.end(link, {"stream": stream})
    await asyncio.gather(*link.hub.voice._jobs)


@pytest.mark.parametrize("method", ["voice.result", "voice.error"])
@pytest.mark.parametrize("stage", ["queued", "send_lock"])
async def test_resubscribe_preserves_pending_voice_events(voice_link, monkeypatch, method, stage):
    link = voice_link
    coordinator = link.hub.sessions
    started = asyncio.Event()
    original_notify = link.notify

    async def notify(name, params):
        if name == ("session.remove" if stage == "queued" else method):
            started.set()
        await original_notify(name, params)

    monkeypatch.setattr(link, "notify", notify)
    await link._send_lock.acquire()
    try:
        if stage == "queued":
            await coordinator.emit(link, "session.remove", {"id": "blocking"})
            await asyncio.wait_for(started.wait(), 1)
        if method == "voice.error":

            async def fail(_pcm):
                raise RuntimeError("test failure")

            link.hub.voice.transcriber = fail
        await queued_transcript(link)
        if stage == "send_lock":
            await asyncio.wait_for(started.wait(), 1)
        await coordinator.subscribe(link)
        await coordinator.emit(link, "session.remove", {"id": "barrier"})
    finally:
        link._send_lock.release()
    received = []
    while True:
        message = await link.ws.receive()
        received.append(message)
        if message["m"] == "session.remove" and message["p"]["id"] == "barrier":
            break
    assert [event["m"] for event in received].count(method) == 1
    assert [event["m"] for event in received].count("sessions.snapshot") == 1
    if method == "voice.result":
        assert await link.hub.voice.end(link, {"stream": 1}) == {}
        link.hub.voice.consume(link, 1)
    await link.hub.voice.begin(link, {"stream": 2, "target": "s"}, link._voice_event)


@pytest.mark.parametrize("stage", ["queued", "send_lock"])
async def test_cancel_invalidates_pending_voice_result_before_transport_write(voice_link, monkeypatch, stage):
    link = voice_link
    coordinator = link.hub.sessions
    started = asyncio.Event()
    original_notify = link.notify

    async def notify(name, params):
        if name == ("session.remove" if stage == "queued" else "voice.result"):
            started.set()
        await original_notify(name, params)

    monkeypatch.setattr(link, "notify", notify)
    await link._send_lock.acquire()
    try:
        if stage == "queued":
            await coordinator.emit(link, "session.remove", {"id": "blocking"})
            await asyncio.wait_for(started.wait(), 1)
        await queued_transcript(link)
        if stage == "send_lock":
            await asyncio.wait_for(started.wait(), 1)
        assert await link._extension_request("voice.cancel", {"stream": 1}) == {}
        assert coordinator._subscribers[link].queue.empty()
        # A replacement stream and unrelated state must survive old-stream cancellation.
        await queued_transcript(link, stream=2)
        await coordinator.emit(link, "session.remove", {"id": "barrier"})
    finally:
        link._send_lock.release()
    received = []
    while True:
        message = await link.ws.receive()
        received.append(message)
        if message["m"] == "session.remove" and message["p"]["id"] == "barrier":
            break
    assert [event["p"]["stream"] for event in received if event["m"] == "voice.result"] == [2]
    if stage == "queued":
        assert received[0] == {"t": "evt", "m": "session.remove", "p": {"id": "blocking"}}
