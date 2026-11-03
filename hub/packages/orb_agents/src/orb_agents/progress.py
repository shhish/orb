"""Bounded local progress reduction; raw records never cross the adapter boundary.

JSONL is an observation source, not a token transport. Replaying a tail is safe:
record and item identity deduplication preserve stable replacement snapshots.
"""

from __future__ import annotations

import difflib
import hashlib
import json
import math
import os
import re
import shlex
import time
from collections import OrderedDict
from datetime import datetime
from pathlib import Path

from .models import summary


def clip(text: object, limit: int) -> str:
    value = str(text or "")
    # Assignments outside the usual credential names can also carry secrets.
    value = re.sub(
        r"\b[A-Za-z_][A-Za-z0-9_]*\s*=\s*(?:\"[^\"]*(?:\"|$)|'[^']*(?:'|$)|[^\s;,]+)",
        "[redacted]",
        value,
    )
    value = summary(value, max(limit * 4, 2))
    value = re.sub(r"\b[0-9a-fA-F]{24,}\b", "[redacted]", value)
    encoded = value.encode("utf-8", "replace")
    if len(encoded) <= limit:
        return value
    return encoded[: max(0, limit - 3)].decode("utf-8", "ignore").rstrip() + (
        "…" if limit >= 3 else ""
    )


def _identity(value: object) -> str:
    text = str(value or "")
    if not text:
        return ""
    # IDs can be opaque; never forward arbitrary input masquerading as an ID.
    if re.fullmatch(r"[A-Za-z0-9_.:-]{1,64}", text):
        return text
    return hashlib.sha256(text.encode()).hexdigest()[:32]


def _digest(value: object) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, default=str).encode()
    ).hexdigest()


def _remember(values: OrderedDict, key: str, maximum: int = 2048) -> bool:
    if key in values:
        return False
    values[key] = None
    while len(values) > maximum:
        values.popitem(last=False)
    return True


class JsonlTail:
    """Read complete records with bounded I/O and bounded partial-line storage.

    A short anchor detects in-place truncate/rewrite even when the new file has
    already grown beyond the prior offset. On first attach, only the recent
    bounded tail is consumed, which keeps discovery independent of archive size.
    """

    def __init__(
        self,
        *,
        max_read_bytes: int = 2 * 1024 * 1024,
        max_line_bytes: int = 1024 * 1024,
    ):
        if max_read_bytes < 1 or max_line_bytes < 1:
            raise ValueError("Tail bounds must be positive")
        self.max_read_bytes = max_read_bytes
        self.max_line_bytes = max_line_bytes
        self._key = None
        self._offset = 0
        self._partial = b""
        self._skip = False
        self._anchor = b""
        self._seen: OrderedDict[str, None] = OrderedDict()

    def read(self, path: Path | str) -> list[dict]:
        path = Path(path)
        with path.open("rb") as stream:
            metadata = os.fstat(stream.fileno())
            key = (str(path), metadata.st_dev, metadata.st_ino)
            reset = key != self._key or metadata.st_size < self._offset
            if not reset and self._anchor:
                stream.seek(max(0, self._offset - len(self._anchor)))
                reset = stream.read(len(self._anchor)) != self._anchor
            if reset:
                self._key = key
                self._offset = max(0, metadata.st_size - self.max_read_bytes)
                self._partial = b""
                self._skip = self._offset > 0
            stream.seek(self._offset)
            chunk = stream.read(self.max_read_bytes)
            self._offset += len(chunk)
            stream.seek(max(0, self._offset - 64))
            self._anchor = stream.read(min(self._offset, 64))
        data = self._partial + chunk
        self._partial = b""
        lines = data.split(b"\n")
        ending = lines.pop()
        result = []
        for line in lines:
            if self._skip:
                self._skip = False
                continue
            if len(line) > self.max_line_bytes:
                continue
            try:
                record = json.loads(line)
            except (UnicodeDecodeError, ValueError, RecursionError):
                continue
            if isinstance(record, dict) and _remember(self._seen, _digest(record)):
                result.append(record)
        if len(ending) <= self.max_line_bytes:
            self._partial = ending
        else:
            self._skip = True
        return result


