import asyncio
import base64
import ipaddress
import json
import socket
import sqlite3
import time
from datetime import UTC, datetime

import pytest
from standardwebhooks.webhooks import Webhook

from orb_mcp.events import (
    EventError,
    Events,
    SafeWebhookTransport,
    WebhookResponse,
    _PinnedResolver,
    sign_headers,
    validate_address,
    validate_url,
)


def key(number=1):
    return "whsec_" + base64.b64encode(bytes([number]) * 32).decode()


class Clock:
    def __init__(self):
        self.now = time.time()

    def __call__(self):
        return self.now


class Sink:
    def __init__(self):
        self.requests = []
        self.status = 204
        self.raise_error = False
        self.before_send = None

    async def __call__(self, url, body, headers):
        self.requests.append((url, body, headers))
        value = json.loads(body)
        if value.get("type") == "verification":
            return WebhookResponse(
                200, json.dumps({"challenge": value["challenge"]}).encode()
            )
        if self.before_send:
            self.before_send()
        if self.raise_error:
            raise OSError("simulated outage")
        return WebhookResponse(self.status)


@pytest.fixture
def state():
    conn = sqlite3.connect(":memory:")
    clock, sink = Clock(), Sink()
    events = Events(conn, clock=clock, transport=sink)
    yield events, conn, clock, sink
    conn.close()


async def setup_event(events, conn):
    result = await events.dispatch(
        "events/subscribe",
        {
            "name": "card.answered",
            "arguments": {},
            "delivery": {
                "mode": "webhook",
                "url": "https://receiver.example/events",
                "secret": key(),
            },
        },
        "dot",
    )
    with conn:
        event_id = events.enqueue(
            "card.answered",
            {
                "card_id": "one",
                "kind": "dismissed",
                "device_id": "orb-test",
                "answered_at": "2026-10-26T14:00:00Z",
            },
            "dot",
        )
    return result["id"], event_id


def test_signature_verified_by_independent_standardwebhooks_library():
    body = json.dumps(
        {"eventId": "evt_1", "text": "Hello 🌐"}, ensure_ascii=False
    ).encode()
    now = time.time()
    headers = sign_headers("evt_1", body, [key()], "sub_test", now)
    assert Webhook(key()).verify(body, headers) == json.loads(body)
    expected = Webhook(key()).sign(
        "evt_1", datetime.fromtimestamp(int(now), UTC), body.decode()
    )
    assert headers["webhook-signature"] == expected
    with pytest.raises(Exception):
        Webhook(key()).verify(body + b" ", headers)


def test_rotation_signatures_verify_with_either_key():
    body = b'{"eventId":"evt_1"}'
    headers = sign_headers("evt_1", body, [key(1), key(2)], "sub_test", time.time())
    assert len(headers["webhook-signature"].split()) == 2
    assert Webhook(key(1)).verify(body, headers) == {"eventId": "evt_1"}
    assert Webhook(key(2)).verify(body, headers) == {"eventId": "evt_1"}


async def test_verification_and_delivery_validate_independently(state):
    events, conn, _, sink = state
    subscription_id, event_id = await setup_event(events, conn)
    await events.deliver_due()
    for _, body, headers in sink.requests:
        assert Webhook(key()).verify(body, headers) == json.loads(body)
        assert headers["X-MCP-Subscription-Id"] == subscription_id
    data = json.loads(sink.requests[-1][1])
    assert data["eventId"] == event_id == sink.requests[-1][2]["webhook-id"]
    assert set(data) == {"eventId", "name", "timestamp", "data", "cursor"}
    assert data["cursor"] is None
    assert datetime.fromisoformat(data["timestamp"]).tzinfo is not None


async def test_durable_claim_precedes_io_and_retry_reuses_exact_body(state):
    events, conn, clock, sink = state
    await setup_event(events, conn)
    sink.status = 503

    def inspect():
        attempts, next_attempt = conn.execute(
            "SELECT attempts,next_attempt FROM mcp_outbox"
        ).fetchone()
        assert attempts >= 1 and next_attempt > clock.now
        assert conn.in_transaction is False

    sink.before_send = inspect
    assert await events.deliver_due() == 1
    first = sink.requests[-1]
    assert await events.deliver_due() == 0
    clock.now += 1
    sink.status = 204
    assert await events.deliver_due() == 1
    second = sink.requests[-1]
    assert first[1] == second[1]
    assert first[2]["webhook-id"] == second[2]["webhook-id"]
    assert first[2]["webhook-timestamp"] != second[2]["webhook-timestamp"]
    assert first[2]["webhook-signature"] != second[2]["webhook-signature"]
    assert events.status("dot")["outbox"] == {"delivered": 1}


