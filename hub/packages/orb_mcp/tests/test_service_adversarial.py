"""State, device acknowledgement, and webhook atomicity at failure boundaries."""

import asyncio
import base64
import json
import sqlite3
from contextlib import contextmanager

import pytest

from orb_mcp.backend import FakeDeviceBackend, Transcript
from orb_mcp.events import Events, WebhookResponse
from orb_mcp.service import OrbService, ServiceError


class Sink:
    def __init__(self):
        self.requests = []

    async def __call__(self, url, body, headers):
        self.requests.append((url, body, headers))
        value = json.loads(body)
        return WebhookResponse(
            200, json.dumps({"challenge": value.get("challenge")}).encode()
        )


@contextmanager
def runtime(path=":memory:", backend=None):
    connection = sqlite3.connect(path)
    connection.row_factory = sqlite3.Row
    sink = Sink()
    events = Events(connection, transport=sink)
    backend = backend or FakeDeviceBackend()
    service = OrbService(connection, backend, events)
    try:
        yield service, backend, events, connection, sink
    finally:
        connection.close()


def card(card_id="one"):
    return {
        "card_id": card_id,
        "text": "Proceed?",
        "options": [{"id": "yes", "label": "Proceed"}, {"id": "no", "label": "Stop"}],
    }


async def subscribe(events, principal="dot", name="card.answered", arguments=None):
    return await events.dispatch(
        "events/subscribe",
        {
            "name": name,
            "arguments": arguments or {},
            "delivery": {
                "mode": "webhook",
                "url": "https://receiver.example/" + principal,
                "secret": "whsec_" + base64.b64encode(b"k" * 32).decode(),
            },
        },
        principal,
    )


async def test_answer_acknowledges_only_committed_source_and_outbox():
    with runtime() as (service, backend, events, conn, sink):
        await subscribe(events)
        await service.call_tool("orb_ask", card(), "dot")
        result = await backend.answer("one", "yes")
        assert result["kind"] == "option"
        assert not conn.in_transaction
        assert conn.execute("SELECT status FROM cards").fetchone()[0] == "answered"
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1
        assert len(sink.requests) == 1  # No delivery needed to ACK the device.
        await events.deliver_due()
        assert json.loads(sink.requests[-1][1])["data"] == result


async def test_outbox_failure_rolls_back_card_answer(monkeypatch):
    with runtime() as (service, backend, events, conn, _):
        await subscribe(events)
        await service.call_tool("orb_ask", card(), "dot")
        original = events.enqueue

        def fail_after_write(*args, **kwargs):
            original(*args, **kwargs)
            raise sqlite3.OperationalError("simulated full disk")

        monkeypatch.setattr(events, "enqueue", fail_after_write)
        with pytest.raises(sqlite3.OperationalError):
            await backend.answer("one", "yes")
        row = conn.execute("SELECT status,answer FROM cards").fetchone()
        assert tuple(row) == ("showing", None)
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 0
        assert conn.execute("SELECT COUNT(*) FROM mcp_event_records").fetchone()[0] == 0
        monkeypatch.setattr(events, "enqueue", original)
        await backend.answer("one", "yes")
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


async def test_simultaneous_answers_first_wins_and_retries_dedup():
    with runtime() as (service, backend, events, conn, _):
        await subscribe(events)
        await service.call_tool("orb_ask", card(), "dot")
        results = await asyncio.gather(
            backend.answer("one", "yes"),
            backend.answer("one", "no"),
            backend.answer("one", "yes"),
            return_exceptions=True,
        )
        assert results[0] == results[2]
        assert isinstance(results[1], ServiceError)
        assert conn.execute("SELECT COUNT(*) FROM mcp_event_records").fetchone()[0] == 1
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


