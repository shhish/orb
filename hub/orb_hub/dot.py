"""Production MCP adapter: private loopback HTTP to the authenticated device link."""

import asyncio
import base64
import hashlib
import json
import os
import re
import secrets
import stat
import time

from aiohttp import web
from orb_mcp.auth import Tokens, provision_tokens
from orb_mcp.backend import Answer, Transcript
from orb_mcp.events import Events
from orb_mcp.server import create_app
from orb_mcp.service import OrbService, ServiceError
from orb_mcp.storage import connect

from orb_hub.envelope import LinkError
from orb_hub.lifecycle import LifecycleSupervisor


def wire_id(card_id: str) -> str:
    """Use the full digest so multi-byte caller IDs never collide after display truncation."""
    return "mcp:" + base64.b32encode(hashlib.sha256(card_id.encode()).digest()).decode().rstrip("=").lower()


def clipped(text: str, limit: int) -> str:
    return text.encode()[:limit].decode("utf-8", errors="ignore")


def last_reply(text: str) -> str:
    """Bound the last prose sentence without requiring the optional agents package."""
    value = re.sub(r"```.*?(?:```|$)|~~~.*?(?:~~~|$)", " ", text, flags=re.DOTALL)
    value = re.sub(r"(?m)^(?: {4}|\t).*$", " ", value)
    value = re.sub(r"!?\[([^]\n]*)\]\([^\n)]*\)", r"\1", value)
    value = re.sub(r"<[^>]+>|https?://\S+", " ", value)
    value = re.sub(r"(?i)\b(bearer\s+)\S+", r"\1[redacted]", value)
    value = re.sub(
        r"(?i)\b(api[_-]?key|token|password|secret|authorization)\s*[:=]\s*\S+",
        r"\1=[redacted]",
        value,
    )
    value = re.sub(r"\b(?:sk|whsec)[_-][A-Za-z0-9_-]{8,}|\b[0-9a-fA-F]{24,}\b", "[redacted]", value)
    value = re.sub(r"(?:~?/|[A-Za-z]:\\)(?:[^\s<>\"'()]+[/\\])+([^\s<>\"'()]+)", r"\1", value)
    value = re.sub(r"(?m)^\s*(?:#{1,6}\s+|>\s*|[-*+]\s+|\d+[.)]\s+)", "", value)
    value = value.replace("`", "")
    value = re.sub(r"(\*\*|__|~~)(.*?)\1", r"\2", value)
    value = re.sub(r"(?<!\w)[*_]([^*_\n]+)[*_](?!\w)", r"\1", value)
    value = " ".join(value.split())
    # Each MCP card is already a complete message boundary, even if its final
    # sentence omits punctuation. Prefer that final sentence over earlier text.
    sentences = re.findall(r".*?[.!?](?=\s|$)|.+$", value)
    sentence = sentences[-1].strip() if sentences else ""
    return sentence if len(sentence.encode()) <= 160 else clipped(sentence, 157).rstrip() + "…"


def private_directory(path):
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid():
        raise ValueError("MCP directory must be owned and private (0700)")
    return path


