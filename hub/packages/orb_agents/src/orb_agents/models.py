"""Public immutable values; no transcript or raw tool input crosses this boundary."""

from __future__ import annotations

import re
import shlex
from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any, Literal

Kind = Literal["claude", "codex"]
Origin = Literal["desktop app", "cli", "hub-started"]
State = Literal["working", "waiting_input", "needs_approval", "idle", "done", "error"]
ApprovalDecision = Literal["allow", "deny"]


@dataclass(frozen=True, slots=True)
class TranscriptSource:
    """Provider-selected passive transcript input; discovery stays with its owner."""

    path: Path
    provider: Kind
    session_id: str | None = None


@dataclass(frozen=True, slots=True)
class DeliveryResult:
    """Transport outcome; admission to a queue never claims execution."""

    status: Literal["delivered", "queued"]
    reason: str


def utcnow() -> datetime:
    return datetime.now(UTC)


def summary(text: object, limit: int = 300) -> str:
    """Compact display text, excluding fenced code and common credential forms.

    This is a display redactor, not a general secret detector. Consumers must
    restrict access to session metadata, which can itself be sensitive.
    """
    value = str(text or "")
    value = re.sub(r"```.*?(?:```|$)", "[code omitted]", value, flags=re.S)
    value = re.sub(r"(?i)\b(bearer\s+)\S+", r"\1[redacted]", value)
    value = re.sub(
        r"(?i)\b(api[_-]?key|token|password|secret|authorization)\s*[:=]\s*\S+",
        r"\1=[redacted]",
        value,
    )
    value = re.sub(r"\b(?:sk|whsec)[_-][A-Za-z0-9_-]{8,}", "[redacted]", value)
    value = " ".join(value.split())
    return value if len(value) <= limit else value[: limit - 1] + "…"


def summarize_tool(tool: str, tool_input: object) -> str:
    """Describe the operation without forwarding file bodies or shell scripts.

    Unknown tools expose only parameter names. Even Bash arguments can contain
    whole files (heredocs, python -c, echo), so only the executable is shown.
    """
    if not isinstance(tool_input, dict):
        return summary(f"{tool}: input omitted")
    parts = [summary(tool, 80)]
    for key in ("file_path", "path", "cwd", "grantRoot", "host", "protocol"):
        value = tool_input.get(key)
        if isinstance(value, str):
            parts.append(f"{key}={summary(value, 100)}")
    command = tool_input.get("command")
    if isinstance(command, list) and command:
        executable = str(command[0])
    elif isinstance(command, str):
        try:
            executable = shlex.split(command)[0] if command.strip() else ""
        except ValueError:
            executable = "[shell]"
    else:
        executable = ""
    if executable:
        # An assignment, substitution, or multiline executable is not metadata.
        if re.fullmatch(r"[A-Za-z0-9_./+-]+", executable):
            parts.append(f"executable={executable[:60]}; arguments omitted")
        else:
            parts.append("shell command; contents omitted")
    permissions = tool_input.get("permissions")
    if isinstance(permissions, dict):
        network = permissions.get("network")
        if isinstance(network, dict) and network.get("enabled") is True:
            parts.append("network access requested")
        filesystem = permissions.get("fileSystem")
        if isinstance(filesystem, dict):
            for access in ("read", "write"):
                paths = filesystem.get(access)
                if isinstance(paths, list):
                    parts.append(f"filesystem {access}: {len(paths)} path(s)")
            entries = filesystem.get("entries")
            if isinstance(entries, list):
                parts.append(f"filesystem scopes: {len(entries)}")
    omitted = len(
        set(tool_input)
        - {"file_path", "path", "cwd", "grantRoot", "host", "protocol", "command"}
    )
    if omitted:
        parts.append(f"{omitted} additional input field(s) omitted")
    return summary("; ".join(parts))


@dataclass(frozen=True, slots=True)
class Session:
    id: str
    kind: Kind
    origin: Origin
    title: str
    cwd: str
    state: State = "idle"
    waiting_for: str | None = None
    last_assistant_message: str = ""
    updated_at: datetime = field(default_factory=utcnow)
    question_id: str | None = None
    # A hook can first see a later turn; canonical first-prompt metadata may
    # improve that fallback, but must never displace a provider's real name.
    title_source: Literal["name", "prompt", "hook", ""] = ""

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["updated_at"] = self.updated_at.isoformat()
        return result


@dataclass(frozen=True, slots=True)
class Decision:
    decision: ApprovalDecision
    message: str | None = None


@dataclass(frozen=True, slots=True)
class ApprovalRequest:
    request_id: str
    session_id: str
    tool: str
    summary: str
    created_at: datetime = field(default_factory=utcnow)
    scope: str = "operation"
    expires_at: datetime | None = None


@dataclass(frozen=True, slots=True)
class Event:
    type: Literal[
        "session.changed",
        "approval.requested",
        "approval.resolved",
        "turn.completed",
        "stream.gap",
    ]
    session_id: str | None = None
    session: Session | None = None
    approval: ApprovalRequest | None = None
    request_id: str | None = None
    decision: ApprovalDecision | None = None
    reason: str | None = None
    sequence: int = 0
    updated_at: datetime = field(default_factory=utcnow)

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["updated_at"] = self.updated_at.isoformat()
        if self.session:
            result["session"] = self.session.to_dict()
        if self.approval:
            result["approval"]["created_at"] = self.approval.created_at.isoformat()
            if self.approval.expires_at is not None:
                result["approval"]["expires_at"] = self.approval.expires_at.isoformat()
        return result


class AgentError(Exception):
    def __init__(self, code: str, message: str):
        self.code = code
        super().__init__(message)


class SessionUnavailable(AgentError):
    def __init__(self, message: str = "Session cannot accept a message"):
        super().__init__("session_unavailable", message)


class UnsupportedOperation(AgentError):
    def __init__(self, message: str = "This session does not support this operation"):
        super().__init__("unsupported", message)
