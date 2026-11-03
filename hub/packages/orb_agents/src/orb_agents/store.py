"""Single-event-loop session state, fan-out, and one-shot approval arbitration."""

from __future__ import annotations

import asyncio
import math
import uuid
from collections.abc import AsyncIterator, Iterator
from contextlib import contextmanager
from dataclasses import replace
from datetime import timedelta

from .models import (
    AgentError,
    ApprovalRequest,
    Decision,
    Event,
    Session,
    summarize_tool,
    summary,
    utcnow,
)


@contextmanager
def delivery_receipt() -> Iterator[asyncio.Future[None]]:
    """Complete only after a provider write; wake callers on failure/cancellation."""
    receipt = asyncio.get_running_loop().create_future()
    # External resolution/timeout can deliver a denial without an approve caller.
    receipt.add_done_callback(lambda future: future.exception())
    try:
        yield receipt
    except BaseException:
        receipt.set_exception(
            AgentError("delivery_failed", "Provider response could not be delivered")
        )
        raise
    else:
        receipt.set_result(None)


class StateStore:
    def __init__(self, *, queue_size: int = 256):
        if queue_size < 2:
            raise ValueError("queue_size must be at least 2")
        self._sessions: dict[str, Session] = {}
        self._pending: dict[str, tuple[ApprovalRequest, asyncio.Future[Decision]]] = {}
        self._deliveries: dict[str, asyncio.Future[None]] = {}
        self._subscribers: set[asyncio.Queue[Event | None]] = set()
        self._queue_size = queue_size
        self._sequence = 0
        self._closed = False

    def get(self, session_id: str) -> Session | None:
        return self._sessions.get(session_id)

    def sessions(self) -> list[Session]:
        return sorted(self._sessions.values(), key=lambda s: s.updated_at, reverse=True)

    def pending_approvals(self) -> list[ApprovalRequest]:
        return [request for request, _ in self._pending.values()]

    def update(self, session: Session) -> Session:
        if self._closed:
            return session
        session = replace(
            session,
            title=summary(session.title, 120),
            last_assistant_message=summary(session.last_assistant_message),
            waiting_for=summary(session.waiting_for) if session.waiting_for else None,
            question_id=session.question_id
            if session.state == "waiting_input"
            else None,
        )
        # Discovery must not overwrite a blocking approval owned by this store.
        if any(r.session_id == session.id for r, _ in self._pending.values()):
            session = replace(
                session, state="needs_approval", waiting_for="Permission decision"
            )
        previous = self.get(session.id)
        if previous and replace(previous, updated_at=session.updated_at) == session:
            self._sessions[session.id] = session
            return session
        self._sessions[session.id] = session
        self._emit(Event("session.changed", session_id=session.id, session=session))
        return session

    def _emit(self, event: Event) -> None:
        if self._closed:
            return
        self._sequence += 1
        event = replace(event, sequence=self._sequence)
        for queue in self._subscribers:
            if queue.full():
                while not queue.empty():
                    queue.get_nowait()
                queue.put_nowait(
                    Event(
                        "stream.gap",
                        reason="Subscriber fell behind; refresh sessions and pending_approvals",
                        sequence=event.sequence - 1,
                    )
                )
            queue.put_nowait(event)

    async def events(self) -> AsyncIterator[Event]:
        """Subscribe when iteration starts; no replay. Slow readers get stream.gap."""
        if self._closed:
            return
        queue: asyncio.Queue[Event | None] = asyncio.Queue(self._queue_size)
        self._subscribers.add(queue)
        try:
            while (event := await queue.get()) is not None:
                yield event
        finally:
            self._subscribers.discard(queue)

    async def request_approval(
        self,
        session_id: str,
        tool: str,
        tool_input: object,
        *,
        request_id: str | None = None,
        timeout: float = 120,
        scope: str = "operation",
        delivery: asyncio.Future[None] | None = None,
    ) -> Decision:
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Approval timeout must be positive")
        if self._closed:
            return Decision("deny", "Agent service is closed")
        session = self.get(session_id)
        if session is None:
            raise AgentError("not_found", "Approval refers to an unknown session")
        request_id = request_id or f"apr_{uuid.uuid4().hex}"
        if request_id in self._pending:
            raise AgentError("duplicate_request", "Approval request is already pending")
        request = ApprovalRequest(
            request_id,
            session_id,
            summary(tool, 80),
            summarize_tool(tool, tool_input),
            scope=scope,
            expires_at=utcnow() + timedelta(seconds=timeout),
        )
        future = asyncio.get_running_loop().create_future()
        self._pending[request_id] = (request, future)
        if delivery is not None:
            self._deliveries[request_id] = delivery
            delivery.add_done_callback(
                lambda receipt: (
                    self._deliveries.pop(request_id, None)
                    if self._deliveries.get(request_id) is receipt
                    else None
                )
            )
        self.update(
            replace(
                session,
                state="needs_approval",
                waiting_for="Permission decision",
                updated_at=utcnow(),
            )
        )
        self._emit(
            Event(
                "approval.requested",
                session_id=session_id,
                approval=request,
                request_id=request_id,
            )
        )
        try:
            async with asyncio.timeout(timeout):
                return await asyncio.shield(future)
        except TimeoutError:
            self.resolve(
                request_id, reason="timeout", message="Permission request expired"
            )
            return future.result()
        except asyncio.CancelledError:
            self.resolve(request_id, reason="cancelled")
            raise

    def resolve(
        self,
        request_id: str,
        decision: str = "deny",
        reason: str = "external",
        message: str | None = None,
    ) -> bool:
        if decision not in ("allow", "deny"):
            raise ValueError("decision must be allow or deny")
        pending = self._pending.pop(request_id, None)
        if pending is None:
            return False
        request, future = pending
        if not future.done():
            future.set_result(Decision(decision, message))
        self._emit(
            Event(
                "approval.resolved",
                session_id=request.session_id,
                request_id=request_id,
                decision=decision if reason in {"user", "timeout"} else None,
                reason=reason,
            )
        )
        session = self.get(request.session_id)
        if session and session.state == "needs_approval":
            self.update(
                replace(session, state="working", waiting_for=None, updated_at=utcnow())
            )
        return True

    async def approve(
        self,
        request_id: str,
        decision: str,
        message: str | None = None,
        *,
        expected_request: ApprovalRequest | None = None,
    ) -> None:
        if message is not None and (
            not isinstance(message, str) or len(message) > 8192
        ):
            raise ValueError("message must be a string of at most 8192 characters")
        pending = self._pending.get(request_id)
        if expected_request is not None and (
            pending is None or pending[0] is not expected_request
        ):
            raise AgentError(
                "stale_approval", "Permission request is no longer pending"
            )
        delivery = self._deliveries.get(request_id)
        if not self.resolve(request_id, decision, "user", message):
            raise AgentError(
                "stale_approval", "Permission request is no longer pending"
            )
        if delivery is not None:
            await asyncio.shield(delivery)

    def resolve_session(self, session_id: str, reason: str = "external") -> None:
        for request, _ in list(self._pending.values()):
            if request.session_id == session_id:
                self.resolve(request.request_id, reason=reason)

    def turn_complete(
        self, session_id: str, summary: str = "", error: str | None = None
    ) -> None:
        self.resolve_session(session_id, "turn_completed")
        session = self.get(session_id)
        if session:
            session = self.update(
                replace(
                    session,
                    state="error" if error else "idle",
                    waiting_for=None,
                    question_id=None,
                    last_assistant_message=summary or session.last_assistant_message,
                    updated_at=utcnow(),
                )
            )
            self._emit(
                Event(
                    "turn.completed",
                    session_id=session_id,
                    session=session,
                    reason="error" if error else "completed",
                )
            )

    def fail_session(self, session_id: str, message: str) -> None:
        self.resolve_session(session_id, "session_error")
        session = self.get(session_id)
        if session:
            self.update(
                replace(
                    session, state="error", waiting_for=message, updated_at=utcnow()
                )
            )

    async def close(self) -> None:
        if self._closed:
            return
        for request_id in list(self._pending):
            self.resolve(request_id, reason="shutdown")
        self._closed = True
        for queue in self._subscribers:
            if queue.full():
                queue.get_nowait()
            queue.put_nowait(None)
