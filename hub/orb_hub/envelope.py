"""Link envelopes (docs/protocol.md, "Link"): JSON requests, responses and events, at most 16 KB each."""

import json
import math
from dataclasses import dataclass, field
from typing import Any

MAX_MESSAGE = 16 * 1024

BAD_REQUEST = "bad_request"
UNAUTHORIZED = "unauthorized"
NOT_FOUND = "not_found"
UNKNOWN_METHOD = "unknown_method"
BUSY = "busy"
INTERNAL = "internal"
TIMEOUT = "timeout"
UNSUPPORTED = "unsupported"
CONFLICT = "conflict"
EXPIRED = "expired"
NOT_LISTENING = "not_listening"
TOO_LARGE = "too_large"
ERROR_CODES = frozenset(
    {
        BAD_REQUEST,
        UNAUTHORIZED,
        NOT_FOUND,
        UNKNOWN_METHOD,
        BUSY,
        INTERNAL,
        TIMEOUT,
        UNSUPPORTED,
        CONFLICT,
        EXPIRED,
        NOT_LISTENING,
        TOO_LARGE,
    }
)


class LinkError(Exception):
    """A failed request: the error the other side answered with, or a local timeout."""

    def __init__(self, code: str, msg: str = ""):
        super().__init__(f"{code}: {msg}" if msg else code)
        self.code = code
        self.msg = msg


class EnvelopeError(ValueError):
    """A frame that isn't a well-formed envelope. req_id is set when it was a request whose id could be read, so
    the request can still be answered with bad_request."""

    def __init__(self, msg: str, req_id: int | None = None):
        super().__init__(msg)
        self.req_id = req_id


@dataclass
class Envelope:
    t: str
    id: int = 0
    m: str = ""
    p: dict = field(default_factory=dict)
    ok: bool = False
    r: dict = field(default_factory=dict)
    e: dict = field(default_factory=dict)


def _is_id(value: Any) -> bool:
    # bool is an int in Python; the protocol's ids are plain positive integers.
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def parse(text: str) -> Envelope:
    """Parses one text frame. Raises EnvelopeError when it isn't a well-formed envelope."""
    try:
        obj = json.loads(text)
    except (ValueError, RecursionError):
        raise EnvelopeError("invalid JSON") from None
    if not isinstance(obj, dict):
        raise EnvelopeError("not a JSON object")
    req_id = obj.get("id") if obj.get("t") == "req" and _is_id(obj.get("id")) else None
    stack = [(obj, 1)]
    while stack:
        value, depth = stack.pop()
        if isinstance(value, (dict, list)):
            if depth > 32:
                raise EnvelopeError("message nesting exceeds 32 levels", req_id)
            children = [*value.keys(), *value.values()] if isinstance(value, dict) else value
            stack.extend((child, depth + 1) for child in children)
        elif isinstance(value, str):
            try:
                value.encode("utf-8")
            except UnicodeError:
                raise EnvelopeError("invalid UTF-8 text", req_id) from None
        elif isinstance(value, float) and not math.isfinite(value):
            raise EnvelopeError("invalid JSON number", req_id)
    t = obj.get("t")
    if t == "req":
        req_id = obj.get("id")
        if not _is_id(req_id) or req_id == 0:
            raise EnvelopeError("request needs a positive id")
        if not isinstance(obj.get("m"), str):
            raise EnvelopeError("request needs a method", req_id)
        params = obj.get("p", {})
        if not isinstance(params, dict):
            raise EnvelopeError("request params must be an object", req_id)
        return Envelope(t="req", id=req_id, m=obj["m"], p=params)
    if t == "res":
        if not _is_id(obj.get("id")) or not isinstance(obj.get("ok"), bool):
            raise EnvelopeError("response needs an id and ok")
        result = obj.get("r", {})
        error = obj.get("e", {})
        if not isinstance(result, dict) or not isinstance(error, dict):
            raise EnvelopeError("response r and e must be objects")
        return Envelope(t="res", id=obj["id"], ok=obj["ok"], r=result, e=error)
    if t == "evt":
        if not isinstance(obj.get("m"), str):
            raise EnvelopeError("event needs a name")
        params = obj.get("p", {})
        if not isinstance(params, dict):
            raise EnvelopeError("event params must be an object")
        return Envelope(t="evt", m=obj["m"], p=params)
    raise EnvelopeError("unknown envelope type")


def request(req_id: int, method: str, params: dict) -> dict:
    return {"t": "req", "id": req_id, "m": method, "p": params}


def success(req_id: int, result: dict) -> dict:
    return {"t": "res", "id": req_id, "ok": True, "r": result}


def failure(req_id: int, code: str, msg: str) -> dict:
    return {"t": "res", "id": req_id, "ok": False, "e": {"code": code, "msg": msg}}


def event(name: str, params: dict) -> dict:
    return {"t": "evt", "m": name, "p": params}


def encode(obj: dict) -> str:
    """Serialises an envelope, refusing anything the device couldn't receive."""
    text = json.dumps(obj, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
    if len(text.encode()) > MAX_MESSAGE:
        raise ValueError(f"message is over {MAX_MESSAGE} bytes")
    return text