async def test_restart_replays_pending_only_and_keeps_answer_identity(tmp_path):
    path = tmp_path / "state.db"
    with runtime(path) as (service, backend, events, _, _):
        await subscribe(events)
        await service.call_tool("orb_ask", card("answered"), "dot")
        first = await backend.answer("answered", "yes")
        await service.call_tool("orb_ask", card("pending"), "dot")
    with runtime(path) as (service, backend, events, conn, sink):
        await service.deliver_pending()
        assert backend.shown == ["pending"]
        assert await backend.answer("answered", "yes") == first
        assert conn.execute("SELECT COUNT(*) FROM mcp_event_records").fetchone()[0] == 1
        assert await events.deliver_due() == 1
        assert json.loads(sink.requests[-1][1])["data"] == first
        result = await service.call_tool("orb_ask", card("answered"), "dot")
        assert result["status"] == "answered"
        assert backend.shown == ["pending"]


async def test_answer_and_transcript_events_do_not_cross_principals():
    with runtime() as (service, backend, events, _, sink):
        for principal in ("dot", "claude", "codex"):
            await subscribe(events, principal)
            await subscribe(events, principal, "voice.transcribed")
        await service.call_tool("orb_ask", card(), "claude")
        await backend.answer("one", "yes")
        await events.deliver_due()
        delivered = [
            (url, json.loads(body))
            for url, body, _ in sink.requests
            if "eventId" in json.loads(body)
        ]
        assert len(delivered) == 1 and delivered[0][0].endswith("/claude")
        transcript = Transcript(
            "rec_1", "Untrusted voice", 1.0, "orb-fake", "dot", True
        )
        await service.on_transcript(transcript)
        await events.deliver_due()
        delivered = [
            (url, json.loads(body))
            for url, body, _ in sink.requests
            if "eventId" in json.loads(body)
        ]
        assert len(delivered) == 2 and delivered[-1][0].endswith("/dot")
        with pytest.raises(ServiceError):
            await service.call_tool("orb_get_card", {"card_id": "one"}, "dot")
        with pytest.raises(ServiceError):
            await service.call_tool(
                "orb_get_transcript", {"recording_id": "rec_1"}, "claude"
            )


async def test_voice_requires_matching_active_dot_subscription_and_confirmation():
    with runtime() as (service, _, events, conn, _):
        unconfirmed = Transcript("rec_1", "test", 1.0, "orb-fake", "dot", False)
        with pytest.raises(ServiceError):
            await service.on_transcript(unconfirmed)
        confirmed = Transcript("rec_1", "test", 1.0, "orb-fake", "dot", True)
        assert await service.on_transcript(confirmed) == {
            "accepted": False,
            "reason": "not_listening",
        }
        await subscribe(events, "claude", "voice.transcribed")
        await subscribe(events, "dot", "voice.transcribed", {"device": "other"})
        assert (await service.on_transcript(confirmed))["accepted"] is False
        assert conn.execute("SELECT COUNT(*) FROM transcripts").fetchone()[0] == 0
        await subscribe(events, "dot", "voice.transcribed", {"device": "orb-fake"})
        assert (await service.on_transcript(confirmed))["accepted"] is True
        assert (await service.on_transcript(confirmed))["accepted"] is True
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1
        # Local focus is persisted for the hub but never sent to Dot.
        await service.on_transcript(
            Transcript("rec_2", "local", 1.0, "orb-fake", "claude", True)
        )
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


async def test_offline_cancel_is_durable_and_retried_after_restart(tmp_path):
    path = tmp_path / "state.db"
    with runtime(path) as (service, backend, _, conn, _):
        await service.call_tool("orb_ask", card(), "dot")
        backend.devices[0]["online"] = False
        result = await service.call_tool("orb_cancel", {"card_id": "one"}, "dot")
        assert result["status"] == "cancelled"
        assert conn.execute("SELECT cancel_pending FROM cards").fetchone()[0] == 1
        with pytest.raises(ServiceError):
            await backend.answer("one", "yes")
    with runtime(path) as (service, backend, _, conn, _):
        await service.deliver_pending()
        assert backend.cancelled == ["one"]
        assert backend.shown == []
        assert conn.execute("SELECT cancel_pending FROM cards").fetchone()[0] == 0


