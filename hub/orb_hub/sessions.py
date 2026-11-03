"""Compatibility facade composing session domain, actions and delivery services."""

import asyncio
import copy
import time
from functools import partial
from pathlib import Path

from orb_hub import models, temperature_preferences
from orb_hub.envelope import LinkError
from orb_hub.lifecycle import LifecycleSupervisor, finish_shutdown
from orb_hub.session_actions import SessionActions
from orb_hub.session_delivery import SessionDelivery, Subscriber
from orb_hub.session_store import SessionStore
from orb_hub.session_wire import (
    COALESCE_SECONDS,
    MAX_DELIVERY_QUEUE,
    MAX_FOCUS_SESSIONS,
    SOURCES,
    USAGE_SECONDS,
    check_wire,
    encoded,
)

__all__ = [
    "COALESCE_SECONDS",
    "MAX_DELIVERY_QUEUE",
    "MAX_FOCUS_SESSIONS",
    "SOURCES",
    "USAGE_SECONDS",
    "SessionHub",
    "Subscriber",
    "encoded",
]


class SessionHub:
    def __init__(self, data_dir: Path, *, send_timeout=10.0, clock=time.time, temperature=None):
        self.store = SessionStore(data_dir, send_timeout=send_timeout, clock=clock, temperature=temperature)
        self.supervisor = LifecycleSupervisor("sessions")
        self.delivery = SessionDelivery(self.store, self.supervisor)
        self.actions = SessionActions(self.store, self.delivery, self.supervisor)
        self.store.device_connected = self.actions.device_connected
        self.on_speech_trigger = None
        self._manage_units = temperature is None
        self._units_refreshed = float("-inf")
        self._worker = None
        self._pending_sources = {}
        self._prefs_dirty = set()
        self._closed = False

    @classmethod
    async def open(cls, data_dir, **kwargs):
        # The synchronous constructor remains a compatibility seam. Production
        # opens schema/migrations before exposing the store, outside the loop.
        future = asyncio.get_running_loop().run_in_executor(None, partial(cls, data_dir, **kwargs))
        cancelled = False
        while True:
            try:
                instance = await asyncio.shield(future)
                break
            except asyncio.CancelledError:
                cancelled = True
        if cancelled:
            await instance.close()
            raise asyncio.CancelledError
        return instance

    async def start(self):
        if self._worker is not None:
            return
        if self._manage_units:
            await self.refresh_units()
        self._worker = self.supervisor.run(self._run, name="orb-sessions")

    async def close(self):
        await finish_shutdown(self._close(), name="orb-sessions-stop")

    async def _close(self):
        if self._closed:
            return
        for link in list(self.delivery.subscribers):
            self.delivery.unsubscribe(link)
        await self.supervisor.stop()
        # Wait for in-flight durable mutations before closing the read handle.
        async with self.store.mutation_lock:
            self.store.close()
        self._worker = None
        self._closed = True

    def status(self):
        return {
            "sessions": list(self.store.sessions.values()),
            "live_cards": len(self.store.cards),
            "subscribers": len(self.delivery.subscribers),
            "health": self.supervisor.health(),
            "failed_subscribers": sum(s.failed for s in self.delivery.subscribers.values()),
        }

    def register_pending_source(self, name, counter):
        if name in self._pending_sources:
            raise ValueError("Pending source already registered")
        self._pending_sources[name] = counter

    def unregister_pending_source(self, name):
        self._pending_sources.pop(name, None)

    def pending_extra(self, exclude_id=None):
        return sum(counter(exclude_id=exclude_id) for counter in self._pending_sources.values())

    def pending_card_ids(self, *, exclude_prefix=None):
        return frozenset(
            key for key in self.store.card_ids() if exclude_prefix is None or not key.startswith(exclude_prefix)
        )

    def has_card(self, key):
        return key in self.store.card_ids()

    def has_session(self, key):
        return key in self.store.sessions

    def session_snapshot(self, key):
        value = self.store.sessions.get(key)
        return copy.deepcopy(value) if value is not None else None

    def speech_baseline(self, device_id, target):
        session = self.store.sessions.get(target, {})
        stream = self.store.device_streams.get(device_id, {}).get(target, self.store.streams.get(target, {}))
        return frozenset(text for text in (session.get("summary"), stream.get("assistant", {}).get("text")) if text)

    def set_weather_provider(self, snapshot):
        self.delivery.weather_snapshot = snapshot

    def subscribe_weather(self, link):
        self.delivery.refresh_weather(self.delivery.attach(link))

    def publish_weather(self):
        for subscriber in self.delivery.subscribers.values():
            self.delivery.refresh_weather(subscriber)

    def weather_units(self):
        return {
            self.store.get_prefs(s.link.device_id)["units"]["temperature"]
            for s in self.delivery.subscribers.values()
            if "weather" in getattr(s.link, "caps", ()) and not s.failed
        }

    def _speech(self, target, text, device_id):
        if self.on_speech_trigger:
            result = self.on_speech_trigger(target, text, device_id)
            if asyncio.iscoroutine(result):
                self.supervisor.spawn(result, name="orb-session-speech")

    def refresh_temperature(self, temperature):
        if self.store.refresh_temperature(temperature):
            for subscriber in self.delivery.subscribers.values():
                self.delivery.refresh_prefs(subscriber)

    async def refresh_units(self):
        self.refresh_temperature(await asyncio.to_thread(temperature_preferences.read_temperature_unit))
        self._units_refreshed = self.store.clock()

    async def set_prefs(self, device_id, patch):
        # Commit now yields. Fence queued state immediately, so disabling a
        # source/location cannot release its previous value during that fsync.
        token = object()
        pending = self.store.preference_updates.setdefault(device_id, set())
        pending.add(token)
        try:
            return await self.supervisor.spawn(
                self._set_prefs(device_id, patch), name="orb-preferences-write", expected_errors=(LinkError,)
            )
        finally:
            pending.discard(token)
            if not pending:
                self.store.preference_updates.pop(device_id, None)
                # Publish once every concurrent patch has settled. A suppressed
                # intermediate value must not poison a subscriber's sent cache.
                force = device_id in self._prefs_dirty
                self._prefs_dirty.discard(device_id)
                self._refresh_preferences(device_id, force=force)

    async def _set_prefs(self, device_id, patch):
        try:
            async with self.store.mutation_lock:
                prefs = await self.store.set_prefs(device_id, patch)
        except asyncio.CancelledError:
            if "sources" in patch or "idle_minutes" in patch:
                self._prefs_dirty.add(device_id)
            raise
        if "sources" in patch or "idle_minutes" in patch:
            self._prefs_dirty.add(device_id)
        return prefs

    def _refresh_preferences(self, device_id, *, force):
        for subscriber in self.delivery.subscribers.values():
            if subscriber.link.device_id == device_id:
                self.delivery.refresh_prefs(subscriber)
                if force:
                    self.delivery.refresh(subscriber, force=True)
                    self.delivery.sync_cards(subscriber)

    async def upsert_session(self, value, *, reservation=None):
        value = models.session(value)
        previous = self.store.sessions.get(value.get("id"))
        await self.store.upsert_session(value, reservation=reservation)
        if previous != value and value.get("summary") and value.get("state") in ("idle", "waiting"):
            self._speech(value["id"], value["summary"], None)

    async def remove_session(self, key):
        if self.store.sessions.pop(key, None) is None:
            return
        self.store.remove_session_data(key)
        self.delivery.remove_session(key)
        self.actions.unregister_message_handler(key)
        for card in list(self.store.cards.values()):
            if card.get("session") == key:
                await self.cancel_card(card["id"])

    def upsert_stream(self, value, *, device_id=None):
        self.store.upsert_stream(value, device_id=device_id)
        assistant = value.get("assistant") or {}
        if assistant.get("final") and assistant.get("text"):
            self._speech(value["session"], assistant["text"], device_id)

    async def subscribe(self, link):
        await self._expire()
        await self.delivery.subscribe(link)

    async def _expire(self):
        for value in list(self.store.cards.values()):
            if value.get("expires", float("inf")) <= self.store.clock():
                await self.cancel_card(value["id"])

    async def _run(self):
        while True:
            await asyncio.sleep(COALESCE_SECONDS)
            await self._expire()
            if self._manage_units and self.store.clock() - self._units_refreshed >= 60:
                await self.refresh_units()
            for subscriber in self.delivery.subscribers.values():
                self.delivery.refresh(subscriber)
                self.delivery.refresh_usage(subscriber)
                self.delivery.refresh_weather(subscriber)

    async def show_card(self, value, *, device_id=None, answer_handler=None, source=None):
        return await self.supervisor.spawn(
            self._show_card(value, device_id=device_id, answer_handler=answer_handler, source=source),
            name="orb-card-publish",
            expected_errors=(LinkError,),
        )

    async def _show_card(self, value, *, device_id, answer_handler, source):
        value = models.card(value)
        check_wire("card.show", value)
        await self._expire()
        value, source, accepted, changed = await self.store.show_card(
            value, device_id=device_id, source=source, pending_extra=self.pending_extra
        )
        if not accepted:
            return False
        if answer_handler is not None:
            self.actions.register_answer_handler(value["id"], answer_handler)
        if changed:
            self.delivery.show_card(value, device_id, source)
            target = "dot" if source == "dot" else value.get("session") or source
            body = value.get("body") or value.get("title") or ""
            if target and body:
                self._speech(target, body, device_id)
        return True

    async def cancel_card(self, key):
        return await self.supervisor.spawn(self._cancel_card(key), name="orb-card-cancel", expected_errors=(LinkError,))

    async def _cancel_card(self, key):
        row = await self.store.cancel_card(key)
        if row is not None:
            self.actions.unregister_answer_handler(key)
            self.delivery.broadcast("card.cancel", {"card": key}, row["device"])

    def get_prefs(self, *args, **kwargs):
        return self.store.get_prefs(*args, **kwargs)

    def list_sessions(self, *args, **kwargs):
        return self.store.list_sessions(*args, **kwargs)

    def check_source(self, *args, **kwargs):
        return self.store.check_source(*args, **kwargs)

    def reserve_session_slot(self, *args, **kwargs):
        return self.store.reserve_session_slot(*args, **kwargs)

    def release_session_slot(self, *args, **kwargs):
        return self.store.release_session_slot(*args, **kwargs)

    def retain_device_streams(self, *args, **kwargs):
        return self.store.retain_device_streams(*args, **kwargs)

    def unavailable_stream(self, *args, **kwargs):
        return self.store.unavailable_stream(*args, **kwargs)

    def set_usage(self, *args, **kwargs):
        return self.store.set_usage(*args, **kwargs)

    async def answer(self, *args, **kwargs):
        return await self.actions.answer(*args, **kwargs)

    async def message(self, *args, **kwargs):
        return await self.actions.message(*args, **kwargs)

    def register_message_handler(self, *args, **kwargs):
        return self.actions.register_message_handler(*args, **kwargs)

    def unregister_message_handler(self, *args, **kwargs):
        return self.actions.unregister_message_handler(*args, **kwargs)

    def check_target(self, *args, **kwargs):
        return self.actions.check_target(*args, **kwargs)

    def subscribe_usage(self, *args, **kwargs):
        return self.delivery.subscribe_usage(*args, **kwargs)

    def attach(self, *args, **kwargs):
        return self.delivery.attach(*args, **kwargs)

    def replay_cards(self, link):
        subscriber = self.delivery.subscribers.get(link)
        if subscriber is not None:
            self.delivery.sync_cards(subscriber, replay=True)

    def delivery_allowed(self, *args, **kwargs):
        return self.delivery.delivery_allowed(*args, **kwargs)

    def unsubscribe(self, *args, **kwargs):
        return self.delivery.unsubscribe(*args, **kwargs)

    async def emit(self, *args, **kwargs):
        return await self.delivery.emit(*args, **kwargs)

    def discard_voice_result(self, *args, **kwargs):
        return self.delivery.discard_voice_result(*args, **kwargs)

    @property
    def sessions(self):
        return self.store.sessions

    @property
    def cards(self):
        return self.store.cards

    @property
    def streams(self):
        return self.store.streams

    @property
    def usage(self):
        return self.store.usage

    @property
    def db(self):
        return self.store.db

    @property
    def clock(self):
        return self.store.clock

    @property
    def send_timeout(self):
        return self.store.send_timeout

    @property
    def available_session_slots(self):
        return self.store.available_session_slots

    @property
    def weather_snapshot(self):
        return self.delivery.weather_snapshot

    @weather_snapshot.setter
    def weather_snapshot(self, snapshot):
        self.set_weather_provider(snapshot)

    # Legacy test/caller seams; production integrations use the public ports.
    @property
    def _subscribers(self):
        return self.delivery.subscribers

    @property
    def _device_streams(self):
        return self.store.device_streams

    @property
    def _message_handlers(self):
        return self.actions.message_handlers

    @property
    def _tasks(self):
        return self.supervisor.tasks

    def _selection(self, *args, **kwargs):
        return self.store.selection(*args, **kwargs)

    def _refresh(self, *args, **kwargs):
        return self.delivery.refresh(*args, **kwargs)

    def _refresh_usage(self, *args, **kwargs):
        return self.delivery.refresh_usage(*args, **kwargs)

    def _refresh_weather(self, *args, **kwargs):
        return self.delivery.refresh_weather(*args, **kwargs)

    def _refresh_prefs(self, *args, **kwargs):
        return self.delivery.refresh_prefs(*args, **kwargs)

    def _sync_cards(self, *args, **kwargs):
        return self.delivery.sync_cards(*args, **kwargs)

    def _enqueue(self, *args, **kwargs):
        return self.delivery.enqueue(*args, **kwargs)

    def _snapshot(self, *args, **kwargs):
        return self.delivery.snapshot(*args, **kwargs)

    def _refresh_streams(self, *args, **kwargs):
        return self.delivery.refresh_streams(*args, **kwargs)

    @property
    def _session_reservations(self):
        return self.store.session_reservations
