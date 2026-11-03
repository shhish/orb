"""Claude Code discovery and owned, persistent stream-json sessions.

Only processes launched by this adapter receive writes. Discovered sessions are
read-only: the private inbox socket protocol is deliberately not guessed.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import uuid
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from .models import (
    AgentError,
    DeliveryResult,
    Session,
    SessionUnavailable,
    TranscriptSource,
    UnsupportedOperation,
    summary,
    utcnow,
)
from .store import StateStore, delivery_receipt
from .tasks import spawn

_MAX_LINE = 2 * 1024 * 1024
_MAX_DISCOVERY = 4 * 1024 * 1024


def _timestamp(value: object) -> datetime | None:
    try:
        if isinstance(value, (int, float)):
            # The installed agents command reports Unix milliseconds.
            return datetime.fromtimestamp(value / 1000 if value > 1e11 else value, UTC)
        if isinstance(value, str):
            result = datetime.fromisoformat(value.replace("Z", "+00:00"))
            return result.replace(tzinfo=UTC) if result.tzinfo is None else result
    except (ValueError, OverflowError, OSError):
        pass
    return None


def _assistant_text(message: object) -> str:
    if not isinstance(message, dict):
        return ""
    content = message.get("content", [])
    if isinstance(content, str):
        return summary(content)
    if not isinstance(content, list):
        return ""
    return summary(
        " ".join(
            block["text"]
            for block in content
            if isinstance(block, dict)
            and block.get("type") == "text"
            and isinstance(block.get("text"), str)
        )
    )


def _agent_title(record: Mapping[str, Any]) -> str:
    for key in ("name", "title"):
        value = record.get(key)
        if isinstance(value, str) and value.strip():
            return summary(value, 120)
    return ""


def parse_agent_record(
    record: Mapping[str, Any], *, origin: str = "cli", previous: Session | None = None
) -> Session | None:
    """Normalize the installed CLI's agents JSON (and harmless older aliases)."""
    native_id = record.get("sessionId") or record.get("session_id") or record.get("id")
    if not isinstance(native_id, str) or not native_id:
        return None
    status = record.get("status", "idle")
    waiting = record.get("waitingFor") or record.get("waiting_for")
    waiting = waiting if isinstance(waiting, str) else None
    terminal = record.get("state")
    if terminal in ("failed", "failure", "error"):
        state = "error"
    elif terminal in ("done", "success", "stopped"):
        state = "done"
    elif status in ("busy", "working"):
        state = "working"
    elif status == "waiting":
        state = (
            "needs_approval"
            if waiting
            and any(word in waiting.lower() for word in ("permission", "approval"))
            else "waiting_input"
        )
    else:
        state = "idle"
    title = _agent_title(record)
    return Session(
        id=f"claude:{native_id}",
        kind="claude",
        origin=origin,
        title=title or (previous.title if previous else ""),
        cwd=str(record.get("cwd") or (previous.cwd if previous else "")),
        state=state,
        waiting_for=waiting,
        last_assistant_message=previous.last_assistant_message if previous else "",
        updated_at=_timestamp(record.get("updatedAt"))
        or (previous.updated_at if previous else None)
        or _timestamp(record.get("startedAt"))
        or utcnow(),
        title_source="name" if title else previous.title_source if previous else "",
    )


def _desktop_pids(ps_output: str) -> set[int]:
    processes: dict[int, tuple[int, str]] = {}
    for line in ps_output.splitlines():
        fields = line.strip().split(None, 2)
        if len(fields) == 3 and fields[0].isdigit() and fields[1].isdigit():
            processes[int(fields[0])] = (int(fields[1]), fields[2])
    desktop: set[int] = set()
    for pid in processes:
        cursor, visited = pid, set()
        while cursor in processes and cursor not in visited:
            visited.add(cursor)
            parent, command = processes[cursor]
            if "/Claude.app/" in command or command.endswith("/Claude.app"):
                desktop.add(pid)
                break
            cursor = parent
    return desktop


