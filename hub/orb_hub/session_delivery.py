"""Per-device subscriptions, bounded delivery, coalescing and final authorization."""

import asyncio
import contextlib
import json
from dataclasses import dataclass, field

from aiohttp import WSCloseCode

from orb_hub import envelope, models
from orb_hub.session_wire import COALESCE_SECONDS, MAX_DELIVERY_QUEUE, SOURCES, USAGE_SECONDS, check_wire, encoded


@dataclass
class Subscriber:
    link: object
    queue: asyncio.Queue = field(default_factory=lambda: asyncio.Queue(MAX_DELIVERY_QUEUE))
    task: asyncio.Task | None = None
    failed: bool = False
    focus: tuple[str, ...] = ()
    values: dict = field(default_factory=dict)
    summary: dict | None = None
    cards: set[str] = field(default_factory=set)
    sessions_enabled: bool = False
    streams: dict = field(default_factory=dict)
    stream_sent: dict = field(default_factory=dict)
    stream_seq: dict = field(default_factory=dict)
    stream_pending: set = field(default_factory=set)
    stream_replay: set = field(default_factory=set)
    usage: dict | None = None
    usage_sent: float = float("-inf")
    usage_pending: bool = False
    prefs: dict | None = None
    weather: dict | None = None
    weather_pending: bool = False


