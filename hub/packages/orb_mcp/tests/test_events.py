import base64
import json
import sqlite3
from datetime import datetime

import pytest

from orb_mcp.events import (
    DEFAULT_TTL_MS,
    EVENT_DEFINITIONS,
    MAX_TTL_MS,
    MIN_TTL_MS,
    EventError,
    Events,
    WebhookResponse,
    decode_secret,
)


class Clock:
    def __init__(self):
        self.now = 1_793_030_000.0

    def __call__(self):
        return self.now

    def advance(self, amount):
        self.now += amount


class Receiver:
    def __init__(self):
        self.requests = []
        self.statuses = []
        self.failure = None
        self.bad_echo = False
        self.before_send = None

    async def __call__(self, url, body, headers):
        self.requests.append((url, body, headers))
        if self.before_send:
            self.before_send()
        if self.failure:
            raise self.failure
        value = json.loads(body)
        if value.get("type") == "verification":
            echo = "wrong" if self.bad_echo else value["challenge"]
            return WebhookResponse(200, json.dumps({"challenge": echo}).encode())
        return WebhookResponse(self.statuses.pop(0) if self.statuses else 204)


@pytest.fixture
def state():
    connection = sqlite3.connect(":memory:")
    clock = Clock()
    receiver = Receiver()
    events = Events(connection, clock=clock, transport=receiver)
    yield events, clock, receiver, connection
    connection.close()


def secret(byte=1, size=32):
    return "whsec_" + base64.b64encode(bytes([byte]) * size).decode()


def subscription(**changes):
    params = {
        "name": "card.answered",
        "arguments": {},
        "delivery": {
            "mode": "webhook",
            "url": "https://receiver.example/callback",
            "secret": secret(),
        },
        "cursor": None,
    }
    params.update(changes)
    return params


def answer(card_id="card_1", device="orb-test"):
    return {
        "card_id": card_id,
        "device_id": device,
        "kind": "option",
        "option_id": "yes",
        "option_label": "Yes",
        "answered_at": "2026-10-26T14:00:00Z",
    }


def enqueue(events, connection, payload=None, **kwargs):
    with connection:
        return events.enqueue("card.answered", payload or answer(), "dot", **kwargs)


async def test_catalog_has_strict_filters_and_payloads(state):
    events, *_ = state
    result = await events.dispatch("events/list", {}, "dot")
    assert {item["name"] for item in result["events"]} == {
        "card.answered",
        "voice.transcribed",
    }
    for event in result["events"]:
        assert event["delivery"] == ["webhook"]
        assert event["inputSchema"]["additionalProperties"] is False
        assert event["payloadSchema"]["additionalProperties"] is False
    result["events"][0]["name"] = "mutated"
    assert EVENT_DEFINITIONS[0]["name"] == "card.answered"


@pytest.mark.parametrize("size", [24, 32, 64])
def test_valid_secret_sizes(size):
    assert len(decode_secret(secret(size=size))) == size


@pytest.mark.parametrize(
    "value",
    [None, "", "a" * 32, "whsec_%%%%", "whsec_", secret(size=23), secret(size=65)],
)
def test_invalid_secrets(value):
    with pytest.raises(EventError) as exc:
        decode_secret(value)
    assert exc.value.code == -32602


async def test_verification_idempotency_canonical_identity_and_cache(state):
    events, clock, receiver, conn = state
    params = subscription(arguments={"device": "orb-test", "card_id": "card_1"})
    first = await events.dispatch("events/subscribe", params, "dot")
    assert first["cursor"] is None and first["truncated"] is False
    assert len(first["id"]) == 68
    assert (
        datetime.fromisoformat(first["refreshBefore"]).timestamp()
        == clock.now + DEFAULT_TTL_MS / 1000
    )
    _, body, headers = receiver.requests[0]
    assert json.loads(body)["type"] == "verification"
    assert headers["webhook-id"].startswith("msg_verification_")
    assert headers["X-MCP-Subscription-Id"] == first["id"]
    clock.advance(20)
    params["arguments"] = {"card_id": "card_1", "device": "orb-test"}
    second = await events.dispatch("events/subscribe", params, "dot")
    assert second["id"] == first["id"]
    assert second["refreshBefore"] != first["refreshBefore"]
    assert len(receiver.requests) == 1
    assert conn.execute("SELECT COUNT(*) FROM mcp_subscriptions").fetchone()[0] == 1
    clock.advance(301)
    await events.dispatch("events/subscribe", params, "dot")
    assert len(receiver.requests) == 2
    assert receiver.requests[0][1] != receiver.requests[1][1]


