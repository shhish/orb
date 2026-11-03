"""Paired devices, in sqlite. A device's token is only ever stored as its SHA-256."""

import hashlib
import hmac
import json
import os
import sqlite3
import time
from dataclasses import dataclass
from pathlib import Path

from orb_hub.battery import BatterySample

BATTERY_RETENTION_MS = 30 * 24 * 60 * 60 * 1000
BATTERY_COALESCE_MS = 5 * 60 * 1000
BATTERY_COALESCE_MV = 10

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    device_id    TEXT PRIMARY KEY,
    name         TEXT NOT NULL,
    model        TEXT NOT NULL DEFAULT '',
    channel      TEXT NOT NULL,
    token_sha256 TEXT NOT NULL UNIQUE,
    paired_at    INTEGER NOT NULL,
    last_seen    INTEGER,
    last_ip      TEXT,
    fw           TEXT NOT NULL DEFAULT '',
    build        TEXT NOT NULL DEFAULT '',
    hello        TEXT NOT NULL DEFAULT '{}',
    state        TEXT NOT NULL DEFAULT '{}',
    ota          TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE IF NOT EXISTS battery_samples (
    at            INTEGER NOT NULL,
    device_id     TEXT NOT NULL,
    pct           INTEGER,
    mv            INTEGER,
    charging      INTEGER,
    usb           INTEGER,
    display_state TEXT
);
CREATE INDEX IF NOT EXISTS battery_samples_at ON battery_samples(at);
CREATE INDEX IF NOT EXISTS battery_samples_device_at ON battery_samples(device_id, at);
"""


def now_ms() -> int:
    return int(time.time() * 1000)


def token_hash(token_hex: str) -> str:
    return hashlib.sha256(bytes.fromhex(token_hex)).hexdigest()


@dataclass
class Device:
    device_id: str
    name: str
    model: str
    channel: str
    token_sha256: str
    paired_at: int
    last_seen: int | None
    last_ip: str | None
    fw: str
    build: str
    hello: dict  # the rest of the last hello: idf, boot slot, reset reason, uptime, power, rssi
    state: dict  # the last state.report
    ota: dict  # the last ota.report, and "failed": the last version that didn't install

    def token_matches(self, token_hex: str) -> bool:
        return hmac.compare_digest(self.token_sha256, token_hash(token_hex))

    @classmethod
    def from_row(cls, row: sqlite3.Row) -> "Device":
        values = dict(row)
        for column in ("hello", "state", "ota"):
            values[column] = json.loads(values[column])
        return cls(**values)


class Store:
    def __init__(self, path: Path):
        self._db = sqlite3.connect(path, isolation_level=None)
        os.chmod(path, 0o600)
        self._db.row_factory = sqlite3.Row
        # The CLI reads and writes the same file when the hub isn't running; WAL lets both get along.
        self._db.execute("PRAGMA journal_mode=WAL")
        self._db.execute("PRAGMA busy_timeout=5000")
        self._db.executescript(SCHEMA)

    def close(self) -> None:
        self._db.close()

    def prune_battery(self, at: int | None = None) -> int:
        """Drops samples older than 30 days; the table is a rolling measurement window.
        A writer passes its sample time so a deliberate backfill is not cut against
        the wall clock, and a live hub (whose samples are current) still rolls."""
        cutoff = (now_ms() if at is None else at) - BATTERY_RETENTION_MS
        return self._db.execute("DELETE FROM battery_samples WHERE at < ?", (cutoff,)).rowcount

    def get(self, device_id: str) -> Device | None:
        row = self._db.execute("SELECT * FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        return Device.from_row(row) if row else None

    def find_by_token(self, token_hex: str) -> Device | None:
        row = self._db.execute("SELECT * FROM devices WHERE token_sha256 = ?", (token_hash(token_hex),)).fetchone()
        return Device.from_row(row) if row else None

    def all(self) -> list[Device]:
        return [Device.from_row(r) for r in self._db.execute("SELECT * FROM devices ORDER BY device_id")]

    def pair(self, device_id: str, name: str, model: str, fw: str, token_hex: str, channel: str) -> Device:
        """Records a newly paired device. A device that pairs again (it lost its token) keeps the name and channel
        set on the hub and gets the new token."""
        self._db.execute(
            """
            INSERT INTO devices (device_id, name, model, channel, token_sha256, paired_at, fw)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                model = excluded.model, token_sha256 = excluded.token_sha256,
                paired_at = excluded.paired_at, fw = excluded.fw
            """,
            (device_id, name, model, channel, token_hash(token_hex), now_ms(), fw),
        )
        return self.get(device_id)

    def unpair(self, device_id: str) -> bool:
        return self._db.execute("DELETE FROM devices WHERE device_id = ?", (device_id,)).rowcount > 0

    def record_hello(self, device_id: str, ip: str, fw: str, build: str, hello: dict) -> None:
        self._db.execute(
            "UPDATE devices SET fw = ?, build = ?, hello = ?, last_ip = ?, last_seen = ? WHERE device_id = ?",
            (fw, build, json.dumps(hello), ip, now_ms(), device_id),
        )

    def record_state(self, device_id: str, state: dict) -> None:
        self._db.execute(
            "UPDATE devices SET state = ?, last_seen = ? WHERE device_id = ?",
            (json.dumps(state), now_ms(), device_id),
        )

    def record_battery(
        self,
        device_id: str,
        *,
        pct: int | None,
        mv: int | None,
        charging: bool | None,
        usb: bool | None,
        display_state: str | None,
        at: int | None = None,
    ) -> bool:
        """Adds one sample unless it is the same reading within 5 minutes and
        10 mV. Returns whether a row was written; the caller turns failures
        into a log line rather than a link error."""
        timestamp = now_ms() if at is None else at
        previous = self._db.execute(
            "SELECT at, pct, mv, charging, usb, display_state FROM battery_samples "
            "WHERE device_id = ? ORDER BY at DESC LIMIT 1",
            (device_id,),
        ).fetchone()
        if previous is not None:
            elapsed = timestamp - previous["at"]
            unchanged = (
                previous["pct"],
                previous["charging"],
                previous["usb"],
                previous["display_state"],
            ) == (pct, None if charging is None else int(charging), None if usb is None else int(usb), display_state)
            if (
                0 <= elapsed < BATTERY_COALESCE_MS
                and unchanged
                and previous["mv"] is not None
                and mv is not None
                and abs(mv - previous["mv"]) < BATTERY_COALESCE_MV
            ):
                return False
        self._db.execute(
            "INSERT INTO battery_samples (at, device_id, pct, mv, charging, usb, display_state) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            (
                timestamp,
                device_id,
                pct,
                mv,
                None if charging is None else int(charging),
                None if usb is None else int(usb),
                display_state,
            ),
        )
        self.prune_battery(timestamp)
        return True

    def battery_samples(
        self,
        *,
        device_id: str | None = None,
        since_ms: int | None = None,
        until_ms: int | None = None,
    ) -> list[BatterySample]:
        clauses, values = [], []
        if device_id is not None:
            clauses.append("device_id = ?")
            values.append(device_id)
        if since_ms is not None:
            clauses.append("at >= ?")
            values.append(since_ms)
        if until_ms is not None:
            clauses.append("at <= ?")
            values.append(until_ms)
        where = f" WHERE {' AND '.join(clauses)}" if clauses else ""
        rows = self._db.execute(
            f"SELECT at, device_id, pct, mv, charging, usb, display_state FROM battery_samples{where} "
            "ORDER BY device_id, at",
            values,
        )
        return [
            BatterySample(
                at=row["at"],
                device_id=row["device_id"],
                pct=row["pct"],
                mv=row["mv"],
                charging=None if row["charging"] is None else bool(row["charging"]),
                usb=None if row["usb"] is None else bool(row["usb"]),
                display_state=row["display_state"],
            )
            for row in rows
        ]

    def touch(self, device_id: str) -> None:
        self._db.execute("UPDATE devices SET last_seen = ? WHERE device_id = ?", (now_ms(), device_id))

    def update_ota(self, device_id: str, **fields) -> None:
        """Merges fields into the device's OTA record; a None value removes that key."""
        device = self.get(device_id)
        if not device:
            return
        ota = device.ota | fields
        ota = {k: v for k, v in ota.items() if v is not None}
        self._db.execute("UPDATE devices SET ota = ? WHERE device_id = ?", (json.dumps(ota), device_id))

    def set_config(self, device_id: str, name: str | None = None, channel: str | None = None) -> None:
        if name is not None:
            self._db.execute("UPDATE devices SET name = ? WHERE device_id = ?", (name, device_id))
        if channel is not None:
            self._db.execute("UPDATE devices SET channel = ? WHERE device_id = ?", (channel, device_id))
