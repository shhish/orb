"""Validated v1.1 display models; limits are UTF-8 bytes, as on the device."""

import math

from orb_hub.envelope import BAD_REQUEST, LinkError

MAX_SESSIONS = 24
MAX_CARDS = 16
MAX_EXTENSION_MESSAGE = 4096


def fields(value: dict, allowed: set[str], required: set[str] | None = None) -> None:
    if not isinstance(value, dict) or value.keys() - allowed or (required or set()) - value.keys():
        raise LinkError(BAD_REQUEST, "invalid fields")


def text(value, limit: int, *, empty: bool = False) -> str:
    try:
        valid = isinstance(value, str) and (empty or bool(value)) and len(value.encode("utf-8")) <= limit
    except UnicodeError:
        valid = False
    if not valid:
        raise LinkError(BAD_REQUEST, f"text must fit {limit} UTF-8 bytes")
    return value


def timestamp(value):
    try:
        valid = not isinstance(value, bool) and isinstance(value, (float, int)) and math.isfinite(value) and value >= 0
    except OverflowError:
        valid = False
    if not valid:
        raise LinkError(BAD_REQUEST, "invalid timestamp")
    return value


def one_of(value, choices):
    if not isinstance(value, str) or value not in choices:
        raise LinkError(BAD_REQUEST, "invalid model value")
    return value


def boolean(value):
    if not isinstance(value, bool):
        raise LinkError(BAD_REQUEST, "expected a boolean")
    return value


def session(value: dict) -> dict:
    keys = {"id", "agent", "title", "project", "state", "reason", "summary", "updated", "can_message"}
    fields(value, keys, keys)
    return {
        "id": text(value["id"], 48),
        "agent": one_of(value["agent"], {"claude", "codex", "dot"}),
        "title": text(value["title"], 64, empty=True),
        "project": text(value["project"], 32, empty=True),
        "state": one_of(value["state"], {"busy", "waiting", "idle", "done", "error"}),
        "reason": text(value["reason"], 64, empty=True),
        "summary": text(value["summary"], 160, empty=True),
        "updated": timestamp(value["updated"]),
        "can_message": boolean(value["can_message"]),
    }


def card(value: dict) -> dict:
    required = {"id", "kind", "title", "body", "options", "reply", "priority"}
    fields(value, required | {"session", "detail", "expires", "scope"}, required)
    options = value["options"]
    if not isinstance(options, list) or len(options) > 4:
        raise LinkError(BAD_REQUEST, "a card has at most four options")
    cleaned = []
    for option in options:
        fields(option, {"id", "label", "role"}, {"id", "label", "role"})
        cleaned.append(
            {
                "id": text(option["id"], 32),
                "label": text(option["label"], 20),
                "role": one_of(option["role"], {"default", "primary", "cancel", "destructive"}),
            }
        )
    if len({o["id"] for o in cleaned}) != len(cleaned):
        raise LinkError(BAD_REQUEST, "option IDs must be unique")
    result = {
        "id": text(value["id"], 64),
        "kind": one_of(value["kind"], {"approval", "choice", "message", "attention"}),
        "title": text(value["title"], 64, empty=True),
        "body": text(value["body"], 280, empty=True),
        "options": cleaned,
        "reply": boolean(value["reply"]),
        "priority": one_of(value["priority"], {"low", "normal", "high"}),
    }
    if "session" in value:
        result["session"] = text(value["session"], 48)
    if "detail" in value:
        result["detail"] = text(value["detail"], 200, empty=True)
    if "expires" in value:
        result["expires"] = timestamp(value["expires"])
    if "scope" in value:
        if result["kind"] != "approval":
            raise LinkError(BAD_REQUEST, "only approval cards have a permission scope")
        result["scope"] = one_of(value["scope"], {"operation", "turn"})
    return result


def agent_stream(value: dict) -> dict:
    keys = {"session", "seq", "at", "state", "stale", "activity", "steps", "assistant"}
    fields(value, keys, keys)
    seq = value["seq"]
    if isinstance(seq, bool) or not isinstance(seq, int) or seq < 0:
        raise LinkError(BAD_REQUEST, "invalid stream sequence")
    steps = value["steps"]
    if not isinstance(steps, list) or len(steps) > 3:
        raise LinkError(BAD_REQUEST, "a stream has at most three steps")
    cleaned = []
    for step in steps:
        fields(step, {"id", "kind", "label", "state", "added", "removed"}, {"id", "kind", "label", "state"})
        cleaned.append(
            {
                "id": text(step["id"], 64),
                "kind": one_of(step["kind"], {"tool", "reasoning"}),
                "label": text(step["label"], 64, empty=True),
                "state": one_of(step["state"], {"running", "done", "failed"}),
            }
        )
        for key in ("added", "removed"):
            if key not in step:
                continue
            count = step[key]
            if isinstance(count, bool) or not isinstance(count, int) or not 0 <= count <= 2147483647:
                raise LinkError(BAD_REQUEST, "invalid step line count")
            cleaned[-1][key] = count
    if len({s["id"] for s in cleaned}) != len(cleaned):
        raise LinkError(BAD_REQUEST, "step IDs must be unique")
    assistant = value["assistant"]
    fields(assistant, {"id", "text", "final"}, {"id", "text", "final"})
    return {
        "session": text(value["session"], 48),
        "seq": seq,
        "at": timestamp(value["at"]),
        "state": one_of(value["state"], {"busy", "waiting", "idle", "done", "error"}),
        "stale": boolean(value["stale"]),
        "activity": text(value["activity"], 64, empty=True),
        "steps": cleaned,
        "assistant": {
            "id": text(assistant["id"], 64, empty=True),
            "text": text(assistant["text"], 160, empty=True),
            "final": boolean(assistant["final"]),
        },
    }


def usage_snapshot(value: dict) -> dict:
    fields(value, {"windows", "unknown"}, {"windows", "unknown"})
    windows, unknown = value["windows"], value["unknown"]
    if not isinstance(windows, list) or len(windows) > 10 or not isinstance(unknown, list) or len(unknown) > 2:
        raise LinkError(BAD_REQUEST, "invalid usage windows")
    cleaned = []
    keys = {"provider", "label", "used_percent", "resets_at", "observed_at", "stale", "source", "on_credits"}
    for window in windows:
        fields(window, keys, keys - {"source", "on_credits"})
        used = timestamp(window["used_percent"])
        if used > 100:
            raise LinkError(BAD_REQUEST, "invalid usage percentage")
        entry = {
            "provider": one_of(window["provider"], {"claude", "codex"}),
            "label": text(window["label"], 32),
            "used_percent": used,
            "resets_at": timestamp(window["resets_at"]),
            "observed_at": timestamp(window["observed_at"]),
            "stale": boolean(window["stale"]),
            "source": one_of(
                window.get("source", "unknown"),
                {"statusline", "usage", "rollout", "app-server", "unknown"},
            ),
        }
        if "on_credits" in window:
            entry["on_credits"] = boolean(window["on_credits"])
        cleaned.append(entry)
    unknown = [one_of(p, {"claude", "codex"}) for p in unknown]
    if len(set(unknown)) != len(unknown) or any(
        sum(w["provider"] == p for w in cleaned) > (8 if p == "claude" else 2)
        or p in unknown
        and any(w["provider"] == p for w in cleaned)
        for p in ("claude", "codex")
    ):
        raise LinkError(BAD_REQUEST, "invalid provider usage")
    return {"windows": cleaned, "unknown": unknown}
