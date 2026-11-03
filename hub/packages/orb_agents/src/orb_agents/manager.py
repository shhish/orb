"""Public asyncio facade; independent of the embedding hub's HTTP framework."""

from __future__ import annotations

import asyncio
import math
from collections.abc import AsyncIterator, Mapping
from pathlib import Path
from typing import Protocol

from .models import (
    AgentError,
    ApprovalRequest,
    DeliveryResult,
    Event,
    Kind,
    Session,
    SessionUnavailable,
)
from .store import StateStore
from .tasks import spawn


class Adapter(Protocol):
    async def refresh(self) -> None: ...
    async def start(
        self, cwd: str, prompt: str, model: str | None = None
    ) -> Session: ...
    async def send(self, session_id: str, text: str) -> DeliveryResult | None: ...
    async def answer_question(
        self, session_id: str, question_id: str, text: str
    ) -> None: ...
    async def close(self) -> None: ...
    def owns(self, session_id: str) -> bool: ...


class AgentManager:
    """One manager per hub process, used from one asyncio event loop.

    Constructing has no I/O. Entering the context starts read-only discovery;
    starting a session is always explicit. Inject adapters for tests or to select
    executable/configuration directories. Adapters must share ``store``.
    """

    def __init__(
        self,
        *,
        store: StateStore | None = None,
        adapters: Mapping[Kind, Adapter] | None = None,
        poll_interval: float = 2.0,
        refresh_timeout: float = 15.0,
        enable_codex_queue: bool = False,
        task_owner=None,
    ):
        if not all(
            math.isfinite(v) and v > 0 for v in (poll_interval, refresh_timeout)
        ):
            raise ValueError("Polling intervals and timeouts must be positive")
        self.task_owner = task_owner
        self.store = store or StateStore()
        if adapters is None:
            from .claude import ClaudeAdapter
            from .codex import CodexAdapter

            adapters = {
                "claude": ClaudeAdapter(self.store, task_owner=task_owner),
                "codex": CodexAdapter(
                    self.store, enable_queue=enable_codex_queue, task_owner=task_owner
                ),
            }
        self.adapters = dict(adapters)
        self.poll_interval = poll_interval
        self.refresh_timeout = refresh_timeout
        self.health: dict[str, str | None] = dict.fromkeys(self.adapters)
        self._poll_task: asyncio.Task | None = None
        self._refresh_lock = asyncio.Lock()
        self._closed = False

    def bind_task_owner(self, owner):
        if self._poll_task is not None:
            raise RuntimeError("Cannot change lifecycle ownership after startup")
        self.task_owner = owner
        for adapter in self.adapters.values():
            bind = getattr(adapter, "bind_task_owner", None)
            if bind is not None:
                bind(owner)

    def transcript_sources(self, sessions):
        result = []
        for adapter in self.adapters.values():
            sources = getattr(adapter, "transcript_sources", None)
            if sources is not None:
                result.extend(sources(sessions))
        return result

    def usage_observations(self):
        """Sanitized quota readings from transports this manager already owns."""
        return tuple(
            observation
            for adapter in self.adapters.values()
            if (read := getattr(adapter, "usage_observations", None)) is not None
            for observation in read()
        )

    async def __aenter__(self) -> AgentManager:
        self._ensure_open()
        if self._poll_task is None:
            await self.refresh()
            self._poll_task = spawn(
                self._poll(), name="orb-agents-discovery", owner=self.task_owner
            )
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()

    def _ensure_open(self) -> None:
        if self._closed:
            raise AgentError("closed", "Agent manager is closed")

    async def _poll(self) -> None:
        while True:
            await asyncio.sleep(self.poll_interval)
            await self.refresh()

    async def refresh(self) -> None:
        self._ensure_open()
        async with self._refresh_lock:

            async def one(kind: str, adapter: Adapter) -> None:
                try:
                    async with asyncio.timeout(self.refresh_timeout):
                        await adapter.refresh()
                except Exception as exc:
                    # Never forward stderr, paths to credentials, or raw payloads.
                    self.health[kind] = f"Discovery unavailable ({type(exc).__name__})"
                else:
                    self.health[kind] = None

            await asyncio.gather(
                *(one(kind, adapter) for kind, adapter in self.adapters.items())
            )

    async def sessions(self) -> list[Session]:
        await self.refresh()
        return self.store.sessions()

    def events(self) -> AsyncIterator[Event]:
        return self.store.events()

    def pending_approvals(self) -> list[ApprovalRequest]:
        return self.store.pending_approvals()

    async def approve(
        self,
        request_id: str,
        decision: str,
        message: str | None = None,
        *,
        expected_request: ApprovalRequest | None = None,
    ) -> None:
        self._ensure_open()
        await self.store.approve(
            request_id, decision, message, expected_request=expected_request
        )

    @staticmethod
    def _validate_text(text: str) -> None:
        if not isinstance(text, str) or not text.strip() or len(text) > 8192:
            raise ValueError("Message must contain 1–8192 characters")

    def can_message(self, session_id: str) -> bool:
        session = self.store.get(session_id)
        if self._closed or session is None:
            return False
        adapter = self.adapters.get(session.kind)
        if adapter is None:
            return False
        check = getattr(adapter, "can_message", None)
        if check is not None:
            return check(session_id)
        return adapter.owns(session_id) and session.state not in {
            "done",
            "error",
            "needs_approval",
        }

    async def send(self, session_id: str, text: str) -> DeliveryResult:
        self._ensure_open()
        self._validate_text(text)
        session = self.store.get(session_id)
        if session is None:
            raise AgentError(
                "not_found", "Session is not known; refresh sessions first"
            )
        adapter = self.adapters.get(session.kind)
        if adapter is None:
            raise SessionUnavailable("Session provider is not configured")
        result = await adapter.send(session_id, text)
        return result or DeliveryResult(
            "delivered", "Owned transport accepted the message"
        )

    async def answer_question(
        self, session_id: str, question_id: str, text: str
    ) -> None:
        self._ensure_open()
        self._validate_text(text)
        session = self.store.get(session_id)
        if session is None:
            raise AgentError("stale_question", "Question is no longer pending")
        adapter = self.adapters.get(session.kind)
        if adapter is None:
            raise SessionUnavailable("Session provider is not configured")
        await adapter.answer_question(session_id, question_id, text)

    async def start(
        self, kind: Kind, cwd: str, prompt: str, model: str | None = None
    ) -> Session:
        self._ensure_open()
        self._validate_text(prompt)
        if kind not in self.adapters:
            raise AgentError(
                "unsupported", "Requested agent provider is not configured"
            )
        path = await asyncio.to_thread(lambda: Path(cwd).expanduser().resolve())
        if not await asyncio.to_thread(path.is_dir):
            raise ValueError("cwd must be an existing directory")
        return await self.adapters[kind].start(str(path), prompt, model)

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._poll_task:
            self._poll_task.cancel()
            await asyncio.gather(self._poll_task, return_exceptions=True)
        # Wake approval waiters before stopping transports.
        await self.store.close()
        results = await asyncio.gather(
            *(a.close() for a in self.adapters.values()), return_exceptions=True
        )
        for kind, result in zip(self.adapters, results, strict=True):
            if isinstance(result, BaseException):
                self.health[kind] = f"Adapter shutdown failed ({type(result).__name__})"
