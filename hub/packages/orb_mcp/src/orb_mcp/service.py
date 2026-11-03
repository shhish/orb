"""Durable shared card queue and the hub's callback boundary."""

import asyncio
import json
import math
import sqlite3
import time
from collections import Counter
from collections.abc import Callable
from datetime import UTC, datetime

from .backend import Answer, DeviceBackend, Transcript
from .events import Events

PENDING = ("queued", "showing")
TERMINAL = ("answered", "dismissed", "cancelled")


def canonical(value) -> str:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    )


def timestamp(now: float) -> str:
    return datetime.fromtimestamp(now, UTC).isoformat().replace("+00:00", "Z")


class ServiceError(ValueError):
    """A safe, caller-visible domain error (never contains secrets)."""


class OrbService:
    def __init__(
        self,
        conn: sqlite3.Connection,
        backend: DeviceBackend,
        events: Events,
        *,
        clock=time.time,
        whisper_ready: bool = False,
        max_pending: int = 20,
        other_pending: Callable[[], int] | None = None,
    ):
        if (
            isinstance(max_pending, bool)
            or not isinstance(max_pending, int)
            or max_pending < 1
        ):
            raise ValueError("max_pending must be a positive integer")
        self.max_pending = max_pending
        self.other_pending = other_pending or (lambda: 0)
        self.conn, self.backend, self.events = conn, backend, events
        self.clock, self.whisper_ready = clock, whisper_ready
        self._changed = asyncio.Condition()
        self._delivery_lock = asyncio.Lock()
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS cards (
                card_id TEXT PRIMARY KEY, principal TEXT NOT NULL,
                content TEXT NOT NULL, device_id TEXT,
                status TEXT NOT NULL, answer TEXT,
                created_at REAL NOT NULL, cancel_pending INTEGER NOT NULL DEFAULT 0
            );
            CREATE TABLE IF NOT EXISTS transcripts (
                recording_id TEXT PRIMARY KEY, principal TEXT NOT NULL,
                content TEXT NOT NULL
            );
        """)
        # A process restart cannot establish what remains on the device.
        with conn:
            conn.execute("UPDATE cards SET status='queued' WHERE status='showing'")
        backend.set_handlers(self.on_answer, self.on_transcript)

    def _row(self, card_id: str, principal: str | None = None):
        row = self.conn.execute(
            "SELECT * FROM cards WHERE card_id=?", (card_id,)
        ).fetchone()
        if row is None or (principal is not None and row["principal"] != principal):
            raise ServiceError("Card not found")
        return row

    async def _card(self, row) -> dict:
        devices = await self.backend.list_devices()
        return {
            "card_id": row["card_id"],
            "status": row["status"],
            "device_online": any(
                d["device_id"] == row["device_id"] and d["online"] for d in devices
            ),
            "device_id": row["device_id"],
            **json.loads(row["content"]),
            "answer": json.loads(row["answer"]) if row["answer"] else None,
        }

    async def call_tool(self, name: str, arguments: dict, principal: str) -> dict:
        if name in ("orb_show", "orb_ask"):
            return await self._create(name, arguments, principal)
        if name == "orb_get_card":
            return await self._card(self._row(arguments["card_id"], principal))
        if name == "orb_get_transcript":
            row = self.conn.execute(
                "SELECT * FROM transcripts WHERE recording_id=? AND principal=?",
                (arguments["recording_id"], principal),
            ).fetchone()
            if row is None:
                raise ServiceError("Transcript not found")
            return json.loads(row["content"])
        if name == "orb_cancel":
            row = self._row(arguments["card_id"], principal)
            if row["status"] in PENDING:
                with self.conn:
                    self.conn.execute(
                        "UPDATE cards SET status='cancelled',cancel_pending=CASE WHEN device_id IS NULL THEN 0 ELSE 1 END WHERE card_id=?",
                        (row["card_id"],),
                    )
                await self._notify()
                await self.deliver_pending()
            return await self._card(self._row(row["card_id"], principal))
        if name == "orb_wait":
            return await self.wait(
                arguments["card_id"], principal, arguments.get("timeout_s", 50)
            )
        if name == "orb_status":
            queue = Counter(
                r[0]
                for r in self.conn.execute(
                    "SELECT status FROM cards WHERE principal=?", (principal,)
                )
            )
            return {
                "devices": await self.backend.list_devices(),
                "queue": dict(queue),
                **self.events.status(principal),
                "whisper_ready": self.whisper_ready,
            }
        raise ServiceError("Unknown tool")

    async def _create(self, name: str, args: dict, principal: str) -> dict:
        card_id = args["card_id"]
        content = {
            "kind": "ask" if name == "orb_ask" else "show",
            "text": args["text"],
            "title": args.get("title", ""),
            "options": args.get("options", []),
            "device": args.get("device"),
            "priority": args.get("priority", "normal"),
            "source": principal,
        }
        options = content["options"]
        if len({o["id"] for o in options}) != len(options):
            raise ServiceError("Option IDs must be unique")
        encoded = canonical(content)
        devices = await self.backend.list_devices()
        # All DB inspection and insertion below is synchronous on one event loop.
        existing = self.conn.execute(
            "SELECT * FROM cards WHERE card_id=?", (card_id,)
        ).fetchone()
        if existing is not None:
            if existing["principal"] != principal or existing["content"] != encoded:
                raise ServiceError(
                    "card_id already exists with different content or owner"
                )
            return self._summary(await self._card(existing))
        if (
            self.conn.execute(
                "SELECT COUNT(*) FROM cards WHERE status IN ('queued','showing')"
            ).fetchone()[0]
            + self.other_pending()
            >= self.max_pending
        ):
            raise ServiceError(f"At most {self.max_pending} cards may be pending")
        requested = args.get("device")
        if requested is not None and not any(
            d["device_id"] == requested for d in devices
        ):
            raise ServiceError("Unknown device")
        device_id = requested or next(
            (d["device_id"] for d in devices if d["online"]),
            devices[0]["device_id"] if devices else None,
        )
        with self.conn:
            self.conn.execute(
                "INSERT INTO cards(card_id,principal,content,device_id,status,created_at) VALUES (?,?,?,?,?,?)",
                (card_id, principal, encoded, device_id, "queued", self.clock()),
            )
        await self.deliver_pending()
        return self._summary(await self._card(self._row(card_id, principal)))

    @staticmethod
    def _summary(card: dict) -> dict:
        return {key: card[key] for key in ("card_id", "status", "device_online")}

    async def deliver_pending(self) -> None:
        """Replay queued work; hub backend guarantees idempotent acceptance."""
        async with self._delivery_lock:
            rows = self.conn.execute(
                "SELECT * FROM cards WHERE status='queued' OR cancel_pending=1 ORDER BY CASE json_extract(content,'$.priority') WHEN 'urgent' THEN 0 ELSE 1 END,created_at"
            ).fetchall()
            for old in rows:
                row = self._row(old["card_id"])
                try:
                    if row["cancel_pending"]:
                        if await self.backend.cancel_card(
                            row["card_id"], row["device_id"]
                        ):
                            with self.conn:
                                self.conn.execute(
                                    "UPDATE cards SET cancel_pending=0 WHERE card_id=?",
                                    (row["card_id"],),
                                )
                    elif row["status"] == "queued":
                        if row["device_id"] is None:
                            devices = await self.backend.list_devices()
                            if not devices:
                                continue
                            # A cancellation can commit while registry I/O yields.
                            # An unassigned card has no device cancellation to
                            # retry, so it must never acquire a display afterward.
                            row = self._row(row["card_id"])
                            if row["status"] != "queued" or row["cancel_pending"]:
                                continue
                            with self.conn:
                                self.conn.execute(
                                    "UPDATE cards SET device_id=? WHERE card_id=? AND status='queued' AND device_id IS NULL AND cancel_pending=0",
                                    (devices[0]["device_id"], row["card_id"]),
                                )
                            row = self._row(row["card_id"])
                        card = {
                            **json.loads(row["content"]),
                            "card_id": row["card_id"],
                            "device_id": row["device_id"],
                        }
                        if await self.backend.show_card(card):
                            with self.conn:
                                self.conn.execute(
                                    "UPDATE cards SET status='showing' WHERE card_id=? AND status='queued'",
                                    (row["card_id"],),
                                )
                except (OSError, TimeoutError):
                    # Persisted queued/cancel_pending state is retried by the worker.
                    continue

    async def sync_device(self, device_id: str) -> None:
        with self.conn:
            self.conn.execute(
                "UPDATE cards SET status='queued' WHERE device_id=? AND status='showing'",
                (device_id,),
            )
        await self.deliver_pending()

    async def on_answer(self, answer: Answer) -> dict:
        row = self._row(answer.card_id)
        if answer.device_id != row["device_id"]:
            raise ServiceError("Answer came from a different device")
        if answer.kind not in ("option", "dismissed"):
            raise ServiceError("Invalid answer kind")
        payload = {
            "card_id": answer.card_id,
            "kind": answer.kind,
            "device_id": answer.device_id,
        }
        if answer.kind == "option":
            option = next(
                (
                    o
                    for o in json.loads(row["content"])["options"]
                    if o["id"] == answer.option_id
                ),
                None,
            )
            if option is None:
                raise ServiceError("Unknown option")
            payload.update(option_id=option["id"], option_label=option["label"])
        elif answer.option_id is not None:
            raise ServiceError("A dismissal cannot include an option")
        if row["answer"]:
            prior = json.loads(row["answer"])
            if {k: v for k, v in prior.items() if k != "answered_at"} == payload:
                return prior
            raise ServiceError("Card already has a different answer")
        if row["status"] not in PENDING:
            raise ServiceError("Card is no longer pending")
        payload["answered_at"] = timestamp(self.clock())
        with self.conn:
            self.conn.execute(
                "UPDATE cards SET status=?,answer=? WHERE card_id=?",
                (
                    "answered" if answer.kind == "option" else "dismissed",
                    canonical(payload),
                    answer.card_id,
                ),
            )
            self.events.enqueue("card.answered", payload, principal=row["principal"])
        await self._notify()
        return payload

    async def on_transcript(self, transcript: Transcript) -> dict:
        if transcript.confirmed is not True:
            raise ServiceError("Transcript must be confirmed on the device")
        if transcript.focus not in ("dot", "claude", "codex"):
            raise ServiceError("Unknown transcript focus")
        if (
            not isinstance(transcript.text, str)
            or not 1 <= len(transcript.text) <= 8192
        ):
            raise ServiceError("Transcript text must contain 1 to 8192 characters")
        if (
            not isinstance(transcript.recording_id, str)
            or not 1 <= len(transcript.recording_id) <= 64
        ):
            raise ServiceError("Invalid recording ID")
        if (
            isinstance(transcript.duration_s, bool)
            or not isinstance(transcript.duration_s, (int, float))
            or not math.isfinite(transcript.duration_s)
            or transcript.duration_s < 0
        ):
            raise ServiceError("Invalid transcript duration")
        if not any(
            d["device_id"] == transcript.device_id
            for d in await self.backend.list_devices()
        ):
            raise ServiceError("Unknown device")
        payload = {
            "recording_id": transcript.recording_id,
            "text": transcript.text,
            "duration_s": transcript.duration_s,
            "device_id": transcript.device_id,
        }
        encoded = canonical(payload)
        prior = self.conn.execute(
            "SELECT * FROM transcripts WHERE recording_id=?", (transcript.recording_id,)
        ).fetchone()
        if prior:
            if prior["principal"] != transcript.focus or prior["content"] != encoded:
                raise ServiceError(
                    "recording_id already exists with different content or owner"
                )
            return {"accepted": True, "recording_id": transcript.recording_id}
        if transcript.focus == "dot" and not self.events.has_subscription(
            "voice.transcribed", principal="dot", device=transcript.device_id
        ):
            return {"accepted": False, "reason": "not_listening"}
        with self.conn:
            self.conn.execute(
                "INSERT INTO transcripts VALUES (?,?,?)",
                (transcript.recording_id, transcript.focus, encoded),
            )
            if transcript.focus == "dot":
                self.events.enqueue("voice.transcribed", payload, principal="dot")
        return {"accepted": True, "recording_id": transcript.recording_id}

    async def _notify(self):
        async with self._changed:
            self._changed.notify_all()

    async def wait(self, card_id: str, principal: str, timeout_s: float = 50) -> dict:
        if (
            isinstance(timeout_s, bool)
            or not isinstance(timeout_s, (float, int))
            or not math.isfinite(timeout_s)
            or not 0 <= timeout_s <= 50
        ):
            raise ServiceError("timeout_s must be between 0 and 50")
        async with self._changed:
            try:
                async with asyncio.timeout(timeout_s):
                    await self._changed.wait_for(
                        lambda: self._row(card_id, principal)["status"] in TERMINAL
                    )
            except TimeoutError:
                pass
        return await self._card(self._row(card_id, principal))