class HubDeviceBackend:
    """Convert MCP content to v1.1 cards without losing original answer identities."""

    def __init__(self, hub, sessions):
        self.hub, self.sessions = hub, sessions
        self._answer = None
        self._transcript = None
        self.changed = None
        self._recording_scopes = {}

    def set_handlers(self, answer, transcript) -> None:
        self._answer, self._transcript = answer, transcript

    async def list_devices(self) -> list[dict]:
        if self.hub.store is None:
            return []
        devices = []
        for device in self.hub.store.all():
            link = self.hub.online.get(device.device_id)
            devices.append(
                {
                    "device_id": device.device_id,
                    "name": device.name,
                    "online": bool(link and link.state == "online" and "cards" in link.caps),
                }
            )
        return devices

    async def show_card(self, card: dict) -> bool:
        source = card["source"]
        options = card["options"]
        mapped = [
            {"id": f"option-{index}", "label": clipped(option["label"], 20), "role": "default"}
            for index, option in enumerate(options)
        ]
        # Dismissal must be an explicit owner action, including for plain messages.
        mapped.append({"id": "dismiss", "label": "Dismiss", "role": "cancel"})
        display = {
            "id": wire_id(card["card_id"]),
            "kind": "choice" if card["kind"] == "ask" else "message",
            "title": clipped(card["title"] or {"dot": "ChatGPT Dot", "claude": "Claude", "codex": "Codex"}[source], 64),
            "body": clipped(card["text"], 280),
            "options": mapped,
            "reply": False,
            "priority": "high" if card["priority"] == "urgent" else "normal",
        }
        if source == "dot":
            display["session"] = "dot"

        async def answered(answer: dict) -> dict:
            if self._answer is None:
                raise LinkError("unsupported", "MCP answer handler is unavailable")
            option_id = answer.get("option")
            if option_id == "dismiss":
                value = Answer(card["card_id"], answer["device_id"], "dismissed")
            else:
                match = next((i for i, option in enumerate(mapped[:-1]) if option["id"] == option_id), None)
                if match is None:
                    raise LinkError("bad_request", "Unknown card option")
                value = Answer(card["card_id"], answer["device_id"], "option", options[match]["id"])
            try:
                # OrbService commits the source answer and matching outbox records
                # before the link may acknowledge the owner's decision.
                return await self._answer(value)
            except ServiceError as exc:
                raise LinkError("conflict", "MCP card is no longer pending") from exc

        try:
            accepted = await self.sessions.show_card(
                display, device_id=card["device_id"], answer_handler=answered, source=source
            )
        except LinkError as exc:
            if exc.code == "busy":
                return False
            raise
        if source == "dot" and self.changed is not None:
            await self.changed()
        return (
            accepted is not False
            and self.sessions.get_prefs(card["device_id"])["sources"][source]
            and any(d["device_id"] == card["device_id"] and d["online"] for d in await self.list_devices())
        )

    async def cancel_card(self, card_id: str, device_id: str | None) -> bool:
        # Persisted root cancellation makes reconnect replay safe even offline.
        await self.sessions.cancel_card(wire_id(card_id))
        return True

    def retain_devices(self, device_ids: set[str]) -> None:
        self._recording_scopes = {key: value for key, value in self._recording_scopes.items() if key in device_ids}

    def clear_recordings(self) -> None:
        self._recording_scopes.clear()

    async def send_message(self, device_id: str, text: str, stream=None) -> dict:
        if self._transcript is None:
            raise LinkError("unsupported", "MCP transcript handler is unavailable")
        duration_s = 0.0
        recording_id = "msg_" + secrets.token_hex(16)
        if stream is not None:
            link = self.hub.online.get(device_id)
            if link is None:
                raise LinkError("not_found", "Voice source is offline")
            duration_s = self.hub.voice.confirmed(link, stream, "dot", text)
            previous, scope = self._recording_scopes.get(device_id, (None, None))
            if previous is not link:
                scope = secrets.token_hex(16)
                self._recording_scopes[device_id] = (link, scope)
            recording_id = f"voice_{scope}_{stream}"
        try:
            result = await self._transcript(
                Transcript(
                    recording_id=recording_id,
                    text=text,
                    duration_s=duration_s,
                    device_id=device_id,
                    focus="dot",
                    confirmed=True,
                )
            )
        except ServiceError as exc:
            raise LinkError("bad_request", "Dot message could not be accepted") from exc
        if result.get("accepted") is not True:
            raise LinkError("not_listening", "Dot is not listening on this device")
        if self.changed is not None:
            await self.changed()
        # A durable webhook outbox receipt does not prove that ChatGPT has
        # received, read or acted on the message.
        return {"accepted": True, "status": "queued", "reason": "Queued for ChatGPT Dot"}


