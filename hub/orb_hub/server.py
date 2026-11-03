"""The hub service: the TLS port (link and firmware downloads), the pairing port, the control socket and mDNS."""

import asyncio
import logging
import re
import secrets
import socket
import ssl
from dataclasses import asdict

from aiohttp import ClientError, WSCloseCode, web

from orb_hub import firmware, units
from orb_hub.config import CHANNELS, Config, ensure_private_dir, lan_ipv4, local_host_name
from orb_hub.control import ControlError, ControlServer
from orb_hub.devlog import DeviceLogs
from orb_hub.devtools import Devtools
from orb_hub.envelope import MAX_MESSAGE, LinkError
from orb_hub.lifecycle import LifecycleSupervisor, finish_shutdown
from orb_hub.link import DEVICE_ID, MAX_NAME_BYTES, OTA_RESULTS, TOKEN, Link
from orb_hub.mdns import Advertiser, advertised_host, mdns_host, service_info
from orb_hub.pairing import PairingBook, PendingPair
from orb_hub.sessions import SessionHub
from orb_hub.store import Device, Store
from orb_hub.tls import HubCert, ensure_cert, server_context
from orb_hub.voice import VoiceService

ANY_ADDRESS = ("", "0.0.0.0", "::")
SAS_CODE = re.compile(r"^\d{6}$")
# Links that haven't paired or said hello yet, from one address and in all. A device opens one at a time; the caps
# keep anyone else from running the hub out of file descriptors in the handshake_timeout each one gets.
MAX_HANDSHAKES_PER_PEER = 4
MAX_HANDSHAKES = 32
# Refused connections are logged at most this often.
REFUSAL_LOG_INTERVAL = 60.0
PREFERENCES_POLL = 60.0
# Firmware images are streamed to the device in bounded chunks, never via sendfile, which over
# TLS trips asyncio's _sendfile_fallback when the device disconnects mid-download.
FIRMWARE_CHUNK = 256 * 1024

log = logging.getLogger(__name__)


def describe(device: Device, online: bool) -> dict:
    """A device as `orb-hub devices` shows it."""
    power = device.state.get("power") or device.hello.get("power") or {}
    return {
        "device_id": device.device_id,
        "name": device.name,
        "model": device.model,
        "channel": device.channel,
        "fw": device.fw,
        "build": device.build,
        "online": online,
        "last_seen": device.last_seen,
        "ip": device.last_ip,
        "battery_pct": power.get("battery_pct"),
        "charging": power.get("charging"),
        "rssi": device.state.get("rssi", device.hello.get("rssi")),
        "ota": device.ota,
    }


def reachable_addresses(bind: str) -> list[str]:
    """The addresses devices reach the hub on: the one it is bound to, or every LAN address when it listens on all."""
    return lan_ipv4() if bind in ANY_ADDRESS else [bind]


def _listen(host: str, port: int) -> socket.socket:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(128)
    sock.setblocking(False)
    return sock