def _text(content: object) -> str:
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    return " ".join(
        block["text"]
        for block in content
        if isinstance(block, dict)
        and block.get("type") in ("text", "output_text", "summary_text")
        and isinstance(block.get("text"), str)
    )


def display_text(text: object, limit: int = 160) -> str:
    """Plain, bounded display prose; code and link destinations stay on the Mac."""
    value = str(text or "")
    value = re.sub(r"```.*?(?:```|$)|~~~.*?(?:~~~|$)", " ", value, flags=re.S)
    value = re.sub(r"(?m)^(?: {4}|\t).*$", " ", value)
    value = re.sub(r"!?\[([^]\n]*)\]\([^\n)]*\)", r"\1", value)
    value = re.sub(r"<https?://[^>]*>|https?://\S+", "", value)
    value = re.sub(r"(?m)^\s*(?:#{1,6}\s+|>\s*|[-*+]\s+|\d+[.)]\s+)", "", value)
    value = re.sub(r"(\*\*|__|~~)(.*?)\1", r"\2", value)
    value = re.sub(r"(?<!\w)[*_]([^*_\n]+)[*_](?!\w)", r"\1", value)
    value = value.replace("`", "")
    # File locations are useful names, never a reason to publish a home path.
    value = re.sub(
        r"(?:~?/|[A-Za-z]:\\)(?:[^\s<>\"'()]+[/\\])+([^\s<>\"'()]+)",
        r"\1",
        value,
    )
    return clip(value, limit)


def last_sentence(text: str, *, final: bool = False, limit: int = 160) -> str:
    """Hold the last complete sentence while a streamed fragment is still growing.

    A final message boundary also completes punctuation-free prose. Truncation
    keeps the beginning of that sentence, with an ellipsis, rather than an
    unexplained suffix. Stable completed text avoids per-token label flicker.
    """
    value = display_text(text, max(limit, len(text.encode("utf-8", "replace"))))
    previous_end = last_end = 0
    for punctuation in re.finditer(r"[.!?](?=\s|$)", value):
        previous_end, last_end = last_end, punctuation.end()
    tail = value[last_end:].strip()
    sentence = tail if final and tail else value[previous_end:last_end].strip()
    if len(sentence.encode("utf-8")) <= limit:
        return sentence
    return (
        sentence.encode("utf-8")[: limit - 3].decode("utf-8", "ignore").rstrip() + "…"
    )


def _basename(value: object) -> str:
    return clip(str(value or "").replace("\\", "/").rstrip("/").rsplit("/", 1)[-1], 44)


def _search_label(pattern: object) -> str:
    if not isinstance(pattern, str) or not pattern.strip():
        return "Searched files"
    # Search text is untrusted too; omit assignment values, URLs and paths.
    query = display_text(pattern, 40)
    if not query or "[redacted]" in query:
        return "Searched files"
    return clip(f"Searched for {query}", 64)


def _command_words(command: object) -> list[str]:
    if isinstance(command, list):
        return [str(word) for word in command]
    if not isinstance(command, str) or re.search(r"[\n\r;|`<>]|\$[({]", command):
        return []
    # A common agent command is `cd project && nice make sim-test`. Inspect only
    # the operation after a simple directory change, never forward its path.
    parts = command.split("&&")
    if len(parts) > 2 or "&" in "".join(parts):
        return []
    try:
        if len(parts) == 2:
            prefix = shlex.split(parts[0])
            if len(prefix) != 2 or prefix[0] != "cd":
                return []
        words = shlex.split(parts[-1])
    except (ValueError, RecursionError):
        return []
    while words:
        if re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]):
            words.pop(0)
        elif words[0] in {"env", "command"}:
            words.pop(0)
        elif words[0] == "nice":
            words.pop(0)
            if words[:1] == ["-n"]:
                words = words[2:]
        else:
            break
    return words


def _shell_search(words: list[str]) -> str:
    takes_value = {
        "-g",
        "--glob",
        "-t",
        "--type",
        "-T",
        "--type-not",
        "--iglob",
        "-A",
        "-B",
        "-C",
        "--max-count",
        "-m",
    }
    skip = False
    for word in words[1:]:
        if skip:
            skip = False
            continue
        if word in takes_value:
            skip = True
        elif word in {"-e", "--regexp"}:
            continue
        elif word == "--files":
            return "Listed files"
        elif not word.startswith("-"):
            return _search_label(word)
    return "Searched files"