def _transcript_metadata(
    config_dir: Path, native_id: str
) -> tuple[str, str, datetime | None]:
    # UUID validation prevents untrusted discovery IDs from becoming glob/path
    # syntax. Never scan or retain whole transcripts.
    try:
        uuid.UUID(native_id)
    except ValueError:
        return "", "", None
    candidates = list((config_dir / "projects").glob(f"*/{native_id}.jsonl"))
    for path in candidates:
        try:
            stat = path.stat()
            with path.open("rb") as source:
                head = source.read(128 * 1024)
                # A cut record isn't evidence of a prompt. Never read an
                # unbounded first line to find a title in a large transcript.
                head_lines = head.splitlines()
                if stat.st_size > len(head) and not head.endswith(b"\n"):
                    head_lines = head_lines[:-1]
                start = max(0, stat.st_size - 128 * 1024)
                source.seek(start)
                if start:
                    source.readline()
                lines = source.read(128 * 1024).splitlines()
            title = ""
            for line in head_lines:
                try:
                    record = json.loads(line)
                except (ValueError, UnicodeDecodeError, RecursionError):
                    continue
                if (
                    isinstance(record, dict)
                    and record.get("type") == "user"
                    and not record.get("isMeta")
                ):
                    title = summary(_assistant_text(record.get("message")), 120)
                    if title:
                        break
            for line in reversed(lines):
                try:
                    record = json.loads(line)
                except (ValueError, UnicodeDecodeError, RecursionError):
                    continue
                if isinstance(record, dict) and record.get("type") == "assistant":
                    text = _assistant_text(record.get("message"))
                    if text:
                        return title, text, datetime.fromtimestamp(stat.st_mtime, UTC)
            if title:
                return title, "", None
        except OSError:
            continue
    return "", "", None


@dataclass
class _Question:
    request_id: str
    tool_input: dict[str, Any]
    keys: dict[str, str]
    answer: asyncio.Future[dict[str, str]]
    delivered: asyncio.Future[None]
    identity: str


@dataclass
class _Owned:
    session_id: str
    process: asyncio.subprocess.Process
    initialized: asyncio.Future[None]
    reader: asyncio.Task[None] | None = None
    stderr: asyncio.Task[None] | None = None
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    approvals: dict[str, asyncio.Task[None]] = field(default_factory=dict)
    question: _Question | None = None
    closed: bool = False


