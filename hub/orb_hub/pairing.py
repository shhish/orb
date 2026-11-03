"""Pairing codes and the requests waiting for `orb-hub pair <code>` (docs/protocol.md, "Pairing")."""

import asyncio
import hashlib
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Protocol

from orb_hub.lifecycle import LifecycleSupervisor

SAS_DOMAIN = b"orb-pair-v1"
# Anyone on the LAN can ask to pair, and each request holds a connection open for 5 minutes.
MAX_PENDING = 8
# Each attempt gives a man in the middle one chance in a million that the hub's code matches the one it got the
# device to show; this keeps it from simply asking the hub again and again until one does.
MAX_ATTEMPTS = 10
ATTEMPT_WINDOW = 60.0


def compute_sas(cert_fp: bytes, nonce_d: bytes, nonce_h: bytes) -> str:
    """The 6-digit code both sides show: the first 4 bytes of SHA-256("orb-pair-v1" ‖ cert_fp ‖ nonce_d ‖ nonce_h)
    as a big-endian integer, mod 10^6. A man in the middle presents a different certificate, so the device's code
    stops matching the hub's."""
    digest = hashlib.sha256(SAS_DOMAIN + cert_fp + nonce_d + nonce_h).digest()
    return f"{int.from_bytes(digest[:4], 'big') % 1_000_000:06d}"


class PairingLink(Protocol):
    """What a pending request needs from its connection."""

    async def pair_failed(self, reason: str) -> None: ...


@dataclass(eq=False)
class PendingPair:
    device_id: str
    name: str
    model: str
    fw: str
    peer: str
    sas: str
    link: PairingLink
    created: float = field(default_factory=time.monotonic)
    timer: asyncio.TimerHandle | None = None


class PairingBook:
    """Pending requests, at most one per device; each expires after ttl seconds."""

    def __init__(self, ttl: float):
        self.ttl = ttl
        self._pending: dict[str, PendingPair] = {}
        self._attempts: deque[float] = deque()
        # The event loop only keeps weak references to tasks.
        self.supervisor = LifecycleSupervisor("pairing")

    def has_room(self, device_id: str) -> bool:
        return device_id in self._pending or len(self._pending) < MAX_PENDING

    def allow_attempt(self) -> bool:
        """Counts a pairing attempt (pair.begin), from any device; False, counting nothing, when MAX_ATTEMPTS were
        made in the last ATTEMPT_WINDOW seconds."""
        now = time.monotonic()
        while self._attempts and now - self._attempts[0] >= ATTEMPT_WINDOW:
            self._attempts.popleft()
        if len(self._attempts) >= MAX_ATTEMPTS:
            return False
        self._attempts.append(now)
        return True

    def add(self, pending: PendingPair) -> PendingPair | None:
        """Holds a new request and returns the one it replaces (same device, retrying with new nonces)."""
        replaced = self._pending.pop(pending.device_id, None)
        if replaced:
            self._cancel(replaced)
        pending.timer = asyncio.get_running_loop().call_later(self.ttl, self._expire, pending)
        self._pending[pending.device_id] = pending
        return replaced

    def take(self, code: str) -> PendingPair:
        """Removes and returns the request showing code. LookupError if none does, or (one in a million) several
        do: then nothing changes and the devices can retry with new nonces."""
        matches = [p for p in self._pending.values() if p.sas == code]
        if not matches:
            raise LookupError(f"no device is waiting with code {code}")
        if len(matches) > 1:
            raise LookupError(f"more than one device shows {code}; restart pairing on one of them")
        pending = matches[0]
        self.remove(pending)
        return pending

    def remove(self, pending: PendingPair) -> None:
        if self._pending.get(pending.device_id) is pending:
            del self._pending[pending.device_id]
        self._cancel(pending)

    def list(self) -> list[dict]:
        # The code is left out on purpose: it must be read off the device's screen, or it proves nothing.
        now = time.monotonic()
        return [
            {
                "device_id": p.device_id,
                "name": p.name,
                "model": p.model,
                "fw": p.fw,
                "peer": p.peer,
                "expires_in": max(0, round(self.ttl - (now - p.created))),
            }
            for p in self._pending.values()
        ]

    def clear(self) -> None:
        for pending in list(self._pending.values()):
            self.remove(pending)

    async def stop(self) -> None:
        self.clear()
        await self.supervisor.stop()

    def _expire(self, pending: PendingPair) -> None:
        self.remove(pending)
        self.supervisor.spawn(pending.link.pair_failed("expired"), name="orb-pair-expired")

    @staticmethod
    def _cancel(pending: PendingPair) -> None:
        if pending.timer:
            pending.timer.cancel()
            pending.timer = None