@pytest.mark.parametrize("status", [301, 302, 307, 400, 401, 403, 404, 410, 413])
async def test_permanent_failure_is_not_retried(state, status):
    events, conn, clock, sink = state
    await setup_event(events, conn)
    sink.status = status
    assert await events.deliver_due() == 1
    clock.now += 600
    assert await events.deliver_due() == 0
    assert events.status("dot")["outbox"] == {"failed": 1}
    assert events.has_subscription("card.answered", "dot") is (status != 410)


@pytest.mark.parametrize("status", [408, 425, 429, 500, 502, 503])
async def test_transient_http_failures_back_off(state, status):
    events, conn, clock, sink = state
    await setup_event(events, conn)
    sink.status = status
    assert await events.deliver_due() == 1
    assert (
        conn.execute("SELECT next_attempt FROM mcp_outbox").fetchone()[0]
        == clock.now + 1
    )
    clock.now += 1
    assert await events.deliver_due() == 1
    assert (
        conn.execute("SELECT next_attempt FROM mcp_outbox").fetchone()[0]
        == clock.now + 2
    )
    clock.now += 2
    assert await events.deliver_due() == 1
    assert (
        conn.execute("SELECT next_attempt FROM mcp_outbox").fetchone()[0]
        == clock.now + 4
    )


async def test_network_failure_stops_at_fifteen_minutes(state):
    events, conn, clock, sink = state
    await setup_event(events, conn)
    sink.raise_error = True
    started = clock.now
    while clock.now < started + 900:
        await events.deliver_due()
        row = conn.execute("SELECT state,next_attempt FROM mcp_outbox").fetchone()
        assert row[0] == "pending"
        clock.now = row[1]
    assert await events.deliver_due() == 0
    assert events.status("dot")["outbox"] == {"failed": 1}
    assert len(sink.requests) <= 17


async def test_crash_after_claim_can_retry_on_restart(tmp_path):
    path = tmp_path / "outbox.db"
    clock, sink = Clock(), Sink()
    conn = sqlite3.connect(path)
    events = Events(conn, clock=clock, transport=sink)
    await setup_event(events, conn)

    def crash():
        raise asyncio.CancelledError

    sink.before_send = crash
    with pytest.raises(asyncio.CancelledError):
        await events.deliver_due()
    conn.close()
    conn = sqlite3.connect(path)
    try:
        events = Events(conn, clock=clock, transport=sink)
        sink.before_send = None
        assert await events.deliver_due() == 0
        clock.now += 31
        assert await events.deliver_due() == 1
        assert events.status("dot")["outbox"] == {"delivered": 1}
    finally:
        conn.close()


@pytest.mark.parametrize(
    "ip",
    [
        "127.0.0.1",
        "172.16.0.3",
        "172.16.0.1",
        "172.16.0.2",
        "169.254.169.254",
        str(ipaddress.IPv4Address(0x64400001)),  # RFC 6598 shared address, synthetic
        "0.0.0.0",
        "224.0.0.1",
        "192.0.2.1",
        "198.51.100.1",
        "203.0.113.1",
        "::1",
        "::",
        "fe80::1",
        "fc00::1",
        "ff00::1",
        "::ffff:127.0.0.1",
    ],
)
def test_non_public_addresses_rejected(ip):
    with pytest.raises(EventError) as exc:
        validate_address(ip)
    assert exc.value.data["reason"] == "non_public_address"


@pytest.mark.parametrize("ip", ["1.1.1.1", "8.8.8.8", "2606:4700:4700::1111"])
def test_global_addresses_allowed(ip):
    validate_address(ip)


@pytest.mark.parametrize(
    "url",
    [
        "http://receiver.example/x",
        "https://user:pass@receiver.example/x",
        "https://receiver.example/#fragment",
        "https://receiver.example:bad/x",
        "https://receiver.example/\n",
        "https://receiver.example\\@127.0.0.1",
        "https://[fe80::1%lo0]/",
        "file:///tmp/test",
        "https://",
    ],
)
def test_bad_callback_urls_rejected(url):
    with pytest.raises(EventError):
        validate_url(url)


async def test_transport_rejects_literal_loopback_without_connecting():
    with pytest.raises(EventError):
        await SafeWebhookTransport()("https://127.0.0.1:9443/events", b"{}", {})