async def test_failed_device_delivery_remains_queued_and_recovers():
    class FlakyBackend(FakeDeviceBackend):
        fail = True

        async def show_card(self, item):
            if self.fail:
                raise OSError("link disconnected")
            return await super().show_card(item)

    backend = FlakyBackend()
    with runtime(backend=backend) as (service, _, _, _, _):
        assert (await service.call_tool("orb_ask", card(), "dot"))["status"] == "queued"
        backend.fail = False
        await service.deliver_pending()
        assert backend.shown == ["one"]
        assert (await service.call_tool("orb_get_card", {"card_id": "one"}, "dot"))[
            "status"
        ] == "showing"


async def test_answer_during_show_does_not_resurrect_card():
    class AnsweringBackend(FakeDeviceBackend):
        async def show_card(self, item):
            accepted = await super().show_card(item)
            await self.answer(item["card_id"], "yes")
            return accepted

    with runtime(backend=AnsweringBackend()) as (service, _, events, conn, _):
        await subscribe(events)
        result = await service.call_tool("orb_ask", card(), "dot")
        assert result["status"] == "answered"
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


async def test_cancel_during_inflight_show_eventually_removes_device_card():
    started, finish = asyncio.Event(), asyncio.Event()

    class SlowBackend(FakeDeviceBackend):
        async def show_card(self, item):
            started.set()
            await finish.wait()
            return await super().show_card(item)

    with runtime(backend=SlowBackend()) as (service, backend, _, conn, _):
        showing = asyncio.create_task(service.call_tool("orb_ask", card(), "dot"))
        await started.wait()
        cancelling = asyncio.create_task(
            service.call_tool("orb_cancel", {"card_id": "one"}, "dot")
        )
        await asyncio.sleep(0)
        assert conn.execute("SELECT status FROM cards").fetchone()[0] == "cancelled"
        finish.set()
        await asyncio.gather(showing, cancelling)
        assert backend.cards == {}
        assert backend.cancelled == ["one"]
        assert conn.execute("SELECT status FROM cards").fetchone()[0] == "cancelled"


async def test_cancel_unassigned_card_does_not_leave_impossible_device_work():
    backend = FakeDeviceBackend()
    backend.devices = []
    with runtime(backend=backend) as (service, _, _, conn, _):
        result = await service.call_tool("orb_ask", card(), "dot")
        assert result["status"] == "queued"
        await service.call_tool("orb_cancel", {"card_id": "one"}, "dot")
        assert conn.execute("SELECT cancel_pending FROM cards").fetchone()[0] == 0
        assert backend.shown == [] and backend.cancelled == []


async def test_cancel_during_registry_lookup_never_shows_unassigned_card():
    entered, finish = asyncio.Event(), asyncio.Event()

    class AppearingBackend(FakeDeviceBackend):
        lookups = 0

        async def list_devices(self):
            self.lookups += 1
            if self.lookups == 1:
                return []
            if self.lookups == 2:
                entered.set()
                await finish.wait()
            return await super().list_devices()

    with runtime(backend=AppearingBackend()) as (service, backend, _, conn, _):
        creating = asyncio.create_task(service.call_tool("orb_ask", card(), "dot"))
        await entered.wait()
        cancelling = asyncio.create_task(
            service.call_tool("orb_cancel", {"card_id": "one"}, "dot")
        )
        await asyncio.sleep(0)
        finish.set()
        await asyncio.gather(creating, cancelling)
        row = conn.execute(
            "SELECT status,device_id,cancel_pending FROM cards"
        ).fetchone()
        assert tuple(row) == ("cancelled", None, 0)
        assert backend.cards == {}
        assert backend.shown == []
        assert backend.cancelled == []


@pytest.mark.parametrize("confirmed", ["false", "true", 1, 0, None])
async def test_voice_confirmation_requires_literal_true(confirmed):
    with runtime() as (service, _, events, conn, _):
        await subscribe(events, "dot", "voice.transcribed")
        transcript = Transcript("rec_1", "untrusted", 1.0, "orb-fake", "dot", confirmed)
        with pytest.raises(ServiceError, match="confirmed"):
            await service.on_transcript(transcript)
        assert conn.execute("SELECT COUNT(*) FROM transcripts").fetchone()[0] == 0
        assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 0
