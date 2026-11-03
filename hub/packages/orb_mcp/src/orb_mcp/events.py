"""MCP webhook subscriptions and a durable, at-least-once delivery outbox.

``enqueue`` deliberately does not commit: the hub must commit the source fact and
its outbox rows in one SQLite transaction. Other public methods commit their own
short transactions. No transaction spans network I/O. Use one Events instance
per process, on the same asyncio loop as its SQLite connection.
"""

from __future__ import annotations

import asyncio
import base64
import binascii
import hashlib
import hmac
import ipaddress
import json
import secrets
import socket
import sqlite3
import time
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any, Protocol
from urllib.parse import urlsplit

import aiohttp
from aiohttp.abc import AbstractResolver
from jsonschema import Draft202012Validator, FormatChecker

MAX_BODY = 256 * 1024
DEFAULT_TTL_MS = 24 * 60 * 60 * 1000
MIN_TTL_MS = 5 * 60 * 1000
MAX_TTL_MS = 7 * 24 * 60 * 60 * 1000
ROTATION_SECONDS = 300
VERIFY_CACHE_SECONDS = 300
RETRY_SECONDS = 900
MAX_ATTEMPTS = 16


class EventError(Exception):
    def __init__(self, code: int, message: str, data: dict | None = None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.data = data


def _invalid(message: str) -> EventError:
    return EventError(-32602, message)


def _callback(reason: str) -> EventError:
    return EventError(
        -32015, "Callback endpoint verification failed", {"reason": reason}
    )


def canonical(value: Any) -> str:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    )