class DotRuntime:
    """Embed orb_mcp on its own loopback listener; never load tunnel or launchd."""

    def __init__(self, hub, sessions):
        self.hub, self.sessions = hub, sessions
        self.backend = HubDeviceBackend(hub, sessions)
        self.service = None
        self.tokens = None
        self.port = 0
        self._conn = None
        self._runner = None
        self._refresh_task = None
        self.lifecycle = LifecycleSupervisor("dot")
        self._last_session = None
        self._dot_state = "idle"
        self._dot_updated = 0.0
        self._empty_updated = time.time()
        self.backend.changed = self.refresh_session

    async def start(self, port: int = 8445) -> None:
        if self._runner is not None:
            return
        if self.lifecycle.health()["stopped"]:
            self.lifecycle = LifecycleSupervisor("dot")
        directory = private_directory(self.hub.cfg.data_dir / "mcp")
        try:
            self.tokens = Tokens(provision_tokens(private_directory(directory / "tokens")))
            self._conn = connect(directory / "orb-mcp.sqlite3")
            events = Events(self._conn, authorize=lambda principal, name, arguments: self.tokens.active(principal))
            self.service = OrbService(
                self._conn,
                self.backend,
                events,
                whisper_ready=self.hub.voice.available,
                max_pending=16,
                other_pending=lambda: len(self.sessions.pending_card_ids(exclude_prefix="mcp:")),
            )
            self._conn.execute(
                "CREATE INDEX IF NOT EXISTS dot_exchange_device ON cards(principal,device_id,created_at DESC)"
            )
            self._conn.execute(
                "CREATE INDEX IF NOT EXISTS dot_sent_device ON mcp_event_records "
                "(principal,name,json_extract(payload,'$.device_id'),occurred_at DESC)"
            )
            self._conn.execute(
                "CREATE INDEX IF NOT EXISTS dot_sent_latest ON mcp_event_records (principal,name,occurred_at DESC)"
            )
            self._conn.commit()
            self.sessions.register_pending_source("dot", self._pending_extra)
            self.sessions.register_message_handler("dot", self.backend.send_message, check=self.check_target)
            self._runner = web.AppRunner(
                create_app(self.service, self.tokens, task_owner=self.lifecycle), access_log=None
            )
            await self._runner.setup()
            site = web.TCPSite(self._runner, "127.0.0.1", port)
            await site.start()
            self.port = self._runner.addresses[0][1]
            await self.refresh_session()
            self._refresh_task = self.lifecycle.run(self._refresh, name="orb-dot-status", restarts=2)
        except BaseException:
            await self.stop()
            raise

    def _pending_extra(self, exclude_id: str | None = None) -> int:
        if self._conn is None:
            return 0
        return sum(
            wire_id(row[0]) != exclude_id and not self.sessions.has_card(wire_id(row[0]))
            for row in self._conn.execute("SELECT card_id FROM cards WHERE status IN ('queued','showing')")
        )

    def check_target(self, device_id: str | None) -> None:
        if self.service is None or not self.service.events.has_subscription(
            "voice.transcribed", principal="dot", device=device_id
        ):
            raise LinkError("not_listening", "Dot is not connected on this device")

    async def refresh_session(self) -> None:
        if self.service is None:
            return
        self.service.whisper_ready = self.hub.voice.available
        listening = self.service.events.has_subscription("voice.transcribed", principal="dot")
        latest = self._conn.execute(
            "SELECT card_id,created_at FROM cards WHERE principal='dot' AND device_id IS NOT NULL "
            "AND status!='cancelled' ORDER BY created_at DESC,rowid DESC LIMIT 1"
        ).fetchone()
        self._dot_state, self._dot_updated = self._exchange_state(latest)
        state = (listening, self._dot_state, self._dot_updated)
        if self._last_session != state:
            try:
                await self.sessions.upsert_session(
                    {
                        "id": "dot",
                        "agent": "dot",
                        "title": "ChatGPT Dot",
                        "project": "",
                        # Missing plugin enrollment is connectivity, not a pending
                        # permission: actual needs-you sessions keep first place.
                        "state": self._dot_state,
                        "reason": "" if listening else "Dot not connected",
                        "summary": "Ready for your message" if listening else "Connect Orb in ChatGPT to send messages",
                        "updated": self._dot_updated,
                        "can_message": listening,
                    }
                )
            except LinkError as exc:
                if exc.code != "busy":
                    raise
            else:
                self._last_session = state
        if not self.sessions.has_session("dot"):
            return
        now = time.time()
        devices = await self.backend.list_devices()
        self.backend.retain_devices({device["device_id"] for device in devices})
        self.sessions.retain_device_streams({device["device_id"] for device in devices})
        for device in devices:
            device_id = device["device_id"]
            connected = self.service.events.has_subscription("voice.transcribed", principal="dot", device=device_id)
            # Query only the exchange addressed to this paired device. Shared
            # Session summaries intentionally never contain Dot conversation.
            row = self._conn.execute(
                "SELECT card_id,content,created_at FROM cards WHERE principal='dot' AND device_id=? "
                "AND status!='cancelled' ORDER BY created_at DESC,rowid DESC LIMIT 1",
                (device_id,),
            ).fetchone()
            text = last_reply(json.loads(row["content"])["text"]) if row else ""
            state, updated = self._exchange_state(row, device_id=device_id)
            self.sessions.upsert_stream(
                {
                    "session": "dot",
                    "seq": 0,
                    "at": updated,
                    "state": state,
                    "stale": bool(row and now - row["created_at"] > 900),
                    "activity": "Ready for your message" if connected else "Dot not connected",
                    "steps": [],
                    "assistant": {"id": wire_id(row["card_id"]) if row else "", "text": text, "final": bool(text)},
                },
                device_id=device_id,
            )

    def _exchange_state(self, reply, *, device_id=None):
        # The durable transcript event is the send boundary. Comparing actual
        # exchanges keeps another device's replies/sends from changing this
        # device's state, and preserves queued/idle truth across hub restarts.
        query = "SELECT MAX(occurred_at) FROM mcp_event_records WHERE principal='dot' AND name='voice.transcribed'"
        args = ()
        if device_id is not None:
            query += " AND json_extract(payload,'$.device_id')=?"
            args = (device_id,)
        sent_at = self._conn.execute(query, args).fetchone()[0] or 0
        reply_at = reply["created_at"] if reply else 0
        return (
            "done" if reply is not None and reply_at > sent_at else "idle",
            max(reply_at, sent_at) or self._empty_updated,
        )

    async def _refresh(self) -> None:
        while True:
            await asyncio.sleep(1)
            await self.refresh_session()

    async def sync_device(self, device_id: str) -> None:
        if self.service is not None:
            await self.service.sync_device(device_id)

    async def status(self) -> dict:
        if self.service is None:
            return {"enabled": False}
        return {"enabled": True, "port": self.port, **await self.service.call_tool("orb_status", {}, "dot")}

    async def stop(self) -> None:
        await self.lifecycle.stop()
        self._refresh_task = None
        if self._runner is not None:
            await self._runner.cleanup()
            self._runner = None
        self.sessions.unregister_message_handler("dot")
        self.sessions.unregister_pending_source("dot")
        if self._last_session is not None and self.sessions.has_session("dot"):
            session = self.sessions.session_snapshot("dot")
            await self.sessions.upsert_session(
                {**session, "state": "idle", "reason": "Dot not connected", "can_message": False}
            )
            self.sessions.unavailable_stream("dot", "Dot not connected")
        self._last_session = None
        self.service = None
        self.backend.clear_recordings()
        self.port = 0
        if self._conn is not None:
            self._conn.close()
            self._conn = None