class ClaudeAdapter:
    def __init__(
        self,
        store: StateStore,
        *,
        executable: str | Sequence[str] = "claude",
        env: Mapping[str, str] | None = None,
        settings: str | Path | None = None,
        config_dir: str | Path | None = None,
        experimental_inbox: bool = False,
        permission_transport: str = "stdio",
        startup_timeout: float = 30,
        approval_timeout: float = 120,
        discovery_timeout: float = 10,
        write_timeout: float = 5,
        max_budget_usd: float | None = None,
        task_owner=None,
    ):
        self.task_owner = task_owner
        self.store = store
        if executable == "claude":
            # launchd's PATH lacks ~/.local/bin, where the Claude installer puts the CLI.
            from .usage import resolve_claude_binary

            executable = str(resolve_claude_binary() or "claude")
        self.command = [executable] if isinstance(executable, str) else list(executable)
        self.env = dict(env) if env is not None else None
        self.settings = str(settings) if settings is not None else None
        self.config_dir = Path(
            config_dir
            or (env or os.environ).get(
                "CLAUDE_CONFIG_DIR", str(Path.home() / ".claude")
            )
        )
        self.experimental_inbox = experimental_inbox
        if permission_transport not in ("stdio", "hooks"):
            raise ValueError("permission_transport must be stdio or hooks")
        if permission_transport == "hooks" and settings is None:
            raise ValueError(
                "hooks permission transport requires an explicit settings file"
            )
        self.permission_transport = permission_transport
        self.startup_timeout = startup_timeout
        self.approval_timeout = approval_timeout
        self.discovery_timeout = discovery_timeout
        self.write_timeout = write_timeout
        self.max_budget_usd = max_budget_usd
        self._owned: dict[str, _Owned] = {}
        self._observed: set[str] = set()
        self._closed = False

    def bind_task_owner(self, owner):
        self.task_owner = owner

    def transcript_sources(self, sessions: list[Session]) -> list[TranscriptSource]:
        """Select admitted Claude transcripts without exposing config layout."""
        result = []
        for session in sessions:
            if session.kind != "claude":
                continue
            native = session.id.removeprefix("claude:")
            try:
                uuid.UUID(native)
            except ValueError:
                continue
            for path in (self.config_dir / "projects").glob(f"*/{native}.jsonl"):
                if not path.is_symlink():
                    result.append(TranscriptSource(path, "claude", session.id))
        return result

    def owns(self, session_id: str) -> bool:
        return session_id in self._owned

    def can_message(self, session_id: str) -> bool:
        owned = self._owned.get(session_id)
        session = self.store.get(session_id)
        return bool(
            not self._closed
            and owned is not None
            and not owned.closed
            and owned.process.returncode is None
            and owned.process.stdin is not None
            and session is not None
            and session.state not in {"done", "error", "needs_approval"}
        )

    def _environment(self) -> dict[str, str]:
        result = dict(os.environ if self.env is None else self.env)
        result.pop("CLAUDECODE", None)
        result["CLAUDE_CONFIG_DIR"] = str(self.config_dir)
        return result

    async def _capture(self, *args: str, command: Sequence[str] | None = None) -> bytes:
        process = await asyncio.create_subprocess_exec(
            *(command or self.command),
            *args,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            env=self._environment(),
            limit=_MAX_DISCOVERY,
        )
        try:
            async with asyncio.timeout(self.discovery_timeout):
                assert process.stdout is not None
                output = bytearray()
                while chunk := await process.stdout.read(64 * 1024):
                    output.extend(chunk)
                    if len(output) > _MAX_DISCOVERY:
                        raise AgentError(
                            "discovery_failed",
                            "Claude discovery output exceeds size limit",
                        )
                await process.wait()
            if process.returncode:
                raise AgentError(
                    "discovery_failed",
                    f"Claude discovery exited with code {process.returncode}",
                )
            return bytes(output)
        finally:
            if process.returncode is None:
                process.kill()
                await process.wait()

    async def refresh(self) -> None:
        if self._closed:
            raise SessionUnavailable("Claude adapter is closed")
        try:
            payload = json.loads(await self._capture("agents", "--json"))
        except (OSError, ValueError, TimeoutError, RecursionError) as exc:
            raise AgentError(
                "discovery_failed",
                f"Claude discovery unavailable ({type(exc).__name__})",
            ) from exc
        if not isinstance(payload, list):
            raise AgentError(
                "discovery_failed", "Claude agents output must be a JSON array"
            )
        try:
            desktop = _desktop_pids(
                (
                    await self._capture("-A", "-o", "pid=,ppid=,comm=", command=["ps"])
                ).decode(errors="replace")
            )
        except (OSError, AgentError, TimeoutError):
            desktop = set()
        seen: set[str] = set()
        for record in payload:
            if not isinstance(record, dict):
                continue
            native_id = (
                record.get("sessionId") or record.get("session_id") or record.get("id")
            )
            session_id = f"claude:{native_id}"
            if self.owns(session_id):
                previous = self.store.get(session_id)
                title = _agent_title(record)
                if previous and title:
                    self.store.update(
                        replace(previous, title=title, title_source="name")
                    )
                continue
            title, text, timestamp = await asyncio.to_thread(
                _transcript_metadata, self.config_dir, str(native_id)
            )
            # Hooks may arrive while transcript I/O yields. Merge with the
            # current record, so a partial listing cannot erase a new title.
            previous = self.store.get(session_id)
            pid = record.get("pid")
            origin = "desktop app" if type(pid) is int and pid in desktop else "cli"
            session = parse_agent_record(record, origin=origin, previous=previous)
            if session is None:
                continue
            if title and (not session.title or session.title_source == "hook"):
                session = replace(session, title=title, title_source="prompt")
            if text:
                session = replace(
                    session,
                    last_assistant_message=text,
                    updated_at=max(session.updated_at, timestamp or session.updated_at),
                )
            if previous and (session.state, session.waiting_for, session.title) != (
                previous.state,
                previous.waiting_for,
                previous.title,
            ):
                session = replace(session, updated_at=utcnow())
            self.store.update(session)
            seen.add(session.id)
        for disappeared in self._observed - seen:
            previous = self.store.get(disappeared)
            if previous:
                self.store.resolve_session(disappeared, "session_disappeared")
                self.store.update(
                    replace(
                        previous, state="done", waiting_for=None, updated_at=utcnow()
                    )
                )
        self._observed = seen

    async def start(self, cwd: str, prompt: str, model: str | None = None) -> Session:
        if self._closed:
            raise SessionUnavailable("Claude adapter is closed")
        directory = await asyncio.to_thread(
            lambda: Path(cwd).expanduser().resolve(strict=True)
        )
        if not await asyncio.to_thread(directory.is_dir):
            raise ValueError("cwd must be a directory")
        if not prompt.strip():
            raise ValueError("prompt must not be empty")
        native_id = str(uuid.uuid4())
        session_id = f"claude:{native_id}"
        command = [
            *self.command,
            "-p",
            "--input-format",
            "stream-json",
            "--output-format",
            "stream-json",
            "--verbose",
            "--include-partial-messages",
            "--permission-mode",
            "manual",
            "--permission-prompts",
            "none" if self.permission_transport == "hooks" else "host",
            "--session-id",
            native_id,
        ]
        if model:
            command.extend(["--model", model])
        # Controlled sessions don't silently inherit user/project hooks, MCPs,
        # or permission bypass settings. Explicit settings may opt into hooks.
        command.extend(
            [
                "--settings",
                self.settings or "{}",
                "--setting-sources",
                "",
                "--strict-mcp-config",
            ]
        )
        if self.max_budget_usd is not None:
            command.extend(["--max-budget-usd", str(self.max_budget_usd)])
        try:
            process = await asyncio.create_subprocess_exec(
                *command,
                cwd=directory,
                env=self._environment(),
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                limit=_MAX_LINE,
            )
        except OSError as exc:
            raise AgentError(
                "start_failed", f"Claude could not start ({type(exc).__name__})"
            ) from exc
        owned = _Owned(session_id, process, asyncio.get_running_loop().create_future())
        # close() can run while subprocess creation yields. Never publish or
        # send to a child created after shutdown; terminate and reap it first.
        if self._closed:
            await self._stop(owned)
            raise SessionUnavailable("Claude adapter closed while starting")
        session = self.store.update(
            Session(
                session_id,
                "claude",
                "hub-started",
                summary(prompt, 120),
                str(directory),
                title_source="prompt",
            )
        )
        self._owned[session_id] = owned
        owned.reader = spawn(
            self._read(owned), name="orb-claude-reader", owner=self.task_owner
        )
        owned.stderr = spawn(
            self._drain_stderr(owned), name="orb-claude-stderr", owner=self.task_owner
        )
        try:
            await self._write(
                owned,
                {
                    "type": "control_request",
                    "request_id": "orb_initialize",
                    "request": {"subtype": "initialize", "hooks": None},
                },
            )
            async with asyncio.timeout(self.startup_timeout):
                await asyncio.shield(owned.initialized)
            await self.send(session_id, prompt)
        except BaseException:
            await self._stop(owned)
            self.store.fail_session(session_id, "Claude session failed to initialize")
            raise
        return self.store.get(session_id) or session

    async def send(self, session_id: str, text: str) -> DeliveryResult:
        if not isinstance(text, str) or not text.strip() or len(text) > 8192:
            raise ValueError("text must contain 1 to 8192 characters")
        owned = self._owned.get(session_id)
        if owned is None:
            if self.experimental_inbox:
                raise UnsupportedOperation(
                    "Experimental Claude inbox delivery is unavailable: its wire format has not been verified; start a hub-owned session"
                )
            raise UnsupportedOperation(
                "Existing Claude desktop and CLI sessions are read-only; start a hub-owned session to send messages"
            )
        session = self.store.get(session_id)
        if (
            owned.closed
            or owned.process.returncode is not None
            or session is None
            or session.state in ("done", "error")
        ):
            raise SessionUnavailable(
                "Claude session has exited or failed; start a new session"
            )
        if question := owned.question:
            if question.answer.done():
                raise SessionUnavailable(
                    "Claude already received this question's answer"
                )
            if len(question.keys) == 1:
                answers = {next(iter(question.keys.values())): text}
            else:
                try:
                    values = json.loads(text)
                except (ValueError, RecursionError) as exc:
                    raise AgentError(
                        "bad_request",
                        "Multiple questions need a JSON object mapping every question ID (q1, q2, ...) to an answer",
                    ) from exc
                if (
                    not isinstance(values, dict)
                    or set(values) != set(question.keys)
                    or any(
                        not isinstance(value, str) or not value.strip()
                        for value in values.values()
                    )
                ):
                    raise AgentError(
                        "bad_request",
                        "Answer every question ID (q1, q2, ...) with a nonempty string",
                    )
                answers = {question.keys[key]: value for key, value in values.items()}
            question.answer.set_result(answers)
            self.store.update(
                replace(session, state="working", waiting_for=None, updated_at=utcnow())
            )
            await asyncio.shield(question.delivered)
            return DeliveryResult("delivered", "Claude accepted the question answer")
        if session.state == "needs_approval":
            raise SessionUnavailable(
                "Claude is waiting for permission; answer the pending approval first"
            )
        self.store.update(
            replace(session, state="working", waiting_for=None, updated_at=utcnow())
        )
        # The reader may receive a complete turn while drain yields. Publish
        # before the write so its newer state/summary is never overwritten.
        try:
            await self._write(
                owned,
                {
                    "type": "user",
                    "session_id": session_id.removeprefix("claude:"),
                    "uuid": str(uuid.uuid4()),
                    "parent_tool_use_id": None,
                    "client_composed": True,
                    "message": {"role": "user", "content": text},
                },
            )
        except (AgentError, OSError):
            current = self.store.get(session_id)
            if current is not None and current.state == "working":
                self.store.fail_session(session_id, "Claude message delivery failed")
            raise
        return DeliveryResult("delivered", "Claude stream accepted the user message")

    async def answer_question(
        self, session_id: str, question_id: str, text: str
    ) -> None:
        owned = self._owned.get(session_id)
        question = owned.question if owned else None
        if (
            question is None
            or question.identity != question_id
            or question.answer.done()
        ):
            raise AgentError("stale_question", "Question is no longer pending")
        # send claims the future synchronously, before its first transport await.
        await self.send(session_id, text)

    async def _write(self, owned: _Owned, message: dict[str, Any]) -> None:
        async with owned.lock:
            if (
                owned.closed
                or owned.process.returncode is not None
                or owned.process.stdin is None
            ):
                raise SessionUnavailable("Claude stream is closed")
            if message.get("type") == "user":
                session = self.store.get(owned.session_id)
                if (
                    self._closed
                    or self._owned.get(owned.session_id) is not owned
                    or session is None
                    or session.state in {"done", "error", "needs_approval"}
                ):
                    raise SessionUnavailable("Claude can no longer accept this message")
            try:
                owned.process.stdin.write(
                    (json.dumps(message, ensure_ascii=True) + "\n").encode()
                )
                async with asyncio.timeout(self.write_timeout):
                    await owned.process.stdin.drain()
            except TimeoutError as exc:
                raise AgentError(
                    "timeout",
                    "Claude transport timed out; delivery is uncertain and was not retried",
                ) from exc
            except (BrokenPipeError, ConnectionResetError) as exc:
                raise SessionUnavailable(
                    "Claude stream closed before accepting the message"
                ) from exc

    async def _drain_stderr(self, owned: _Owned) -> None:
        assert owned.process.stderr is not None
        # Stderr may contain prompts, paths, or provider credentials. Drain it
        # without retaining or publishing it; normalized result errors are used.
        while await owned.process.stderr.read(8192):
            pass

    async def _read(self, owned: _Owned) -> None:
        assert owned.process.stdout is not None
        try:
            while line := await owned.process.stdout.readline():
                if not line.strip() or not line.lstrip().startswith(b"{"):
                    continue
                message = json.loads(line)
                if not isinstance(message, dict):
                    continue
                await self._handle(owned, message)
            code = await owned.process.wait()
            if not owned.closed:
                self.store.fail_session(
                    owned.session_id, f"Claude process exited (code {code})"
                )
        except (
            ValueError,
            TypeError,
            OSError,
            RecursionError,
            asyncio.LimitOverrunError,
        ) as exc:
            self.store.fail_session(
                owned.session_id, f"Claude stream failed ({type(exc).__name__})"
            )
        finally:
            if owned.process.returncode is None and not owned.closed:
                with contextlib.suppress(ProcessLookupError):
                    owned.process.terminate()
                try:
                    async with asyncio.timeout(3):
                        await owned.process.wait()
                except TimeoutError:
                    with contextlib.suppress(ProcessLookupError):
                        owned.process.kill()
                    await owned.process.wait()
            if not owned.initialized.done():
                owned.initialized.set_exception(
                    AgentError(
                        "start_failed",
                        "Claude closed before the initialization handshake",
                    )
                )
            for task in list(owned.approvals.values()):
                task.cancel()
            self.store.resolve_session(owned.session_id, "stream_closed")

    async def _handle(self, owned: _Owned, message: dict[str, Any]) -> None:
        kind = message.get("type")
        if kind == "control_response":
            response = message.get("response", {})
            if not isinstance(response, dict):
                raise ValueError("Invalid Claude control response")
            if (
                response.get("request_id") == "orb_initialize"
                and not owned.initialized.done()
            ):
                if response.get("subtype") == "error":
                    owned.initialized.set_exception(
                        AgentError(
                            "start_failed", "Claude rejected stream initialization"
                        )
                    )
                else:
                    owned.initialized.set_result(None)
        elif kind == "control_request":
            request_id = message.get("request_id")
            if not isinstance(request_id, str) or request_id in owned.approvals:
                return
            task = spawn(
                self._control(owned, request_id, message.get("request", {})),
                name="orb-claude-control",
                owner=self.task_owner,
            )
            owned.approvals[request_id] = task
            task.add_done_callback(
                lambda _task, key=request_id: owned.approvals.pop(key, None)
            )
        elif kind == "control_cancel_request":
            request_id = message.get("request_id")
            if not isinstance(request_id, str):
                raise ValueError("Invalid Claude cancellation request")
            if task := owned.approvals.get(request_id):
                task.cancel()
        elif kind == "assistant":
            if message.get("parent_tool_use_id"):
                return
            if message.get("is_api_error_message") or message.get("error"):
                self.store.fail_session(
                    owned.session_id,
                    "Claude authentication is unavailable in this configuration"
                    if message.get("error") == "authentication_failed"
                    else "Claude provider request failed",
                )
                return
            session = self.store.get(owned.session_id)
            if session:
                self.store.update(
                    replace(
                        session,
                        state="working",
                        last_assistant_message=_assistant_text(message.get("message"))
                        or session.last_assistant_message,
                        updated_at=utcnow(),
                    )
                )
        elif kind == "result":
            # A request task may not have run yet when the result frame arrives.
            # Cancel all queued callbacks as well as already-visible questions.
            for task in list(owned.approvals.values()):
                task.cancel()
            owned.question = None
            error = (
                bool(message.get("is_error"))
                or message.get("subtype", "success") != "success"
            )
            self.store.turn_complete(
                owned.session_id,
                summary="" if error else summary(message.get("result", "")),
                error="Claude turn failed" if error else None,
            )
            if error:
                # Provider errors can include request bodies or credentials.
                # Expose only diagnostic categories, never the raw error.
                diagnostic = str(message.get("result", "")).lower()
                if any(
                    term in diagnostic
                    for term in (
                        "not logged",
                        "api key",
                        "authentication",
                        "unauthorized",
                    )
                ):
                    reason = (
                        "Claude authentication is unavailable in this configuration"
                    )
                elif any(
                    term in diagnostic
                    for term in (
                        "eperm",
                        "operation not permitted",
                        "permission denied",
                    )
                ):
                    reason = "Claude was blocked by filesystem or sandbox permissions"
                else:
                    reason = "Claude turn failed; verify authentication, budget, and model availability"
                self.store.fail_session(owned.session_id, reason)

    async def _control(
        self, owned: _Owned, request_id: str, request: dict[str, Any]
    ) -> None:
        try:
            with delivery_receipt() as delivery:
                if (
                    not isinstance(request, dict)
                    or request.get("subtype") != "can_use_tool"
                ):
                    await self._write(
                        owned,
                        {
                            "type": "control_response",
                            "response": {
                                "subtype": "error",
                                "request_id": request_id,
                                "error": "Unsupported Orb control request",
                            },
                        },
                    )
                    return
                tool = request.get("tool_name", "unknown")
                tool_input = request.get("input", {})
                if tool == "AskUserQuestion":
                    response = await self._question(
                        owned, request_id, tool_input, delivery
                    )
                else:
                    approval_id = f"{owned.session_id}:{request_id}"
                    decision = await self.store.request_approval(
                        owned.session_id,
                        str(tool),
                        tool_input,
                        request_id=approval_id,
                        timeout=self.approval_timeout,
                        delivery=delivery,
                    )
                    response = {"behavior": decision.decision}
                    if decision.decision == "allow":
                        response["updatedInput"] = tool_input
                    else:
                        response["message"] = (
                            decision.message or "Permission denied by Orb"
                        )
                await self._write(
                    owned,
                    {
                        "type": "control_response",
                        "response": {
                            "subtype": "success",
                            "request_id": request_id,
                            "response": response,
                        },
                    },
                )
        except asyncio.CancelledError:
            self.store.resolve(f"{owned.session_id}:{request_id}", reason="external")
            raise
        except (AgentError, OSError, ValueError):
            self.store.resolve(
                f"{owned.session_id}:{request_id}", reason="transport_error"
            )
            self.store.fail_session(
                owned.session_id, "Could not deliver Claude permission response"
            )

    async def _question(
        self,
        owned: _Owned,
        request_id: str,
        tool_input: object,
        delivery: asyncio.Future[None],
    ) -> dict[str, Any]:
        questions = (
            tool_input.get("questions") if isinstance(tool_input, dict) else None
        )
        if (
            not isinstance(questions, list)
            or not 1 <= len(questions) <= 4
            or not all(
                isinstance(item, dict)
                and isinstance(item.get("question"), str)
                and item["question"].strip()
                for item in questions
            )
        ):
            return {"behavior": "deny", "message": "Unsupported Claude question format"}
        if owned.question is not None:
            return {
                "behavior": "deny",
                "message": "Another question is already awaiting an answer",
            }
        keys = {
            f"q{index}": item["question"] for index, item in enumerate(questions, 1)
        }
        if len(set(keys.values())) != len(keys):
            return {"behavior": "deny", "message": "Question texts must be distinct"}
        question = _Question(
            request_id,
            tool_input,
            keys,
            asyncio.get_running_loop().create_future(),
            delivery,
            f"{owned.session_id}:{request_id}:{uuid.uuid4().hex}",
        )
        owned.question = question
        if session := self.store.get(owned.session_id):
            self.store.update(
                replace(
                    session,
                    state="waiting_input",
                    question_id=question.identity,
                    waiting_for="; ".join(
                        f"{key}: {summary(text, 60)}" for key, text in keys.items()
                    ),
                    updated_at=utcnow(),
                )
            )
        try:
            async with asyncio.timeout(self.approval_timeout):
                answers = await question.answer
            return {
                "behavior": "allow",
                "updatedInput": {**tool_input, "answers": answers},
            }
        except TimeoutError:
            return {"behavior": "deny", "message": "User input request expired"}
        finally:
            if owned.question is question:
                owned.question = None
            session = self.store.get(owned.session_id)
            if session and session.state == "waiting_input":
                self.store.update(
                    replace(
                        session, state="working", waiting_for=None, updated_at=utcnow()
                    )
                )

    async def _stop(self, owned: _Owned) -> None:
        if owned.closed:
            return
        owned.closed = True
        self.store.resolve_session(owned.session_id, "shutdown")
        for task in list(owned.approvals.values()):
            task.cancel()
        if owned.process.stdin:
            owned.process.stdin.close()
        if owned.process.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                owned.process.terminate()
            try:
                async with asyncio.timeout(3):
                    await owned.process.wait()
            except TimeoutError:
                with contextlib.suppress(ProcessLookupError):
                    owned.process.kill()
                await owned.process.wait()
        tasks = [
            task
            for task in (owned.reader, owned.stderr, *owned.approvals.values())
            if task
        ]
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if not owned.initialized.done():
            owned.initialized.cancel()
        elif not owned.initialized.cancelled():
            owned.initialized.exception()

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        await asyncio.gather(*(self._stop(owned) for owned in self._owned.values()))
        for session_id in self._owned:
            if session := self.store.get(session_id):
                self.store.update(
                    replace(
                        session, state="done", waiting_for=None, updated_at=utcnow()
                    )
                )
