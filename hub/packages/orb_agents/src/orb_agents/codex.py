"""Owned Codex app-server and passive discovery with opt-in queue delivery.

Observed desktop/CLI threads remain read-only unless queue is explicitly enabled.
They are never resumed, forked or connected to through private sockets.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import re
import uuid
from dataclasses import dataclass, field, replace
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .models import (
    AgentError,
    DeliveryResult,
    Session,
    SessionUnavailable,
    TranscriptSource,
    UnsupportedOperation,
    summary,
)
from .store import delivery_receipt
from .usage import UsageCollector

if TYPE_CHECKING:
    from .store import StateStore
from .tasks import spawn

DEFAULT_CODEX = "/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex"
MAX_LINE = 4 * 1024 * 1024


def _time(value: Any, fallback: datetime | None = None) -> datetime:
    try:
        if isinstance(value, (int, float)):
            return datetime.fromtimestamp(value, UTC)
        if isinstance(value, str):
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
            return (
                parsed.replace(tzinfo=UTC) if parsed.tzinfo is None else parsed
            ).astimezone(UTC)
    except (ValueError, OverflowError, OSError):
        pass
    return fallback or datetime.now(UTC)


def _text(content: Any) -> str:
    """Extract only assistant text, never tool output or reasoning."""
    if isinstance(content, str):
        return content[:300]
    if not isinstance(content, list):
        return ""
    return " ".join(
        part["text"][:300]
        for part in content
        if isinstance(part, dict)
        and part.get("type") in {"text", "output_text", "input_text"}
        and isinstance(part.get("text"), str)
    )[:300]


def _status(status: Any) -> tuple[str, str | None]:
    if not isinstance(status, dict):
        return "idle", None
    flags = status.get("activeFlags", [])
    if not isinstance(flags, list):
        flags = []
    if "waitingOnApproval" in flags:
        return "needs_approval", "Tool permission"
    if "waitingOnUserInput" in flags:
        return "waiting_input", "User response"
    kind = status.get("type")
    if not isinstance(kind, str):
        return "idle", None
    return {
        "active": ("working", None),
        "idle": ("idle", None),
        "notLoaded": ("idle", "Thread is not loaded"),
        "systemError": ("error", "Codex reported a system error"),
    }.get(kind, ("idle", None))


@dataclass
class _Cursor:
    inode: int
    offset: int = 0
    session: Session | None = None
    dropping_line: bool = False
    completions: list[Session] = field(default_factory=list)
    hydrated: bool = False
    named_title: bool = False
    first_prompt_seen: bool = False


@dataclass
class _DiscoveryBatch:
    """Worker-owned snapshot and ordered actions; never touches asyncio state."""

    sessions: dict[str, Session]
    actions: list[tuple[Session, bool]] = field(default_factory=list)

    def get(self, session_id: str) -> Session | None:
        return self.sessions.get(session_id)

    def update(self, session: Session) -> None:
        self.sessions[session.id] = session
        self.actions.append((session, False))

    def turn_complete(self, session_id: str, *, summary: str) -> None:
        if session := self.get(session_id):
            self.actions.append(
                (replace(session, last_assistant_message=summary), True)
            )


class RolloutObserver:
    """Incremental bounded JSONL reader; it never writes to the sessions tree.

    Only files modified within lookback are considered current enough to show.
    Unchanged timestamps remain unchanged. A partial final line is retried on
    the next scan; oversized lines are discarded without retaining their body.
    """

    def __init__(
        self,
        store: StateStore,
        root: Path,
        *,
        lookback: timedelta = timedelta(days=1),
        bytes_per_scan: int = 8 * 1024 * 1024,
    ) -> None:
        self.store = store
        self.root = Path(root)
        self.lookback = lookback
        self.bytes_per_scan = bytes_per_scan
        self._cursors: dict[Path, _Cursor] = {}

    def collect(self, owned: set[str], sessions: list[Session]) -> _DiscoveryBatch:
        batch = _DiscoveryBatch({session.id: session for session in sessions})
        self.scan(owned, _store=batch)
        return batch

    def scan(
        self,
        owned: set[str] | None = None,
        *,
        _store: StateStore | _DiscoveryBatch | None = None,
    ) -> None:
        owned = owned or set()
        store = self.store if _store is None else _store
        cutoff = datetime.now(UTC).timestamp() - self.lookback.total_seconds()
        try:
            paths = list(self.root.glob("**/rollout-*.jsonl"))
        except OSError:
            return
        for missing in self._cursors.keys() - set(paths):
            self._expire(self._cursors.pop(missing), owned, store)
        for path in paths:
            try:
                if path.is_symlink():
                    continue
                stat = path.stat()
                if stat.st_mtime < cutoff:
                    if old := self._cursors.pop(path, None):
                        self._expire(old, owned, store)
                    continue
                cursor = self._cursors.get(path)
                if (
                    cursor is None
                    or cursor.inode != stat.st_ino
                    or stat.st_size < cursor.offset
                ):
                    if cursor is not None:
                        self._expire(cursor, owned, store)
                    cursor = self._cursors[path] = _Cursor(stat.st_ino)
                initial = not cursor.hydrated
                cursor.completions.clear()
                with path.open("rb") as stream:
                    stream.seek(cursor.offset)
                    consumed = 0
                    while consumed < self.bytes_per_scan:
                        offset = stream.tell()
                        line = stream.readline(MAX_LINE + 1)
                        if not line:
                            break
                        consumed += len(line)
                        if len(line) > MAX_LINE or cursor.dropping_line:
                            cursor.dropping_line = not line.endswith(b"\n")
                            cursor.offset = stream.tell()
                            continue
                        if not line.endswith(b"\n"):
                            cursor.offset = offset
                            break
                        cursor.offset = stream.tell()
                        try:
                            record = json.loads(line)
                        except (ValueError, UnicodeDecodeError, RecursionError):
                            continue
                        if isinstance(record, dict):
                            self._record(cursor, record, owned, store)
                    cursor.hydrated = cursor.hydrated or stream.tell() >= stat.st_size
                if cursor.session and cursor.session.id not in owned:
                    session = cursor.session
                    if session.state in {
                        "working",
                        "needs_approval",
                        "waiting_input",
                    } and datetime.now(UTC) - session.updated_at > timedelta(minutes=5):
                        session = replace(
                            session,
                            state="idle",
                            waiting_for="No recent rollout activity; live state unknown",
                        )
                    if not initial:
                        for completion in cursor.completions:
                            store.update(completion)
                            store.turn_complete(
                                completion.id, summary=completion.last_assistant_message
                            )
                    store.update(session)
            except (OSError, ValueError):
                # Rotation or an unreadable file must not hide other sessions.
                continue

    def _expire(
        self, cursor: _Cursor, owned: set[str], store: StateStore | _DiscoveryBatch
    ) -> None:
        if cursor.session and cursor.session.id not in owned:
            store.update(
                replace(
                    cursor.session,
                    state="done",
                    waiting_for="Rollout no longer recent or present; live state unknown",
                )
            )

    def _record(
        self,
        cursor: _Cursor,
        record: dict[str, Any],
        owned: set[str],
        store: StateStore | _DiscoveryBatch,
    ) -> None:
        payload = record.get("payload")
        if not isinstance(payload, dict):
            return
        kind = record.get("type")
        timestamp = _time(record.get("timestamp"))
        if kind == "session_meta":
            raw_id = payload.get("id") or payload.get("session_id")
            if not isinstance(raw_id, str):
                return
            sid = f"codex:{raw_id}"
            if sid in owned:
                cursor.session = None
                return
            source = payload.get("source")
            originator = str(payload.get("originator", "")).lower()
            desktop = "desktop" in originator or source == "appServer"
            hub_started = (
                originator == "orb_agents"
                or payload.get("thread_source") == "orb_agents"
            )
            previous = (
                cursor.session if cursor.session and cursor.session.id == sid else None
            )
            cached = store.get(sid) if previous is None else None
            if previous is None:
                # Rotation resets the read cursor, not the session's identity.
                # Retain only its title metadata: expiry may have marked the
                # cached session done, which must not become fresh live state.
                cursor.named_title = (
                    cached is not None and cached.title_source == "name"
                )
                cursor.first_prompt_seen = (
                    cached is not None and cached.title_source == "prompt"
                )
            title = payload.get("title")
            named_title = summary(title, 120) if isinstance(title, str) else ""
            cursor.named_title = cursor.named_title or bool(named_title)
            if previous is not None:
                cursor.session = replace(
                    previous,
                    title=named_title or previous.title,
                    title_source="name" if named_title else previous.title_source,
                    cwd=payload.get("cwd")
                    if isinstance(payload.get("cwd"), str)
                    else previous.cwd,
                    updated_at=timestamp,
                )
                return
            cursor.session = Session(
                id=sid,
                kind="codex",
                origin="hub-started"
                if hub_started
                else "desktop app"
                if desktop
                else "cli",
                title=named_title or (cached.title if cached else ""),
                title_source="name"
                if named_title
                else cached.title_source
                if cached
                else "",
                cwd=str(payload.get("cwd", "")),
                updated_at=timestamp,
            )
            return
        session = cursor.session
        if session is None or session.id in owned:
            return
        updates: dict[str, Any] = {}
        completed = False
        error: str | None = None
        if kind == "turn_context" and isinstance(payload.get("cwd"), str):
            updates["cwd"] = payload["cwd"]
        elif kind == "event_msg":
            event = payload.get("type")
            if not isinstance(event, str):
                return
            if event in {"task_started", "turn_started", "user_message"}:
                updates.update(state="working", waiting_for=None)
                if event == "user_message":
                    self._prompt_title(cursor, payload.get("message"), updates)
            elif event in {"task_complete", "turn_completed", "turn_aborted"}:
                completed = True
                updates.update(state="idle", waiting_for=None)
                if payload.get("last_agent_message"):
                    updates["last_assistant_message"] = str(
                        payload["last_agent_message"]
                    )[:300]
            elif event in {"agent_message", "agent_message_delta"} and isinstance(
                payload.get("message"), str
            ):
                updates["last_assistant_message"] = payload["message"][:300]
            elif event == "error":
                error = "Codex reported an error (see its native session)"
                updates.update(state="error", waiting_for=error)
            elif event in {"request_user_input", "request_user_input_request"}:
                updates.update(
                    state="waiting_input",
                    waiting_for="Respond in the native Codex session",
                )
            elif event in {"exec_approval_request", "apply_patch_approval_request"}:
                updates.update(
                    state="needs_approval",
                    waiting_for="Approve in the native Codex session",
                )
        elif kind == "response_item" and payload.get("type") == "message":
            if payload.get("role") == "assistant":
                updates["last_assistant_message"] = _text(payload.get("content"))
            elif payload.get("role") == "user":
                content = payload.get("content")
                if isinstance(content, list):
                    content = " ".join(
                        part["text"]
                        for part in content
                        if isinstance(part, dict)
                        and part.get("type") in {"text", "input_text"}
                        and isinstance(part.get("text"), str)
                    )
                self._prompt_title(cursor, content, updates)
        cursor.session = replace(session, updated_at=timestamp, **updates)
        if completed:
            cursor.completions.append(cursor.session)

    @staticmethod
    def _prompt_title(cursor: _Cursor, prompt: Any, updates: dict[str, Any]) -> None:
        if (
            cursor.named_title
            or cursor.first_prompt_seen
            or not isinstance(prompt, str)
        ):
            return
        # Codex also records bootstrap context as user messages. It is not the
        # user's task and must not become a generated session name.
        if prompt.lstrip().startswith(
            ("# AGENTS.md instructions", "<environment_context>", "<user_instructions>")
        ):
            return
        title = summary(prompt, 120)
        if not title:
            return
        # Keep only the first user prompt; later turns must not rename a thread.
        cursor.first_prompt_seen = True
        updates["title"] = title
        updates["title_source"] = "prompt"


class CodexAdapter:
    """One owned app-server process, lazily started; no daemon manipulation.

    env is a complete child environment when provided. In isolated tests set
    CODEX_HOME to a new temporary directory and never copy credentials there.
    """

    def __init__(
        self,
        store: StateStore,
        *,
        executable: str = DEFAULT_CODEX,
        env: dict[str, str] | None = None,
        sessions_root: Path | None = None,
        command: list[str] | None = None,
        process_cwd: str | None = None,
        rpc_timeout: float = 30,
        approval_timeout: float = 120,
        approval_policy: str = "on-request",
        sandbox: str = "workspace-write",
        observe: bool = True,
        enable_queue: bool = False,
        task_owner=None,
    ) -> None:
        self.task_owner = task_owner
        self.store = store
        self.executable = executable
        self.env = env
        self.command = command or [executable, "app-server", "--listen", "stdio://"]
        self.process_cwd = process_cwd
        self.rpc_timeout = rpc_timeout
        self.approval_timeout = approval_timeout
        self.approval_policy = approval_policy
        self.sandbox = sandbox
        self.enable_queue = enable_queue
        codex_home = Path(
            (env or os.environ).get("CODEX_HOME", str(Path.home() / ".codex"))
        )
        self.observer = (
            RolloutObserver(store, sessions_root or codex_home / "sessions")
            if observe
            else None
        )
        self._proc: asyncio.subprocess.Process | None = None
        self._reader: asyncio.Task[None] | None = None
        self._stderr: asyncio.Task[None] | None = None
        self._startup = asyncio.Lock()
        self._write_lock = asyncio.Lock()
        self._locks: dict[str, asyncio.Lock] = {}
        self._owned: set[str] = set()
        self._active_turns: dict[str, str] = {}
        self._terminal_turns: set[str] = set()
        self._pending: dict[str, asyncio.Future[dict[str, Any]]] = {}
        self._requests: dict[str, asyncio.Task[None]] = {}
        self._request_threads: dict[str, str] = {}
        self._questions: dict[str, tuple[Any, list[dict[str, Any]], str]] = {}
        self._question_answers: set[str] = set()
        self._request_prefix = uuid.uuid4().hex
        self._sequence = 0
        self._closed = False
        self._disconnected = False
        self._queue_processes: set[asyncio.subprocess.Process] = set()
        self._discovery_lock = asyncio.Lock()
        self._usage = UsageCollector()

    def bind_task_owner(self, owner):
        self.task_owner = owner

    def usage_observations(self):
        """Already-owned account notifications; never starts an app-server."""
        return tuple(self._usage.observations.values())

    def transcript_sources(self, sessions: list[Session]) -> list[TranscriptSource]:
        """Keep account usage independent of the device's admitted session cap."""
        if self.observer is None:
            return []
        candidates = []
        for path in self.observer.root.glob("**/rollout-*.jsonl"):
            try:
                if not path.is_symlink():
                    candidates.append((path.stat().st_mtime, path))
            except OSError:
                continue
        result = []
        for _, path in sorted(candidates, reverse=True)[:32]:
            identity = next(
                (
                    session.id
                    for session in sessions
                    if session.kind == "codex"
                    and path.name.endswith(
                        f"-{session.id.removeprefix('codex:')}.jsonl"
                    )
                ),
                None,
            )
            result.append(TranscriptSource(path, "codex", identity))
        return result

    def owns(self, session_id: str) -> bool:
        return session_id in self._owned

    def can_message(self, session_id: str) -> bool:
        session = self.store.get(session_id)
        if (
            self._closed
            or session is None
            or session.kind != "codex"
            or session.state in {"done", "error", "needs_approval"}
        ):
            return False
        if self.owns(session_id):
            return bool(
                not self._disconnected
                and self._proc is not None
                and self._proc.returncode is None
                and self._proc.stdin is not None
            )
        return bool(
            self.enable_queue
            and re.fullmatch(r"codex:[A-Za-z0-9][A-Za-z0-9._:-]{0,199}", session_id)
            and os.path.isfile(self.executable)
            and os.access(self.executable, os.X_OK)
        )

    async def refresh(self) -> None:
        if self.observer is None or self._closed:
            return
        async with self._discovery_lock:
            # Filesystem traversal and JSON decoding can take seconds on large
            # histories. Keep that work off the loop, then publish in order on
            # the loop so approvals, sockets and their deadlines keep running.
            worker = spawn(
                asyncio.to_thread(
                    self.observer.collect, set(self._owned), self.store.sessions()
                ),
                name="orb-codex-discovery",
                owner=self.task_owner,
            )
            try:
                batch = await asyncio.shield(worker)
            except asyncio.CancelledError:
                # A thread cannot be cancelled. Keep its cursor serialized with
                # the next scan, and never publish a cancelled refresh's result.
                await worker
                raise
            if self._closed:
                return
            for session, completed in batch.actions:
                if session.id in self._owned:
                    continue
                self.store.update(session)
                if completed:
                    self.store.turn_complete(
                        session.id, summary=session.last_assistant_message
                    )

    async def _ensure_server(self) -> None:
        async with self._startup:
            if self._closed:
                raise SessionUnavailable("Codex adapter is closed")
            if self._disconnected:
                raise SessionUnavailable(
                    "Owned Codex app-server disconnected; create a new adapter"
                )
            if self._proc is not None:
                if self._proc.returncode is not None:
                    raise SessionUnavailable(
                        "Owned Codex app-server has exited; create a new adapter"
                    )
                return
            try:
                self._proc = await asyncio.create_subprocess_exec(
                    *self.command,
                    stdin=asyncio.subprocess.PIPE,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                    env=self.env,
                    cwd=self.process_cwd,
                    limit=MAX_LINE,
                )
                if self._closed:
                    await self._reap_process(terminate=True)
                    raise SessionUnavailable("Codex adapter closed during startup")
                self._reader = spawn(
                    self._read_loop(), name="orb-codex-reader", owner=self.task_owner
                )
                self._stderr = spawn(
                    self._drain_stderr(), name="orb-codex-stderr", owner=self.task_owner
                )
                await self._rpc(
                    "initialize",
                    {
                        "clientInfo": {
                            "name": "orb_agents",
                            "title": "Orb Agents",
                            "version": "0.1.0",
                        }
                    },
                )
                await self._write({"method": "initialized", "params": {}})
            except BaseException:
                await self.close()
                raise

    async def _drain_stderr(self) -> None:
        assert self._proc is not None and self._proc.stderr is not None
        while await self._proc.stderr.read(65536):
            # Never retain arbitrary diagnostics containing prompts/credentials.
            pass

    async def _write(
        self,
        message: dict[str, Any],
        *,
        question_guard: tuple[str, tuple] | None = None,
    ) -> None:
        proc = self._proc
        if proc is None or proc.returncode is not None or proc.stdin is None:
            raise SessionUnavailable("Owned Codex app-server is not connected")
        async with self._write_lock:
            if self._closed or proc.returncode is not None:
                raise SessionUnavailable("Owned Codex app-server is closed")
            if message.get("method") in {"turn/start", "turn/steer"}:
                params = message["params"]
                sid = f"codex:{params['threadId']}"
                session = self.store.get(sid)
                if (
                    not self.owns(sid)
                    or session is None
                    or session.state in {"done", "error", "needs_approval"}
                ):
                    raise SessionUnavailable("Codex can no longer accept this message")
                expected = params.get("expectedTurnId")
                if self._active_turns.get(sid) != expected:
                    raise SessionUnavailable(
                        "Codex turn changed before delivery; refresh before retrying"
                    )
            if question_guard is not None:
                sid, question = question_guard
                if self._questions.get(sid) is not question:
                    raise AgentError("stale_question", "Question is no longer pending")
            try:
                proc.stdin.write(
                    json.dumps(message, separators=(",", ":")).encode() + b"\n"
                )
                async with asyncio.timeout(self.rpc_timeout):
                    await proc.stdin.drain()
            except TimeoutError as exc:
                raise AgentError(
                    "timeout",
                    "Codex transport timed out; delivery is uncertain and was not retried",
                ) from exc
            except (BrokenPipeError, ConnectionResetError) as exc:
                raise SessionUnavailable("Owned Codex app-server disconnected") from exc

    async def _rpc(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        self._sequence += 1
        rid = f"orb-{self._sequence}"
        future = asyncio.get_running_loop().create_future()
        self._pending[rid] = future
        try:
            async with asyncio.timeout(self.rpc_timeout):
                await self._write({"id": rid, "method": method, "params": params})
                return await future
        except TimeoutError as exc:
            raise AgentError(
                "timeout",
                f"Codex {method} timed out; delivery is uncertain and was not retried",
            ) from exc
        finally:
            self._pending.pop(rid, None)

    async def _read_loop(self) -> None:
        assert self._proc is not None and self._proc.stdout is not None
        try:
            while line := await self._proc.stdout.readline():
                message = json.loads(line)
                if not isinstance(message, dict):
                    continue
                if "method" in message:
                    if "id" in message:
                        key = self._approval_id(message["id"])
                        if key in self._requests or any(
                            question[0] == message["id"]
                            for question in self._questions.values()
                        ):
                            raise ValueError("Duplicate server request ID")
                        task = spawn(
                            self._server_request(message),
                            name="orb-codex-request",
                            owner=self.task_owner,
                        )
                        self._requests[key] = task
                        self._request_threads[key] = (
                            f"codex:{message.get('params', {}).get('threadId', '')}"
                        )
                        task.add_done_callback(
                            lambda done, key=key: self._request_finished(key, done)
                        )
                    else:
                        self._notification(message)
                elif "id" in message:
                    future = self._pending.get(message["id"])
                    if future is not None and not future.done():
                        if "error" in message:
                            code = message.get("error", {}).get("code", "unknown")
                            if type(code) is not int:
                                code = "unknown"
                            future.set_exception(
                                AgentError(
                                    "backend_error",
                                    f"Codex rejected the request (RPC code {code}); inspect its native diagnostics",
                                )
                            )
                        else:
                            result = message.get("result", {})
                            future.set_result(
                                result if isinstance(result, dict) else {}
                            )
        except (
            ValueError,
            UnicodeDecodeError,
            RecursionError,
            OSError,
            TypeError,
            AttributeError,
            KeyError,
        ):
            pass
        finally:
            self._disconnected = True
            for future in self._pending.values():
                if not future.done():
                    future.set_exception(
                        SessionUnavailable("Owned Codex app-server disconnected")
                    )
            for key, task in list(self._requests.items()):
                self.store.resolve(key, reason="disconnected")
                task.cancel()
            self._questions.clear()
            if not self._closed:
                for sid in self._owned:
                    self.store.fail_session(sid, "Owned Codex app-server disconnected")
                await self._reap_process(terminate=True)

    def _request_finished(self, key: str, task: asyncio.Task[None]) -> None:
        self._requests.pop(key, None)
        sid = self._request_threads.pop(key, None)
        if not task.cancelled():
            # Retrieving the exception avoids unhandled background-task errors.
            if task.exception() is not None and sid in self._owned:
                self.store.fail_session(
                    sid, "Codex request could not be answered; delivery is uncertain"
                )

    def _approval_id(self, rpc_id: Any) -> str:
        encoded = json.dumps(rpc_id, separators=(",", ":"))
        return f"codex:{self._request_prefix}:{encoded}"

    async def start(self, cwd: str, prompt: str, model: str | None = None) -> Session:
        await self._ensure_server()
        resolved = await asyncio.to_thread(lambda: Path(cwd).resolve())
        params: dict[str, Any] = {
            "cwd": str(resolved),
            "approvalPolicy": self.approval_policy,
            "approvalsReviewer": "user",
            "sandbox": self.sandbox,
            "threadSource": "orb_agents",
        }
        if model:
            params["model"] = model
        result = await self._rpc("thread/start", params)
        thread = result.get("thread", {})
        if not isinstance(thread.get("id"), str):
            raise AgentError(
                "protocol_error", "Codex thread/start returned no thread ID"
            )
        sid = f"codex:{thread['id']}"
        self._owned.add(sid)
        self._locks[sid] = asyncio.Lock()
        title = thread.get("title")
        named_title = summary(title, 120) if isinstance(title, str) else ""
        session = self.store.update(
            Session(
                id=sid,
                kind="codex",
                origin="hub-started",
                title=named_title or summary(prompt, 120),
                cwd=params["cwd"],
                title_source="name" if named_title else "prompt",
            )
        )
        try:
            await self.send(sid, prompt)
        except BaseException:
            self.store.fail_session(sid, "Codex could not start the requested turn")
            raise
        return self.store.get(sid) or session

    async def send(self, session_id: str, text: str) -> DeliveryResult:
        if not isinstance(text, str) or not text.strip():
            raise AgentError("bad_request", "A nonempty user message is required")
        if len(text) > 8192:
            raise AgentError(
                "bad_request", "User messages are limited to 8192 characters"
            )
        if not self.owns(session_id):
            if not self.enable_queue:
                raise UnsupportedOperation(
                    "Observed Codex desktop/CLI sessions are read-only; enable queue delivery explicitly"
                )
            return await self._queue(session_id, text)
        async with self._locks[session_id]:
            await self._ensure_server()
            session = self.store.get(session_id)
            if session is None or session.state in {"done", "error", "needs_approval"}:
                raise SessionUnavailable("Owned Codex thread cannot accept a message")
            question = self._questions.get(session_id)
            if question:
                await self._answer_question(session_id, question, text)
                return DeliveryResult("delivered", "Codex accepted the question answer")
            raw_id = session_id.removeprefix("codex:")
            params: dict[str, Any] = {
                "threadId": raw_id,
                "input": [{"type": "text", "text": text}],
            }
            active = self._active_turns.get(session_id)
            if active:
                params["expectedTurnId"] = active
                await self._rpc("turn/steer", params)
            else:
                result = await self._rpc("turn/start", params)
                turn = result.get("turn")
                if not isinstance(turn, dict) or not isinstance(turn.get("id"), str):
                    raise AgentError(
                        "protocol_error",
                        "Codex returned no turn identity; delivery is uncertain",
                    )
                turn_id = turn["id"]
                if (
                    isinstance(turn_id, str)
                    and turn_id not in self._terminal_turns
                    and result.get("turn", {}).get("status")
                    not in {"completed", "failed", "interrupted"}
                ):
                    self._active_turns[session_id] = turn_id
            session = self.store.get(session_id)
            if (
                session_id in self._active_turns
                and session
                and session.state not in {"needs_approval", "waiting_input", "error"}
            ):
                self._set_state(session_id, "working")
            return DeliveryResult("delivered", "Codex acknowledged the user message")

    async def _queue(self, session_id: str, text: str) -> DeliveryResult:
        async with self._locks.setdefault(session_id, asyncio.Lock()):
            if not self.can_message(session_id):
                raise SessionUnavailable("Codex queue is unavailable for this session")
            proc = None
            try:
                proc = await asyncio.create_subprocess_exec(
                    self.executable,
                    "queue",
                    "--thread",
                    session_id.removeprefix("codex:"),
                    "--message",
                    text,
                    stdin=asyncio.subprocess.DEVNULL,
                    stdout=asyncio.subprocess.DEVNULL,
                    stderr=asyncio.subprocess.DEVNULL,
                    env=self.env,
                    cwd=self.process_cwd,
                )
                self._queue_processes.add(proc)
                if self._closed:
                    raise SessionUnavailable(
                        "Codex adapter closed before queue acknowledgement"
                    )
                async with asyncio.timeout(self.rpc_timeout):
                    code = await proc.wait()
                if code != 0:
                    raise AgentError(
                        "delivery_failed", "Codex queue rejected the message"
                    )
                return DeliveryResult(
                    "queued",
                    "Codex queue accepted the message for the existing session",
                )
            except TimeoutError as exc:
                raise AgentError(
                    "timeout",
                    "Codex queue timed out; admission is uncertain and was not retried",
                ) from exc
            except OSError as exc:
                raise SessionUnavailable("Codex queue could not be started") from exc
            finally:
                if proc is not None:
                    await self._stop_queue(proc)
                    self._queue_processes.discard(proc)

    @staticmethod
    async def _stop_queue(proc: asyncio.subprocess.Process) -> None:
        if proc.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                proc.terminate()
            try:
                async with asyncio.timeout(1):
                    await proc.wait()
            except TimeoutError:
                with contextlib.suppress(ProcessLookupError):
                    proc.kill()
                await proc.wait()

    async def answer_question(
        self, session_id: str, question_id: str, text: str
    ) -> None:
        if not self.owns(session_id):
            raise UnsupportedOperation("Observed Codex sessions are read-only")
        if not isinstance(text, str) or not text.strip() or len(text) > 8192:
            raise AgentError("bad_request", "A nonempty user message is required")
        async with self._locks[session_id]:
            await self._ensure_server()
            question = self._questions.get(session_id)
            if (
                question is None
                or question[2] != question_id
                or question_id in self._question_answers
            ):
                raise AgentError("stale_question", "Question is no longer pending")
            await self._answer_question(session_id, question, text)

    async def _answer_question(
        self, session_id: str, question: tuple, text: str
    ) -> None:
        rpc_id, questions, identity = question
        if identity in self._question_answers:
            raise AgentError("stale_question", "Question already has an answer")
        if len(questions) == 1:
            answers = {questions[0]["id"]: {"answers": [text]}}
        else:
            try:
                values = json.loads(text)
            except (ValueError, RecursionError) as exc:
                raise AgentError(
                    "bad_request",
                    "Multiple questions need a JSON object mapping every question ID to its answer",
                ) from exc
            if (
                not isinstance(values, dict)
                or set(values) != {q["id"] for q in questions}
                or not all(isinstance(v, str) for v in values.values())
            ):
                raise AgentError(
                    "bad_request",
                    "Provide exactly one string answer for every question ID",
                )
            answers = {key: {"answers": [value]} for key, value in values.items()}
        # Claim this instance before yielding, but retain its identity until the
        # write lock is acquired so cancellation cannot reach a reused RPC ID.
        self._question_answers.add(identity)
        self._set_state(session_id, "working")
        try:
            await self._write(
                {"id": rpc_id, "result": {"answers": answers}},
                question_guard=(session_id, question),
            )
        finally:
            self._question_answers.discard(identity)
            if self._questions.get(session_id) is question:
                self._questions.pop(session_id)

    def _set_state(self, sid: str, state: str, waiting: str | None = None) -> None:
        session = self.store.get(sid)
        if session:
            self.store.update(
                replace(
                    session,
                    state=state,
                    waiting_for=waiting,
                    question_id=(
                        self._questions[sid][2]
                        if state == "waiting_input" and sid in self._questions
                        else None
                    ),
                    updated_at=datetime.now(UTC),
                )
            )

    def _notification(self, message: dict[str, Any]) -> None:
        method = message.get("method")
        params = message.get("params", {})
        if not isinstance(params, dict):
            return
        if method == "account/rateLimits/updated":
            # Account events have no threadId. Normalize them before the
            # owned-thread filter without retaining any raw account payload.
            self._usage.observe_app_server(params)
            return
        sid = f"codex:{params.get('threadId', '')}"
        if method == "serverRequest/resolved":
            key = self._approval_id(params.get("requestId"))
            self.store.resolve(key, reason="external")
            if task := self._requests.get(key):
                task.cancel()
            question = self._questions.get(sid)
            if question and question[0] == params.get("requestId"):
                self._questions.pop(sid, None)
                self._set_state(sid, "working")
            return
        if sid not in self._owned:
            return
        if method == "thread/status/changed":
            state, waiting = _status(params.get("status"))
            self._set_state(sid, state, waiting)
        elif method == "turn/started":
            turn_id = params.get("turn", {}).get("id")
            if isinstance(turn_id, str):
                self._active_turns[sid] = turn_id
            self._set_state(sid, "working")
        elif method == "turn/completed":
            self._active_turns.pop(sid, None)
            self._questions.pop(sid, None)
            self._cancel_requests(sid)
            turn = params.get("turn", {})
            if isinstance(turn.get("id"), str):
                self._terminal_turns.add(turn["id"])
            error = (
                "Codex turn failed; inspect the native session"
                if turn.get("status") == "failed"
                else None
            )
            session = self.store.get(sid)
            self.store.turn_complete(
                sid,
                summary=session.last_assistant_message if session else "",
                error=error,
            )
        elif method == "item/completed":
            item = params.get("item", {})
            if isinstance(item, dict) and item.get("type") == "agentMessage":
                session = self.store.get(sid)
                if session:
                    self.store.update(
                        replace(
                            session,
                            last_assistant_message=_text(item.get("text", "")),
                            updated_at=datetime.now(UTC),
                        )
                    )
        elif method == "thread/closed":
            self._active_turns.pop(sid, None)
            self._questions.pop(sid, None)
            self._cancel_requests(sid)
            self._set_state(sid, "done", "Owned thread was closed")
        elif method == "error" and not params.get("willRetry", False):
            self.store.fail_session(
                sid, "Codex reported an error; inspect the native session"
            )

    def _cancel_requests(self, sid: str) -> None:
        for key, owner in list(self._request_threads.items()):
            if owner == sid:
                self.store.resolve(key, reason="turn_completed")
                if task := self._requests.get(key):
                    task.cancel()

    async def _server_request(self, message: dict[str, Any]) -> None:
        method = message.get("method", "")
        rpc_id = message["id"]
        params = message.get("params", {})
        if not isinstance(params, dict):
            params = {}
        sid = f"codex:{params.get('threadId', '')}"
        if sid not in self._owned:
            await self._write(
                {
                    "id": rpc_id,
                    "error": {
                        "code": -32601,
                        "message": "Orb only handles owned thread requests",
                    },
                }
            )
            return
        if method == "item/tool/requestUserInput":
            questions = params.get("questions", [])
            if not questions or not all(
                isinstance(q, dict) and isinstance(q.get("id"), str) for q in questions
            ):
                await self._write(
                    {
                        "id": rpc_id,
                        "error": {"code": -32602, "message": "Invalid user questions"},
                    }
                )
                return
            if sid in self._questions:
                await self._write(
                    {
                        "id": rpc_id,
                        "error": {
                            "code": -32000,
                            "message": "A user question is already pending",
                        },
                    }
                )
                return
            self._questions[sid] = (
                rpc_id,
                questions,
                f"{self._approval_id(rpc_id)}:{uuid.uuid4().hex}",
            )
            question_text = "; ".join(
                f"{q['id']}: {q.get('question', '')}" for q in questions
            )
            self._set_state(sid, "waiting_input", question_text[:300])
            return
        if method not in {
            "item/commandExecution/requestApproval",
            "item/fileChange/requestApproval",
            "item/permissions/requestApproval",
        }:
            await self._write(
                {
                    "id": rpc_id,
                    "error": {
                        "code": -32601,
                        "message": "Unsupported Codex server request",
                    },
                }
            )
            return
        key = self._approval_id(rpc_id)
        with delivery_receipt() as delivery:
            decision = await self.store.request_approval(
                sid,
                method.split("/")[1],
                params,
                request_id=key,
                timeout=self.approval_timeout,
                scope="turn"
                if method == "item/permissions/requestApproval"
                else "operation",
                delivery=delivery,
            )
            if method == "item/permissions/requestApproval":
                permissions = (
                    params.get("permissions", {})
                    if decision.decision == "allow"
                    else {}
                )
                result = {"permissions": permissions, "scope": "turn"}
            else:
                result = {
                    "decision": "accept" if decision.decision == "allow" else "decline"
                }
            await self._write({"id": rpc_id, "result": result})

    async def _reap_process(self, *, terminate: bool = False) -> None:
        proc = self._proc
        if proc and proc.returncode is None:
            if proc.stdin:
                proc.stdin.close()
            if terminate:
                with contextlib.suppress(ProcessLookupError):
                    proc.terminate()
            try:
                async with asyncio.timeout(1):
                    await proc.wait()
            except TimeoutError:
                if not terminate:
                    with contextlib.suppress(ProcessLookupError):
                        proc.terminate()
                try:
                    async with asyncio.timeout(1):
                        await proc.wait()
                except TimeoutError:
                    with contextlib.suppress(ProcessLookupError):
                        proc.kill()
                    await proc.wait()

    async def close(self) -> None:
        self._closed = True
        await asyncio.gather(
            *(self._stop_queue(proc) for proc in tuple(self._queue_processes))
        )
        for key, task in list(self._requests.items()):
            self.store.resolve(key, reason="shutdown")
            task.cancel()
        if self._requests:
            await asyncio.gather(*self._requests.values(), return_exceptions=True)
        self._questions.clear()
        await self._reap_process()
        tasks = [task for task in (self._reader, self._stderr) if task]
        for task in tasks:
            if not task.done():
                task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        for sid in self._owned:
            self._set_state(sid, "done", "Hub adapter closed")
