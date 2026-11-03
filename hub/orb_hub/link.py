"""One device connection on wss://<hub>:8443/v1/link (docs/protocol.md, "Link").

The first message decides what the connection is: pair.begin, then pair.reveal, makes it a pairing request that
waits for `orb-hub pair <code>`; hello (with the device's token) makes it the device's live link. Anything else is
unauthorized and closed. Either has to be done within handshake_timeout, pings or not; after that a link that hears
nothing at all, pings included, for idle_timeout is closed.

Close codes: 1000 after pair.done or pair.failed; 1001 when idle, too slow to pair or say hello, replaced by a
newer connection or the hub is stopping; 1008 when unauthorized, after unpair or for a malformed pair.begin or
pair.reveal; 1009 for a message over 16 KB; 1013 when too many pairing requests are waiting or were made lately.
"""

import asyncio
import hashlib
import logging
import re
import secrets
from dataclasses import dataclass

from aiohttp import WSCloseCode, WSMessage, WSMsgType, web

from orb_hub import __version__, envelope, models
from orb_hub.battery import DISPLAY_STATES
from orb_hub.clock_preferences import local_hour24
from orb_hub.crash import crash_note
from orb_hub.envelope import Envelope, EnvelopeError, LinkError
from orb_hub.lifecycle import LifecycleSupervisor
from orb_hub.link_ports import LinkServices
from orb_hub.link_power import PowerDelivery
from orb_hub.pairing import PendingPair, compute_sas
from orb_hub.speech import SpeechSender
from orb_hub.speech_synth import SpeechError
from orb_hub.store import now_ms
from orb_hub.tz import posix_tz_for_zone

DEVICE_ID = re.compile(r"^orb-[0-9a-f]{12}$")
NONCE = re.compile(r"^[0-9a-fA-F]{32}$")
COMMIT = re.compile(r"^[0-9a-fA-F]{64}$")
TOKEN = re.compile(r"^[0-9a-fA-F]{64}$")
# Fields of hello kept with the device record, besides fw and build.
HELLO_DETAILS = ("idf", "boot_slot", "reset_reason", "uptime_s", "power", "rssi", "assets")
OTA_RESULTS = ("ok", "rolled_back", "failed")
OTA_STATES = ("downloading", "verifying", "ready", "failed")
MAX_LOG_LINES = 50
# Match the firmware's bounded request table. Reject excess work before it can
# accumulate behind a stalled WebSocket send or a missing device response.
MAX_INFLIGHT_REQUESTS = 16
# The device keeps its name in a 32-byte buffer (orb_link_status_t.device_name).
MAX_NAME_BYTES = 31

log = logging.getLogger(__name__)


def text_field(params: dict, key: str, max_bytes: int, required: bool = True) -> str:
    """A string parameter of at most max_bytes UTF-8 bytes; LinkError(bad_request) otherwise."""
    value = params.get(key, None if required else "")
    if not isinstance(value, str) or (required and not value) or len(value.encode()) > max_bytes:
        raise LinkError(envelope.BAD_REQUEST, f"{key} must be a string of 1-{max_bytes} bytes")
    return value


@dataclass(frozen=True)
class PairBegin:
    """An accepted pair.begin, waiting for the nonce its commit_d promised."""

    device_id: str
    name: str
    model: str
    fw: str
    commit_d: bytes
    nonce_h: bytes