async def test_dns_mixed_public_private_is_rejected_before_connection(monkeypatch):
    loop = asyncio.get_running_loop()

    async def lookup(*args, **kwargs):
        return [
            (socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", (ip, 443))
            for ip in ["1.1.1.1", "127.0.0.1"]
        ]

    monkeypatch.setattr(loop, "getaddrinfo", lookup)
    with pytest.raises(EventError) as exc:
        await SafeWebhookTransport()("https://receiver.example/events", b"{}", {})
    assert exc.value.data["reason"] == "non_public_address"


async def test_pinned_resolver_never_does_second_dns_lookup():
    resolver = _PinnedResolver("receiver.example", [(socket.AF_INET, "1.1.1.1")])
    result = await resolver.resolve("receiver.example", 8443)
    assert result[0]["host"] == "1.1.1.1"
    assert result[0]["hostname"] == "receiver.example"
    assert result[0]["port"] == 8443
    with pytest.raises(EventError):
        await resolver.resolve("attacker.example", 443)
    await resolver.close()


async def test_transport_preserves_hostname_pins_dns_and_disables_proxy_redirect(
    monkeypatch,
):
    import orb_mcp.events as module

    loop = asyncio.get_running_loop()
    lookups = []

    async def lookup(host, port, **kwargs):
        lookups.append(host)
        return [
            (
                socket.AF_INET,
                socket.SOCK_STREAM,
                socket.IPPROTO_TCP,
                "",
                ("1.1.1.1", port),
            )
        ]

    monkeypatch.setattr(loop, "getaddrinfo", lookup)

    class Connector:
        def __init__(self, **kwargs):
            assert kwargs["use_dns_cache"] is False
            self.resolver = kwargs["resolver"]

    class Content:
        async def readexactly(self, limit):
            assert limit == 4097
            raise asyncio.IncompleteReadError(b"complete-response", limit)

    class Response:
        status = 302
        content = Content()

        async def __aenter__(self):
            return self

        async def __aexit__(self, *args):
            pass

    class Session:
        def __init__(self, **kwargs):
            assert kwargs["trust_env"] is False
            self.connector = kwargs["connector"]

        async def __aenter__(self):
            records = await self.connector.resolver.resolve("receiver.example", 443)
            assert records[0]["host"] == "1.1.1.1"
            return self

        async def __aexit__(self, *args):
            pass

        def post(self, url, **kwargs):
            assert url == "https://receiver.example/events"
            assert kwargs["allow_redirects"] is False
            assert kwargs["data"] == b"raw-bytes"
            return Response()

    monkeypatch.setattr(module.aiohttp, "TCPConnector", Connector)
    monkeypatch.setattr(module.aiohttp, "ClientSession", Session)
    response = await SafeWebhookTransport()(
        "https://receiver.example/events", b"raw-bytes", {}
    )
    assert response.status == 302
    assert response.body == b"complete-response"
    assert lookups == ["receiver.example"]


@pytest.mark.parametrize("ip", ["64:ff9b::c0a8:1", "64:ff9b::7f00:1", "64:ff9b::a00:1"])
def test_nat64_cannot_wrap_private_ipv4_addresses(ip):
    with pytest.raises(EventError):
        validate_address(ip)


async def test_rotation_during_batch_uses_current_key_for_next_delivery(state):
    events, conn, clock, sink = state
    await setup_event(events, conn)
    with conn:
        events.enqueue(
            "card.answered",
            {
                "card_id": "two",
                "kind": "dismissed",
                "device_id": "orb-test",
                "answered_at": "2026-10-26T14:00:00Z",
            },
            "dot",
        )
    first = True

    async def rotating_transport(url, body, headers):
        nonlocal first
        value = json.loads(body)
        if first and "eventId" in value:
            first = False
            await events.dispatch(
                "events/subscribe",
                {
                    "name": "card.answered",
                    "arguments": {},
                    "delivery": {
                        "mode": "webhook",
                        "url": "https://receiver.example/events",
                        "secret": key(2),
                    },
                },
                "dot",
            )
            clock.now += 301
        return await sink(url, body, headers)

    events.transport = rotating_transport
    assert await events.deliver_due() == 2
    deliveries = [
        (body, headers)
        for _, body, headers in sink.requests
        if "eventId" in json.loads(body)
    ]
    second_body, second_headers = deliveries[-1]
    expected = sign_headers(
        second_headers["webhook-id"],
        second_body,
        [key(2)],
        second_headers["X-MCP-Subscription-Id"],
        clock.now,
    )
    assert second_headers["webhook-signature"] == expected["webhook-signature"]
    assert len(second_headers["webhook-signature"].split()) == 1


async def test_expiry_during_batch_stops_next_delivery(state):
    events, conn, clock, sink = state
    await setup_event(events, conn)
    with conn:
        events.enqueue(
            "card.answered",
            {
                "card_id": "two",
                "kind": "dismissed",
                "device_id": "orb-test",
                "answered_at": "2026-10-26T14:00:00Z",
            },
            "dot",
        )
    sink.before_send = lambda: setattr(clock, "now", clock.now + 86401)
    assert await events.deliver_due() == 1
    assert not events.has_subscription("card.answered", "dot")
    assert await events.deliver_due() == 0
    assert events.status("dot")["outbox"] == {"delivered": 1, "expired": 1}