def tool_label(name: str, value: object) -> str:
    """Expose operation metadata, never a tool body or an arbitrary shell script."""
    tool = name.rsplit(".", 1)[-1].lower()
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except (ValueError, RecursionError):
            if tool in {"apply_patch", "patch"}:
                match = re.search(r"\*\*\* (?:Update|Add|Delete) File: ([^\n]+)", value)
                return clip(
                    f"Edited {_basename(match[1])}" if match else "Edited files", 64
                )
            value = {"command": value}
    value = value if isinstance(value, dict) else {}
    paths = value.get("file_paths", value.get("paths"))
    path = value.get("file_path", value.get("path", value.get("filename")))
    if tool in {"read", "read_file", "readfile", "read_many_files"}:
        if isinstance(paths, list):
            return clip(f"Read {len(paths)} files", 64)
        return clip(f"Read {_basename(path)}" if path else "Read files", 64)
    if tool in {"edit", "write", "write_file", "edit_file", "multiedit", "apply_patch"}:
        return clip(f"Edited {_basename(path)}" if path else "Edited files", 64)
    if tool in {"glob", "grep", "search", "rg"}:
        return _search_label(value.get("pattern", value.get("query")))
    command = value.get("command", value.get("cmd", ""))
    words = _command_words(command)
    # Environment assignment values are never forwarded, even for unknown keys.
    words = [word for word in words if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", word)]
    if words:
        executable = _basename(words[0])
        if executable in {"cat", "head", "tail"}:
            files = [word for word in words[1:] if not word.startswith("-")]
            return clip(
                f"Read {len(files)} files"
                if len(files) > 1
                else f"Read {_basename(files[0])}"
                if files
                else "Read files",
                64,
            )
        if executable in {"rg", "grep"}:
            return _shell_search(words)
        if executable == "find":
            return "Searched files"
        # A small vocabulary makes useful build/test labels without publishing
        # arbitrary positional values (which often include tokens or prompts).
        safe = {
            "build",
            "clean",
            "fullclean",
            "test",
            "check",
            "lint",
            "format",
            "--check",
            "-q",
            "-v",
            "-x",
            "--quiet",
            "pytest",
            "-m",
        }
        arguments = [
            word for word in words[1:] if word in safe or re.fullmatch(r"-j\d+", word)
        ]
        if executable in {"make", "gmake", "ninja"}:
            arguments = [
                word
                for word in words[1:]
                if word in safe
                or re.fullmatch(r"-j\d+", word)
                or re.fullmatch(
                    r"(?:[a-z0-9]+[-_])*(?:test|build|check|lint|clean|format|emu)",
                    word,
                )
            ]
        if executable == "cmake":
            arguments = [word for word in words[1:] if word in {"--build", "-j4"}]
        if re.fullmatch(r"[A-Za-z0-9_.+-]{1,32}", executable):
            return clip("Ran " + " ".join([executable, *arguments]), 64)
    if tool in {"bash", "exec_command", "shell", "shell_command"}:
        return "Ran shell command"
    return clip(f"Called {_basename(name.rsplit('.', 1)[-1])}", 64)


def _output(value: object, *, inspect_text: bool = True) -> tuple[bool, str | None]:
    """Only a derived exit state and test count may escape tool output."""
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except (ValueError, RecursionError):
            parsed = value
    else:
        parsed = value
    failed = False
    if isinstance(parsed, dict):
        # TaskOutput separates retrieval status from the underlying task's exit
        # status. Visit only these fixed wrapper fields, never arbitrary bodies.
        parts = [parsed]
        frontier = parts
        for _ in range(3):
            children = [
                part[field]
                for part in frontier
                for field in ("metadata", "task", "result")
                if isinstance(part.get(field), dict)
            ]
            parts.extend(children)
            frontier = children
        texts = []
        for part in parts:
            code = part.get("exit_code", part.get("exitCode"))
            failed = failed or (
                part.get("is_error") is True
                or part.get("status") in ("failed", "error", "killed", "cancelled")
                or (isinstance(code, int) and code != 0)
            )
            texts.append(
                _text(part.get("content"))
                or str(part.get("output", part.get("stdout", "")))
            )
        text = " ".join(texts)
    else:
        text = _text(parsed) if isinstance(parsed, list) else str(parsed or "")
    if not inspect_text:
        return failed, None
    exit_match = re.search(
        r"(?:Process exited with code|exit_code(?:[\"': =]+|>))\s*(-?\d+)", text
    )
    failed = failed or bool(exit_match and int(exit_match[1]) != 0)
    failed = failed or bool(
        re.search(r"<status>\s*(?:failed|error|killed|cancelled)\s*</status>", text)
    )
    passed = re.search(r"\b(\d+) passed\b", text)
    return failed, f"Tests: {passed[1]} passed" if passed else None


def _still_running(value: object) -> bool:
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except (ValueError, RecursionError):
            pass
    if isinstance(value, dict):
        for _ in range(3):
            task = value.get("task")
            if not isinstance(task, dict):
                break
            value = task
        if value.get("status") in (
            "completed",
            "done",
            "failed",
            "error",
            "killed",
            "cancelled",
        ):
            return False
        if value.get("status") in ("running", "pending", "in_progress", "queued"):
            return True
        if value.get("exit_code", value.get("exitCode")) is not None:
            return False
        return (
            value.get("session_id") is not None
            or value.get("cell_id") is not None
            or value.get("backgroundTaskId") is not None
        )
    return isinstance(value, str) and bool(
        re.search(
            r"Process running with session ID|Command running in background|Script running with cell ID|<status>\s*(?:running|pending|in_progress)\s*</status>",
            value,
        )
    )


def _process_key(value: object) -> str | None:
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except (ValueError, RecursionError):
            match = re.search(
                r"Process running with session ID (\d+)|Script running with cell ID ([\w.-]+)|Command running in background with ID: ?([\w.-]+)",
                value,
            )
            return (
                (
                    "session:" + match[1]
                    if match[1]
                    else "cell:" + match[2]
                    if match[2]
                    else "task:" + match[3]
                )
                if match
                else None
            )
    if not isinstance(value, dict):
        return None
    for field, prefix in (
        ("session_id", "session:"),
        ("cell_id", "cell:"),
        ("backgroundTaskId", "task:"),
        ("task_id", "task:"),
    ):
        handle = value.get(field)
        if isinstance(handle, (str, int)) and not isinstance(handle, bool):
            return prefix + str(handle)[:64]
    return None


def _edit_counts(value: object) -> dict[str, int]:
    """Count only source-provided edit lines, without retaining any file body."""
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except (ValueError, RecursionError):
            parsed = value
    else:
        parsed = value
    lines = None
    if isinstance(parsed, str) and "*** Begin Patch" in parsed:
        lines = parsed.splitlines()
    elif isinstance(parsed, dict):
        old = parsed.get("old_string", parsed.get("oldString"))
        new = parsed.get("new_string", parsed.get("newString"))
        if (
            isinstance(old, str)
            and isinstance(new, str)
            and len(old) + len(new) <= 16384
            and old.count("\n") + new.count("\n") <= 512
        ):
            diff = list(difflib.ndiff(old.splitlines(), new.splitlines()))
            return {
                "added": sum(line.startswith("+ ") for line in diff),
                "removed": sum(line.startswith("- ") for line in diff),
            }
        hunks = parsed.get("structuredPatch")
        if isinstance(hunks, list):
            lines = [
                line
                for hunk in hunks
                if isinstance(hunk, dict) and isinstance(hunk.get("lines"), list)
                for line in hunk["lines"]
                if isinstance(line, str)
            ]
        elif isinstance(parsed.get("diff"), str):
            lines = parsed["diff"].splitlines()
    if lines is None:
        return {}
    return {
        "added": sum(
            line.startswith("+") and not line.startswith("+++") for line in lines
        ),
        "removed": sum(
            line.startswith("-") and not line.startswith("---") for line in lines
        ),
    }


class ProgressReducer:
    def __init__(self, provider: str):
        if provider not in {"claude", "codex"}:
            raise ValueError("Unknown progress provider")
        self.provider = provider
        self.seq = 0
        self.at = 0.0
        self.state = "idle"
        self.activity = ""
        self.assistant = {"id": "", "text": "", "final": False}
        self._steps: OrderedDict[str, dict] = OrderedDict()
        self._seen: OrderedDict[str, None] = OrderedDict()
        self._current_message = ""
        self._partial_text = ""
        self._record_at = 0.0
        self._running: dict[str, str] = {}
        self._aliases: OrderedDict[str, str] = OrderedDict()

    def _trim_steps(self) -> None:
        while len(self._steps) > 128:
            removed, _ = self._steps.popitem(last=False)
            self._running = {
                handle: item
                for handle, item in self._running.items()
                if item != removed
            }
            self._aliases = OrderedDict(
                (key, item) for key, item in self._aliases.items() if item != removed
            )

    def _visible_steps(self) -> list[dict]:
        groups: list[dict] = []
        last_at = 0.0
        count = 0
        for value in self._steps.values():
            step = {key: item for key, item in value.items() if not key.startswith("_")}
            read = value.get("_read", 0)
            if read and count and 0 <= value["_at"] - last_at <= 2:
                previous = groups[-1]
                count += read
                previous["label"] = f"Read {count} files"
                # A failed member remains visible even after later reads finish.
                states = {previous["state"], step["state"]}
                previous["state"] = (
                    "failed"
                    if "failed" in states
                    else "running"
                    if "running" in states
                    else "done"
                )
            else:
                groups.append(step)
                if len(groups) > 3:
                    groups.pop(0)
                count = read
            last_at = value.get("_at", 0.0)
        return groups[-3:]

    def _value(self) -> tuple:
        return (
            self.state,
            self.activity,
            tuple(tuple(v.items()) for v in self._visible_steps()),
            tuple(self.assistant.items()),
        )

    def _assistant(self, identity: object, text: str, final: bool) -> None:
        if not text:
            return
        sentence = last_sentence(text, final=final)
        if not sentence:
            return
        # Codex can represent one assistant message in both response_item and
        # event_msg forms with different IDs. Keep its first stable identity.
        if sentence == self.assistant["text"] and final == self.assistant["final"]:
            return
        self.assistant = {
            "id": _identity(identity) or _digest(text)[:24],
            "text": sentence,
            "final": final,
        }
        self.state = "idle" if final else "busy"
        self.activity = "Reply complete" if final else "Responding"

    def _tool(self, identity: object, name: str, value: object) -> None:
        key = _identity(identity) or _digest([name, value])[:24]
        if isinstance(value, str):
            try:
                arguments = json.loads(value)
            except (ValueError, RecursionError):
                arguments = None
        else:
            arguments = value
        if name.rsplit(".", 1)[-1].lower() in {
            "write_stdin",
            "wait",
            "taskoutput",
            "task_output",
        }:
            original = self._running.get(_process_key(arguments))
            if original in self._steps:
                self._aliases[key] = original
                while len(self._aliases) > 128:
                    self._aliases.popitem(last=False)
                return
        if key not in self._steps:
            label = tool_label(name, value)
            read = re.fullmatch(r"Read (\d+) files", label)
            self._steps[key] = {
                "id": key,
                "kind": "tool",
                "label": label,
                "state": "running",
                "_at": self._record_at,
                "_command": name.rsplit(".", 1)[-1].lower()
                in {"bash", "exec_command", "shell", "shell_command"}
                and not label.startswith("Read "),
                "_read": int(read[1])
                if read
                else int(label.startswith("Read ") and label != "Read files"),
            }
            if label.startswith("Edited "):
                self._steps[key].update(_edit_counts(value))
            self._trim_steps()
        if self._steps[key]["state"] == "running":
            self.state = "busy"
            self.activity = self._steps[key]["label"]

    def _result(self, identity: object, value: object, failed: bool = False) -> None:
        key = _identity(identity)
        step = self._steps.get(self._aliases.get(key, key))
        if step is None:
            return
        inspect_text = step.get("_command", False) or key in self._aliases
        output_failed, tests = _output(value, inspect_text=inspect_text)
        if step["label"].startswith("Edited "):
            step.update(_edit_counts(value))
        state = (
            "failed"
            if failed or output_failed
            else "running"
            if (inspect_text or isinstance(value, dict)) and _still_running(value)
            else "done"
        )
        if (state == "running" and step["state"] in {"done", "failed"}) or (
            step["state"] == "failed" and state == "done"
        ):
            return
        if state == "running":
            handle = _process_key(value)
            if handle:
                self._running[handle] = step["id"]
                while len(self._running) > 128:
                    self._running.pop(next(iter(self._running)))
        else:
            self._running = {
                handle: item
                for handle, item in self._running.items()
                if item != step["id"]
            }
        if step["state"] == state and (not tests or tests == step["label"]):
            return
        step["state"] = state
        if tests:
            step["label"] = clip(tests, 64)
        self.activity = step["label"]
        # A command failure is one step the agent can recover from. Only a
        # provider turn failure makes the session an error.
        self.state = "busy"

    def _claude(self, record: dict) -> None:
        kind = record.get("type")
        if kind == "stream_event":
            event = record.get("event") or {}
            if not isinstance(event, dict):
                return
            event_type = event.get("type")
            if event_type == "message_start":
                message = event.get("message")
                self._current_message = (
                    message.get("id", "") if isinstance(message, dict) else ""
                )
                self._partial_text = ""
            elif event_type == "content_block_delta":
                delta = event.get("delta") or {}
                if not isinstance(delta, dict):
                    return
                if delta.get("type") == "text_delta":
                    self._partial_text = (
                        self._partial_text + str(delta.get("text", ""))
                    )[-4096:]
                    self._assistant(self._current_message, self._partial_text, False)
            return
        if kind == "result":
            result = record.get("result")
            if isinstance(result, str) and result:
                self._assistant(self._current_message, result, True)
            elif self._partial_text:
                self._assistant(self._current_message, self._partial_text, True)
            self.state = "error" if record.get("is_error") else "idle"
            self.activity = (
                "Turn failed" if record.get("is_error") else "Reply complete"
            )
            if self.assistant["id"]:
                self.assistant = {**self.assistant, "final": True}
            return
        message = record.get("message")
        if not isinstance(message, dict):
            return
        content = message.get("content") or []
        if kind == "assistant":
            identity = message.get("id", record.get("uuid"))
            if isinstance(content, str):
                content = [{"type": "text", "text": content}]
            blocks = (
                [block for block in content if isinstance(block, dict)]
                if isinstance(content, list)
                else []
            )
            tools = [block for block in blocks if block.get("type") == "tool_use"]
            self._assistant(
                identity, _text(blocks), message.get("stop_reason") == "end_turn"
            )
            for block in tools:
                self._tool(
                    block.get("id"), str(block.get("name", "tool")), block.get("input")
                )
        elif kind == "user" and isinstance(content, list):
            result = record.get("toolUseResult")
            blocks = [
                block
                for block in content
                if isinstance(block, dict) and block.get("type") == "tool_result"
            ]
            handled = set()
            for block in blocks:
                identity = block.get("tool_use_id")
                value = block.get("content")
                if isinstance(result, dict) and (
                    len(blocks) == 1
                    or result.get("tool_use_id", result.get("toolUseId")) == identity
                ):
                    value = {**result, "content": value}
                self._result(identity, value, block.get("is_error") is True)
                handled.add(_identity(identity))
            if isinstance(result, dict):
                identity = result.get("tool_use_id", result.get("toolUseId"))
                if identity and _identity(identity) not in handled:
                    self._result(identity, result)

    def _codex_item(self, item: dict, completed: bool = False) -> None:
        kind = item.get("type")
        if not isinstance(kind, str):
            return
        identity = item.get("call_id", item.get("id"))
        if kind in {"message", "agent_message", "agentMessage"}:
            if kind == "message" and item.get("role") != "assistant":
                return
            if kind == "agent_message" and item.get("author") not in (
                None,
                "assistant",
                "agent",
            ):
                return
            text = _text(item.get("content")) or _text(
                item.get("text", item.get("message", ""))
            )
            final = item.get("phase") != "commentary"
            self._assistant(identity, text, final)
        elif kind in {"function_call", "custom_tool_call"}:
            self._tool(
                identity,
                str(item.get("name", "tool")),
                item.get("arguments", item.get("input")),
            )
        elif kind in {"function_call_output", "custom_tool_call_output"}:
            self._result(identity, item.get("output"))
        elif kind in {"commandExecution", "command_execution"}:
            self._tool(identity, "exec_command", {"command": item.get("command")})
            if completed:
                self._result(
                    identity,
                    {
                        "output": item.get("aggregatedOutput", item.get("output")),
                        "exit_code": item.get("exitCode", item.get("exit_code")),
                    },
                    item.get("status") == "failed",
                )
        elif kind in {"mcpToolCall", "dynamicToolCall"}:
            self._tool(identity, str(item.get("tool", "tool")), item.get("arguments"))
            if completed:
                self._result(
                    identity, item.get("result"), item.get("status") == "failed"
                )
        elif kind in {"fileChange", "file_change"}:
            changes = item.get("changes") or []
            path = (
                changes[0].get("path")
                if isinstance(changes, list)
                and changes
                and isinstance(changes[0], dict)
                else None
            )
            self._tool(identity, "edit", {"path": path})
            step = self._steps.get(_identity(identity))
            if step and isinstance(changes, list):
                counts = [
                    _edit_counts(change)
                    for change in changes
                    if isinstance(change, dict)
                ]
                known = [count for count in counts if count]
                if known:
                    step.update(
                        {
                            key: sum(count[key] for count in known)
                            for key in ("added", "removed")
                        }
                    )
            if completed:
                self._result(identity, {}, item.get("status") == "failed")
        elif kind == "reasoning":
            # Never fall back to content, text or encrypted_content here.
            text = _text(item.get("summary"))
            if text:
                key = _identity(identity) or _digest(["summary", text])[:24]
                self._steps[key] = {
                    "id": key,
                    "kind": "reasoning",
                    "label": clip(text, 64),
                    "state": "done",
                }
                self.activity = self._steps[key]["label"]
                self.state = "busy"
                self._trim_steps()

    def _codex(self, record: dict) -> None:
        kind = record.get("type")
        payload = record.get("payload") or {}
        if not isinstance(payload, dict):
            return
        if kind == "response_item":
            self._codex_item(payload)
        elif kind == "event_msg":
            event = payload.get("type")
            if not isinstance(event, str):
                return
            if event == "item_completed" and isinstance(payload.get("item"), dict):
                self._codex_item(payload["item"], True)
            elif event == "agent_message":
                self._codex_item(payload)
            elif event == "agent_message_delta" and isinstance(
                payload.get("delta"), str
            ):
                identity = str(payload.get("item_id", payload.get("turn_id", "")))
                if identity and identity != self._current_message:
                    self._current_message, self._partial_text = identity, ""
                self._partial_text = (self._partial_text + payload["delta"])[-4096:]
                self._assistant(self._current_message, self._partial_text, False)
            elif event in {"task_started", "turn_started"}:
                self.state, self.activity = "busy", "Working"
                self._current_message, self._partial_text = "", ""
            elif event in {"task_complete", "turn_complete", "turn_completed"}:
                self.state, self.activity = "idle", "Reply complete"
                if payload.get("last_agent_message"):
                    self._assistant(
                        payload.get("turn_id"), str(payload["last_agent_message"]), True
                    )
                elif self.assistant["id"]:
                    self.assistant = {**self.assistant, "final": True}
            elif event == "turn_aborted":
                self.state, self.activity = "idle", "Turn stopped"
            elif event == "error":
                self.state, self.activity = "error", "Turn failed"

    def feed(self, record: dict) -> bool:
        if not isinstance(record, dict) or not _remember(self._seen, _digest(record)):
            return False
        observed = record.get("timestamp", record.get("at"))
        try:
            at = (
                datetime.fromisoformat(observed.replace("Z", "+00:00")).timestamp()
                if isinstance(observed, str)
                else float(observed)
            )
        except (TypeError, ValueError, OverflowError):
            at = time.time()
        if not math.isfinite(at) or at < 0:
            at = time.time()
        self._record_at = at
        before = self._value()
        if self.provider == "claude":
            self._claude(record)
        else:
            self._codex(record)
        if self._value() == before:
            return False
        self.at = max(self.at, at)
        self.seq += 1
        return True

    def snapshot(self, session_id: str, *, now: float | None = None) -> dict:
        now = time.time() if now is None else now
        return {
            "session": session_id,
            "seq": self.seq,
            "at": self.at,
            "state": self.state,
            "stale": not self.at or now - self.at > 900,
            "activity": clip(self.activity, 64),
            "steps": self._visible_steps(),
            "assistant": dict(self.assistant),
        }