class SessionDelivery:
    def __init__(self, store, supervisor):
        self.store = store
        self.supervisor = supervisor
        self.subscribers = {}
        self.weather_snapshot = None
        self.send_timeout = store.send_timeout
        self.clock = store.clock

    def refresh_prefs(self, subscriber):
        if self.store.preference_updates.get(subscriber.link.device_id):
            return
        prefs = self.store.get_prefs(subscriber.link.device_id)
        if prefs != subscriber.prefs:
            subscriber.prefs = prefs
            if {"cards", "home", "weather"} & set(getattr(subscriber.link, "caps", ())):
                self.enqueue(subscriber, "prefs.update", prefs)

    def refresh_weather(self, subscriber):
        if self.store.preference_updates.get(subscriber.link.device_id):
            return
        if self.weather_snapshot is None or "weather" not in getattr(subscriber.link, "caps", ()):
            return
        value = self.weather_snapshot(subscriber.link.device_id)
        if not subscriber.weather_pending and subscriber.weather != value:
            self.enqueue(subscriber, "weather.state", value)
            subscriber.weather = value
            subscriber.weather_pending = True

    def subscribe_usage(self, link):
        self.refresh_usage(self.attach(link))

    def refresh_usage(self, subscriber):
        now = self.clock()
        if (
            "usage" in getattr(subscriber.link, "caps", ())
            and not subscriber.usage_pending
            and subscriber.usage != self.store.usage
            and now - subscriber.usage_sent >= USAGE_SECONDS
        ):
            self.enqueue(subscriber, "usage.snapshot", self.store.usage)
            subscriber.usage = self.store.usage
            subscriber.usage_sent = now
            subscriber.usage_pending = True

    def refresh_streams(self, subscriber, *, replay=False):
        now = self.clock()
        subscriber.streams = {k: v for k, v in subscriber.streams.items() if k in subscriber.focus}
        subscriber.stream_replay.intersection_update(subscriber.focus)
        if replay:
            subscriber.stream_replay.update(subscriber.focus)
        for key in subscriber.focus:
            value = self.store.device_streams.get(subscriber.link.device_id, {}).get(key, self.store.streams.get(key))
            if value is None:
                continue
            changed = subscriber.streams.get(key) != value
            if (
                key not in subscriber.stream_pending
                and (key in subscriber.stream_replay or changed)
                and now - subscriber.stream_sent.get(key, float("-inf")) >= COALESCE_SECONDS
            ):
                if changed:
                    subscriber.stream_seq[key] = subscriber.stream_seq.get(key, 0) + 1
                wire_value = {**value, "seq": subscriber.stream_seq[key]}
                self.enqueue(subscriber, "agent.stream", wire_value)
                subscriber.streams[key] = value
                subscriber.stream_sent[key] = now
                subscriber.stream_pending.add(key)
                subscriber.stream_replay.discard(key)

    def attach(self, link):
        """Independent providers share the delivery budget without subscribing to sessions."""
        subscriber = self.subscribers.get(link)
        if subscriber is None:
            subscriber = Subscriber(link)
            subscriber.prefs = self.store.get_prefs(link.device_id)
            self.subscribers[link] = subscriber
            subscriber.task = self.supervisor.spawn(self.deliver(subscriber), name="orb-session-delivery")
        return subscriber

    async def subscribe(self, link):
        subscriber = self.attach(link)
        subscriber.sessions_enabled = True
        # Refresh state on the same ordered worker: replacing it would discard
        # queued voice events and cancel a transcript already waiting to write.
        self.refresh(subscriber, force=True)
        self.refresh_usage(subscriber)
        self.sync_cards(subscriber, replay=True)

    def refresh(self, subscriber, *, force=False):
        if self.store.preference_updates.get(subscriber.link.device_id):
            return
        if not subscriber.sessions_enabled:
            return
        focus, _, summary = self.store.selection(subscriber.link.device_id)
        keys = tuple(s["id"] for s in focus)
        if force or keys != subscriber.focus:
            for key in subscriber.focus:
                if key not in keys:
                    self.enqueue(subscriber, "session.remove", {"id": key})
            subscriber.focus = keys
            self.snapshot(subscriber, focus)
            subscriber.values = {s["id"]: s for s in focus}
            self.enqueue(subscriber, "sessions.summary", summary)
            self.refresh_streams(subscriber, replay=True)
        else:
            for value in focus:
                if subscriber.values.get(value["id"]) != value:
                    self.enqueue(subscriber, "session.update", {"session": value})
                    subscriber.values[value["id"]] = value
            if summary != subscriber.summary:
                self.enqueue(subscriber, "sessions.summary", summary)
            self.refresh_streams(subscriber)
        subscriber.summary = summary

    def snapshot(self, subscriber, values):
        chunks, chunk = [], []
        for value in values:
            candidate = envelope.event(
                "sessions.snapshot", {"sessions": chunk + [value], "reset": False, "more": False}
            )
            if len(encoded(candidate).encode()) > models.MAX_EXTENSION_MESSAGE:
                chunks.append(chunk)
                chunk = []
            chunk.append(value)
        chunks.append(chunk)
        for i, part in enumerate(chunks):
            self.enqueue(
                subscriber, "sessions.snapshot", {"sessions": part, "reset": i == 0, "more": i < len(chunks) - 1}
            )

    def card_visible(self, subscriber, device_id, source):
        return (
            subscriber.sessions_enabled
            and (device_id is None or device_id == subscriber.link.device_id)
            and (
                source == "home"
                and "home" in getattr(subscriber.link, "caps", ())
                or source in SOURCES
                and self.store.get_prefs(subscriber.link.device_id)["sources"][source]
            )
        )

    def sync_cards(self, subscriber, *, replay=False):
        if self.store.preference_updates.get(subscriber.link.device_id):
            return
        visible = set()
        for row in self.store.live_card_records():
            if self.card_visible(subscriber, row["device"], row["source"]):
                visible.add(row["id"])
                if replay or row["id"] not in subscriber.cards:
                    self.enqueue(subscriber, "card.show", json.loads(row["content"]))
        for key in subscriber.cards - visible:
            self.enqueue(subscriber, "card.cancel", {"card": key})
        subscriber.cards = visible

    def delivery_allowed(self, link, method, params):
        """Recheck source privacy after transport backpressure, without awaiting."""
        if method not in {
            "sessions.snapshot",
            "session.update",
            "sessions.summary",
            "card.show",
            "agent.stream",
            "usage.snapshot",
            "prefs.update",
            "weather.state",
        }:
            return True
        subscriber = self.subscribers.get(link)
        if subscriber is None or (method != "usage.snapshot" and self.store.preference_updates.get(link.device_id)):
            return False
        prefs = self.store.get_prefs(link.device_id)
        if method == "prefs.update":
            return bool({"cards", "home", "weather"} & set(getattr(link, "caps", ()))) and params == prefs
        if method == "weather.state":
            return (
                "weather" in getattr(link, "caps", ())
                and self.weather_snapshot is not None
                and params == self.weather_snapshot(link.device_id)
            )
        if method == "usage.snapshot":
            return "usage" in getattr(link, "caps", ())
        if method == "agent.stream":
            value = self.store.sessions.get(params["session"])
            return value is not None and params["session"] in subscriber.focus and prefs["sources"][value["agent"]]
        if method == "sessions.snapshot":
            return all(prefs["sources"][s["agent"]] for s in params["sessions"])
        if method == "session.update":
            value = params["session"]
            return value["id"] in subscriber.focus and prefs["sources"][value["agent"]]
        if method == "sessions.summary":
            return params == subscriber.summary
        row = self.store.card_record(params["id"])
        return (
            row is not None and row["status"] == "live" and self.card_visible(subscriber, row["device"], row["source"])
        )

    def unsubscribe(self, link):
        subscriber = self.subscribers.pop(link, None)
        if subscriber and subscriber.task and subscriber.task is not asyncio.current_task():
            subscriber.task.cancel()

    def enqueue(self, subscriber, method, params):
        if subscriber.failed:
            return
        check_wire(method, params)
        try:
            subscriber.queue.put_nowait((method, params))
        except asyncio.QueueFull:
            subscriber.failed = True
            self.supervisor.spawn(
                subscriber.link.close(WSCloseCode.TRY_AGAIN_LATER, "state delivery queue full"),
                name="orb-session-overflow",
            )

    async def emit(self, link, method, params):
        subscriber = self.subscribers.get(link)
        if subscriber is None:
            raise ConnectionError("device has no subscription")
        self.enqueue(subscriber, method, params)

    def discard_voice_result(self, link, stream):
        subscriber = self.subscribers.get(link)
        if subscriber is None:
            return
        retained = []
        while not subscriber.queue.empty():
            method, params = subscriber.queue.get_nowait()
            subscriber.queue.task_done()
            if method != "voice.result" or params["stream"] != stream:
                retained.append((method, params))
        for item in retained:
            subscriber.queue.put_nowait(item)

    def broadcast(self, method, params, device_id=None):
        for subscriber in self.subscribers.values():
            if device_id is None or subscriber.link.device_id == device_id:
                if method == "card.cancel":
                    if params["card"] not in subscriber.cards:
                        continue
                    subscriber.cards.discard(params["card"])
                self.enqueue(subscriber, method, params)

    async def deliver(self, subscriber):
        try:
            while True:
                method, params = await subscriber.queue.get()
                try:
                    async with asyncio.timeout(self.send_timeout):
                        if self.delivery_allowed(subscriber.link, method, params):
                            await subscriber.link.notify(method, params)
                finally:
                    # Keep at most one outstanding progress/quota event and
                    # throttle from completed writes. A stalled link must not
                    # release accumulated updates in a burst when it recovers.
                    if method == "agent.stream":
                        subscriber.stream_pending.discard(params["session"])
                        # A session may disappear while its final event waits
                        # for transport. Do not resurrect its lifetime records.
                        if params["session"] in self.store.sessions:
                            subscriber.stream_sent[params["session"]] = self.clock()
                    elif method == "usage.snapshot":
                        subscriber.usage_pending = False
                        subscriber.usage_sent = self.clock()
                    elif method == "weather.state":
                        subscriber.weather_pending = False
                        if not self.delivery_allowed(subscriber.link, method, params):
                            # A location/unit change can suppress the queued
                            # replacement at Link's send lock. It must remain
                            # eligible for replay even if the setting changes
                            # back before the next coalescing tick.
                            subscriber.weather = None
                    subscriber.queue.task_done()
        except (TimeoutError, ConnectionError, ValueError):
            subscriber.failed = True
            with contextlib.suppress(ConnectionError):
                await subscriber.link.close(WSCloseCode.TRY_AGAIN_LATER, "state delivery stalled")

    def remove_session(self, key):
        for subscriber in self.subscribers.values():
            subscriber.streams.pop(key, None)
            subscriber.stream_sent.pop(key, None)
            subscriber.stream_seq.pop(key, None)
            subscriber.stream_pending.discard(key)
            subscriber.stream_replay.discard(key)

    def show_card(self, value, device_id, source):
        for subscriber in self.subscribers.values():
            if self.card_visible(subscriber, device_id, source):
                subscriber.cards.add(value["id"])
                self.enqueue(subscriber, "card.show", value)