class Hub:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.cert: HubCert | None = None
        self.store: Store | None = None
        self.releases = firmware.Releases(cfg.firmware_dir)
        self.devlogs = DeviceLogs(cfg.device_log_dir)
        self.pairing = PairingBook(cfg.pair_ttl)
        self.online: dict[str, Link] = {}
        self.tls_port = 0
        self.pair_port = 0
        # The SRV target devices check the certificate against, and the addresses its A records carry.
        self.mdns_host = ""
        self.addresses: list[str] = []
        self._links: set[Link] = set()
        self._runners: list[web.AppRunner] = []
        self._listeners: list[asyncio.Server] = []
        self._control = ControlServer(cfg.control_socket, self.handle_command)
        self._advertiser: Advertiser | None = None
        self._address_watch: asyncio.Task | None = None
        self._preferences_watch: asyncio.Task | None = None
        self._preferences_refresh = asyncio.Event()
        self.supervisor = LifecycleSupervisor("hub")
        self._refused = 0
        self._refusal_logged = float("-inf")
        self.sessions: SessionHub | None = None
        self.voice = VoiceService()
        self.devtools = Devtools(self)
        self.agents = None
        self.dot = None
        self.home = None
        self.speech = None
        self.nowplaying = None
        self.weather = None
        self._weather_config_lock = asyncio.Lock()

    # -- lifecycle

    async def start(self) -> None:
        cfg = self.cfg
        ensure_private_dir(cfg.data_dir)
        ensure_private_dir(cfg.log_dir)
        ensure_private_dir(cfg.firmware_dir)
        host_label = local_host_name()
        self.addresses = reachable_addresses(cfg.bind)
        if not self.addresses:
            # Devices check the link against the host name, not these, but mDNS has no address to give them until
            # one appears (the watch below picks it up).
            log.warning("no LAN IPv4 address yet: mDNS announces none until this Mac has one")
        cert_ips = self.addresses + [ip for ip in lan_ipv4() if ip not in self.addresses]
        self.cert = ensure_cert(cfg.tls_dir, [f"{host_label}.local", mdns_host(host_label)], cert_ips)
        self.mdns_host = advertised_host(self.cert.dns_names) or mdns_host(host_label)
        if self.mdns_host not in self.cert.dns_names:
            log.warning("the certificate doesn't name %s; devices will refuse the link", self.mdns_host)
        self.store = Store(cfg.db_path)
        try:
            await self._control.start()
            temperature = await asyncio.to_thread(units.read_temperature_unit)
            self.sessions = await SessionHub.open(
                cfg.data_dir, send_timeout=cfg.request_timeout, temperature=temperature
            )
            self.sessions.on_speech_trigger = self.maybe_speak
            await self.sessions.start()
            self._preferences_watch = self.supervisor.run(self._watch_preferences, name="orb-preferences")
            from orb_hub.weather import WeatherLocation
            from orb_hub.weather_delivery import WeatherBridge
            from orb_hub.weather_prefs import load_location

            location = await asyncio.to_thread(load_location, cfg.data_dir)
            self.weather = WeatherBridge(self.sessions, WeatherLocation(**location) if location else None)
            await self.weather.start()
            if cfg.home_enabled:
                from orb_hub.home import HomeBridge

                self.home = await HomeBridge.open(self.sessions, cfg.data_dir)
                await self.home.start()
            if cfg.agents_enabled:
                from orb_hub.agents import AgentsBridge

                self.agents = AgentsBridge(
                    self.sessions,
                    cfg.data_dir,
                    hooks_port=cfg.hooks_port,
                    enable_codex_queue=cfg.enable_codex_queue,
                    claude_usage=True,
                )
                await self.agents.start()
            if cfg.mcp_enabled:
                from orb_hub.dot import DotRuntime

                self.dot = DotRuntime(self, self.sessions)
                await self.dot.start(port=cfg.mcp_port)
            if cfg.speech_enabled:
                from orb_hub.speech_synth import SpeechService

                self.speech = SpeechService()
                await self.speech.start()
            if cfg.nowplaying_enabled:
                from orb_hub.nowplaying import NowPlayingService

                self.nowplaying = NowPlayingService(self.sessions)
                await self.nowplaying.start()
            tls_app = web.Application()
            tls_app.router.add_get("/v1/link", self._link_handler)
            tls_app.router.add_get("/v1/firmware/{name}", self._firmware_handler)
            pair_app = web.Application()
            pair_app.router.add_get("/v1/hub-cert", self._cert_handler)
            self.tls_port = await self._serve(tls_app, cfg.tls_port, server_context(self.cert))
            self.pair_port = await self._serve(pair_app, cfg.pair_port, None)
            if cfg.mdns:
                self._advertiser = Advertiser(
                    service_info(
                        cfg.hub_name,
                        self.mdns_host,
                        self.tls_port,
                        self.pair_port,
                        self.cert.fingerprint_hex,
                        self.addresses,
                    )
                )
                await self._advertiser.start()
            self._address_watch = self.supervisor.run(self._watch_addresses, name="orb-addresses")
        except BaseException:
            await self.stop()
            raise
        log.info(
            "orb-hub %r on %s: link and firmware on %d (TLS), certificate on %d, fingerprint %s",
            cfg.hub_name,
            cfg.bind,
            self.tls_port,
            self.pair_port,
            self.cert.fingerprint_hex,
        )

    async def stop(self) -> None:
        await finish_shutdown(self._stop(), name="orb-hub-stop")

    async def _stop(self):
        if self._preferences_watch:
            self._preferences_watch.cancel()
            await asyncio.gather(self._preferences_watch, return_exceptions=True)
            self._preferences_watch = None
        if self._address_watch:
            self._address_watch.cancel()
            await asyncio.gather(self._address_watch, return_exceptions=True)
            self._address_watch = None
        if self._advertiser:
            await self._advertiser.stop()
            self._advertiser = None
        await self._control.stop()
        for listener in self._listeners:
            listener.close()
        self._listeners.clear()
        await self.pairing.stop()
        await asyncio.gather(
            *(link.close(WSCloseCode.GOING_AWAY, "hub stopping") for link in list(self._links)),
            return_exceptions=True,
        )
        if self.agents:
            await self.agents.stop()
            self.agents = None
        if self.dot:
            await self.dot.stop()
            self.dot = None
        if self.speech:
            await self.speech.stop()
            self.speech = None
        if self.nowplaying:
            await self.nowplaying.stop()
            self.nowplaying = None
        await self.voice.close()
        if self.home:
            await self.home.stop()
            self.home = None
        if self.weather:
            await self.weather.stop()
            self.weather = None
        if self.sessions:
            await self.sessions.close()
            self.sessions = None
        await self.supervisor.stop()
        for runner in self._runners:
            await runner.cleanup()
        self._runners.clear()
        self.devlogs.close()
        if self.store:
            self.store.close()
            self.store = None

    async def _serve(self, app: web.Application, port: int, ssl_context: ssl.SSLContext | None) -> int:
        sock = _listen(self.cfg.bind, port)
        # A connection that stalls in the TLS handshake (asyncio's default allows 60 s) or never sends a request
        # (keepalive_timeout covers that too; aiohttp's default is an hour) holds a file descriptor all the while.
        runner = web.AppRunner(app, access_log=None, shutdown_timeout=2.0, keepalive_timeout=self.cfg.handshake_timeout)
        await runner.setup()
        self._runners.append(runner)
        # Served without web.SockSite, which has no way to set the TLS handshake timeout.
        listener = await asyncio.get_running_loop().create_server(
            runner.server,
            sock=sock,
            ssl=ssl_context,
            ssl_handshake_timeout=self.cfg.handshake_timeout if ssl_context else None,
        )
        self._listeners.append(listener)
        return sock.getsockname()[1]

    async def refresh_addresses(self) -> None:
        """Re-announces the A records when the LAN addresses changed: Wi-Fi joined after the hub started, or DHCP
        handed out a new lease."""
        addresses = reachable_addresses(self.cfg.bind)
        if addresses == self.addresses:
            return
        log.info("LAN addresses now %s (were %s)", ", ".join(addresses) or "none", ", ".join(self.addresses) or "none")
        self.addresses = addresses
        if self._advertiser:
            await self._advertiser.update(addresses)

    async def _watch_addresses(self) -> None:
        while True:
            await asyncio.sleep(self.cfg.address_poll)
            await self.refresh_addresses()

    def request_preferences_refresh(self) -> None:
        self._preferences_refresh.set()

    async def _watch_preferences(self) -> None:
        while True:
            try:
                async with asyncio.timeout(PREFERENCES_POLL):
                    await self._preferences_refresh.wait()
            except TimeoutError:
                pass
            self._preferences_refresh.clear()
            temperature = await asyncio.to_thread(units.read_temperature_unit)
            self.sessions.refresh_temperature(temperature)

    @property
    def _tasks(self):
        return self.supervisor.tasks

    def spawn(self, coro) -> None:
        self.supervisor.spawn(coro, name="orb-hub-operation")

    def health(self) -> dict:
        services = {"hub": self.supervisor.health(), "pairing": self.pairing.supervisor.health()}
        for name in ("sessions", "voice", "agents", "home", "nowplaying", "dot", "weather"):
            service = getattr(self, name, None)
            owner = getattr(service, "supervisor", None) or getattr(service, "lifecycle", None)
            if owner:
                services[name] = owner.health()
                if name == "sessions":
                    failed = service.status()["failed_subscribers"]
                    services[name]["delivery_failures"] = failed
                    services[name]["healthy"] = services[name]["healthy"] and not failed
        for index, link in enumerate(self.online.values()):
            services[f"link:{index}"] = link.lifecycle.health()
            services[f"speech:{index}"] = link.speech.supervisor.health()
        return {"healthy": all(value["healthy"] for value in services.values()), "services": services}

    async def cmd_health(self, args: dict) -> dict:
        return self.health()

    # -- HTTP

    async def _cert_handler(self, request: web.Request) -> web.Response:
        return web.Response(body=self.cert.pem, content_type="application/x-pem-file")

    async def _link_handler(self, request: web.Request) -> web.StreamResponse:
        peer = request.remote or "unknown"
        handshakes = [link.peer for link in self._links if link.in_handshake]
        if len(handshakes) >= MAX_HANDSHAKES or handshakes.count(peer) >= MAX_HANDSHAKES_PER_PEER:
            self._note_refusal(peer)
            return web.Response(status=503, text="too many connections\n", headers={"Retry-After": "10"})
        # The link enforces the 16 KB limit itself, after reading the whole message, so it can close cleanly with
        # 1009. aiohttp stops reading mid-frame at its own limit, and the unread bytes turn the close into a reset.
        # timeout bounds the close handshake with an unresponsive device.
        ws = web.WebSocketResponse(max_msg_size=4 * MAX_MESSAGE, compress=False, autoping=True, timeout=5.0)
        sockname = request.transport.get_extra_info("sockname") if request.transport else None
        link = Link(self, ws, peer, sockname[0] if sockname else self.cfg.bind)
        # Counted from before the upgrade, so a burst of connections can't all pass the caps while upgrading.
        self._links.add(link)
        try:
            await link.run(request)
        finally:
            self._links.discard(link)
        return ws

    def _note_refusal(self, peer: str) -> None:
        self._refused += 1
        now = asyncio.get_running_loop().time()
        if now - self._refusal_logged >= REFUSAL_LOG_INTERVAL:
            log.warning("refused %d link(s) over the connection caps, the latest from %s", self._refused, peer)
            self._refused = 0
            self._refusal_logged = now

    async def _firmware_handler(self, request: web.Request) -> web.StreamResponse:
        scheme, _, token = request.headers.get("Authorization", "").partition(" ")
        device = None
        if scheme.lower() == "bearer" and TOKEN.match(token.strip()):
            device = self.store.find_by_token(token.strip())
        if device is None:
            return web.Response(status=401, text="unauthorized\n", headers={"WWW-Authenticate": "Bearer"})
        path = self.releases.image_path(request.match_info["name"])
        if path is None:
            raise web.HTTPNotFound()
        log.info("%s downloads %s", device.device_id, path.name)
        # Stream the image in bounded chunks rather than web.FileResponse. FileResponse uses loop.sendfile(),
        # and over TLS that falls back to asyncio's _sendfile_fallback; when a device disconnects mid-download
        # its restore() calls set_protocol() on the already-closed _SSLProtocolTransport (whose _ssl_protocol is
        # None), crashing the handler with AttributeError in asyncio/sslproto.py set_protocol.
        response = web.StreamResponse(headers={"Content-Type": "application/octet-stream"})
        response.content_length = path.stat().st_size
        await response.prepare(request)
        try:
            with path.open("rb") as image:
                while chunk := image.read(FIRMWARE_CHUNK):
                    await response.write(chunk)
            await response.write_eof()
        except ConnectionError:
            # The device dropped the TLS connection mid-download; there is nobody left to send the rest to.
            # Swallow it here so aiohttp's finish_response sees the reset and closes the connection quietly.
            log.info("%s aborted the download of %s", device.device_id, path.name)
        return response

    # -- links

    def pairing_started(self, pending: PendingPair) -> None:
        replaced = self.pairing.add(pending)
        if replaced:
            self.spawn(replaced.link.pair_failed("rejected"))

    def link_online(self, link: Link) -> None:
        old = self.online.get(link.device_id)
        self.online[link.device_id] = link
        if old and old is not link:
            self.devtools.disconnect(old)
            self.spawn(old.close(WSCloseCode.GOING_AWAY, "replaced"))
        if self.dot and "cards" in link.caps:
            self.spawn(self.dot.sync_device(link.device_id))

    def link_closed(self, link: Link) -> None:
        self.devtools.disconnect(link)
        if link.pending:
            self.pairing.remove(link.pending)
        if link.device_id and self.online.get(link.device_id) is link:
            del self.online[link.device_id]
            if self.store:
                self.store.touch(link.device_id)
            self.devlogs.note(link.device_id, "disconnected")
            log.info("%s offline", link.device_id)

    def offer_for(self, device: Device, local_ip: str) -> dict:
        # The address the device reached us on is one it can download from.
        host = f"[{local_ip}]" if ":" in local_ip else local_ip
        return firmware.make_offer(
            self.releases.current(device.channel),
            device.fw,
            device.ota.get("failed"),
            f"https://{host}:{self.tls_port}",
        )

    def assets_offer_for(self, device: Device, local_ip: str) -> dict:
        version = device.hello.get("assets")
        if not isinstance(version, str):
            return {"available": False}
        app = self.releases.current(device.channel)
        if app is None or not firmware.is_current(app, device.fw) or app.get("build") != device.build:
            # A bench/newer/older application may require a different font
            # identity. Do not replace its pack with the channel app's assets.
            return {"available": False}
        host = f"[{local_ip}]" if ":" in local_ip else local_ip
        return firmware.make_offer(
            self.releases.current(device.channel, assets=True),
            version,
            device.ota.get("assets_failed"),
            f"https://{host}:{self.tls_port}",
        )

    def ota_outcome(self, device_id: str, outcome: dict) -> None:
        """Records how an update attempt ended (hello's last_ota, or ota.report failed)."""
        version, result = outcome.get("version"), outcome.get("result")
        if not isinstance(version, str) or result not in OTA_RESULTS:
            return
        error = outcome.get("error") if isinstance(outcome.get("error"), str) else None
        last = {"version": version, "result": result, "error": error}
        self.store.update_ota(device_id, last=last, failed=None if result == "ok" else last)
        self.devlogs.note(device_id, f"update to {version}: {result} {error or ''}".rstrip())
        log.info("%s: update to %s %s %s", device_id, version, result, error or "")

    # -- control commands

    async def handle_command(self, cmd: str, args: dict):
        handler = {
            "pair": self.cmd_pair,
            "unpair": self.cmd_unpair,
            "devices": self.cmd_devices,
            "identify": self.cmd_identify,
            "reboot": self.cmd_reboot,
            "dev": self.cmd_dev,
            "config": self.cmd_config,
            "prefs.show": self.cmd_prefs_show,
            "prefs.set": self.cmd_prefs_set,
            "weather.prefs.show": self.cmd_weather_prefs_show,
            "weather.prefs.set": self.cmd_weather_prefs_set,
            "release.notify": self.cmd_release_notify,
            "agents.status": self.cmd_agents_status,
            "agents.start": self.cmd_agents_start,
            "dot.status": self.cmd_dot_status,
            "home.status": self.cmd_home_status,
            "home.favourites.list": self.cmd_home_favourites,
            "home.favourites.add": self.cmd_home_favourite_add,
            "home.favourites.remove": self.cmd_home_favourite_remove,
            "demo-cards": self.cmd_demo_cards,
        }.get(cmd)
        if handler is None:
            raise ControlError(f"unknown command {cmd}")
        try:
            return await handler(args)
        except LinkError as exc:
            raise ControlError(exc.msg or exc.code) from None

    async def cmd_agents_status(self, args):
        return {
            "enabled": self.agents is not None,
            "agents": await self.agents.status() if self.agents else None,
            **self.sessions.status(),
            "voice_ready": self.voice.available,
        }

    async def cmd_prefs_show(self, args):
        device = self._device(args)
        return self.sessions.get_prefs(device.device_id)

    async def cmd_prefs_set(self, args):
        device = self._device(args)
        return await self.sessions.set_prefs(device.device_id, args.get("prefs"))

    async def cmd_weather_prefs_show(self, args):
        if args:
            raise ControlError("weather preferences do not accept device arguments")
        return {"location": asdict(self.weather.provider.location) if self.weather.provider.location else None}

    async def cmd_weather_prefs_set(self, args):
        from orb_hub.weather import geocode
        from orb_hub.weather_prefs import save_location, validate_location

        if set(args) not in ({"weather"}, {"latlon"}):
            raise ControlError("give a weather city, coordinates, or off")
        async with self._weather_config_lock:
            if "weather" in args:
                city = args["weather"]
                if not isinstance(city, str) or not 1 <= len(city.strip().encode()) <= 200:
                    raise ControlError("weather city must contain 1-200 UTF-8 bytes")
                if city.strip().lower() == "off":
                    location = None
                else:
                    try:
                        location = asdict(await geocode(city.strip()))
                    except (ClientError, ValueError, OSError, TimeoutError):
                        raise ControlError("could not resolve weather location; check the city and try again") from None
            else:
                coordinates = args["latlon"]
                if not isinstance(coordinates, list) or len(coordinates) != 2:
                    raise ControlError("weather coordinates must be LAT,LON")
                latitude, longitude = coordinates
                location = {"name": f"{latitude}, {longitude}", "latitude": latitude, "longitude": longitude}
            try:
                location = validate_location(location)
            except ValueError:
                raise ControlError("invalid weather location") from None
            await asyncio.to_thread(save_location, self.cfg.data_dir, location)
            self.weather.set_location(location)
            return {"location": location}

    async def cmd_agents_start(self, args):
        if not self.agents:
            raise ControlError("start the hub with --agents first")
        from pathlib import Path

        kind, cwd, prompt = args.get("agent"), args.get("cwd"), args.get("prompt")
        if kind not in ("claude", "codex") or not isinstance(cwd, str) or not await asyncio.to_thread(Path(cwd).is_dir):
            raise ControlError("agent must be claude or codex and cwd an existing directory")
        if not isinstance(prompt, str) or not 1 <= len(prompt.encode()) <= 8192:
            raise ControlError("prompt must contain 1–8192 UTF-8 bytes")
        try:
            session = await self.agents.start_session(kind, cwd, prompt, model=args.get("model"))
        except LinkError as exc:
            raise ControlError(exc.msg or exc.code) from None
        except Exception:  # noqa: BLE001 - provider failures must not disclose payloads or secrets.
            raise ControlError("agent session could not be started") from None
        return {"session": session}

    async def cmd_dot_status(self, args):
        return {"enabled": self.dot is not None, "dot": await self.dot.status() if self.dot else None}

    async def cmd_home_status(self, args):
        return self.home.status() if self.home else {"enabled": False, "available": False}

    def _home_device(self, args):
        device = self._device(args)
        if not self.home:
            raise ControlError("start the hub with --home first")
        return device.device_id

    async def cmd_home_favourites(self, args):
        device_id = self._home_device(args)
        return self.home.favourites(device_id)

    async def cmd_home_favourite_add(self, args):
        device_id = self._home_device(args)
        return await self.home.set_favourite(device_id, args.get("id"), True)

    async def cmd_home_favourite_remove(self, args):
        device_id = self._home_device(args)
        return await self.home.set_favourite(device_id, args.get("id"), False)

    async def cmd_demo_cards(self, args):
        import time

        from orb_hub import models

        link = self._link(args)
        if "cards" not in link.caps:
            raise ControlError("device must reconnect with the cards capability")
        # Repeatable samples let the UI stream exercise fresh answers without
        # ever reusing a durable first-answer identity.
        sessions = [
            {
                "id": "demo-claude",
                "agent": "claude",
                "title": "Recipe",
                "project": "RecipeApp",
                "state": "busy",
                "reason": "",
                "summary": "Reviewing the import flow",
                "updated": time.time(),
                "can_message": False,
            },
            {
                "id": "demo-codex",
                "agent": "codex",
                "title": "Orb hub",
                "project": "Orb",
                "state": "waiting",
                "reason": "Question",
                "summary": "Choose the next check",
                "updated": time.time(),
                "can_message": False,
            },
        ]
        samples = [
            {
                "kind": "approval",
                "title": "Run tests?",
                "body": "A sample approval for visual review.",
                "detail": "Demo only: pytest hub/tests",
                "options": [
                    {"id": "allow", "label": "Allow once", "role": "primary"},
                    {"id": "deny", "label": "Deny", "role": "cancel"},
                ],
            },
            {
                "kind": "choice",
                "title": "Next check",
                "body": "Which sample should stay in focus?",
                "options": [
                    {"id": "link", "label": "Device link", "role": "default"},
                    {"id": "voice", "label": "Voice", "role": "default"},
                ],
            },
            {
                "kind": "message",
                "title": "Ready to review",
                "body": "The import preview is ready.",
                "options": [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}],
            },
            {
                "kind": "attention",
                "title": "All checks passed",
                "body": "A sample status card.",
                "options": [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}],
            },
        ]
        if len({s["id"] for s in sessions} - self.sessions.sessions.keys()) > self.sessions.available_session_slots:
            raise ControlError("not enough session capacity for demo")
        prefix = f"demo:{link.device_id}:"
        previous = [key for key in self.sessions.cards if key.startswith(prefix)]
        if len(self.sessions.cards) + self.sessions.pending_extra() - len(previous) + len(samples) > models.MAX_CARDS:
            raise ControlError("not enough card capacity for demo")

        async def answer(_answer):
            return True

        for session in sessions:
            await self.sessions.upsert_session(session)
        for key in previous:
            await self.sessions.cancel_card(key)
        batch = secrets.token_hex(4)
        shown = []
        for index, sample in enumerate(samples):
            key = f"{prefix}{batch}:{index}"
            if await self.sessions.show_card(
                {"id": key, "session": sessions[index % 2]["id"], "reply": False, "priority": "normal", **sample},
                device_id=link.device_id,
                answer_handler=answer,
            ):
                shown.append(key)
        return {"device_id": link.device_id, "cards": shown, "sessions": [s["id"] for s in sessions]}

    async def cmd_pair(self, args: dict) -> dict:
        code = str(args.get("code", ""))
        if not SAS_CODE.match(code):
            raise ControlError("the code is the 6 digits on the Orb's screen")
        try:
            pending = self.pairing.take(code)
        except LookupError as exc:
            raise ControlError(str(exc)) from None
        token = secrets.token_hex(32)
        device = self.store.pair(
            pending.device_id, pending.name, pending.model, pending.fw, token, self.cfg.default_channel
        )
        try:
            await pending.link.finish_pairing(token)
        except ConnectionError:
            raise ControlError(f"{pending.device_id} disconnected before it got its token; pair it again") from None
        log.info("paired %s (%s)", device.device_id, device.name)
        self.devlogs.note(device.device_id, f"paired from {pending.peer}")
        return {"device_id": device.device_id, "name": device.name, "channel": device.channel}

    def _device(self, args: dict) -> Device:
        device_id = args.get("device_id")
        device = self.store.get(device_id) if isinstance(device_id, str) and DEVICE_ID.match(device_id) else None
        if device is None:
            raise ControlError(f"no paired device {device_id}")
        return device

    def _link(self, args: dict) -> Link:
        device = self._device(args)
        link = self.online.get(device.device_id)
        if link is None:
            raise ControlError(f"{device.device_id} is offline")
        return link

    async def cmd_unpair(self, args: dict) -> dict:
        device = self._device(args)
        self.store.unpair(device.device_id)
        log.info("unpaired %s", device.device_id)
        link = self.online.get(device.device_id)
        if link:
            await link.close(WSCloseCode.POLICY_VIOLATION, "unpaired")
        return {"device_id": device.device_id}

    async def cmd_devices(self, args: dict) -> dict:
        return {
            "devices": [describe(d, d.device_id in self.online) for d in self.store.all()],
            "pending": self.pairing.list(),
        }

    async def cmd_identify(self, args: dict) -> dict:
        link = self._link(args)
        await link.request("device.identify")
        return {"device_id": link.device_id}

    async def cmd_dev(self, args: dict) -> dict:
        if set(args) != {"device_id", "command", "args"}:
            raise ControlError("invalid developer request")
        return await self.devtools.run(self._link(args), args["command"], args["args"])

    async def cmd_reboot(self, args: dict) -> dict:
        link = self._link(args)
        await link.request("device.reboot")
        return {"device_id": link.device_id}

    async def cmd_config(self, args: dict) -> dict:
        device = self._device(args)
        name, channel, brightness = args.get("name"), args.get("channel"), args.get("brightness")
        if name is not None and (not isinstance(name, str) or not name or len(name.encode()) > MAX_NAME_BYTES):
            raise ControlError(f"the name must be 1-{MAX_NAME_BYTES} bytes")
        if channel is not None and channel not in CHANNELS:
            raise ControlError(f"the channel must be one of {', '.join(CHANNELS)}")
        if brightness is not None and (
            not isinstance(brightness, int) or isinstance(brightness, bool) or not 0 <= brightness <= 100
        ):
            raise ControlError("brightness is 0-100")
        changes = {k: v for k, v in (("name", name), ("channel", channel), ("brightness", brightness)) if v is not None}
        if not changes:
            raise ControlError("nothing to change: give --name, --channel or --brightness")
        link = self.online.get(device.device_id)
        if brightness is not None and link is None:
            raise ControlError(f"{device.device_id} is offline; brightness can only be set while it's connected")

        # The hub owns name and channel (hello hands them out), so they change here even when the device is away.
        self.store.set_config(device.device_id, name=name, channel=channel)
        if channel is not None and channel != device.channel and link is not None:
            self.devtools.revoke(link)
        if link is None:
            return {"device_id": device.device_id, "applied": "on next connect"}
        try:
            await link.request("device.config", changes)
        except LinkError as exc:
            raise ControlError(f"saved on the hub, but the device didn't confirm: {exc.msg or exc.code}") from None
        if channel is not None:
            try:
                await link.offer_update()
            except ConnectionError:
                pass  # it gets the offer after its next hello
        return {"device_id": device.device_id, "applied": "now"}

    async def cmd_release_notify(self, args: dict) -> dict:
        """Called by `orb-hub release` after it publishes: offers the release to online devices on the channel."""
        channel = args.get("channel")
        if channel not in CHANNELS:
            raise ControlError(f"the channel must be one of {', '.join(CHANNELS)}")
        assets = args.get("kind", "app") == "assets"
        if args.get("kind", "app") not in ("app", "assets"):
            raise ControlError("kind must be app or assets")
        notified = []
        for device_id, link in list(self.online.items()):
            device = self.store.get(device_id)
            if device and device.channel == channel:
                offer = (
                    self.assets_offer_for(device, link.local_ip) if assets else self.offer_for(device, link.local_ip)
                )
                if not offer["available"]:
                    continue
                try:
                    await link.notify("ota.available", offer)
                except ConnectionError:
                    continue  # it just disconnected; it gets the offer after its next hello
                notified.append(device_id)
        log.info("release on %s offered to %s", channel, ", ".join(notified) or "no online devices")
        return {"notified": notified}

    async def maybe_speak(self, target: str, text: str, device_id: str | None = None) -> None:
        if not self.speech or not self.speech.available or not text:
            return
        if device_id is None:
            links = list(self.online.items())
        else:
            # Device-private Dot answers must stay private even after that Orb disconnects.
            links = [(device_id, self.online[device_id])] if device_id in self.online else []
        source = "dot" if target == "dot" else (self.sessions.session_snapshot(target) or {}).get("agent")
        for _, link in links:
            if link:
                link.trigger_speech(target, text, source)