class Link:
    def __init__(self, hub: LinkServices, ws: web.WebSocketResponse, peer: str, local_ip: str):
        self.services = hub if isinstance(hub, LinkServices) else LinkServices.from_hub(hub)
        self.lifecycle = LifecycleSupervisor("link")
        self.ws = ws
        self.peer = peer
        self.local_ip = local_ip
        # new → committed → pairing → closing, or new → online
        self.state = "new"
        self.device_id: str | None = None
        self.caps: set[str] = set()
        self.power = PowerDelivery(self, self._send)
        self.pending: PendingPair | None = None
        self._begin: PairBegin | None = None
        self._next_id = 1
        self.devtools_next_stream = 1
        self._waiting: dict[int, asyncio.Future] = {}
        self._send_lock = asyncio.Lock()
        # Other tasks ask run() to close the link rather than closing it themselves: aiohttp drops the TCP
        # connection without waiting for the device's close frame when close() races a pending receive().
        self._close_request: asyncio.Future = asyncio.get_running_loop().create_future()
        self._finished = asyncio.Event()
        self._task: asyncio.Task | None = None
        self._transport = None
        self.speech = SpeechSender(self.notify, self._speech_audio)
        self._pending_voice_target: str | None = None
        self._speech_source: str | None = None
        self._speech_target: str | None = None
        self._speech_baseline: frozenset[str] = frozenset()
        self._speaking_task: asyncio.Task | None = None

    def allocate_devtools_stream(self, maximum: int) -> int | None:
        if self.devtools_next_stream > maximum:
            return None
        stream = self.devtools_next_stream
        self.devtools_next_stream += 1
        return stream

    @property
    def hub(self) -> LinkServices:
        """Legacy test/caller alias for the explicit ports, never the Hub."""
        return self.services

    @property
    def label(self) -> str:
        return self.device_id or self.peer

    @property
    def in_handshake(self) -> bool:
        """Whether the link has yet to finish pairing's opening or say hello."""
        return self.state in ("new", "committed")

    # -- running

    async def run(self, request: web.Request) -> None:
        """Upgrades request to the WebSocket and serves it until it closes."""
        self._task = asyncio.current_task()
        self._transport = request.transport
        # Pings restart the idle timeout, so without a deadline a connection could hold a socket forever without
        # ever saying who it is.
        deadline = asyncio.get_running_loop().time() + self.services.cfg.handshake_timeout
        try:
            await self.ws.prepare(request)
            frame = await self._next_frame(deadline)
            if frame is None:
                return
            await self._first(frame)
            if self.state == "committed":
                frame = await self._next_frame(deadline)
                if frame is None:
                    return
                await self._reveal(frame)
            if self.state in ("pairing", "online"):
                await self._serve()
        except ConnectionError:
            pass  # the device went away mid-send
        finally:
            if self.services.home:
                self.services.home.unsubscribe(self)
            if self.services.nowplaying:
                self.services.nowplaying.unsubscribe(self)
            self.services.sessions.unsubscribe(self)
            await self.lifecycle.stop()
            self._speaking_task = None
            await self.speech.close()
            await self.services.voice.disconnect(self)
            for future in self._waiting.values():
                if not future.done():
                    future.set_exception(LinkError(envelope.TIMEOUT, "the device disconnected"))
            self.services.link_closed(self)
            self._finished.set()

    async def close(self, code: int, reason: str) -> None:
        """Closes the link with the close handshake; from another task, waits until run() has ended."""
        self.state = "closing"
        if asyncio.current_task() is self._task:
            await self.ws.close(code=code, message=reason.encode())
            return
        if not self._close_request.done():
            self._close_request.set_result((code, reason))
        try:
            async with asyncio.timeout(max(0.1, self.services.cfg.request_timeout)):
                await self._finished.wait()
        except TimeoutError:
            # A close request must also interrupt a provider call or a blocked
            # response send; waiting only for receive() can deadlock shutdown.
            if self._transport:
                self._transport.close()
            if self._task and not self._task.done():
                self._task.cancel()
                await asyncio.gather(self._task, return_exceptions=True)

    async def _receive_frame(self):
        try:
            return await self.ws.receive(timeout=self.power.idle_timeout)
        except (TimeoutError, ConnectionError):
            # An idle timeout or an abrupt drop (Wi-Fi loss, TCP reset) is the
            # device going away, not a failed service worker.
            return None

    async def _next_frame(self, deadline: float | None = None) -> WSMessage | None:
        """The next text or binary frame, or None once the connection is closed: by the device, by close(), after
        idle_timeout of silence or at deadline (loop time). aiohttp answers pings itself, and each one restarts the
        idle timeout but not the deadline."""
        receive = self.lifecycle.spawn(self._receive_frame(), name="orb-link-receive")
        timeout = None if deadline is None else max(0.0, deadline - asyncio.get_running_loop().time())
        await asyncio.wait((receive, self._close_request), timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
        if not receive.done():
            receive.cancel()
            await asyncio.gather(receive, return_exceptions=True)
            if self._close_request.done():
                code, reason = self._close_request.result()
            else:
                log.info("link %s: didn't pair or say hello in time, closing", self.label)
                code, reason = WSCloseCode.GOING_AWAY, "too slow"
            await self.ws.close(code=code, message=reason.encode())
            return None
        msg = receive.result()
        if msg is None:
            log.info("link %s: silent for %.0f s, closing", self.label, self.power.idle_timeout)
            await self.ws.close(code=WSCloseCode.GOING_AWAY, message=b"idle")
            return None
        if msg.type in (WSMsgType.TEXT, WSMsgType.BINARY):
            size = len(msg.data.encode()) if msg.type is WSMsgType.TEXT else len(msg.data)
            limit = models.MAX_EXTENSION_MESSAGE if self.caps else envelope.MAX_MESSAGE
            if size > limit:
                log.info("link %s: %d-byte message is over the limit, closing", self.label, size)
                await self.close(WSCloseCode.MESSAGE_TOO_BIG, "message too big")
                return None
            return msg
        if msg.type is WSMsgType.ERROR:
            log.info("link %s: %s", self.label, msg.data)
        return None

    def _handshake_envelope(self, frame: WSMessage) -> Envelope | None:
        if frame.type is WSMsgType.TEXT:
            try:
                return envelope.parse(frame.data)
            except EnvelopeError as exc:
                log.info("link %s: bad message before pairing or hello: %s", self.peer, exc)
        return None

    async def _first(self, frame: WSMessage) -> None:
        env = self._handshake_envelope(frame)
        if env and env.t == "req" and env.m == "pair.begin":
            await self._pair_begin(env)
        elif env and env.t == "req" and env.m == "hello":
            await self._hello(env)
        else:
            await self._refuse(
                env.id if env and env.t == "req" else None, "the first message must be pair.begin or hello"
            )

    async def _serve(self) -> None:
        while frame := await self._next_frame():
            if frame.type is WSMsgType.BINARY:
                if "voice" in self.caps:
                    try:
                        await self.services.voice.frame(self, frame.data)
                    except LinkError as exc:
                        stream = int.from_bytes(frame.data[1:3], "big") if len(frame.data) >= 3 else 0
                        await self._voice_event("voice.error", {"stream": stream, "code": exc.code, "msg": exc.msg})
                continue
            try:
                env = envelope.parse(frame.data)
            except EnvelopeError as exc:
                log.info("link %s: %s", self.label, exc)
                if exc.req_id:
                    await self._send(envelope.failure(exc.req_id, envelope.BAD_REQUEST, str(exc)))
                continue
            if env.t == "res":
                self._resolve(env)
            elif env.t == "req":
                await self._on_request(env)
            else:
                self._on_event(env)

    async def _send(
        self, obj: dict, *, bounded: bool = True, voice_stream: int | None = None, session_page: dict | None = None
    ) -> None:
        text = envelope.encode(obj)
        if self.caps and len(text.encode()) > models.MAX_EXTENSION_MESSAGE:
            raise ValueError("extension message exceeds 4096 bytes")
        try:
            # request() already owns a deadline spanning send AND response.
            # Responses/events need their own deadline to let close progress.
            async with asyncio.timeout(self.services.cfg.request_timeout if bounded else None):
                async with self._send_lock:
                    if session_page is not None:
                        # Preferences can change while a response waits for
                        # backpressure. Build the page only at its actual write.
                        try:
                            result = self.services.sessions.list_sessions(
                                self.device_id, session_page, request_id=obj["id"]
                            )
                            obj = envelope.success(obj["id"], result)
                        except LinkError as exc:
                            obj = envelope.failure(obj["id"], exc.code, exc.msg)
                        text = envelope.encode(obj)
                        if len(text.encode()) > models.MAX_EXTENSION_MESSAGE:
                            obj = envelope.failure(obj["id"], "too_large", "")
                            text = envelope.encode(obj)
                        if len(text.encode()) > models.MAX_EXTENSION_MESSAGE:
                            raise ValueError("extension response exceeds 4096 bytes")
                    # Cancellation can win while this event waits for another
                    # write. No await may separate this check from send_str.
                    if voice_stream is not None and not self.services.voice.result_pending(self, voice_stream):
                        return
                    if obj.get("m") == "voice.speak":
                        self._check_speech_target()
                    if obj.get("t") == "evt" and not self.services.sessions.delivery_allowed(self, obj["m"], obj["p"]):
                        return
                    obj = self.power.prepare(
                        obj, lambda method, params: self.services.sessions.delivery_allowed(self, method, params)
                    )
                    if obj is None:
                        return
                    text = envelope.encode(obj)
                    if self.caps and len(text.encode()) > models.MAX_EXTENSION_MESSAGE:
                        raise ValueError("extension message exceeds 4096 bytes")
                    await self.ws.send_str(text)
                    if self.power.replay_cards:
                        self.power.replay_cards = False
                        # Snapshot commit clears firmware cards. Queue current
                        # authorized cards after it; replay never answers one.
                        self.services.sessions.replay_cards(self)
        except TimeoutError:
            if self._transport:
                self._transport.close()
            raise ConnectionError("device send stalled") from None

    async def _refuse(self, req_id: int | None, msg: str) -> None:
        if req_id is not None:
            await self._send(envelope.failure(req_id, envelope.UNAUTHORIZED, msg))
        await self.close(WSCloseCode.POLICY_VIOLATION, "unauthorized")

    # -- pairing

    async def _busy(self, req_id: int) -> None:
        await self._send(envelope.failure(req_id, envelope.BUSY, "too many pairing requests; try again later"))
        await self.close(WSCloseCode.TRY_AGAIN_LATER, "busy")

    async def _pair_begin(self, env: Envelope) -> None:
        """pair.begin carries commit_d, SHA-256 of the device's nonce, which it reveals only after seeing nonce_h.
        Neither side can then choose its nonce knowing the other's, so a man in the middle can't search for nonces
        that make its code match the hub's: each attempt is one chance in a million, and attempts are limited."""
        try:
            device_id = env.p.get("device_id")
            if not isinstance(device_id, str) or not DEVICE_ID.match(device_id):
                raise LinkError(envelope.BAD_REQUEST, "device_id must be orb- and 12 lowercase hex digits")
            commit_d = env.p.get("commit_d")
            if not isinstance(commit_d, str) or not COMMIT.match(commit_d):
                raise LinkError(envelope.BAD_REQUEST, "commit_d must be a SHA-256 in hex")
            name = text_field(env.p, "name", MAX_NAME_BYTES)
            model = text_field(env.p, "model", 63, required=False)
            fw = text_field(env.p, "fw", 31, required=False)
        except LinkError as exc:
            await self._send(envelope.failure(env.id, exc.code, exc.msg))
            await self.close(WSCloseCode.POLICY_VIOLATION, "bad pair.begin")
            return
        if not self.services.pairing.has_room(device_id) or not self.services.pairing.allow_attempt():
            await self._busy(env.id)
            return

        nonce_h = secrets.token_bytes(16)
        await self._send(envelope.success(env.id, {"nonce_h": nonce_h.hex(), "hub_name": self.services.cfg.hub_name}))
        self.state = "committed"
        self._begin = PairBegin(device_id, name, model, fw, bytes.fromhex(commit_d), nonce_h)

    async def _reveal(self, frame: WSMessage) -> None:
        """pair.reveal: the nonce behind commit_d. Only now is the code worked out and the request held."""
        env = self._handshake_envelope(frame)
        if not (env and env.t == "req" and env.m == "pair.reveal"):
            await self._refuse(env.id if env and env.t == "req" else None, "pair.begin must be followed by pair.reveal")
            return
        begin = self._begin
        nonce_d = env.p.get("nonce_d")
        if not (
            isinstance(nonce_d, str)
            and NONCE.match(nonce_d)
            and hashlib.sha256(bytes.fromhex(nonce_d)).digest() == begin.commit_d
        ):
            log.info("link %s: pair.reveal doesn't match its commitment", self.peer)
            await self._send(envelope.failure(env.id, envelope.BAD_REQUEST, "nonce_d doesn't match commit_d"))
            await self.close(WSCloseCode.POLICY_VIOLATION, "bad pair.reveal")
            return
        # Requests from other devices may have filled the book since pair.begin.
        if not self.services.pairing.has_room(begin.device_id):
            await self._busy(env.id)
            return

        sas = compute_sas(self.services.cert.fingerprint, bytes.fromhex(nonce_d), begin.nonce_h)
        await self._send(envelope.success(env.id, {}))
        self.state = "pairing"
        self.device_id = begin.device_id
        self.pending = PendingPair(
            device_id=begin.device_id,
            name=begin.name,
            model=begin.model,
            fw=begin.fw,
            peer=self.peer,
            sas=sas,
            link=self,
        )
        self.services.pairing_started(self.pending)
        log.info("pairing request from %s (%s, %s) at %s", begin.device_id, begin.name, begin.model, self.peer)

    async def finish_pairing(self, token: str) -> None:
        """Sends pair.done with the device's token; the device stores it and reconnects with hello."""
        self.state = "closing"
        await self._send(envelope.event("pair.done", {"token": token}))
        await self.close(WSCloseCode.OK, "paired")

    async def pair_failed(self, reason: str) -> None:
        if self.state != "pairing":
            return
        log.info("pairing request from %s %s", self.label, reason)
        self.state = "closing"
        try:
            await self._send(envelope.event("pair.failed", {"reason": reason}))
            await self.close(WSCloseCode.OK, reason)
        except ConnectionError:
            pass

    # -- the paired device

    async def _hello(self, env: Envelope) -> None:
        p = env.p
        device_id, token = p.get("device_id"), p.get("token")
        device = None
        if isinstance(device_id, str) and DEVICE_ID.match(device_id) and isinstance(token, str) and TOKEN.match(token):
            device = self.services.store.get(device_id)
        if device is None or not device.token_matches(token):
            log.warning("hello from %s refused: unknown device or wrong token (%.32r)", self.peer, device_id)
            await self._refuse(env.id, "unknown device or wrong token")
            return

        caps = p.get("caps", [])
        if not isinstance(caps, list) or len(caps) > 16 or any(not isinstance(c, str) or len(c) > 32 for c in caps):
            await self._send(envelope.failure(env.id, envelope.BAD_REQUEST, "invalid capabilities"))
            await self.close(WSCloseCode.POLICY_VIOLATION, "invalid capabilities")
            return
        if (
            "cards" in caps
            or "power_hint" in caps
            or "usage" in caps
            or "devtools" in caps
            or "weather" in caps
            or ("home" in caps and self.services.home)
            or ("speech" in caps and self.services.speech and self.services.speech.available)
            or ("nowplaying" in caps and self.services.nowplaying and self.services.nowplaying.available)
        ) and len(envelope.encode(envelope.request(env.id, "hello", p)).encode()) > models.MAX_EXTENSION_MESSAGE:
            await self._send(envelope.failure(env.id, "too_large", "hello exceeds 4096 bytes"))
            await self.close(WSCloseCode.MESSAGE_TOO_BIG, "hello too large")
            return
        if "power_hint" in caps:
            self.caps.add("power_hint")
        if not self.power.configure(p):
            await self._send(envelope.failure(env.id, envelope.BAD_REQUEST, "invalid power hint"))
            await self.close(WSCloseCode.POLICY_VIOLATION, "invalid power hint")
            return
        if "cards" in caps:
            self.caps.add("cards")
            if "voice" in caps and self.services.voice.available:
                self.caps.add("voice")
        if "home" in caps and self.services.home:
            self.caps.add("home")
        if "usage" in caps:
            self.caps.add("usage")
        if "weather" in caps and self.services.weather:
            self.caps.add("weather")
        if "devtools" in caps and device.channel == "dev":
            self.caps.add("devtools")
        if "speech" in caps and self.services.speech and self.services.speech.available:
            self.caps.add("speech")
        if "nowplaying" in caps and self.services.nowplaying and self.services.nowplaying.available:
            self.caps.add("nowplaying")

        if "assets" in p and (
            not isinstance(p["assets"], str)
            or len(p["assets"]) > 10
            or (p["assets"] and (not p["assets"].isascii() or not p["assets"].isdigit()))
        ):
            await self._send(envelope.failure(env.id, envelope.BAD_REQUEST, "invalid assets version"))
            await self.close(WSCloseCode.POLICY_VIOLATION, "invalid assets version")
            return
        self.speech.configure(p.get("speech"), voice="voice" in self.caps)
        fw = p.get("fw") if isinstance(p.get("fw"), str) else ""
        build = p.get("build") if isinstance(p.get("build"), str) else ""
        self.services.store.record_hello(device_id, self.peer, fw, build, {k: p[k] for k in HELLO_DETAILS if k in p})
        if isinstance(p.get("last_ota"), dict):
            self.services.ota_outcome(device_id, p["last_ota"])
        self.device_id = device_id
        prefs = self.services.sessions.get_prefs(device_id) if self.caps & {"cards", "home", "weather"} else None
        tz_posix = posix_tz_for_zone(self.services.cfg.tz)
        await self._send(
            envelope.success(
                env.id,
                {
                    "hub_name": self.services.cfg.hub_name,
                    "hub_version": __version__,
                    "time_ms": now_ms(),
                    "tz": self.services.cfg.tz,
                    **({"tz_posix": tz_posix} if tz_posix is not None else {}),
                    "hour24": local_hour24(),
                    "device": {"name": device.name, "channel": device.channel},
                    **({"caps": sorted(self.caps)} if "caps" in p else {}),
                    **({"prefs": prefs} if prefs is not None else {}),
                },
            )
        )
        self.state = "online"
        if "power_hint" in self.caps:
            self.lifecycle.spawn(self.power.run(), name="orb-link-power")
        self.services.link_online(self)
        if "cards" in self.caps:
            await self.services.sessions.subscribe(self)
        if "usage" in self.caps:
            self.services.sessions.subscribe_usage(self)
        if "home" in self.caps:
            await self.services.home.subscribe(self)
        if "nowplaying" in self.caps:
            await self.services.nowplaying.subscribe(self)
        if "weather" in self.caps:
            self.services.weather.subscribe(self)
        if prefs is not None and (current := self.services.sessions.get_prefs(device_id)) != prefs:
            # A preference refresh can win while hello waits for transport,
            # before this link has joined the shared delivery worker.
            await self.services.sessions.emit(self, "prefs.update", current)
        log.info("%s online from %s: fw %s %s", device_id, self.peer, fw, build)
        self.services.devlogs.note(device_id, f"connected from {self.peer}: fw {fw} build {build}")
        await self.offer_update()

    async def offer_update(self) -> None:
        """Sends ota.available if this device's channel has a release it doesn't run."""
        offer = self.services.offer_for(self.services.store.get(self.device_id), self.local_ip)
        if offer["available"]:
            log.info(
                "offering %s %s to %s (install %s)", offer["version"], offer["build"], self.label, offer["install"]
            )
            await self.notify("ota.available", offer)
        else:
            offer = self.services.assets_offer_for(self.services.store.get(self.device_id), self.local_ip)
            if offer["available"]:
                await self.notify("ota.available", offer)

    async def _on_request(self, env: Envelope) -> None:
        if self.state != "online":
            await self._send(envelope.failure(env.id, envelope.UNAUTHORIZED, "not paired yet"))
            return
        if env.m == "ota.check":
            device = self.services.store.get(self.device_id)
            if env.p.get("kind", "app") not in ("app", "assets"):
                reply = envelope.failure(env.id, envelope.BAD_REQUEST, "kind must be app or assets")
            else:
                offer = (
                    self.services.assets_offer_for(device, self.local_ip)
                    if env.p.get("kind") == "assets"
                    else self.services.offer_for(device, self.local_ip)
                )
                reply = envelope.success(env.id, offer)
        elif env.m in ("hello", "pair.begin", "pair.reveal"):
            reply = envelope.failure(env.id, envelope.BAD_REQUEST, f"{env.m} is only valid while connecting")
        elif env.m in ("home.set", "home.toggle", "home.scene"):
            try:
                if "home" not in self.caps or not self.services.home:
                    raise LinkError("unsupported", "Home capability was not negotiated")
                reply = envelope.success(env.id, await self.services.home.request(self.device_id, env.m, env.p))
            except LinkError as exc:
                reply = envelope.failure(env.id, exc.code, exc.msg)
        elif env.m in ("nowplaying.snapshot", "nowplaying.control"):
            try:
                if "nowplaying" not in self.caps or not self.services.nowplaying:
                    raise LinkError("unsupported", "nowplaying capability was not negotiated")
                reply = envelope.success(env.id, await self.services.nowplaying.request(self.device_id, env.m, env.p))
            except LinkError as exc:
                reply = envelope.failure(env.id, exc.code, exc.msg)
        elif env.m in (
            "sessions.subscribe",
            "sessions.list",
            "prefs.set",
            "card.answer",
            "message.send",
            "voice.begin",
            "voice.end",
            "voice.cancel",
        ):
            try:
                required = {"cards", "home", "weather"} if env.m == "prefs.set" else {"cards"}
                if not self.caps & required or (env.m.startswith("voice.") and "voice" not in self.caps):
                    raise LinkError("unsupported", "capability was not negotiated")
                if env.m == "sessions.subscribe":
                    models.fields(env.p, set())
                    await self._send(envelope.success(env.id, {}))
                    await self.services.sessions.subscribe(self)
                    return
                if env.m == "sessions.list":
                    await self._send(envelope.success(env.id, {}), session_page=env.p)
                    return
                elif env.m == "prefs.set":
                    result = await self.services.sessions.set_prefs(self.device_id, env.p)
                else:
                    result = await self._extension_request(env.m, env.p)
                reply = envelope.success(env.id, result)
            except LinkError as exc:
                reply = envelope.failure(env.id, exc.code, exc.msg)
                if env.m == "message.send":
                    reply["e"]["status"] = "failed"
        else:
            reply = envelope.failure(env.id, envelope.UNKNOWN_METHOD, "unknown method")
        await self._send(reply)

    async def _extension_request(self, method, params):
        if method == "card.answer":
            return await self.services.sessions.answer(self.device_id, params)
        if method == "message.send":
            if params.get("target") == "time":
                raise LinkError("bad_request", "Time reminders are saved locally on Orb")
            if "stream" in params:
                if "voice" not in self.caps:
                    raise LinkError("unsupported", "voice was not negotiated")
                self.services.voice.confirmed(self, params["stream"], params.get("target"), params.get("text"))
                await self.stop_speech("interrupted")
                self._arm_speech(params.get("target"))
            else:
                self._pending_voice_target = None
            try:
                result = await self.services.sessions.message(self.device_id, params)
            except BaseException:
                # A rejected/interrupted Send leaves the transcript reviewable,
                # but must not speak a later unrelated provider update.
                self._pending_voice_target = None
                await self.stop_speech("stop")
                raise
            if "stream" in params:
                self.services.voice.consume(self, params["stream"])
            return result
        if method == "voice.begin":
            if params.get("target") != "time":
                self.services.sessions.check_target(params.get("target"), self.device_id)
            await self.stop_speech("interrupted")
            # Recording and review grant no permission to speak a reply. Only
            # the explicit, matching message.send above arms the next answer.
            self._pending_voice_target = None
            return await self.services.voice.begin(self, params, self._voice_event)
        if method == "voice.end":
            return await self.services.voice.end(self, params)
        result = await self.services.voice.cancel(self, params)
        self._pending_voice_target = None
        await self.stop_speech("stop")
        self.services.sessions.discard_voice_result(self, params["stream"])
        return result

    def _arm_speech(self, target: str | None) -> None:
        """Speak the next answer from target. Text it already showed before this
        request (a summary or final reply re-sent by a late update) is not that
        answer, so remember it and never read it aloud as one."""
        self._pending_voice_target = target
        self._speech_baseline = self.services.sessions.speech_baseline(self.device_id, target)

    async def _voice_event(self, method, params):
        if method == "voice.error":
            self._pending_voice_target = None
        await self.services.sessions.emit(self, method, params)

    def _on_event(self, env: Envelope) -> None:
        if self.state != "online":
            return
        p = env.p
        if env.m == "voice.speak.stopped" and "voice" in self.caps:
            accepted = self.speech.stopped(p)
            if accepted and self._speaking_task and not self._speaking_task.done():
                self._speaking_task.cancel()
                self._speaking_task = None
            return
        if env.m == "state.report":
            self.power.configure(p)
            self.services.store.record_state(self.device_id, p)
            self._record_crash(p)
            self._record_battery(p)
        elif env.m == "log.batch":
            lines = p.get("lines")
            if isinstance(lines, list):
                self.services.devlogs.lines(self.device_id, [s for s in lines[:MAX_LOG_LINES] if isinstance(s, str)])
        elif env.m == "ota.report":
            self._ota_report(p)
        elif env.m in ("devtools.output", "devtools.done"):
            self.services.devtools.event(self, env.m, p)

    def _record_crash(self, report: dict) -> None:
        note = crash_note(report.get("crash"))
        if note:
            self.services.devlogs.note(self.device_id, note)

    def _record_battery(self, report: dict) -> None:
        """A battery-log failure is a log line, never a link failure."""
        power = report.get("power")
        power = power if isinstance(power, dict) else {}

        def integer(name: str) -> int | None:
            value = power.get(name)
            return value if isinstance(value, int) and not isinstance(value, bool) else None

        state = power.get("display_state")
        try:
            self.services.store.record_battery(
                self.device_id,
                pct=integer("battery_pct"),
                mv=integer("battery_mv"),
                charging=power.get("charging") if isinstance(power.get("charging"), bool) else None,
                usb=power.get("usb") if isinstance(power.get("usb"), bool) else None,
                display_state=state if state in DISPLAY_STATES else None,
            )
        except Exception as exc:  # noqa: BLE001 - an extra log row must not break the device link
            log.warning("link %s: battery sample not recorded: %s", self.label, exc)

    def _ota_report(self, p: dict) -> None:
        version, state = p.get("version"), p.get("state")
        if not isinstance(version, str) or state not in OTA_STATES:
            log.info("link %s: ignoring malformed ota.report", self.label)
            return
        report = {"version": version, "state": state, "at": now_ms()}
        if isinstance(p.get("progress"), int):
            report["progress"] = p["progress"]
        if isinstance(p.get("error"), str):
            report["error"] = p["error"][:200]
        if p.get("kind") == "assets":
            self.services.store.update_ota(
                self.device_id,
                assets_report=report,
                **(
                    {"assets_failed": report}
                    if state == "failed"
                    else {"assets_failed": None}
                    if state == "ready"
                    else {}
                ),
            )
            return
        if p.get("kind", "app") != "app":
            return
        self.services.store.update_ota(self.device_id, report=report)
        if state in ("ready", "failed"):
            log.info("%s: OTA %s %s %s", self.device_id, version, state, report.get("error", ""))
            self.services.devlogs.note(self.device_id, f"OTA {version} {state} {report.get('error', '')}".rstrip())
        if state == "failed":
            self.services.ota_outcome(
                self.device_id, {"version": version, "result": "failed", "error": report.get("error")}
            )

    # -- hub → device

    # Keep the public RPC keyword; this wrapper already uses asyncio.timeout_at.
    async def request(self, method: str, params: dict | None = None, timeout: float | None = None) -> dict:
        """Returns the device's result, or LinkError (device error, busy, offline or timeout).

        The deadline covers admission, the send lock, transport backpressure and
        the reply. Admission never waits, so callers cannot build an unbounded queue.
        """
        if self.state != "online":
            raise LinkError(envelope.NOT_FOUND, f"{self.label} is offline")
        if len(self._waiting) >= MAX_INFLIGHT_REQUESTS:
            raise LinkError(envelope.BUSY, f"{self.label} has too many pending requests")
        duration = self.services.cfg.request_timeout if timeout is None else timeout
        if duration <= 0:
            raise LinkError(envelope.TIMEOUT, f"{self.label} didn't answer {method}")
        loop = asyncio.get_running_loop()
        deadline = loop.time() + duration
        req_id = self._next_id
        self._next_id += 1
        future = loop.create_future()
        self._waiting[req_id] = future
        try:
            async with asyncio.timeout_at(deadline):
                await self._send(envelope.request(req_id, method, params or {}), bounded=False)
                return await future
        except TimeoutError:
            raise LinkError(envelope.TIMEOUT, f"{self.label} didn't answer {method}") from None
        except ConnectionError:
            raise LinkError(envelope.TIMEOUT, f"{self.label} disconnected") from None
        finally:
            self._waiting.pop(req_id, None)
            if not future.done():
                future.cancel()
            elif not future.cancelled():
                # A disconnect or device error can resolve it while send is
                # blocked, before this task ever awaits it. Consume that error
                # even when timeout/cancellation ends the send first.
                future.exception()

    async def stop_speech(self, reason: str = "interrupted") -> None:
        if self._speaking_task and self._speaking_task is not asyncio.current_task() and not self._speaking_task.done():
            self._speaking_task.cancel()
            self._speaking_task = None
        if self.speech.stream:
            stream = self.speech.stream
            self.speech.stopped({"stream": stream, "reason": reason})
            try:
                await self.notify("voice.speak.stop", {"stream": stream})
            except (ConnectionError, TimeoutError):
                pass

    def trigger_speech(self, target: str, text: str, source: str | None) -> None:
        """Admit one requested answer without exposing link-local speech state."""
        if self.state != "online" or self._pending_voice_target != target or text in self._speech_baseline:
            return
        self._pending_voice_target = None
        if source not in {"claude", "codex", "dot"}:
            return
        try:
            self.services.sessions.check_source(self.device_id, source)
        except LinkError:
            return
        self._speech_source = source
        self._speech_target = target
        self._speaking_task = self.lifecycle.spawn(self.speak_answer(text), name="orb-link-speech")

    async def speak_answer(self, text: str) -> int | None:
        if not self.services.speech or not self.services.speech.available:
            return None
        if not self.speech.rates or self.state != "online":
            return None
        if self.speech.stream:
            await self.stop_speech("interrupted")
        rate = 24000 if 24000 in self.speech.rates else 16000
        try:
            caption, chunk_stream = await self.services.speech.render_pcm(text, rate=rate)
            self._check_speech_target()
            stream = self.speech.start(
                chunk_stream, rate=rate, text=caption, voice=getattr(self.services.speech, "selected_voice", None)
            )
            return stream
        except (LinkError, SpeechError, OSError, ValueError, RuntimeError) as exc:
            # Producer diagnostics can contain transcript text or private
            # temporary paths. Retain only the error category in hub logs.
            log.warning("speech playback failed for %s (%s)", self.label, type(exc).__name__)
            return None

    def _check_speech_target(self) -> None:
        if self._speech_source is None:
            return  # Low-level playback can have no conversation target.
        self.services.sessions.check_source(self.device_id, self._speech_source)
        if self._speech_target != "dot" and (
            (self.services.sessions.session_snapshot(self._speech_target) or {}).get("agent") != self._speech_source
        ):
            raise LinkError("expired", "Speech session is no longer available")

    async def _speech_audio(self, stream: int, frame: bytes) -> None:
        async with asyncio.timeout(self.services.cfg.request_timeout):
            async with self._send_lock:
                if self.state != "online" or stream != self.speech.stream:
                    return
                self._check_speech_target()
                await self.ws.send_bytes(frame)

    async def notify(self, name: str, params: dict) -> None:
        if self.state == "online":
            if self.power.defer(name, params):
                return
            if not self.power.background:
                await self.power.flush()
            await self._send(
                envelope.event(name, params), voice_stream=params["stream"] if name == "voice.result" else None
            )

    def _resolve(self, env: Envelope) -> None:
        future = self._waiting.get(env.id)
        if future is None or future.done():
            return  # late answer to a request that already timed out
        if env.ok:
            future.set_result(env.r)
        else:
            future.set_exception(LinkError(str(env.e.get("code", envelope.INTERNAL)), str(env.e.get("msg", ""))))