@pytest.mark.parametrize(
    "ttl,granted",
    [
        (None, DEFAULT_TTL_MS),
        (0, MIN_TTL_MS),
        (1, MIN_TTL_MS),
        (600000, 600000),
        (MAX_TTL_MS + 1, MAX_TTL_MS),
    ],
)
async def test_ttl_grants(state, ttl, granted):
    events, clock, *_ = state
    result = await events.dispatch("events/subscribe", subscription(ttlMs=ttl), "dot")
    assert (
        datetime.fromisoformat(result["refreshBefore"]).timestamp()
        == clock.now + granted / 1000
    )


@pytest.mark.parametrize(
    "change",
    [
        {"ttlMs": -1},
        {"ttlMs": True},
        {"ttlMs": 1.5},
        {"ttlMs": "300000"},
        {"name": "unknown"},
        {"name": []},
        {"arguments": {"extra": 1}},
        {"arguments": {"card_id": "x" * 65}},
        {"arguments": {"device": False}},
        {"cursor": "old"},
        {"extra": True},
        {"delivery": {"mode": "stream"}},
    ],
)
async def test_bad_subscription_fields_rejected_without_network(state, change):
    events, _, receiver, _ = state
    with pytest.raises(EventError):
        await events.dispatch("events/subscribe", subscription(**change), "dot")
    assert receiver.requests == []


async def test_failed_challenge_never_persists_active_subscription(state):
    events, _, receiver, conn = state
    receiver.bad_echo = True
    with pytest.raises(EventError) as exc:
        await events.dispatch("events/subscribe", subscription(), "dot")
    assert exc.value.code == -32015
    assert exc.value.data == {"reason": "challenge_failed"}
    assert conn.execute("SELECT COUNT(*) FROM mcp_subscriptions").fetchone()[0] == 0


@pytest.mark.parametrize(
    "failure,reason", [(TimeoutError(), "timeout"), (OSError(), "connection_failed")]
)
async def test_verification_failure_categories(state, failure, reason):
    events, _, receiver, _ = state
    receiver.failure = failure
    with pytest.raises(EventError) as exc:
        await events.dispatch("events/subscribe", subscription(), "dot")
    assert exc.value.code == -32015
    assert exc.value.data["reason"] == reason


async def test_expired_challenge_is_not_accepted(state):
    events, clock, receiver, _ = state
    receiver.before_send = lambda: clock.advance(11)
    with pytest.raises(EventError) as exc:
        await events.dispatch("events/subscribe", subscription(), "dot")
    assert exc.value.data["reason"] == "timeout"


async def test_rotation_reverification_and_overlap_window(state):
    events, clock, receiver, conn = state
    params = subscription()
    first = await events.dispatch("events/subscribe", params, "dot")
    params["delivery"]["secret"] = secret(2)
    clock.advance(20)
    second = await events.dispatch("events/subscribe", params, "dot")
    assert first["id"] == second["id"] and len(receiver.requests) == 2
    enqueue(events, conn)
    await events.deliver_due()
    signature = receiver.requests[-1][2]["webhook-signature"]
    assert len(signature.split(" ")) == 2
    clock.advance(301)
    enqueue(events, conn, answer("card_2"))
    await events.deliver_due()
    assert len(receiver.requests[-1][2]["webhook-signature"].split(" ")) == 1


async def test_failed_rotation_keeps_prior_subscription(state):
    events, _, receiver, conn = state
    await events.dispatch("events/subscribe", subscription(), "dot")
    receiver.bad_echo = True
    changed = subscription()
    changed["delivery"]["secret"] = secret(2)
    with pytest.raises(EventError):
        await events.dispatch("events/subscribe", changed, "dot")
    assert (
        conn.execute("SELECT secret FROM mcp_subscriptions").fetchone()[0] == secret()
    )


async def test_filter_principal_isolation_and_no_replay(state):
    events, _, receiver, conn = state
    enqueue(events, conn, answer("before"), event_id="evt_before")
    await events.dispatch(
        "events/subscribe",
        subscription(arguments={"card_id": "card_1", "device": "orb-test"}),
        "dot",
    )
    await events.dispatch("events/subscribe", subscription(), "claude")
    enqueue(events, conn, answer("before"), event_id="evt_before")
    enqueue(events, conn, answer("different"))
    enqueue(events, conn, answer(device="different"))
    event_id = enqueue(events, conn)
    assert await events.deliver_due() == 1
    assert json.loads(receiver.requests[-1][1])["eventId"] == event_id
    assert events.status("claude")["outbox"] == {}
    assert events.status("dot")["outbox"] == {"delivered": 1}


async def test_unsubscribe_is_idempotent_owned_and_cancels_pending(state):
    events, _, receiver, conn = state
    await events.dispatch("events/subscribe", subscription(), "dot")
    enqueue(events, conn)
    params = subscription()
    params.pop("cursor")
    params["delivery"].pop("secret")
    assert await events.dispatch("events/unsubscribe", params, "claude") == {}
    assert events.has_subscription("card.answered", "dot")
    assert await events.dispatch("events/unsubscribe", params, "dot") == {}
    assert await events.dispatch("events/unsubscribe", params, "dot") == {}
    assert not events.has_subscription("card.answered", "dot")
    assert await events.deliver_due() == 0
    assert len(receiver.requests) == 1
    assert events.status("dot")["outbox"] == {"cancelled": 1}


