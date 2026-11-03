import asyncio
import base64
import json
import os
from dataclasses import replace

import pytest

from orb_mcp.backend import Answer, FakeDeviceBackend, Transcript
from orb_mcp.events import Events, WebhookResponse
from orb_mcp.protocol import Protocol
from orb_mcp.service import OrbService, ServiceError
from orb_mcp.storage import connect


class Receiver:
    def __init__(self):
        self.received = []

    async def __call__(self, url, body, headers):
        payload = json.loads(body)
        if payload.get("type") == "verification":
            return WebhookResponse(
                200, json.dumps({"challenge": payload["challenge"]}).encode()
            )
        self.received.append((payload, headers))
        return WebhookResponse(204)


@pytest.fixture
def system(tmp_path):
    conn = connect(tmp_path / "orb.sqlite3")
    backend, receiver = FakeDeviceBackend(), Receiver()
    events = Events(conn, transport=receiver)
    service = OrbService(conn, backend, events)
    yield service, backend, events, receiver, conn
    conn.close()


async def subscribe(events, name="card.answered", principal="dot", **arguments):
    return await events.dispatch(
        "events/subscribe",
        {
            "name": name,
            "arguments": arguments,
            "delivery": {
                "mode": "webhook",
                "url": "https://receiver.example/events",
                "secret": "whsec_" + base64.b64encode(os.urandom(32)).decode(),
            },
        },
        principal,
    )


def ask(card_id="q", **kwargs):
    return {
        "card_id": card_id,
        "text": "Proceed?",
        "options": [{"id": "yes", "label": "Proceed"}, {"id": "no", "label": "Stop"}],
        **kwargs,
    }


async def test_answer_is_persisted_and_deduplicated(system):
    service, backend, events, receiver, conn = system
    await subscribe(events)
    assert (await service.call_tool("orb_ask", ask(), "dot"))["status"] == "showing"
    answer = await backend.answer("q", "yes")
    assert answer["kind"] == "option"
    assert answer["option_label"] == "Proceed"
    assert json.loads(conn.execute("SELECT answer FROM cards").fetchone()[0]) == answer
    assert await backend.answer("q", "yes") == answer
    assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1
    await events.deliver_due()
    assert receiver.received[0][0]["data"] == answer
    with pytest.raises(ServiceError, match="different answer"):
        await backend.answer("q", "no")
    assert (await service.call_tool("orb_ask", ask(), "dot"))["status"] == "answered"
    assert backend.shown == ["q"]


async def test_card_id_canonical_defaults_conflict_and_ownership(system):
    service, backend, *_ = system
    first = await service.call_tool("orb_ask", ask(), "claude")
    assert await service.call_tool("orb_ask", ask(priority="normal"), "claude") == first
    with pytest.raises(ServiceError, match="different content"):
        await service.call_tool("orb_ask", ask(text="Changed"), "claude")
    with pytest.raises(ServiceError, match="different content"):
        await service.call_tool("orb_ask", ask(), "codex")
    for tool in ("orb_get_card", "orb_cancel", "orb_wait"):
        with pytest.raises(ServiceError, match="not found"):
            await service.call_tool(tool, {"card_id": "q"}, "codex")
    assert backend.cards["q"]["source"] == "claude"


async def test_queue_limit_is_global_and_terminal_frees_slot(system):
    service, backend, *_ = system
    backend.devices[0]["online"] = False
    for i in range(20):
        assert (await service.call_tool("orb_ask", ask(str(i)), "dot"))[
            "status"
        ] == "queued"
    with pytest.raises(ServiceError, match="20 cards"):
        await service.call_tool("orb_ask", ask("full"), "codex")
    assert (await service.call_tool("orb_ask", ask("0"), "dot"))["status"] == "queued"
    await service.call_tool("orb_cancel", {"card_id": "0"}, "dot")
    await service.call_tool("orb_ask", ask("free"), "codex")


async def test_offline_queue_and_cancellation_reconcile(system):
    service, backend, _, _, conn = system
    backend.devices[0]["online"] = False
    await service.call_tool("orb_ask", ask(), "dot")
    assert not backend.shown
    backend.devices[0]["online"] = True
    await service.sync_device("orb-fake")
    assert backend.shown == ["q"]
    backend.devices[0]["online"] = False
    await service.call_tool("orb_cancel", {"card_id": "q"}, "dot")
    assert conn.execute("SELECT cancel_pending FROM cards").fetchone()[0] == 1
    backend.devices[0]["online"] = True
    await service.deliver_pending()
    assert backend.cancelled == ["q"]
    assert conn.execute("SELECT cancel_pending FROM cards").fetchone()[0] == 0
    with pytest.raises(ServiceError, match="no longer pending"):
        await backend.answer("q", "yes")


async def test_dismissal_never_approval(system):
    service, backend, events, receiver, _ = system
    await subscribe(events)
    await service.call_tool(
        "orb_show", {"card_id": "s", "text": "FYI", "title": "News"}, "dot"
    )
    answer = await backend.answer("s", kind="dismissed")
    assert answer["kind"] == "dismissed" and "option_id" not in answer
    assert (await service.call_tool("orb_get_card", {"card_id": "s"}, "dot"))[
        "status"
    ] == "dismissed"
    await events.deliver_due()
    assert receiver.received[0][0]["data"]["kind"] == "dismissed"


