"""Provider discovery and one-shot attention routing for the device SessionHub.

Only this process's owned transports accept messages. Observation never resumes
an existing conversation, and generating a hook snippet never installs it.
"""

from __future__ import annotations

import asyncio
import hashlib
import math
import os
import secrets
import stat
from pathlib import Path
from typing import TYPE_CHECKING

from orb_hub.envelope import LinkError
from orb_hub.lifecycle import LifecycleSupervisor

if TYPE_CHECKING:
    from orb_agents import AgentManager, ApprovalRequest, Event, Session

HOOK_TOKEN_PLACEHOLDER = "REPLACE_WITH_PRIVATE_ORB_HOOK_TOKEN"
DEFAULT_HOOKS_PORT = 8447
MAX_SESSIONS = 24


def _clip(value: object, maximum: int) -> str:
    # The provider redactor omits code/tool bodies before this byte-boundary trim.
    from orb_agents.models import summary

    return summary(value, max(maximum, 2)).encode("utf-8", errors="replace")[:maximum].decode("utf-8", errors="ignore")


def session_id(native_id: str) -> str:
    """Keep ordinary UUID identities readable; hash long provider IDs stably."""
    if len(native_id.encode("utf-8")) <= 48:
        return native_id
    prefix = native_id.split(":", 1)[0]
    if prefix not in {"claude", "codex"}:
        prefix = "agent"
    return f"{prefix}:{hashlib.sha256(native_id.encode()).hexdigest()[:40]}"


def hooks_snippet(port: int = DEFAULT_HOOKS_PORT) -> dict:
    """Print-safe settings fragment; the owner substitutes the private token."""
    from orb_agents.installer import generate_hooks

    return generate_hooks(f"http://127.0.0.1:{port}/hooks/claude", HOOK_TOKEN_PLACEHOLDER)


def _load_secret(directory: Path) -> str:
    from orb_agents.installer import validate_secret

    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    if directory.is_symlink():
        raise ValueError("Agent state directory must not be a symlink")
    directory.chmod(0o700)
    token_file = directory / "hook.token"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    try:
        descriptor = os.open(token_file, flags, 0o600)
    except FileExistsError:
        descriptor = os.open(token_file, os.O_RDONLY | os.O_NOFOLLOW)
        with os.fdopen(descriptor, "r", encoding="ascii") as stream:
            metadata = os.fstat(stream.fileno())
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_mode & 0o077:
                raise ValueError("Agent hook token must be a private regular file")
            secret = stream.read(257).strip()
    else:
        secret = secrets.token_urlsafe(32)
        with os.fdopen(descriptor, "w", encoding="ascii") as stream:
            stream.write(secret + "\n")
    validate_secret(secret)
    return secret


