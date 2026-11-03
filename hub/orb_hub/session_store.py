"""Session domain state and durability; no provider or delivery dependencies."""

import asyncio
import hashlib
import json
import sqlite3
import time
from pathlib import Path

from orb_hub import envelope, models
from orb_hub.config import ensure_private_dir
from orb_hub.envelope import LinkError
from orb_hub.session_wire import CARD_SOURCES, MAX_FOCUS_SESSIONS, SOURCES, check_wire, encoded


class SessionStore:
    def __init__(self, data_dir: Path, *, send_timeout: float = 10.0, clock=time.time, temperature: str | None = None):
        ensure_private_dir(data_dir)
        path = data_dir / "sessions.db"
        self.db = sqlite3.connect(path, isolation_level=None, check_same_thread=False)
        path.chmod(0o600)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS session_cards (
                id TEXT PRIMARY KEY, content TEXT NOT NULL, device TEXT,
                status TEXT NOT NULL, answer TEXT, delivered INTEGER NOT NULL DEFAULT 0,
                source TEXT NOT NULL DEFAULT 'dot'
            );
            CREATE TABLE IF NOT EXISTS device_prefs (device TEXT PRIMARY KEY, content TEXT NOT NULL);
        """)
        if "source" not in {row["name"] for row in self.db.execute("PRAGMA table_info(session_cards)")}:
            # Old unbound MCP cards cannot be attributed from their display
            # model. Keep them private until their provider rebinds ownership.
            self.db.execute("ALTER TABLE session_cards ADD COLUMN source TEXT")
            self.db.commit()
        # Provider ownership cannot be reclaimed after a process restart.
        with self.db:
            self.db.execute(
                "UPDATE session_cards SET status='cancelled' WHERE status='live' "
                "AND (json_extract(content,'$.kind')='approval' OR id LIKE 'question:%')"
            )
        self.sessions: dict[str, dict] = {}
        self.cards: dict[str, dict] = {
            r["id"]: json.loads(r["content"])
            for r in self.db.execute("SELECT * FROM session_cards WHERE status='live'")
        }
        self.clock = clock
        self.send_timeout = send_timeout
        self.session_reservations = set()
        self.card_reservations = set()
        self._state_entered = {}
        self._temperature = models.one_of("C" if temperature is None else temperature, {"F", "C"})
        self.streams = {}
        self.device_streams = {}
        self._device_state_entered = {}
        self.usage = {"windows": [], "unknown": ["claude", "codex"]}
        self.device_connected = lambda target, device_id: None
        self.preference_updates = {}
        self.mutation_lock = asyncio.Lock()
        self.path = path
        self.closed = False

    def _saved_prefs(self, device_id):
        row = self.db.execute("SELECT content FROM device_prefs WHERE device=?", (device_id,)).fetchone()
        prefs = json.loads(row[0]) if row else {"sources": dict.fromkeys(SOURCES, True), "idle_minutes": 10}
        units = prefs.get("units", {})
        temperature = units.get("temperature", "auto")
        # Trunk saved resolved units plus their source; integration saved the
        # requested auto/F/C value. Normalize both before the next partial patch.
        if units.get("source") == "auto":
            temperature = "auto"
        prefs["units"] = {"temperature": temperature}
        return prefs

    def get_prefs(self, device_id):
        prefs = self._saved_prefs(device_id)
        temperature = prefs["units"]["temperature"]
        prefs["units"] = {
            "temperature": self._temperature if temperature == "auto" else temperature,
            "source": "auto" if temperature == "auto" else "owner",
        }
        return prefs

    def selection(self, device_id):
        prefs = self.get_prefs(device_id)
        now = self.clock()
        enabled = [
            self._session_for_device(s, device_id) for s in self.sessions.values() if prefs["sources"][s["agent"]]
        ]

        def state_entered(value):
            return self._device_state_entered.get(device_id, {}).get(value["id"], self._state_entered[value["id"]])

        def rank(value):
            entered = state_entered(value)
            if value["state"] == "waiting":
                return (0, entered, value["id"])
            if value["state"] == "busy":
                return (1, -value["updated"], value["id"])
            return (2, -entered, value["id"])

        focus = sorted(
            (
                s
                for s in enabled
                if s["state"] in {"waiting", "busy"}
                or (prefs["idle_minutes"] > 0 and now - state_entered(s) < prefs["idle_minutes"] * 60)
            ),
            key=rank,
        )[:MAX_FOCUS_SESSIONS]
        focused = {s["id"] for s in focus}
        remainder = sorted((s for s in enabled if s["id"] not in focused), key=lambda s: (-s["updated"], s["id"]))
        summary = {
            "idle": len(remainder),
            "hidden_by_source": {
                source: sum(s["agent"] == source for s in self.sessions.values()) if not prefs["sources"][source] else 0
                for source in SOURCES
            },
        }
        return focus, remainder, summary

    def _session_for_device(self, value, device_id):
        if value["agent"] != "dot":
            return value
        connected = value["can_message"]
        try:
            available = self.device_connected(value["id"], device_id)
        except LinkError:
            connected = False
        else:
            if available is not None:
                connected = available
        stream = self.device_streams.get(device_id, {}).get(value["id"], {})
        reply = stream.get("assistant", {}).get("text", "")
        return {
            **value,
            "state": stream.get("state", value["state"]),
            "updated": stream.get("at", value["updated"]),
            "can_message": connected,
            "reason": "" if connected else "Dot not connected",
            "summary": reply or ("Ready for your message" if connected else "Connect ChatGPT Dot to send messages"),
        }

    def list_sessions(self, device_id, params, *, request_id=1):
        models.fields(params, {"include", "limit", "cursor"}, {"include"})
        models.one_of(params["include"], {"idle"})
        limit = params.get("limit", 8)
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 8:
            raise LinkError(envelope.BAD_REQUEST, "limit must be an integer from 1 to 8")
        _, remainder, _ = self.selection(device_id)
        identity = [device_id, self.get_prefs(device_id), [s["id"] for s in remainder]]
        digest = hashlib.sha256(encoded(identity).encode()).hexdigest()[:32]
        start = 0
        if "cursor" in params:
            cursor = models.text(params["cursor"], 80)
            parts = cursor.split(":")
            if (
                len(parts) != 2
                or len(parts[0]) != 32
                or any(c not in "0123456789abcdef" for c in parts[0])
                or not parts[1].isascii()
                or not parts[1].isdigit()
                or len(parts[1]) > 2
            ):
                raise LinkError(envelope.BAD_REQUEST, "invalid session cursor")
            start = int(parts[1])
            if parts[0] != digest or not 0 < start < len(remainder):
                raise LinkError("conflict", "session cursor is stale; restart paging")
        result = {"sessions": [], "cursor": None}
        for value in remainder[start : start + limit]:
            end = start + len(result["sessions"]) + 1
            candidate = {
                "sessions": result["sessions"] + [value],
                "cursor": f"{digest}:{end}" if end < len(remainder) else None,
            }
            if len(encoded(envelope.success(request_id, candidate)).encode()) > models.MAX_EXTENSION_MESSAGE:
                break
            result = candidate
        if remainder[start:] and not result["sessions"]:
            raise LinkError("too_large", "session page exceeds the 4096-byte wire budget")
        return result

    def check_source(self, device_id, source):
        if source == "home":
            return
        if source not in SOURCES or not self.get_prefs(device_id)["sources"][source]:
            raise LinkError("unsupported", "source is disabled on this device")

    def reserve_session_slot(self):
        if len(self.sessions) + len(self.session_reservations) >= models.MAX_SESSIONS:
            raise LinkError(envelope.BUSY, "at most 24 sessions")
        token = object()
        self.session_reservations.add(token)
        return token

    @property
    def available_session_slots(self):
        return models.MAX_SESSIONS - len(self.sessions) - len(self.session_reservations)

    def release_session_slot(self, token):
        self.session_reservations.discard(token)

    async def upsert_session(self, value: dict, *, reservation=None):
        value = models.session(value)
        check_wire("session.update", {"session": value})
        key = value["id"]
        if reservation is not None and reservation not in self.session_reservations:
            raise LinkError("conflict", "session reservation is no longer valid")
        reserved = len(self.session_reservations) - (reservation is not None)
        if key not in self.sessions and len(self.sessions) + reserved >= models.MAX_SESSIONS:
            raise LinkError(envelope.BUSY, "at most 24 sessions")
        self.release_session_slot(reservation)
        if self.sessions.get(key) != value:
            previous = self.sessions.get(key)
            if previous is None:
                self._state_entered[key] = min(value["updated"], self.clock())
            elif previous["state"] != value["state"]:
                self._state_entered[key] = self.clock()
            self.sessions[key] = value

    def upsert_stream(self, value, *, device_id=None):
        value = models.agent_stream(value)
        key = value["session"]
        if key not in self.sessions:
            raise LinkError(envelope.NOT_FOUND, "unknown stream session")
        # Provider revisions may skip during coalescing. The subscriber numbers
        # actual replacements, so a normal burst never looks like a lost frame.
        value["seq"] = 0
        check_wire("agent.stream", value)
        if device_id is None:
            self.streams[key] = value
        else:
            streams = self.device_streams.setdefault(device_id, {})
            previous = streams.get(key)
            entered = self._device_state_entered.setdefault(device_id, {})
            if previous is None:
                entered[key] = min(value["at"], self.clock())
            elif previous["state"] != value["state"]:
                entered[key] = self.clock()
            streams[key] = value

    def retain_device_streams(self, device_ids):
        self.device_streams = {key: values for key, values in self.device_streams.items() if key in device_ids}
        self._device_state_entered = {
            key: values for key, values in self._device_state_entered.items() if key in device_ids
        }

    def unavailable_stream(self, key, activity):
        for values in (self.streams, *self.device_streams.values()):
            if key in values:
                values[key] = {**values[key], "state": "idle", "stale": True, "activity": activity}

    def set_usage(self, value):
        value = models.usage_snapshot(value)
        check_wire("usage.snapshot", value)
        self.usage = value

    async def set_prefs(self, device_id, patch):
        models.fields(patch, {"sources", "idle_minutes", "units"})
        if not patch or ("idle_minutes" not in patch and not patch.get("sources") and not patch.get("units")):
            raise LinkError(envelope.BAD_REQUEST, "preferences need at least one setting")
        prefs = self._saved_prefs(device_id)
        if "sources" in patch:
            models.fields(patch["sources"], set(SOURCES))
            prefs["sources"].update({key: models.boolean(value) for key, value in patch["sources"].items()})
        if "idle_minutes" in patch:
            value = patch["idle_minutes"]
            if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 1440:
                raise LinkError(envelope.BAD_REQUEST, "idle_minutes must be an integer from 0 to 1440")
            prefs["idle_minutes"] = value
        if "units" in patch:
            models.fields(patch["units"], {"temperature"}, {"temperature"})
            prefs["units"] = {"temperature": models.one_of(patch["units"]["temperature"], {"auto", "F", "C"})}
        await self.write("INSERT OR REPLACE INTO device_prefs VALUES (?,?)", (device_id, encoded(prefs)))
        return self.get_prefs(device_id)

    def refresh_temperature(self, temperature):
        temperature = models.one_of(temperature, {"F", "C"})
        changed = temperature != self._temperature
        self._temperature = temperature
        return changed

    def card_record(self, key):
        return self.db.execute("SELECT * FROM session_cards WHERE id=?", (key,)).fetchone()

    def live_card_records(self):
        return list(self.db.execute("SELECT * FROM session_cards WHERE status='live'"))

    async def write(self, sql, params):
        # A separate connection keeps commits (and fsync) off the event loop.
        # Join even on cancellation: no transaction may outlive service stop.
        operation = asyncio.get_running_loop().run_in_executor(None, self.commit, sql, params)
        cancelled = False
        while not operation.done():
            try:
                await asyncio.shield(operation)
            except asyncio.CancelledError:
                cancelled = True
        operation.result()
        if cancelled:
            # The commit is durable even if its caller disappeared. Keep the
            # projection/admission count consistent without replaying an action.
            self.cards.clear()
            self.cards.update({row["id"]: json.loads(row["content"]) for row in self.live_card_records()})
            raise asyncio.CancelledError

    def commit(self, sql, params):
        connection = sqlite3.connect(self.path)
        try:
            with connection:
                connection.execute(sql, params)
        finally:
            connection.close()

    def close(self):
        self.db.close()
        self.closed = True

    def remove_session_data(self, key):
        self._state_entered.pop(key, None)
        self.streams.pop(key, None)
        for values in self.device_streams.values():
            values.pop(key, None)
        for values in self._device_state_entered.values():
            values.pop(key, None)

    async def show_card(self, value, *, device_id, source, pending_extra):
        value = models.card(value)
        source = models.one_of(source or self.sessions.get(value.get("session"), {}).get("agent", "dot"), CARD_SOURCES)
        check_wire("card.show", value)
        key, content = value["id"], encoded(value)
        async with self.mutation_lock:
            row = self.card_record(key)
            rebound = False
            if row:
                if (
                    row["content"] != content
                    or row["device"] != device_id
                    or row["source"] is not None
                    and row["source"] != source
                ):
                    raise LinkError("conflict", "card ID already has different content")
                if row["status"] != "live":
                    return value, source, False, False
                if row["source"] is None:
                    await self.write("UPDATE session_cards SET source=? WHERE id=?", (source, key))
                    rebound = True
            else:
                if len(self.card_ids()) + pending_extra(exclude_id=key) >= models.MAX_CARDS:
                    raise LinkError(envelope.BUSY, "at most 16 live cards")
                # Other providers may perform their own admission while fsync
                # yields. Include this in-flight card in the public budget.
                self.card_reservations.add(key)
                try:
                    await self.write(
                        "INSERT INTO session_cards(id,content,device,status,source) VALUES (?,?,?,'live',?)",
                        (key, content, device_id, source),
                    )
                finally:
                    self.card_reservations.discard(key)
            self.cards[key] = value
            return value, source, True, not row or rebound

    async def cancel_card(self, key):
        async with self.mutation_lock:
            row = self.card_record(key)
            if row is None or row["status"] != "live":
                return None
            await self.write("UPDATE session_cards SET status='cancelled' WHERE id=? AND status='live'", (key,))
            self.cards.pop(key, None)
            return row

    async def decide(self, device_id, params):
        """Commit the first decision before any provider invocation or receipt."""
        key = params["card"]
        async with self.mutation_lock:
            row = self.card_record(key)
            if row is None:
                raise LinkError(envelope.NOT_FOUND, "unknown card")
            if row["device"] is not None and row["device"] != device_id:
                raise LinkError(envelope.UNAUTHORIZED, "card belongs to another device")
            if row["answer"]:
                return row, json.loads(row["answer"]), False
            self.check_source(device_id, row["source"])
            value = json.loads(row["content"])
            if row["status"] != "live" or value.get("expires", float("inf")) <= self.clock():
                raise LinkError("expired", "card is no longer pending")
            if "option" in params:
                option = models.text(params["option"], 32)
                if option not in {o["id"] for o in value["options"]}:
                    raise LinkError(envelope.BAD_REQUEST, "unknown card option")
                answer = {"option": option}
            else:
                if not value["reply"]:
                    raise LinkError("unsupported", "this card does not accept text")
                answer = {"text": models.text(params["text"], 280)}
            await self.write("UPDATE session_cards SET status='answered',answer=? WHERE id=?", (encoded(answer), key))
            self.cards.pop(key, None)
            return row, answer, True

    async def record_delivery(self, key, delivered):
        async with self.mutation_lock:
            await self.write("UPDATE session_cards SET delivered=? WHERE id=?", (delivered, key))

    def card_ids(self):
        return self.cards.keys() | self.card_reservations
