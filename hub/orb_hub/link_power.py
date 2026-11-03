"""Negotiated, bounded low-traffic delivery for dark device surfaces."""

import asyncio
import copy
import time
from collections import OrderedDict

from aiohttp import WSCloseCode

from orb_hub import envelope, models
from orb_hub.session_wire import encoded

# Re-numbering at the write may add digits to up to eight focus streams.
BATCH_WIRE_RESERVE = 128
BACKGROUND_SECONDS = 60.0
MAX_PENDING_EVENTS = 256
MAX_PENDING_BYTES = 512 * 1024
SNAPSHOTS = {"sessions.snapshot", "home.snapshot"}
QUIET_EVENTS = SNAPSHOTS | {
    "session.update",
    "session.remove",
    "sessions.summary",
    "agent.stream",
    "usage.snapshot",
    "prefs.update",
    "weather.state",
    "home.update",
    "nowplaying.snapshot",
}


class PendingState:
    """Latest values, retaining complete snapshot groups and ordered dependencies."""

    def __init__(self):
        self.groups = OrderedDict()

    @property
    def count(self):
        return sum(len(events) for events in self.groups.values())

    def put(self, method, params):
        params = copy.deepcopy(params)
        if method in SNAPSHOTS:
            key = (method,)
            if params.get("reset"):
                # A replacement supersedes earlier deltas, never later ones.
                superseded = (
                    {"sessions.snapshot", "session.update", "session.remove", "agent.stream"}
                    if method == "sessions.snapshot"
                    else {"home.snapshot", "home.update"}
                )
                self.groups = OrderedDict((k, v) for k, v in self.groups.items() if k[0] not in superseded)
            self.groups.setdefault(key, []).append({"m": method, "p": params})
        else:
            identity = (
                params.get("session")
                if method == "agent.stream"
                else params.get("session", {}).get("id")
                if method == "session.update"
                else params.get("id")
                if method in {"session.remove", "home.update"}
                else None
            )
            kind = "session.update" if method == "session.remove" else method
            key = (kind, identity)
            if method == "home.update" and key in self.groups:
                old = self.groups[key][0]["p"]
                params = {**old, **params, "state": {**old.get("state", {}), **params.get("state", {})}}
            if method == "session.remove":
                self.groups.pop(("agent.stream", identity), None)
            self.groups[key] = [{"m": method, "p": params}]
        if (
            self.count > MAX_PENDING_EVENTS
            or sum(len(encoded(v).encode()) for v in self.groups.values()) > MAX_PENDING_BYTES
        ):
            raise ConnectionError("background state queue full")

    def take_batch(self):
        events = []
        blocked = set()
        for key, group in list(self.groups.items()):
            family = (
                "sessions"
                if key[0] in {"sessions.snapshot", "session.update", "session.remove", "agent.stream"}
                else "home"
                if key[0] in {"home.snapshot", "home.update"}
                else None
            )
            if family in blocked:
                continue
            if key[0] in SNAPSHOTS and group[-1]["p"].get("more"):
                blocked.add(family)
                continue
            candidate = events + group
            if (
                len(encoded(envelope.event("power.batch", {"events": candidate})).encode())
                > models.MAX_EXTENSION_MESSAGE - BATCH_WIRE_RESERVE
            ):
                if key[0] in SNAPSHOTS:
                    blocked.add(family)
                continue
            events = candidate
            del self.groups[key]
        return events

    def drain(self):
        events = [event for group in self.groups.values() for event in group]
        self.groups.clear()
        return events


