"""Shared session wire limits, independent of storage and transport."""

import json

from orb_hub import envelope, models
from orb_hub.envelope import LinkError

MAX_DELIVERY_QUEUE = 64
COALESCE_SECONDS = 0.25
USAGE_SECONDS = 30.0
MAX_FOCUS_SESSIONS = 8
SOURCES = ("claude", "codex", "dot")
CARD_SOURCES = (*SOURCES, "home")


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True, allow_nan=False)


def check_wire(method, params):
    if len(encoded(envelope.event(method, params)).encode()) > models.MAX_EXTENSION_MESSAGE:
        raise LinkError("too_large", "content exceeds the 4096-byte wire budget")