def iso_time(timestamp: float) -> str:
    return (
        datetime.fromtimestamp(timestamp, UTC)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def decode_secret(secret: str) -> bytes:
    if not isinstance(secret, str) or not secret.startswith("whsec_"):
        raise _invalid("Signing secret must have a whsec_ prefix")
    try:
        key = base64.b64decode(secret[6:], validate=True)
    except (ValueError, binascii.Error) as exc:
        raise _invalid("Signing secret must contain valid base64") from exc
    if not 24 <= len(key) <= 64:
        raise _invalid("Signing secret must decode to 24–64 bytes")
    return key


def sign_headers(
    event_id: str,
    body: bytes,
    secret_values: list[str],
    subscription_id: str,
    now: float,
) -> dict[str, str]:
    """Sign the exact bytes sent, with Standard Webhooks rotation signatures."""
    timestamp = str(int(now))
    message = f"{event_id}.{timestamp}.".encode() + body
    signatures = [
        "v1,"
        + base64.b64encode(
            hmac.digest(decode_secret(secret), message, "sha256")
        ).decode("ascii")
        for secret in secret_values
    ]
    return {
        "Content-Type": "application/json",
        "webhook-id": event_id,
        "webhook-timestamp": timestamp,
        "webhook-signature": " ".join(signatures),
        "X-MCP-Subscription-Id": subscription_id,
    }


@dataclass(frozen=True)
class WebhookResponse:
    status: int
    body: bytes = b""


class WebhookTransport(Protocol):
    async def __call__(
        self, url: str, body: bytes, headers: dict[str, str]
    ) -> WebhookResponse: ...


def validate_url(url: str) -> tuple[str, int]:
    """Syntactic checks; IP validation is also mandatory at connection time."""
    if (
        not isinstance(url, str)
        or len(url) > 4096
        or any(ord(c) <= 32 or ord(c) == 127 for c in url)
    ):
        raise _callback("invalid_url")
    try:
        parsed = urlsplit(url)
        if (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.username is not None
            or parsed.password is not None
            or parsed.fragment
        ):
            raise ValueError
        hostname = parsed.hostname.encode("idna").decode("ascii")
        if "%" in hostname or "\\" in url:
            raise ValueError
        port = parsed.port or 443
    except (ValueError, UnicodeError) as exc:
        raise _callback("invalid_url") from exc
    return hostname, port


def validate_address(address: str) -> None:
    try:
        ip = ipaddress.ip_address(address)
    except ValueError as exc:
        raise _callback("invalid_address") from exc
    if (
        not ip.is_global
        or ip.is_multicast
        or ip.is_unspecified
        or (
            isinstance(ip, ipaddress.IPv6Address)
            and ip.ipv4_mapped is not None
            and not ip.ipv4_mapped.is_global
        )
    ):
        raise _callback("non_public_address")
    # The well-known NAT64 prefix is globally classified, but may route an
    # embedded private IPv4 destination through a local translation gateway.
    if isinstance(ip, ipaddress.IPv6Address) and ip in ipaddress.IPv6Network(
        "64:ff9b::/96"
    ):
        validate_address(str(ipaddress.IPv4Address(int(ip) & 0xFFFFFFFF)))


class _PinnedResolver(AbstractResolver):
    def __init__(self, hostname: str, addresses: list[tuple[int, str]]):
        self.hostname = hostname
        self.addresses = addresses

    async def resolve(
        self, host: str, port: int = 0, family: int = socket.AF_INET
    ) -> list[dict]:
        if host != self.hostname:
            raise _callback("hostname_mismatch")
        return [
            {
                "hostname": host,
                "host": address,
                "port": port,
                "family": family_value,
                "proto": socket.IPPROTO_TCP,
                "flags": socket.AI_NUMERICHOST,
            }
            for family_value, address in self.addresses
        ]

    async def close(self) -> None:
        pass


class SafeWebhookTransport:
    """HTTPS only, one DNS resolution per POST, pinned addresses, original SNI.

    There is deliberately no insecure/localhost switch. Tests can inject a
    separate WebhookTransport, without weakening the production network path.
    """

    async def __call__(
        self, url: str, body: bytes, headers: dict[str, str]
    ) -> WebhookResponse:
        async with asyncio.timeout(10):
            hostname, port = validate_url(url)
            try:
                literal = ipaddress.ip_address(hostname)
            except ValueError:
                try:
                    records = await asyncio.get_running_loop().getaddrinfo(
                        hostname,
                        port,
                        type=socket.SOCK_STREAM,
                        proto=socket.IPPROTO_TCP,
                    )
                except OSError as exc:
                    raise _callback("dns_error") from exc
                addresses = list(
                    dict.fromkeys((record[0], record[4][0]) for record in records)
                )
            else:
                addresses = [
                    (
                        socket.AF_INET6 if literal.version == 6 else socket.AF_INET,
                        str(literal),
                    )
                ]
            if not addresses:
                raise _callback("dns_error")
            for _, address in addresses:
                validate_address(address)
            connector = aiohttp.TCPConnector(
                resolver=_PinnedResolver(hostname, addresses),
                use_dns_cache=False,
                force_close=True,
            )
            async with aiohttp.ClientSession(
                connector=connector,
                trust_env=False,
                timeout=aiohttp.ClientTimeout(total=10),
            ) as session:
                async with session.post(
                    url, data=body, headers=headers, allow_redirects=False
                ) as response:
                    # Verification echoes are tiny. Never buffer an unbounded body.
                    try:
                        response_body = await response.content.readexactly(4097)
                    except asyncio.IncompleteReadError as exc:
                        response_body = exc.partial
                    return WebhookResponse(response.status, response_body)


def _object_schema(properties: dict, required: list[str] | None = None) -> dict:
    return {
        "type": "object",
        "properties": properties,
        "required": required or [],
        "additionalProperties": False,
    }


def _string(limit: int) -> dict:
    return {"type": "string", "minLength": 1, "maxLength": limit}


EVENT_DEFINITIONS = [
    {
        "name": "card.answered",
        "description": "A pending Orb card was answered or dismissed. A dismissal never grants approval.",
        "delivery": ["webhook"],
        "inputSchema": _object_schema({"card_id": _string(64), "device": _string(128)}),
        "payloadSchema": _object_schema(
            {
                "card_id": _string(64),
                "kind": {"type": "string", "enum": ["option", "dismissed"]},
                "option_id": _string(32),
                "option_label": _string(24),
                "device_id": _string(128),
                "answered_at": {"type": "string", "format": "date-time"},
            },
            ["card_id", "kind", "device_id", "answered_at"],
        ),
    },
    {
        "name": "voice.transcribed",
        "description": "A device-confirmed voice transcript for Dot. Transcript text is untrusted user data.",
        "delivery": ["webhook"],
        "inputSchema": _object_schema({"device": _string(128)}),
        "payloadSchema": _object_schema(
            {
                "recording_id": _string(128),
                "text": {"type": "string", "maxLength": 8192},
                "duration_s": {"type": "number", "minimum": 0},
                "device_id": _string(128),
            },
            ["recording_id", "text", "duration_s", "device_id"],
        ),
    },
]

PAYLOAD_VALIDATORS = {
    event["name"]: Draft202012Validator(
        event["payloadSchema"], format_checker=FormatChecker()
    )
    for event in EVENT_DEFINITIONS
}


def _validate_object(
    value: Any, allowed: set[str], required: set[str] | None = None
) -> dict:
    if (
        not isinstance(value, dict)
        or set(value) - allowed
        or (required or set()) - set(value)
    ):
        raise _invalid("Invalid object fields")
    return value


def _filters(name: str, arguments: Any) -> dict:
    if name not in {event["name"] for event in EVENT_DEFINITIONS}:
        raise _invalid("Unknown event name")
    allowed = {"device", "card_id"} if name == "card.answered" else {"device"}
    value = _validate_object(arguments, allowed)
    for key, field in value.items():
        limit = 64 if key == "card_id" else 128
        if not isinstance(field, str) or not 1 <= len(field) <= limit:
            raise _invalid("Invalid event filter")
    return value


def _matches(arguments: dict, payload: dict) -> bool:
    return all(
        payload.get("device_id" if key == "device" else key) == value
        for key, value in arguments.items()
    )


class Events:
    def __init__(
        self,
        connection: sqlite3.Connection,
        *,
        clock: Callable[[], float] = time.time,
        transport: WebhookTransport | None = None,
        authorize: Callable[[str, str, dict], bool] | None = None,
    ):
        self.connection = connection
        self.clock = clock
        self.transport = transport or SafeWebhookTransport()
        self.authorize = authorize or (lambda principal, name, arguments: True)
        self._subscribe_lock = asyncio.Lock()
        self._delivery_lock = asyncio.Lock()
        self.connection.executescript("""
            CREATE TABLE IF NOT EXISTS mcp_subscriptions (
                id TEXT PRIMARY KEY, principal TEXT NOT NULL, url TEXT NOT NULL,
                name TEXT NOT NULL, arguments TEXT NOT NULL, secret TEXT NOT NULL,
                old_secrets TEXT NOT NULL DEFAULT '[]', expires_at REAL NOT NULL,
                active INTEGER NOT NULL DEFAULT 1
            );
            CREATE TABLE IF NOT EXISTS mcp_verifications (
                principal TEXT NOT NULL, url TEXT NOT NULL, secret_hash TEXT NOT NULL,
                expires_at REAL NOT NULL, PRIMARY KEY(principal,url)
            );
            CREATE TABLE IF NOT EXISTS mcp_event_records (
                event_id TEXT PRIMARY KEY, name TEXT NOT NULL, payload TEXT NOT NULL,
                principal TEXT, occurred_at REAL NOT NULL
            );
            CREATE TABLE IF NOT EXISTS mcp_outbox (
                subscription_id TEXT NOT NULL REFERENCES mcp_subscriptions(id),
                event_id TEXT NOT NULL, body BLOB NOT NULL, created_at REAL NOT NULL,
                next_attempt REAL NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
                state TEXT NOT NULL DEFAULT 'pending', last_status INTEGER,
                last_error TEXT, PRIMARY KEY(subscription_id,event_id)
            );
            CREATE INDEX IF NOT EXISTS mcp_outbox_due ON mcp_outbox(state,next_attempt);
        """)

    def _rows(self, query: str, parameters: tuple = ()) -> list[sqlite3.Row]:
        cursor = self.connection.cursor()
        cursor.row_factory = sqlite3.Row
        return cursor.execute(query, parameters).fetchall()

    @staticmethod
    def subscription_id(principal: str, url: str, name: str, arguments: dict) -> str:
        return (
            "sub_"
            + hashlib.sha256(
                canonical([principal, url, name, arguments]).encode()
            ).hexdigest()
        )

    async def dispatch(self, method: str, params: dict, principal: str) -> dict:
        if method == "events/list":
            _validate_object(params, {"cursor", "_meta"})
            if params.get("cursor") is not None:
                raise _invalid("This event catalog has no further pages")
            return {
                "events": [
                    json.loads(canonical(event))
                    for event in EVENT_DEFINITIONS
                    if self.authorize(principal, event["name"], {})
                ]
            }
        if method == "events/subscribe":
            async with self._subscribe_lock:
                return await self._subscribe(params, principal)
        if method == "events/unsubscribe":
            return self._unsubscribe(params, principal)
        raise EventError(-32601, "Method not found")

    def _identity(
        self, params: dict, principal: str, *, subscribe: bool
    ) -> tuple[str, str, dict, dict, str]:
        allowed = {"name", "arguments", "delivery", "_meta"}
        if subscribe:
            allowed |= {"cursor", "ttlMs"}
        _validate_object(params, allowed, {"name", "delivery"})
        name = params["name"]
        if not isinstance(name, str):
            raise _invalid("Event name must be a string")
        arguments = _filters(name, params.get("arguments", {}))
        if not self.authorize(principal, name, arguments):
            raise EventError(-32003, "Event access denied")
        fields = {"mode", "url", "secret"} if subscribe else {"mode", "url"}
        delivery = _validate_object(params["delivery"], fields, fields)
        if delivery["mode"] != "webhook":
            raise _invalid("Only webhook delivery is supported")
        url = delivery["url"]
        validate_url(url)
        return (
            name,
            url,
            arguments,
            delivery,
            self.subscription_id(principal, url, name, arguments),
        )

    async def _subscribe(self, params: dict, principal: str) -> dict:
        name, url, arguments, delivery, subscription_id = self._identity(
            params, principal, subscribe=True
        )
        secret = delivery["secret"]
        decode_secret(secret)
        if params.get("cursor") is not None:
            raise _invalid("These events do not support replay cursors")
        ttl_ms = params.get("ttlMs", DEFAULT_TTL_MS)
        if ttl_ms is None:
            ttl_ms = DEFAULT_TTL_MS
        if isinstance(ttl_ms, bool) or not isinstance(ttl_ms, int) or ttl_ms < 0:
            raise _invalid("ttlMs must be a nonnegative integer or null")
        ttl_ms = max(MIN_TTL_MS, min(MAX_TTL_MS, ttl_ms))
        now = self.clock()
        secret_hash = hashlib.sha256(secret.encode()).hexdigest()
        cached = self._rows(
            "SELECT * FROM mcp_verifications WHERE principal=? AND url=? AND secret_hash=? AND expires_at>?",
            (principal, url, secret_hash, now),
        )
        if not cached:
            challenge = secrets.token_urlsafe(32)
            body = canonical({"type": "verification", "challenge": challenge}).encode()
            headers = sign_headers(
                "msg_verification_" + secrets.token_hex(16),
                body,
                [secret],
                subscription_id,
                now,
            )
            try:
                async with asyncio.timeout(10):
                    response = await self.transport(url, body, headers)
                if len(response.body) > 4096:
                    raise _callback("challenge_failed")
                echoed = json.loads(response.body)
                if (
                    not 200 <= response.status < 300
                    or not isinstance(echoed, dict)
                    or not isinstance(echoed.get("challenge"), str)
                    or not hmac.compare_digest(
                        echoed["challenge"].encode(), challenge.encode()
                    )
                ):
                    raise _callback("challenge_failed")
            except EventError:
                raise
            except TimeoutError as exc:
                raise _callback("timeout") from exc
            except (ValueError, UnicodeError) as exc:
                raise _callback("challenge_failed") from exc
            except (OSError, aiohttp.ClientError) as exc:
                raise _callback("connection_failed") from exc
            if self.clock() - now >= 10:
                raise _callback("timeout")
        now = self.clock()
        old = self._rows(
            "SELECT * FROM mcp_subscriptions WHERE id=?", (subscription_id,)
        )
        old_secrets = []
        if old:
            old_secrets = [
                item
                for item in json.loads(old[0]["old_secrets"])
                if item["until"] > now and item["secret"] != secret
            ]
            if (
                old[0]["secret"] != secret
                and old[0]["active"]
                and old[0]["expires_at"] > now
            ):
                old_secrets = [
                    item for item in old_secrets if item["secret"] != old[0]["secret"]
                ]
                old_secrets.append(
                    {"secret": old[0]["secret"], "until": now + ROTATION_SECONDS}
                )
        expires_at = now + ttl_ms / 1000
        with self.connection:
            # Refresh must not revive undelivered events after the prior grant
            # expired, even if the delivery worker has not swept them yet.
            if old and old[0]["expires_at"] <= now:
                self.connection.execute(
                    "UPDATE mcp_outbox SET state='expired' WHERE subscription_id=? AND state='pending'",
                    (subscription_id,),
                )
            self.connection.execute(
                "INSERT INTO mcp_subscriptions(id,principal,url,name,arguments,secret,old_secrets,expires_at,active) VALUES(?,?,?,?,?,?,?,?,1) ON CONFLICT(id) DO UPDATE SET secret=excluded.secret,old_secrets=excluded.old_secrets,expires_at=excluded.expires_at,active=1",
                (
                    subscription_id,
                    principal,
                    url,
                    name,
                    canonical(arguments),
                    secret,
                    canonical(old_secrets),
                    expires_at,
                ),
            )
            if not cached:
                self.connection.execute(
                    "INSERT INTO mcp_verifications VALUES(?,?,?,?) ON CONFLICT(principal,url) DO UPDATE SET secret_hash=excluded.secret_hash,expires_at=excluded.expires_at",
                    (principal, url, secret_hash, now + VERIFY_CACHE_SECONDS),
                )
            self.connection.execute(
                "DELETE FROM mcp_verifications WHERE expires_at<=?", (now,)
            )
        return {
            "id": subscription_id,
            "refreshBefore": iso_time(expires_at),
            "cursor": None,
            "truncated": False,
        }

    def _unsubscribe(self, params: dict, principal: str) -> dict:
        _, _, _, _, subscription_id = self._identity(params, principal, subscribe=False)
        with self.connection:
            self.connection.execute(
                "UPDATE mcp_subscriptions SET active=0 WHERE id=?", (subscription_id,)
            )
            self.connection.execute(
                "UPDATE mcp_outbox SET state='cancelled' WHERE subscription_id=? AND state='pending'",
                (subscription_id,),
            )
        return {}

    def has_subscription(
        self, name: str, principal: str | None = None, device: str | None = None
    ) -> bool:
        for row in self._rows(
            "SELECT * FROM mcp_subscriptions WHERE active=1 AND expires_at>? AND name=?",
            (self.clock(), name),
        ):
            arguments = json.loads(row["arguments"])
            if principal is not None and row["principal"] != principal:
                continue
            if device is not None and arguments.get("device", device) != device:
                continue
            if self.authorize(row["principal"], name, arguments):
                return True
        return False

    def enqueue(
        self,
        name: str,
        payload: dict,
        principal: str | None = None,
        event_id: str | None = None,
        occurred_at: float | None = None,
    ) -> str:
        """Persist once; caller commits atomically with its source state change."""
        if name not in {event["name"] for event in EVENT_DEFINITIONS}:
            raise ValueError("Unknown event name")
        event_id = event_id or "evt_" + secrets.token_hex(16)
        now = self.clock()
        occurred_at = now if occurred_at is None else occurred_at
        payload_json = canonical(payload)
        existing = self._rows(
            "SELECT * FROM mcp_event_records WHERE event_id=?", (event_id,)
        )
        if existing:
            if (
                existing[0]["name"] != name
                or existing[0]["payload"] != payload_json
                or existing[0]["principal"] != principal
            ):
                raise ValueError("Event ID already exists with different content")
            return event_id
        body = canonical(
            {
                "eventId": event_id,
                "name": name,
                "timestamp": iso_time(occurred_at),
                "data": payload,
                "cursor": None,
            }
        ).encode()
        if len(body) > MAX_BODY:
            raise ValueError("Event payload exceeds 256 KiB")
        if not PAYLOAD_VALIDATORS[name].is_valid(payload):
            raise ValueError("Event data does not match its payload schema")
        if name == "card.answered":
            try:
                answered_at = datetime.fromisoformat(payload["answered_at"])
                if answered_at.tzinfo is None:
                    raise ValueError
            except ValueError as exc:
                raise ValueError(
                    "Event answer timestamp needs an ISO timezone"
                ) from exc
            has_options = {"option_id", "option_label"}.intersection(payload)
            if (payload["kind"] == "option" and len(has_options) != 2) or (
                payload["kind"] == "dismissed" and has_options
            ):
                raise ValueError("Event answer fields do not match its kind")
        self.connection.execute(
            "INSERT INTO mcp_event_records VALUES(?,?,?,?,?)",
            (event_id, name, payload_json, principal, occurred_at),
        )
        for row in self._rows(
            "SELECT * FROM mcp_subscriptions WHERE active=1 AND expires_at>? AND name=?",
            (now, name),
        ):
            arguments = json.loads(row["arguments"])
            if principal is not None and row["principal"] != principal:
                continue
            if not _matches(arguments, payload) or not self.authorize(
                row["principal"], name, arguments
            ):
                continue
            self.connection.execute(
                "INSERT INTO mcp_outbox(subscription_id,event_id,body,created_at,next_attempt) VALUES(?,?,?,?,?)",
                (row["id"], event_id, body, now, now),
            )
        return event_id

    async def deliver_due(self) -> int:
        async with self._delivery_lock:
            return await self._deliver_due()

    async def _deliver_due(self) -> int:
        now = self.clock()
        with self.connection:
            self.connection.execute(
                "UPDATE mcp_outbox SET state='expired' WHERE state='pending' AND subscription_id IN (SELECT id FROM mcp_subscriptions WHERE expires_at<=?)",
                (now,),
            )
            self.connection.execute(
                "UPDATE mcp_outbox SET state='cancelled' WHERE state='pending' AND subscription_id IN (SELECT id FROM mcp_subscriptions WHERE active=0)"
            )
            self.connection.execute(
                "UPDATE mcp_outbox SET state='failed',last_error='retry_exhausted' WHERE state='pending' AND (created_at+?<=? OR attempts>=?)",
                (RETRY_SECONDS, now, MAX_ATTEMPTS),
            )
        due = self._rows(
            "SELECT o.*,s.url,s.secret,s.old_secrets,s.principal,s.name,s.arguments,s.expires_at FROM mcp_outbox o JOIN mcp_subscriptions s ON o.subscription_id=s.id WHERE o.state='pending' AND o.next_attempt<=? AND s.active=1 ORDER BY o.next_attempt LIMIT 100",
            (now,),
        )
        attempted = 0
        for candidate in due:
            now = self.clock()
            # Earlier deliveries yielded to refresh/rotation/unsubscribe calls.
            # Fetch the current grant and keys just before claiming and signing.
            current = self._rows(
                "SELECT o.*,s.url,s.secret,s.old_secrets,s.principal,s.name,s.arguments,s.expires_at "
                "FROM mcp_outbox o JOIN mcp_subscriptions s ON o.subscription_id=s.id "
                "WHERE o.subscription_id=? AND o.event_id=? AND o.state='pending' "
                "AND o.next_attempt<=? AND o.created_at+?>? AND o.attempts<? "
                "AND s.active=1 AND s.expires_at>?",
                (
                    candidate["subscription_id"],
                    candidate["event_id"],
                    now,
                    RETRY_SECONDS,
                    now,
                    MAX_ATTEMPTS,
                    now,
                ),
            )
            if not current:
                continue
            row = current[0]
            if not self.authorize(
                row["principal"], row["name"], json.loads(row["arguments"])
            ):
                with self.connection:
                    self.connection.execute(
                        "UPDATE mcp_subscriptions SET active=0 WHERE id=?",
                        (row["subscription_id"],),
                    )
                    self.connection.execute(
                        "UPDATE mcp_outbox SET state='revoked' WHERE subscription_id=? AND state='pending'",
                        (row["subscription_id"],),
                    )
                continue
            with self.connection:
                claimed = self.connection.execute(
                    "UPDATE mcp_outbox SET attempts=attempts+1,next_attempt=? WHERE subscription_id=? AND event_id=? AND state='pending' AND next_attempt<=? AND EXISTS(SELECT 1 FROM mcp_subscriptions WHERE id=? AND active=1 AND expires_at>?)",
                    (
                        now + 30,
                        row["subscription_id"],
                        row["event_id"],
                        now,
                        row["subscription_id"],
                        now,
                    ),
                ).rowcount
            if not claimed:
                continue
            # The claim commits before the HTTP call; a crash retries after its lease.
            attempted += 1
            keys = [row["secret"]] + [
                item["secret"]
                for item in json.loads(row["old_secrets"])
                if item["until"] > now
            ]
            headers = sign_headers(
                row["event_id"], row["body"], keys, row["subscription_id"], now
            )
            status = None
            error = None
            try:
                async with asyncio.timeout(10):
                    response = await self.transport(row["url"], row["body"], headers)
                status = response.status
            except (TimeoutError, OSError, aiohttp.ClientError):
                error = "transport_failure"
            except EventError as exc:
                error = (exc.data or {}).get("reason", "callback_rejected")
            now = self.clock()
            attempts = row["attempts"] + 1
            transient = status is None or status in {408, 425, 429} or status >= 500
            if status is not None and 200 <= status < 300:
                state = "delivered"
            elif (
                transient
                and attempts < MAX_ATTEMPTS
                and now < row["created_at"] + RETRY_SECONDS
            ):
                state = "pending"
            else:
                state = "failed"
            next_attempt = now + min(2 ** (attempts - 1), 120)
            with self.connection:
                self.connection.execute(
                    "UPDATE mcp_outbox SET state=?,next_attempt=?,last_status=?,last_error=? WHERE subscription_id=? AND event_id=? AND state='pending'",
                    (
                        state,
                        next_attempt,
                        status,
                        error,
                        row["subscription_id"],
                        row["event_id"],
                    ),
                )
                if status == 410:
                    self.connection.execute(
                        "UPDATE mcp_subscriptions SET active=0 WHERE id=?",
                        (row["subscription_id"],),
                    )
                    self.connection.execute(
                        "UPDATE mcp_outbox SET state='cancelled' WHERE subscription_id=? AND state='pending'",
                        (row["subscription_id"],),
                    )
        return attempted

    def status(self, principal: str) -> dict:
        now = self.clock()
        subscriptions = [
            {
                "id": row["id"],
                "name": row["name"],
                "arguments": json.loads(row["arguments"]),
                "active": bool(row["active"] and row["expires_at"] > now),
                "refreshBefore": iso_time(row["expires_at"]),
            }
            for row in self._rows(
                "SELECT * FROM mcp_subscriptions WHERE principal=? ORDER BY id",
                (principal,),
            )
        ]
        outbox = {
            row["state"]: row["count"]
            for row in self._rows(
                "SELECT o.state,COUNT(*) AS count FROM mcp_outbox o JOIN mcp_subscriptions s ON o.subscription_id=s.id WHERE s.principal=? GROUP BY o.state",
                (principal,),
            )
        }
        return {"subscriptions": subscriptions, "outbox": outbox}