class AgentsBridge:
    def __init__(
        self,
        sessions,
        data_dir: Path,
        *,
        hooks_port: int = DEFAULT_HOOKS_PORT,
        poll_interval: float = 2.0,
        manager: AgentManager | None = None,
        hooks_secret: str | None = None,
        enable_codex_queue: bool = False,
        claude_usage: bool = False,
        claude_binary: Path | None = None,
    ):
        if not math.isfinite(poll_interval) or poll_interval <= 0:
            raise ValueError("Polling interval must be positive")
        self.sessions = sessions
        self.data_dir = Path(data_dir)
        self.hooks_port = hooks_port
        self.poll_interval = poll_interval
        self.manager = manager
        self.enable_codex_queue = enable_codex_queue
        self.claude_usage = claude_usage
        self.claude_binary = claude_binary
        self._data = None
        self._hooks_secret = hooks_secret
        self.hooks = None
        self.supervisor = LifecycleSupervisor("agents")
        self._lock = asyncio.Lock()
        self._start_lock = asyncio.Lock()
        self._pending_starts = 0
        self._published: set[str] = set()
        self._approval_cards: dict[str, tuple[ApprovalRequest, str]] = {}
        self._waiting_cards: dict[str, tuple[str, str]] = {}
        self._started = False
        self._closed = False
        self._last_error: str | None = None

    @property
    def token_file(self) -> Path:
        return self.data_dir / "agents" / "hook.token"

    async def start(self) -> None:
        if self._closed:
            raise RuntimeError("Agent bridge is closed")
        if self._started:
            return
        from orb_agents import AgentManager
        from orb_agents.hooks import HookServer

        self.manager = self.manager or AgentManager(
            poll_interval=self.poll_interval, enable_codex_queue=self.enable_codex_queue, task_owner=self.supervisor
        )
        if hasattr(self.manager, "bind_task_owner"):
            self.manager.bind_task_owner(self.supervisor)
        from orb_hub.agent_data import AgentData

        self._data = AgentData(self.manager, self.data_dir)
        secret = self._hooks_secret or await asyncio.to_thread(_load_secret, self.token_file.parent)
        self.hooks = HookServer(self.manager.store, secret, port=self.hooks_port, task_owner=self.supervisor)
        try:
            await self.hooks.__aenter__()
            self.hooks_port = self.hooks.port
            # Subscribe before refresh can emit its first sessions/approvals.
            self.supervisor.run(self._events, name="orb-hub-agent-events")
            await asyncio.sleep(0)
            await self.manager.__aenter__()
            await self.reconcile()
            if self.claude_usage:
                from orb_agents.usage import ClaudeUsagePoller

                poller = ClaudeUsagePoller(self._data.usage, binary=self.claude_binary)
                self.supervisor.run(poller.run, name="orb-hub-claude-usage")
            self.supervisor.run(self._periodic, name="orb-hub-agent-reconcile")
            self.supervisor.run(self._progress, name="orb-hub-agent-progress")
            self._started = True
        except BaseException:
            await self.stop()
            raise

    @property
    def _tasks(self):
        # Compatibility for callers that inspect shutdown completion.
        return self.supervisor.tasks

    async def _events(self) -> None:
        async for event in self.manager.events():
            await self._event(event)

    async def _event(self, event: Event) -> None:
        # Reconciliation also repairs an orb_agents subscriber stream.gap. It
        # reads only immutable metadata and live approvals, never full rollouts.
        try:
            await self.reconcile()
        except (LinkError, OSError, ValueError, RuntimeError) as exc:
            self._last_error = f"Agent publication unavailable ({type(exc).__name__})"

    async def _periodic(self) -> None:
        while True:
            await asyncio.sleep(self.poll_interval)
            try:
                await self.reconcile()
            except (LinkError, OSError, ValueError, RuntimeError) as exc:
                self._last_error = f"Agent publication unavailable ({type(exc).__name__})"

    def _owned(self, session: Session) -> bool:
        adapter = self.manager.adapters.get(session.kind)
        return session.origin == "hub-started" and adapter is not None and adapter.owns(session.id)

    def _can_message(self, session: Session) -> bool:
        return not self._closed and self.manager.can_message(session.id)

    async def _progress(self):
        if not hasattr(self.sessions, "upsert_stream"):
            return
        while True:
            try:
                values = self.manager.store.sessions()
                await self._data.refresh(values)
                # Reconcile and background reads share immutable provider
                # values; this publication never performs provider writes.
                for value in values:
                    key = session_id(value.id)
                    if key in self.sessions.sessions:
                        self.sessions.upsert_stream(self._data.snapshot(value, key))
                self.sessions.set_usage(self._data.usage.snapshot())
            except (OSError, ValueError, RuntimeError, LinkError) as exc:
                self._last_error = f"Agent progress unavailable ({type(exc).__name__})"
            await asyncio.sleep(0.25)

    def _session(self, value: Session) -> dict:
        return {
            "id": session_id(value.id),
            "agent": value.kind,
            "title": _clip(value.title, 64),
            "project": _clip(Path(value.cwd).name, 32),
            "state": {"working": "busy", "needs_approval": "waiting", "waiting_input": "waiting"}.get(
                value.state, value.state
            ),
            "reason": _clip(value.waiting_for, 64),
            "summary": _clip(value.last_assistant_message, 160),
            "updated": value.updated_at.timestamp(),
            "can_message": self._can_message(value),
        }

    def _message_handler(self, native_id: str):
        async def send(device_id: str, text: str, stream=None):
            from orb_agents import AgentError

            value = self.manager.store.get(native_id)
            if value is None or not self._can_message(value):
                raise LinkError("unsupported", "This session cannot receive messages")
            try:
                receipt = await self.manager.send(native_id, text)
            except AgentError as exc:
                code = exc.code if exc.code in {"not_found", "unsupported", "bad_request"} else "busy"
                reasons = {
                    "delivery_failed": "Provider rejected message delivery",
                    "timeout": "Agent delivery timed out",
                    "session_unavailable": "Agent transport is unavailable",
                    "unsupported": "Agent delivery is disabled",
                }
                raise LinkError(code, reasons.get(exc.code, "Agent could not accept this message")) from None
            return {
                "accepted": True,
                "status": receipt.status if receipt else "delivered",
                "reason": receipt.reason if receipt else "Owned provider transport accepted the message",
            }

        return send

    async def reconcile(self) -> None:
        if self.manager is None:
            return
        async with self._lock:
            approvals = self.manager.pending_approvals()
            pending_requests = {request.request_id: request for request in approvals}
            pending_sessions = {request.session_id for request in approvals}
            source = self.manager.store.sessions()
            if self._pending_starts:
                # Preserve current metadata while the start caller owns an
                # unassigned reserved slot. New provider events wait until its
                # returned identity can be published with that reservation.
                source = [value for value in source if value.id in self._published]
            values = sorted(
                source,
                key=lambda value: (
                    # A new permission must not be crowded out by 24 owned
                    # working sessions before it can publish its first card.
                    value.id in pending_sessions or value.state in {"needs_approval", "waiting_input"},
                    value.state not in {"done", "error"},
                    self._owned(value),
                    value.updated_at,
                    value.id,
                ),
                reverse=True,
            )[:MAX_SESSIONS]
            selected = {value.id for value in values}
            waiting_ids = {
                value.id
                for value in values
                if value.state == "waiting_input"
                or (value.state == "needs_approval" and value.id not in pending_sessions)
            }
            for native_id in self._published - selected:
                self.sessions.unregister_message_handler(session_id(native_id))
                await self.sessions.remove_session(session_id(native_id))
                self._published.discard(native_id)
            for request_id, (request, card_id) in list(self._approval_cards.items()):
                if pending_requests.get(request_id) is not request:
                    self._approval_cards.pop(request_id)
                    await self.sessions.cancel_card(card_id)
            for native_id in list(self._waiting_cards):
                value = self.manager.store.get(native_id)
                if native_id not in waiting_ids or value is None:
                    _, card_id = self._waiting_cards.pop(native_id)
                    await self.sessions.cancel_card(card_id)
            for value in values:
                try:
                    await self.sessions.upsert_session(self._session(value))
                except LinkError as exc:
                    if exc.code != "busy":
                        raise
                    continue
                self._published.add(value.id)
                wire_id = session_id(value.id)
                if self._can_message(value):
                    self.sessions.register_message_handler(wire_id, self._message_handler(value.id))
                else:
                    self.sessions.unregister_message_handler(wire_id)
                if value.id in waiting_ids:
                    await self._waiting(value)
            for request in approvals:
                if request.session_id in self._published and request.request_id not in self._approval_cards:
                    await self._approval(request)
            self._last_error = None

    async def _approval(self, request: ApprovalRequest) -> None:
        # RPC IDs may be reused after resolution. This published card belongs
        # only to the captured immutable request, including its grant scope.
        card_id = "approval:" + secrets.token_hex(24)
        supported = request.scope in {"operation", "turn"}

        async def answer(value: dict):
            from orb_agents import AgentError

            if not supported:
                if value.get("option") == "dismiss":
                    return
                raise LinkError("unsupported", "Review this permission in the agent application")
            if value.get("option") not in {"allow", "deny"}:
                raise LinkError("bad_request", "Unknown permission decision")
            try:
                await self.manager.approve(request.request_id, value["option"], expected_request=request)
            except AgentError as exc:
                if exc.code == "delivery_failed":
                    raise LinkError("internal", "Permission response could not be delivered") from None
                raise LinkError("expired", "Permission request is no longer pending") from None

        turn_scope = request.scope == "turn"
        card = {
            "id": card_id,
            "session": session_id(request.session_id),
            "kind": "approval" if supported else "attention",
            "title": _clip(f"Approve {request.tool}", 64),
            "body": (
                "Allow this request and subsequent operations in the current turn?"
                if turn_scope
                else "Allow this request for the current operation?"
            ),
            "detail": _clip(request.summary, 200),
            "options": [
                # HIG §7: permission grants have no default/tinted action.
                {"id": "allow", "label": "Allow for turn" if turn_scope else "Allow once", "role": "default"},
                {"id": "deny", "label": "Deny", "role": "cancel"},
            ],
            "reply": False,
            "priority": "high",
        }
        if supported:
            card["scope"] = request.scope
        else:
            card.update(
                title="Review permission",
                body="This permission scope cannot be approved here. Review it in the agent application.",
                options=[{"id": "dismiss", "label": "Dismiss", "role": "cancel"}],
            )
        if getattr(request, "expires_at", None) is not None:
            card["expires"] = request.expires_at.timestamp()
        try:
            await self.sessions.show_card(card, answer_handler=answer)
        except LinkError as exc:
            if exc.code != "busy":
                raise
            return
        self._approval_cards[request.request_id] = (request, card_id)

    async def _waiting(self, value: Session) -> None:
        # A question identity, unlike display text or timestamps, survives
        # metadata refresh and lets the adapter atomically reject stale replies.
        fingerprint = hashlib.sha256(
            f"{value.id}\0{value.question_id or (value.waiting_for, value.updated_at.isoformat())}".encode()
        ).hexdigest()[:54]
        current = self._waiting_cards.get(value.id)
        if current and current[0] == fingerprint:
            return
        if current:
            await self.sessions.cancel_card(current[1])
            self._waiting_cards.pop(value.id, None)
        card_id = f"question:{fingerprint}"
        # A pending provider question has its own bound reply transport. Generic
        # turn-start capability can be unavailable while that question is open.
        can_message = not self._closed and self._owned(value) and value.question_id is not None

        async def answer(result: dict):
            from orb_agents import AgentError

            if "text" in result and can_message:
                try:
                    await self.manager.answer_question(value.id, value.question_id, result["text"])
                except AgentError as exc:
                    code = (
                        "expired" if exc.code in {"stale_question", "not_found", "session_unavailable"} else "internal"
                    )
                    raise LinkError(code, "Question answer could not be delivered") from None
            elif result.get("option") != "dismiss":
                raise LinkError("unsupported", "Answer this question in the agent application")

        card = {
            "id": card_id,
            "session": session_id(value.id),
            "kind": "choice" if can_message else "attention",
            "title": "Question" if can_message else "Agent needs you",
            "body": _clip(value.waiting_for or "Answer in the agent application", 280),
            "options": [] if can_message else [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}],
            "reply": can_message,
            "priority": "normal",
        }
        try:
            await self.sessions.show_card(card, answer_handler=answer)
        except LinkError as exc:
            if exc.code != "busy":
                raise
            return
        self._waiting_cards[value.id] = (fingerprint, card_id)

    async def start_session(self, kind: str, cwd: str, prompt: str, model: str | None = None) -> dict:
        from orb_agents import AgentError

        async with self._start_lock:
            if not self._started or self._closed:
                raise LinkError("unsupported", "Agent integration is not running")
            # Reserve before process/thread creation. Another provider cannot
            # fill this slot while its startup performs asynchronous I/O.
            reservation = self.sessions.reserve_session_slot()
            self._pending_starts += 1
            try:
                try:
                    value = await self.manager.start(kind, cwd, prompt, model)
                except AgentError as exc:
                    code = exc.code if exc.code in {"unsupported", "not_found", "bad_request"} else "internal"
                    raise LinkError(code, "Agent session could not be started") from None
                except ValueError:
                    raise LinkError("bad_request", "Agent session parameters are invalid") from None
                if self._closed:
                    raise LinkError("unsupported", "Agent integration is not running")
                async with self._lock:
                    wire_value = self._session(value)
                    await self.sessions.upsert_session(wire_value, reservation=reservation)
                    self._published.add(value.id)
                    if self._can_message(value):
                        self.sessions.register_message_handler(wire_value["id"], self._message_handler(value.id))
            finally:
                self._pending_starts -= 1
                self.sessions.release_session_slot(reservation)
            await self.reconcile()
            return self._session(self.manager.store.get(value.id) or value)

    async def status(self) -> dict:
        return {
            "running": self._started and not self._closed,
            "hook_url": self.hooks.url if self.hooks and self._started and not self._closed else None,
            "token_file": str(self.token_file),
            "health": dict(self.manager.health) if self.manager else {},
            "publication_error": self._last_error,
            "workers": self.supervisor.health(),
            "sessions": len(self._published),
            "pending_approvals": len(self.manager.pending_approvals()) if self.manager else 0,
            "pending_starts": self._pending_starts,
        }

    def hooks_snippet(self) -> dict:
        return hooks_snippet(self.hooks_port)

    async def stop(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self.hooks:
            await self.hooks.close()
        if self.manager:
            await self.manager.close()
            await self.reconcile()
        await self.supervisor.stop()
        for _, card_id in list(self._approval_cards.values()):
            await self.sessions.cancel_card(card_id)
        for _, card_id in list(self._waiting_cards.values()):
            await self.sessions.cancel_card(card_id)
        for native_id in self._published:
            self.sessions.unregister_message_handler(session_id(native_id))
            await self.sessions.remove_session(session_id(native_id))
        self._approval_cards.clear()
        self._waiting_cards.clear()
        self._published.clear()
        self._started = False