async def test_expiry_stops_delivery_and_refresh_after_expiry_has_no_replay(state):
    events, clock, receiver, conn = state
    params = subscription(ttlMs=MIN_TTL_MS)
    first = await events.dispatch("events/subscribe", params, "dot")
    enqueue(events, conn)
    clock.advance(MIN_TTL_MS / 1000)
    assert not events.has_subscription("card.answered", "dot")
    assert await events.deliver_due() == 0
    assert events.status("dot")["outbox"] == {"expired": 1}
    second = await events.dispatch("events/subscribe", params, "dot")
    assert second["id"] == first["id"]
    assert events.has_subscription("card.answered", "dot")
    assert await events.deliver_due() == 0
    assert len(receiver.requests) == 2


async def test_authorization_is_rechecked_before_sending(state):
    events, _, receiver, conn = state
    await events.dispatch("events/subscribe", subscription(), "dot")
    enqueue(events, conn)
    events.authorize = lambda principal, name, arguments: False
    assert not events.has_subscription("card.answered", "dot")
    assert await events.deliver_due() == 0
    assert events.status("dot")["outbox"] == {"revoked": 1}
    assert len(receiver.requests) == 1
    with pytest.raises(EventError):
        await events.dispatch("events/subscribe", subscription(), "dot")


async def test_atomic_enqueue_rollback_dedup_conflict_and_size(state):
    events, _, _, conn = state
    await events.dispatch("events/subscribe", subscription(), "dot")
    with pytest.raises(RuntimeError), conn:
        events.enqueue("card.answered", answer(), "dot", event_id="evt_atomic")
        raise RuntimeError("source transaction aborted")
    assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 0
    assert enqueue(events, conn, event_id="evt_atomic") == "evt_atomic"
    assert enqueue(events, conn, event_id="evt_atomic") == "evt_atomic"
    assert conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1
    with pytest.raises(ValueError, match="different content"):
        enqueue(events, conn, answer("changed"), event_id="evt_atomic")
    with pytest.raises(ValueError, match="256 KiB"):
        enqueue(events, conn, {"big": "x" * (256 * 1024)})


async def test_restart_persists_expiry_and_outbox(tmp_path):
    path = tmp_path / "events.db"
    clock, receiver = Clock(), Receiver()
    conn = sqlite3.connect(path)
    events = Events(conn, clock=clock, transport=receiver)
    first = await events.dispatch("events/subscribe", subscription(), "dot")
    event_id = enqueue(events, conn)
    conn.close()
    conn = sqlite3.connect(path)
    try:
        restarted = Events(conn, clock=clock, transport=receiver)
        assert (
            restarted.status("dot")["subscriptions"][0]["refreshBefore"]
            == first["refreshBefore"]
        )
        assert await restarted.deliver_due() == 1
        assert json.loads(receiver.requests[-1][1])["eventId"] == event_id
    finally:
        conn.close()


async def test_status_omits_callback_urls_and_keys(state):
    events, *_ = state
    await events.dispatch("events/subscribe", subscription(), "dot")
    text = json.dumps(events.status("dot"))
    assert "https:" not in text and "whsec_" not in text
    assert events.status("claude") == {"subscriptions": [], "outbox": {}}


async def test_refresh_cannot_resurrect_expired_outbox_before_worker_sweep(state):
    events, clock, receiver, conn = state
    params = subscription(ttlMs=MIN_TTL_MS)
    await events.dispatch("events/subscribe", params, "dot")
    enqueue(events, conn)
    clock.advance(301)
    await events.dispatch("events/subscribe", params, "dot")
    assert await events.deliver_due() == 0
    assert events.status("dot")["outbox"] == {"expired": 1}
    assert len(receiver.requests) == 2


@pytest.mark.parametrize(
    "payload",
    [
        {"card_id": "one"},
        {**answer(), "kind": "dismissed"},
        {key: value for key, value in answer().items() if key != "option_id"},
        {**answer(), "answered_at": "invalid-time"},
        {**answer(), "injected": "unexpected"},
    ],
)
def test_invalid_answer_payloads_cannot_enter_outbox(state, payload):
    events, _, _, conn = state
    with pytest.raises(ValueError), conn:
        events.enqueue("card.answered", payload)
    assert conn.execute("SELECT COUNT(*) FROM mcp_event_records").fetchone()[0] == 0


def test_transcript_schema_enforced_at_event_boundary(state):
    events, _, _, conn = state
    payload = {
        "recording_id": "rec_1",
        "device_id": "orb-fake",
        "text": "x" * 8193,
        "duration_s": 1,
    }
    with pytest.raises(ValueError), conn:
        events.enqueue("voice.transcribed", payload)
