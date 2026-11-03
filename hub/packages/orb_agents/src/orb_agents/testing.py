"""Deterministic in-memory fake sessions for hub and device integration tests."""

from __future__ import annotations

import asyncio
import uuid
from dataclasses import replace

from .models import AgentError, Decision, Kind, Session, SessionUnavailable, utcnow
from .store import StateStore


class FakeAdapter:
    """No files, processes, sockets, credentials, or model calls.

    ``send`` records the message. Drive completion or permission requests from
    the test with ``complete`` and ``ask_permission``. This intentionally does
    not guess what a real model would do with a prompt.
    """

    def __init__(self, store: StateStore, kind: Kind = "claude"):
        self.store = store
        self.kind = kind
        self.messages: list[tuple[str, str]] = []
        self._owned: set[str] = set()
        self._requests: set[asyncio.Task] = set()
        self.closed = False

    async def refresh(self) -> None:
        return None

    def owns(self, session_id: str) -> bool:
        return session_id in self._owned

    async def start(self, cwd: str, prompt: str, model: str | None = None) -> Session:
        if self.closed:
            raise SessionUnavailable("Fake adapter is closed")
        session_id = f"{self.kind}:{uuid.uuid4()}"
        self._owned.add(session_id)
        session = self.store.update(
            Session(
                session_id,
                self.kind,
                "hub-started",
                "Fake session",
                cwd,
                state="working",
            )
        )
        self.messages.append((session_id, prompt))
        return session

    async def send(self, session_id: str, text: str) -> None:
        if self.closed or not self.owns(session_id):
            raise SessionUnavailable("Fake session is not controlled by this adapter")
        session = self.store.get(session_id)
        if session.state == "needs_approval":
            raise SessionUnavailable("Resolve the pending approval before sending")
        self.messages.append((session_id, text))
        self.store.update(replace(session, state="working", updated_at=utcnow()))

    async def answer_question(
        self, session_id: str, question_id: str, text: str
    ) -> None:
        session = self.store.get(session_id)
        if (
            session is None
            or session.question_id != question_id
            or session.state != "waiting_input"
        ):
            raise AgentError("stale_question", "Question is no longer pending")
        self.store.update(replace(session, question_id=None))
        await self.send(session_id, text)

    async def ask_permission(
        self,
        session_id: str,
        tool: str = "Write",
        tool_input: object = None,
        *,
        timeout: float = 120,
    ) -> Decision:
        task = asyncio.current_task()
        self._requests.add(task)
        try:
            return await self.store.request_approval(
                session_id,
                tool,
                tool_input
                or {"file_path": "example.txt", "content": "never forwarded"},
                timeout=timeout,
            )
        finally:
            self._requests.discard(task)

    def complete(self, session_id: str, text: str = "Done") -> None:
        self.store.turn_complete(session_id, text)

    async def close(self) -> None:
        self.closed = True
        for task in self._requests:
            task.cancel()
        await asyncio.gather(*self._requests, return_exceptions=True)
