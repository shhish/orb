"""Authenticated loopback Claude hooks with cancellable approval waits.

No transcript paths are opened. Tool bodies are passed only to the store's
redactor, never retained in events or logs. A digest temporarily correlates
PostToolUse with pending PermissionRequest hooks when the desktop wins a race.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import math
import secrets
from contextlib import suppress
from dataclasses import replace
from datetime import UTC, datetime
from typing import TYPE_CHECKING, Any

from .installer import EVENTS, HEADER, HOOK_PATH, validate_secret
from .models import Session, summary
from .store import delivery_receipt

if TYPE_CHECKING:
    from .store import StateStore
from .tasks import spawn


class HookPayloadError(ValueError):
    """A hook payload is not a valid JSON object."""


class _HTTPError(Exception):
    def __init__(self, status: int):
        self.status = status


def _text(payload: dict, key: str, *, maximum: int = 4096) -> str:
    value = payload.get(key, "")
    if not isinstance(value, str) or len(value) > maximum:
        raise ValueError(f"Invalid {key}")
    return value


def _digest(value: Any) -> bytes:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).digest()


class HookServer:
    """Serve hooks at ``127.0.0.1`` until the async context exits.

    ``handle`` can also be mounted in an existing hub HTTP app; that app must
    perform equivalent authorization, origin, size, and disconnect checks.
    The server deliberately has no approve/send HTTP route: authorization of
    device actions belongs to the hub.
    """

    def __init__(
        self,
        store: StateStore,
        secret: str,
        port: int = 0,
        approval_timeout: float = 120,
        *,
        max_body_bytes: int = 1024 * 1024,
        request_timeout: float = 10,
        max_connections: int = 64,
        task_owner=None,
    ) -> None:
        validate_secret(secret)
        if not 0 <= port <= 65535:
            raise ValueError("Invalid port")
        if (
            not math.isfinite(approval_timeout)
            or not math.isfinite(request_timeout)
            or approval_timeout <= 0
            or request_timeout <= 0
        ):
            raise ValueError("Timeouts must be positive")
        if max_body_bytes <= 0 or max_connections <= 0:
            raise ValueError("Limits must be positive")
        self.task_owner = task_owner
        self.store = store
        self._secret = secret.encode("ascii")
        self.port = port
        self.approval_timeout = approval_timeout
        self.max_body_bytes = max_body_bytes
        self.request_timeout = request_timeout
        self.max_connections = max_connections
        self._server: asyncio.Server | None = None
        self._clients: set[asyncio.Task] = set()
        self._pending: dict[str, tuple[str, str, bytes]] = {}

    @property
    def url(self) -> str:
        if self._server is None:
            raise RuntimeError("Hook server is not running")
        return f"http://127.0.0.1:{self.port}{HOOK_PATH}"

    async def __aenter__(self) -> HookServer:
        if self._server is not None:
            raise RuntimeError("Hook server is already running")
        self._server = await asyncio.start_server(
            self._accept, "127.0.0.1", self.port, limit=16384
        )
        self.port = self._server.sockets[0].getsockname()[1]
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()

    async def close(self) -> None:
        server, self._server = self._server, None
        if server is not None:
            server.close()
        clients = tuple(self._clients)
        for task in clients:
            task.cancel()
        await asyncio.gather(*clients, return_exceptions=True)
        if server is not None:
            await server.wait_closed()

    def _accept(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        if len(self._clients) >= self.max_connections:
            writer.close()
            return
        task = spawn(
            self._serve(reader, writer), name="orb-hook-client", owner=self.task_owner
        )
        self._clients.add(task)
        task.add_done_callback(self._clients.discard)

    async def _request(self, reader: asyncio.StreamReader) -> dict:
        async with asyncio.timeout(self.request_timeout):
            try:
                head = await reader.readuntil(b"\r\n\r\n")
            except (asyncio.LimitOverrunError, asyncio.IncompleteReadError):
                raise _HTTPError(400) from None
            if len(head) > 16384:
                raise _HTTPError(431)
            try:
                lines = head.decode("ascii").split("\r\n")
                method, path, version = lines[0].split(" ")
            except (UnicodeDecodeError, ValueError):
                raise _HTTPError(400) from None
            if method != "POST":
                raise _HTTPError(405)
            if path != HOOK_PATH:
                raise _HTTPError(404)
            if version != "HTTP/1.1":
                raise _HTTPError(400)
            headers: dict[str, str] = {}
            for line in lines[1:-2]:
                key, sep, value = line.partition(":")
                if (
                    not sep
                    or not key
                    or any(
                        c
                        not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-"
                        for c in key
                    )
                    or key.lower() in headers
                ):
                    raise _HTTPError(400)
                headers[key.lower()] = value.strip()
            if "origin" in headers:
                raise _HTTPError(403)
            if headers.get("host") != f"127.0.0.1:{self.port}":
                raise _HTTPError(403)
            if not secrets.compare_digest(
                headers.get(HEADER.lower(), "").encode("ascii"), self._secret
            ):
                raise _HTTPError(401)
            if "transfer-encoding" in headers:
                raise _HTTPError(400)
            if (
                headers.get("content-type", "").split(";", 1)[0].strip().lower()
                != "application/json"
            ):
                raise _HTTPError(415)
            raw_length = headers.get("content-length", "")
            if not raw_length.isascii() or not raw_length.isdecimal():
                raise _HTTPError(400)
            length = int(raw_length)
            if length > self.max_body_bytes:
                raise _HTTPError(413)
            try:
                body = await reader.readexactly(length)
                payload = json.loads(body)
            except (
                asyncio.IncompleteReadError,
                ValueError,
                UnicodeDecodeError,
                RecursionError,
            ):
                raise _HTTPError(400) from None
            if not isinstance(payload, dict):
                raise _HTTPError(400)
            return payload

    async def _serve(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        handler: asyncio.Task | None = None
        disconnected: asyncio.Task | None = None
        try:
            payload = await self._request(reader)
            # The decision future only wakes the handler. A receipt covers the
            # HTTP write as well so the device cannot claim earlier delivery.
            with delivery_receipt() as delivery:
                handler = spawn(
                    self.handle(payload, delivery=delivery),
                    name="orb-hook-request",
                    owner=self.task_owner,
                )
                disconnected = spawn(
                    reader.read(1), name="orb-hook-disconnect", owner=self.task_owner
                )
                done, _ = await asyncio.wait(
                    (handler, disconnected), return_when=asyncio.FIRST_COMPLETED
                )
                if disconnected in done:
                    # EOF or pipelining cancels this request. Neither can approve it.
                    raise ConnectionResetError("Hook client disconnected")
                response = await handler
                await self._respond(writer, 200, response)
        except _HTTPError as error:
            await self._respond(writer, error.status)
        except TimeoutError:
            await self._respond(writer, 408)
        except ValueError:
            await self._respond(writer, 400)
        except (ConnectionError, OSError):
            pass
        except Exception:
            # Do not expose provider data or tool bodies in exception responses.
            await self._respond(writer, 500)
        finally:
            for task in (handler, disconnected):
                if task is not None and not task.done():
                    task.cancel()
            await asyncio.gather(
                *(task for task in (handler, disconnected) if task is not None),
                return_exceptions=True,
            )
            writer.close()
            with suppress(ConnectionError, OSError):
                await writer.wait_closed()

    @staticmethod
    async def _respond(
        writer: asyncio.StreamWriter, status: int, data: dict | None = None
    ) -> None:
        body = json.dumps(
            data if data is not None else {"error": "Hook request rejected"}
        ).encode()
        writer.write(
            f"HTTP/1.1 {status} Response\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode()
            + body
        )
        async with asyncio.timeout(5):
            await writer.drain()

    async def handle(
        self, payload: dict, *, delivery: asyncio.Future[None] | None = None
    ) -> dict:
        """Handle an authenticated payload; HTTP embedders supply a write receipt."""
        if not isinstance(payload, dict):
            raise HookPayloadError("Expected a JSON object")
        raw_id = _text(payload, "session_id", maximum=256)
        event = _text(payload, "hook_event_name", maximum=64)
        cwd = _text(payload, "cwd")
        if not raw_id or event not in EVENTS:
            raise ValueError("Invalid session id or hook event")
        tool = ""
        tool_input = {}
        if event in {"PermissionRequest", "PostToolUse", "PostToolUseFailure"}:
            tool = _text(payload, "tool_name", maximum=256)
            tool_input = payload.get("tool_input", {})
            if not tool or not isinstance(tool_input, dict):
                raise ValueError("Invalid tool request")
        session_id = f"claude:{raw_id}"
        session = self.store.get(session_id)
        if session is None:
            session = Session(
                id=session_id,
                kind="claude",
                origin="cli",
                title="",
                cwd=cwd,
            )
        session = replace(session, cwd=cwd or session.cwd, updated_at=datetime.now(UTC))
        if event == "UserPromptSubmit" and not session.title:
            prompt = payload.get("prompt")
            if isinstance(prompt, str):
                # Redact before bounding, and retain the first prompt rather
                # than renaming a session after each turn or notification.
                session = replace(
                    session, title=summary(prompt, 120), title_source="hook"
                )
        session = self.store.update(session)

        if event == "PermissionRequest":
            if tool == "AskUserQuestion":
                # A question needs an answer, not permission to return empty
                # answers. Existing CLI/desktop sessions keep their native UI.
                # Owned stdio sessions answer through their control transport.
                self.store.update(
                    replace(
                        session,
                        state="waiting_input",
                        waiting_for="Answer this question in Claude",
                    )
                )
                return {}
            request_id = f"hook:{secrets.token_urlsafe(18)}"
            self._pending[request_id] = (session_id, tool, _digest(tool_input))
            try:
                answer = await self.store.request_approval(
                    session_id,
                    tool,
                    tool_input,
                    request_id=request_id,
                    timeout=self.approval_timeout,
                    delivery=delivery,
                )
            finally:
                self._pending.pop(request_id, None)
            decision = {"behavior": answer.decision}
            if answer.decision == "deny" and answer.message:
                decision["message"] = answer.message
            return {
                "hookSpecificOutput": {"hookEventName": event, "decision": decision}
            }

        if event in {"PostToolUse", "PostToolUseFailure"}:
            key = (session_id, tool, _digest(tool_input))
            matching = [rid for rid, pending in self._pending.items() if pending == key]
            # PermissionRequest lacks tool_use_id. Ambiguous identical parallel
            # calls cannot safely be correlated; connection cancellation/expiry
            # will resolve those instead.
            if len(matching) == 1:
                self.store.resolve(
                    matching[0],
                    decision="allow" if event == "PostToolUse" else "deny",
                    reason="external",
                )
            return {}

        if event == "SessionEnd":
            self.store.resolve_session(session_id, reason="session_ended")
            self.store.update(replace(session, state="done", waiting_for=None))
        elif event == "Stop":
            final_text = _text(payload, "last_assistant_message", maximum=1024 * 1024)
            if session.origin == "hub-started":
                # The owned stream's result is authoritative. Publishing an
                # early completion here allows a next prompt to be sent before
                # the prior result, which could then make the new turn idle.
                self.store.update(
                    replace(
                        session,
                        last_assistant_message=final_text
                        or session.last_assistant_message,
                    )
                )
            else:
                self.store.resolve_session(session_id, reason="turn_completed")
                self.store.turn_complete(session_id, summary=final_text)
        elif event == "StopFailure":
            self.store.resolve_session(session_id, reason="turn_failed")
            error = (
                "Claude authentication is unavailable in this configuration"
                if payload.get("error") == "authentication_failed"
                else "Claude turn failed"
            )
            self.store.fail_session(session_id, error)
        elif event == "UserPromptSubmit":
            self.store.update(replace(session, state="working", waiting_for=None))
        elif event == "SessionStart":
            self.store.update(replace(session, state="idle", waiting_for=None))
        elif event == "Notification":
            notification = _text(payload, "notification_type", maximum=128)
            if notification == "permission_prompt":
                self.store.update(
                    replace(
                        session,
                        state="needs_approval",
                        waiting_for="Permission in Claude",
                    )
                )
            elif notification == "idle_prompt":
                self.store.update(
                    replace(session, state="waiting_input", waiting_for="User input")
                )
        return {}