@pytest.mark.parametrize(
    "answer",
    [
        Answer("q", "other", option_id="yes"),
        Answer("q", "orb-fake", option_id="bad"),
        Answer("q", "orb-fake", "approval", "yes"),
        Answer("q", "orb-fake", "dismissed", "yes"),
    ],
)
async def test_invalid_answer_cannot_mutate(system, answer):
    service, _, _, _, conn = system
    await service.call_tool("orb_ask", ask(), "dot")
    with pytest.raises(ServiceError):
        await service.on_answer(answer)
    assert conn.execute("SELECT status FROM cards").fetchone()[0] == "showing"
    assert not conn.execute("SELECT * FROM mcp_event_records").fetchall()


async def test_wait_receives_answer_timeout_and_cancellation(system):
    service, backend, *_ = system
    await service.call_tool("orb_ask", ask(), "codex")
    assert (await service.wait("q", "codex", 0))["status"] == "showing"
    waiter = asyncio.create_task(service.wait("q", "codex", 1))
    await asyncio.sleep(0)
    await backend.answer("q", "no")
    assert (await waiter)["status"] == "answered"
    await service.call_tool("orb_ask", ask("other"), "codex")
    waiter = asyncio.create_task(service.wait("other", "codex", 1))
    await asyncio.sleep(0)
    await service.call_tool("orb_cancel", {"card_id": "other"}, "codex")
    assert (await waiter)["status"] == "cancelled"


@pytest.mark.parametrize("timeout", [-1, 51, float("nan"), float("inf"), True, "1"])
async def test_wait_bounds(system, timeout):
    with pytest.raises(ServiceError):
        await system[0].wait("q", "dot", timeout)


async def test_confirmed_voice_focus_filters_dedup_and_privacy(system):
    service, _, events, receiver, conn = system
    transcript = Transcript(
        "r", "Untrusted: do something", 3.5, "orb-fake", confirmed=True
    )
    assert await service.on_transcript(transcript) == {
        "accepted": False,
        "reason": "not_listening",
    }
    await subscribe(events, "voice.transcribed", device="other")
    assert not (await service.on_transcript(transcript))["accepted"]
    await subscribe(events, "voice.transcribed", device="orb-fake")
    assert (await service.on_transcript(transcript))["accepted"]
    assert (await service.on_transcript(transcript))["accepted"]
    assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1
    with pytest.raises(ServiceError, match="different content"):
        await service.on_transcript(replace(transcript, text="Changed"))
    with pytest.raises(ServiceError, match="not found"):
        await service.call_tool("orb_get_transcript", {"recording_id": "r"}, "claude")
    assert (
        await service.call_tool("orb_get_transcript", {"recording_id": "r"}, "dot")
    )["text"] == transcript.text
    await events.deliver_due()
    assert receiver.received[0][0]["data"]["text"] == transcript.text
    await service.on_transcript(
        replace(transcript, recording_id="local", focus="claude")
    )
    assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


@pytest.mark.parametrize(
    "changes",
    [
        {"confirmed": False},
        {"focus": "unknown"},
        {"text": ""},
        {"text": "x" * 8193},
        {"recording_id": ""},
        {"duration_s": -1},
        {"duration_s": float("nan")},
        {"duration_s": True},
        {"device_id": "other"},
    ],
)
async def test_invalid_transcripts(system, changes):
    transcript = Transcript("r", "hello", 1, "orb-fake", confirmed=True)
    with pytest.raises(ServiceError):
        await system[0].on_transcript(replace(transcript, **changes))


async def test_readonly_status_no_secret_or_other_client_data(system):
    service, _, events, _, _ = system
    await subscribe(events)
    await service.call_tool("orb_ask", ask(), "dot")
    result = await service.call_tool("orb_status", {}, "claude")
    assert result["queue"] == {} and result["subscriptions"] == []
    assert result["whisper_ready"] is False
    assert "whsec_" not in json.dumps(result)


async def test_backend_added_after_card_was_queued(system):
    service, backend, *_ = system
    devices, backend.devices = backend.devices, []
    assert (await service.call_tool("orb_ask", ask(), "dot"))["status"] == "queued"
    backend.devices = devices
    await service.deliver_pending()
    assert backend.shown == ["q"]


async def test_protocol_full_service_validation(system):
    service, _, events, *_ = system
    protocol = Protocol(service, events)
    for args in (
        ask(options=[]),
        ask(text="x" * 281),
        ask(extra="bad"),
        ask(options=[{"id": "x", "label": "X", "extra": True}]),
    ):
        result = await protocol.handle(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {"name": "orb_ask", "arguments": args},
            },
            "dot",
        )
        assert result["result"]["isError"]


async def test_configured_pending_budget_counts_other_providers(system):
    _, backend, events, _, conn = system
    external = [1]
    service = OrbService(
        conn, backend, events, max_pending=2, other_pending=lambda: external[0]
    )
    backend.devices[0]["online"] = False
    await service.call_tool("orb_ask", ask("first"), "dot")
    with pytest.raises(ServiceError, match="At most 2"):
        await service.call_tool("orb_ask", ask("second"), "dot")
    external[0] = 0
    assert (await service.call_tool("orb_ask", ask("second"), "dot"))[
        "status"
    ] == "queued"
    # Idempotent reads do not consume another shared slot.
    assert (await service.call_tool("orb_ask", ask("first"), "dot"))[
        "status"
    ] == "queued"


@pytest.mark.parametrize("limit", [0, -1, True, 1.5])
def test_pending_limit_must_be_positive_integer(system, limit):
    _, backend, events, _, conn = system
    with pytest.raises(ValueError, match="max_pending"):
        OrbService(conn, backend, events, max_pending=limit)