class PowerDelivery:
    def __init__(self, link, send=None, *, clock=time.monotonic):
        self.link = link
        self.send = send if send is not None else link.send
        self.clock = clock
        self.hint = "interactive"
        self.ping_interval_s = 15
        self.pending = PendingState()
        self.changed = asyncio.Event()
        self.flush_lock = asyncio.Lock()
        self.last_flush = clock()
        self._stream_seq = {}
        self._snapshot_ids = set()
        self.have_sessions = False
        self.replay_cards = False

    @property
    def background(self):
        return "power_hint" in self.link.caps and self.hint == "background"

    @property
    def idle_timeout(self):
        if "power_hint" not in self.link.caps:
            return self.link.services.cfg.idle_timeout
        # A receive already waiting at 135 s remains safe when cadence speeds
        # up: the device's next 15 s ping completes that receive earlier.
        return 3 * self.ping_interval_s

    def configure(self, params):
        if "power_hint" not in self.link.caps:
            return True  # legacy peers ignore additive fields
        if "power_hint" not in params and "ping_interval_s" not in params:
            return True
        hint = params.get("power_hint")
        interval = params.get("ping_interval_s", 15)
        if (
            hint not in ("interactive", "background")
            or type(interval) is not int
            or interval not in (15, 45)
            or (hint == "interactive" and interval != 15)
        ):
            return False
        if hint == "background" and self.hint != hint:
            self.last_flush = self.clock()
        self.hint, self.ping_interval_s = hint, interval
        self.changed.set()
        return True

    def defer(self, method, params):
        # Cards require an initialized firmware session model. The first
        # complete baseline is part of hello, before queued attention cards.
        if method == "sessions.snapshot" and not self.have_sessions:
            return False
        if not self.background or method not in QUIET_EVENTS:
            return False
        try:
            self.pending.put(method, params)
        except ConnectionError:
            self.link.lifecycle.spawn(
                self.link.close(WSCloseCode.TRY_AGAIN_LATER, "background state queue full"),
                name="orb-power-overflow",
            )
            raise
        self.changed.set()
        return True

    async def flush(self):
        async with self.flush_lock:
            await self._flush()

    async def _flush(self):
        if self.link.state != "online":
            return
        if self.background:
            now = self.clock()
            if now - self.last_flush < BACKGROUND_SECONDS:
                return
            self.last_flush = now
            events = self.pending.take_batch()
            if events:
                await self.send(envelope.event("power.batch", {"events": events}))
        else:
            for event in self.pending.drain():
                await self.send(envelope.event(event["m"], event["p"]))

    async def run(self):
        try:
            while True:
                self.changed.clear()
                await self.flush()
                delay = (
                    max(0, self.last_flush + BACKGROUND_SECONDS - self.clock())
                    if self.background and self.pending.count
                    else None
                )
                try:
                    async with asyncio.timeout(delay):
                        await self.changed.wait()
                except TimeoutError:
                    pass
        except (ConnectionError, ValueError):
            await self.link.close(WSCloseCode.TRY_AGAIN_LATER, "background state delivery stalled")

    def prepare(self, obj, allowed):
        """Authorize again at the actual write, and number actual stream writes.

        Provider progress can replace hundreds of retained values. Wire seq
        counts delivered replacements, so deliberate coalescing is not a gap.
        """
        if obj.get("t") != "evt" or "power_hint" not in self.link.caps:
            return obj
        events = obj["p"]["events"] if obj["m"] == "power.batch" else [{"m": obj["m"], "p": obj["p"]}]
        blocked = {event["m"] for event in events if event["m"] in SNAPSHOTS and not allowed(event["m"], event["p"])}
        prepared = []
        for event in events:
            method, params = event["m"], event["p"]
            if method in blocked or not allowed(method, params):
                continue
            if method == "sessions.snapshot":
                if params.get("reset"):
                    self._snapshot_ids.clear()
                self._snapshot_ids.update(s["id"] for s in params["sessions"])
                if not params.get("more"):
                    self.replay_cards = self.have_sessions
                    self.have_sessions = True
                    self._stream_seq = {
                        key: value for key, value in self._stream_seq.items() if key in self._snapshot_ids
                    }
            elif method == "session.remove":
                self._stream_seq.pop(params["id"], None)
            elif method == "agent.stream":
                key = params["session"]
                self._stream_seq[key] = self._stream_seq.get(key, 0) + 1
                params = {**params, "seq": self._stream_seq[key]}
            prepared.append({"m": method, "p": params})
        if not prepared:
            return None
        if obj["m"] == "power.batch":
            return envelope.event("power.batch", {"events": prepared})
        return envelope.event(prepared[0]["m"], prepared[0]["p"])
