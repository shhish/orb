"""HomeRelay adapter. Credentials and HomeKit identities never cross the device link."""

import asyncio
import contextlib
import hashlib
import json
import math
import os
import re
import sqlite3
import stat
import time
import uuid
from datetime import datetime
from functools import partial
from pathlib import Path
from urllib.parse import quote, urlsplit

import aiohttp
from zeroconf import ServiceStateChange
from zeroconf.asyncio import AsyncServiceBrowser, AsyncServiceInfo, AsyncZeroconf

from orb_hub import envelope, models
from orb_hub.config import ensure_private_dir
from orb_hub.envelope import LinkError
from orb_hub.lifecycle import LifecycleSupervisor, finish_shutdown
from orb_hub.sessions import encoded

DEFAULT_URL = "http://127.0.0.1:18796/api/v1"
SERVICE = "_homerelay._tcp.local."
MAX_RELAY_BYTES = 4 * 1024 * 1024
RANGES = {
    "brightness": (0, 100),
    "hue": (0, 360),
    "sat": (0, 100),
    "ct": (50, 1000),
    "temp": (-100, 100),
    "target": (-100, 100),
    "mode": (0, 3),
    "humidity": (0, 100),
    "speed": (0, 100),
    "position": (0, 100),
    "battery": (0, 100),
}
BOOLS = {"on", "motion", "contact", "locked", "low_battery"}
STATE_KEYS = BOOLS | RANGES.keys()
WRITE_KEYS = {"on", "brightness", "hue", "sat", "ct", "target", "mode", "locked", "speed", "position"}
# Read current state, never infer physical lock/cover position from a target.
READ_KEYS = {
    "on": "on",
    "active": "on",
    "brightness": "brightness",
    "hue": "hue",
    "saturation": "sat",
    "colorTemperature": "ct",
    "currentTemperature": "temp",
    "targetTemperature": "target",
    "targetHeatingCooling": "mode",
    "humidity": "humidity",
    "motion": "motion",
    "occupancy": "motion",
    "contact": "contact",
    "lockState": "locked",
    "rotationSpeed": "speed",
    "position": "position",
    "batteryLevel": "battery",
    "lowBattery": "low_battery",
}
WRITE_NAMES = {
    "on": "on",
    "active": "on",
    "brightness": "brightness",
    "hue": "hue",
    "saturation": "sat",
    "colorTemperature": "ct",
    "targetTemperature": "target",
    "targetHeatingCooling": "mode",
    "lockTarget": "locked",
    "rotationSpeed": "speed",
    "targetPosition": "position",
}
KINDS = {"lightbulb": "light", "doorLock": "lock", "windowCovering": "cover", "window": "cover", "door": "cover"}
SECURITY_COVERS = {"window", "door"}
KINDS.update({key: key for key in ("switch", "outlet", "fan", "thermostat", "sensor", "doorbell", "button")})
DEFAULT_ROOMS = {"living room", "bedroom", "kitchen", "office"}
EXCLUDED_ROOMS = {"admin", "server room"}
DEFAULT_SCENES = {"good morning", "evening", "good night", "i'm leaving", "i'm home"}


def alias(prefix: str, key: str) -> str:
    return prefix + hashlib.sha256(key.encode()).hexdigest()[:12]


def clipped(value, limit=32):
    return str(value or "").encode("utf-8", errors="replace")[:limit].decode("utf-8", errors="ignore")


def event_observation(event):
    """Only an aware observation can identify a press, order an event or supply local time."""
    try:
        observed = datetime.fromisoformat(event.get("ts"))
        if observed.tzinfo is not None:
            return observed.astimezone()
    except (TypeError, ValueError, OverflowError, OSError):
        pass
    return None


def valid_value(key, value):
    if key in BOOLS:
        return isinstance(value, bool)
    if key not in RANGES or isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return (
            math.isfinite(value) and RANGES[key][0] <= value <= RANGES[key][1] and (key != "mode" or type(value) is int)
        )
    except OverflowError:
        return False


def compact_state(raw, *, order=()):
    result = {}
    for relay_key in dict.fromkeys((*order, *READ_KEYS)):
        key = READ_KEYS[relay_key]
        if relay_key not in raw or key in result:
            continue
        value = raw[relay_key]
        if key in BOOLS and type(value) is int and value in (0, 1):
            value = bool(value)
        if valid_value(key, value):
            result[key] = value
    return {key: result[key] for key in dict.fromkeys(READ_KEYS.values()) if key in result}


def state_source(raw, key, order):
    """Resolve aliases such as primary fan active versus secondary switch on."""
    return next(
        (
            relay_key
            for relay_key in dict.fromkeys((*order, *READ_KEYS))
            if READ_KEYS[relay_key] == key and key in compact_state({relay_key: raw.get(relay_key)})
        ),
        None,
    )


async def discover_url():
    """A short Bonjour browse, cleaned up even when shutdown interrupts discovery."""
    zc, browser = None, None
    found = asyncio.Event()
    names = set()

    def changed(zeroconf, service_type, name, state_change):
        if state_change is ServiceStateChange.Added:
            names.add(name)
            found.set()

    try:
        zc = AsyncZeroconf()
        browser = AsyncServiceBrowser(zc.zeroconf, SERVICE, handlers=[changed])
        async with asyncio.timeout(2):
            await found.wait()
            for name in sorted(names):
                info = AsyncServiceInfo(SERVICE, name)
                if await info.async_request(zc.zeroconf, 700):
                    addresses = info.parsed_scoped_addresses()
                    if addresses:
                        host = addresses[0]
                        host = f"[{host}]" if ":" in host else host
                        api = info.properties.get(b"api", b"/api/v1")
                        if not isinstance(api, bytes):
                            continue
                        path = api.decode("utf-8")
                        if (
                            path.startswith("/")
                            and not path.startswith("//")
                            and not any(c in path for c in ("?", "#", "\r", "\n"))
                        ):
                            return f"http://{host}:{info.port}{path}"
    except (TimeoutError, OSError, UnicodeError, ValueError):
        pass
    finally:
        try:
            if browser:
                await browser.async_cancel()
        finally:
            if zc:
                await zc.async_close()
    return DEFAULT_URL


class HomeBridge:
    def __init__(self, sessions, data_dir: Path, *, url=None, reconnect_delay=1.0, stream_timeout=45.0):
        self.sessions = sessions
        self.directory = ensure_private_dir(data_dir / "home")
        self.token_path = self.directory / "homerelay.token"
        path = self.directory / "home.db"
        self._db_path = path
        self._db_lock = asyncio.Lock()
        self.supervisor = LifecycleSupervisor("home")
        self.db = sqlite3.connect(path, check_same_thread=False)
        path.chmod(0o600)
        # WAL lets on-loop reads proceed while a worker-thread commit holds the write lock.
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute(
            "CREATE TABLE IF NOT EXISTS favourites(device TEXT, id TEXT, enabled INTEGER, PRIMARY KEY(device,id))"
        )
        self.url = url
        self._explicit_url = url
        self.reconnect_delay = reconnect_delay
        self.stream_timeout = stream_timeout
        self.available = False
        self.error = "starting"
        self.accessories = {}
        self.rooms = {}
        self.scenes = {}
        self._raw_ids = {}
        self._write_names = {}
        self._subscribers = {}
        self._client = None
        self._tasks = []
        self._locks = {}
        self._write_slots = asyncio.Semaphore(8)
        self._write_events = {}
        self._last_doorbell = {}

    @classmethod
    async def open(cls, sessions, data_dir: Path, **options):
        # Schema creation can fsync too. Production opens away from the loop;
        # the synchronous constructor remains for existing embedded callers.
        future = asyncio.get_running_loop().run_in_executor(None, partial(cls, sessions, data_dir, **options))
        cancelled = False
        while True:
            try:
                instance = await asyncio.shield(future)
                break
            except asyncio.CancelledError:
                cancelled = True
        if cancelled:
            await instance.stop()
            raise asyncio.CancelledError
        return instance

    async def start(self):
        self._client = aiohttp.ClientSession(
            trust_env=False, timeout=aiohttp.ClientTimeout(total=8), read_bufsize=65536
        )
        self._tasks = [
            self.supervisor.run(self._run, name="orb-home-relay"),
            self.supervisor.run(self._coalesce, name="orb-home-updates"),
        ]

    async def stop(self):
        await finish_shutdown(self._stop(), name="orb-home-stop")

    async def _stop(self):
        for task in self._tasks:
            task.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks.clear()
        await self.supervisor.stop()
        self._subscribers.clear()
        if self._client:
            await self._client.close()
            self._client = None
        async with self._db_lock:
            self.db.close()

    def _token(self):
        try:
            # Reject links and loose permissions; never alter an owner's credential file.
            info = self.token_path.lstat()
            if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o600 or info.st_size > 4096:
                raise LinkError("unauthorized", "Home credential requires a private file")
            token = self.token_path.read_text().strip()
            if not token or any(c.isspace() or ord(c) < 33 or ord(c) > 126 for c in token):
                raise LinkError("unauthorized", "Home credential is invalid")
            return token
        except OSError:
            raise LinkError("unauthorized", "Home credential is unavailable") from None

    @staticmethod
    def _validate_url(value):
        try:
            parsed = urlsplit(value)
            if (
                parsed.scheme not in ("http", "https")
                or not parsed.hostname
                or parsed.username
                or parsed.password
                or parsed.query
                or parsed.fragment
            ):
                raise ValueError
            _ = parsed.port
        except (ValueError, TypeError):
            raise LinkError("unsupported", "Home relay address is invalid") from None
        return value.rstrip("/")

    @staticmethod
    def _http_error(status):
        code = {
            401: "unauthorized",
            403: "unauthorized",
            404: "not_found",
            409: "conflict",
            422: "bad_request",
            429: "busy",
            503: "unsupported",
            504: "timeout",
        }.get(status, "internal")
        return LinkError(code, "Home relay could not complete the request")

    async def _json(self, method, path, *, values=None):
        try:
            token = await asyncio.to_thread(self._token)
            async with self._client.request(
                method,
                self.url + path,
                json=values,
                headers={"Authorization": f"Bearer {token}"},
                allow_redirects=False,
            ) as response:
                if response.status != 200:
                    raise self._http_error(response.status)
                data = bytearray()
                async for chunk in response.content.iter_chunked(65536):
                    data.extend(chunk)
                    if len(data) > MAX_RELAY_BYTES:
                        raise LinkError("too_large", "Home relay response exceeds its budget")
                return json.loads(data)
        except TimeoutError:
            raise LinkError("timeout", "Home relay request timed out") from None
        except (aiohttp.ClientError, OSError, ValueError, UnicodeError, RecursionError):
            raise LinkError("internal", "Home relay request failed") from None

    def _overrides(self, device_id):
        return dict(self.db.execute("SELECT id,enabled FROM favourites WHERE device=?", (device_id,)))

    def _favourite(self, item, overrides):
        return bool(overrides.get(item["id"], item["_default"]))

    def snapshot(self, device_id):
        overrides = self._overrides(device_id)
        eligible = [a for a in self.accessories.values() if not a["_excluded"] or overrides.get(a["id"]) == 1]
        eligible.sort(key=lambda a: (not self._favourite(a, overrides), a["id"]))
        selected, room_ids = [], set()
        for item in eligible:
            if item["room"] not in room_ids and len(room_ids) >= 16:
                continue
            room_ids.add(item["room"])
            selected.append(
                {k: v for k, v in item.items() if not k.startswith("_")}
                | {
                    "state": dict(item["state"]),
                    "favourite": self._favourite(item, overrides),
                    "reachable": self.available and item["reachable"],
                }
            )
            if len(selected) == 64:
                break
        scenes = sorted(self.scenes.values(), key=lambda s: (not self._favourite(s, overrides), s["id"]))[:32]
        return {
            "rooms": [self.rooms[key] for key in sorted(room_ids)],
            "accessories": selected,
            "scenes": [{"id": s["id"], "name": s["name"], "favourite": self._favourite(s, overrides)} for s in scenes],
            "available": self.available,
        }

    def status(self):
        return {
            "enabled": True,
            "available": self.available,
            "error": self.error,
            "workers": self.supervisor.health(),
            "accessories": [
                {
                    "id": a["id"],
                    "name": a["name"],
                    "room": self.rooms[a["room"]]["name"],
                    "kind": a["kind"],
                    "excluded": a["_excluded"],
                }
                for a in self.accessories.values()
            ],
            "scenes": [{"id": s["id"], "name": s["name"]} for s in self.scenes.values()],
        }

    def favourites(self, device_id):
        overrides = self._overrides(device_id)
        return {
            "device_id": device_id,
            "favourites": [
                {"id": a["id"], "name": a["name"], "type": kind}
                for kind, items in (("accessory", self.accessories), ("scene", self.scenes))
                for a in items.values()
                if self._favourite(a, overrides) and (not a.get("_excluded") or overrides.get(a["id"]) == 1)
            ],
        }

    def _save_favourite(self, device_id, key, enabled):
        with contextlib.closing(sqlite3.connect(self._db_path)) as db, db:
            db.execute("INSERT OR REPLACE INTO favourites VALUES (?,?,?)", (device_id, key, enabled))

    async def set_favourite(self, device_id, key, enabled):
        models.text(key, 16)
        if key not in self.accessories and key not in self.scenes:
            raise LinkError("not_found", "unknown Home object")
        async with self._db_lock:
            future = asyncio.get_running_loop().run_in_executor(None, self._save_favourite, device_id, key, enabled)
            cancelled = False
            while True:
                try:
                    await asyncio.shield(future)
                    break
                except asyncio.CancelledError:
                    # Repeated shutdown cancellation still cannot interrupt a
                    # native commit or release its lifetime lock prematurely.
                    cancelled = True
            if cancelled:
                raise asyncio.CancelledError
        for link in self._subscribers:
            if link.device_id == device_id:
                await self._refresh(link, force=True)
        return self.favourites(device_id)

    async def subscribe(self, link):
        self.sessions.attach(link)
        self._subscribers[link] = None
        await self._refresh(link, force=True)

    def unsubscribe(self, link):
        self._subscribers.pop(link, None)

    async def _snapshot(self, link, value):
        chunks = []
        chunk = {
            "rooms": [],
            "accessories": [],
            "scenes": [],
            "available": value["available"],
            "reset": False,
            "more": False,
        }
        for field in ("rooms", "accessories", "scenes"):
            for item in value[field]:
                chunk[field].append(item)
                if len(encoded(envelope.event("home.snapshot", chunk)).encode()) > models.MAX_EXTENSION_MESSAGE:
                    chunk[field].pop()
                    chunks.append(chunk)
                    chunk = {
                        "rooms": [],
                        "accessories": [],
                        "scenes": [],
                        "available": value["available"],
                        "reset": False,
                        "more": False,
                    }
                    chunk[field].append(item)
        chunks.append(chunk)
        for i, part in enumerate(chunks):
            part["reset"] = i == 0
            part["more"] = i < len(chunks) - 1
            await self.sessions.emit(link, "home.snapshot", part)

    async def _refresh(self, link, *, force=False):
        value = self.snapshot(link.device_id)
        old = self._subscribers.get(link)

        def structure(snapshot):
            return {
                **snapshot,
                "accessories": [
                    {k: v for k, v in a.items() if k not in ("state", "reachable")} for a in snapshot["accessories"]
                ],
            }

        if force or old is None or structure(value) != structure(old):
            await self._snapshot(link, value)
        else:
            before = {a["id"]: a for a in old["accessories"]}
            for item in value["accessories"]:
                previous = before[item["id"]]
                # Removing an unknown/transitional field requires a replacement snapshot.
                if previous["state"].keys() - item["state"].keys():
                    await self._snapshot(link, value)
                    break
                patch = {"id": item["id"]}
                changed = {
                    key: v
                    for key, v in item["state"].items()
                    if previous["state"].get(key) != v or key not in previous["state"]
                }
                if changed:
                    patch["state"] = changed
                if item["reachable"] != previous["reachable"]:
                    patch["reachable"] = item["reachable"]
                if len(patch) > 1:
                    await self.sessions.emit(link, "home.update", patch)
        self._subscribers[link] = value

    async def _coalesce(self):
        while True:
            await asyncio.sleep(0.25)
            for link in list(self._subscribers):
                await self._refresh(link)

    def _load(self, homes, *, started_at=None):
        if not isinstance(homes, list) or len(homes) > 64:
            raise LinkError("internal", "Home relay snapshot is invalid")
        accessories, rooms, scenes, raw_ids, write_names = {}, {}, {}, {}, {}

        def identify(prefix, raw):
            key = alias(prefix, raw)
            if key in raw_ids and raw_ids[key] != raw:
                raise LinkError("conflict", "Home identity collision")
            raw_ids[key] = raw
            return key

        for home in homes:
            home_id = home["id"]
            by_name = {}
            for room in home["rooms"]:
                key = identify("r", home_id + ":" + room["id"])
                rooms[key] = {"id": key, "name": clipped(room["name"])}
                by_name[room["name"]] = key
            for raw in home["accessories"]:
                if raw["category"] in ("bridge", "camera"):
                    continue
                key = identify("a", raw["id"])
                room_name = raw.get("room") or "Other"
                room = by_name.get(room_name)
                if room is None:
                    room = identify("r", home_id + ":unassigned:" + room_name)
                    rooms[room] = {"id": room, "name": clipped(room_name)}
                kind = KINDS.get(raw["category"], "other")
                names, read_sources, read_order = {}, {}, []
                services = sorted(raw.get("services", []), key=lambda s: not s.get("primary", False))
                for service in services:
                    if service.get("type") == "accessoryInformation" or service.get("type", "").startswith("custom."):
                        continue
                    for characteristic in service.get("characteristics", []):
                        relay_key = characteristic.get("type")
                        if relay_key in READ_KEYS:
                            if relay_key not in read_order:
                                read_order.append(relay_key)
                            read_sources.setdefault(relay_key, []).append(
                                {
                                    "id": characteristic.get("id"),
                                    "service": service.get("id"),
                                    "value": characteristic.get("value"),
                                }
                            )
                        wire_key = WRITE_NAMES.get(relay_key)
                        if wire_key in WRITE_KEYS and characteristic.get("writable") is True and wire_key not in names:
                            names[wire_key] = relay_key
                if raw["category"] in SECURITY_COVERS:
                    # Doors and windows are physical security: Orb shows their
                    # position but only Apple Home opens them. Blinds stay writable.
                    names.pop("position", None)
                write_names[key] = names
                excluded = room_name.casefold() in EXCLUDED_ROOMS
                accessories[key] = {
                    "id": key,
                    "name": clipped(raw["name"]),
                    "room": room,
                    "kind": kind,
                    "reachable": raw.get("reachable") is True,
                    "state": compact_state(raw["state"], order=read_order),
                    "_relay_state": {k: v for k, v in raw["state"].items() if k in READ_KEYS},
                    "_read_sources": read_sources,
                    "_read_order": read_order,
                    "writable": sorted(names),
                    "_default": not excluded
                    and (
                        kind in ("thermostat", "lock", "fan")
                        or kind in ("light", "switch")
                        and room_name.casefold() in DEFAULT_ROOMS
                    ),
                    "_excluded": excluded,
                    "_snapshot_at": started_at,
                }
            for raw in home["scenes"]:
                # Scene actions are usually unavailable, but explicit safety
                # metadata must never be discarded when a relay supplies it.
                if re.fullmatch(r"Trigger\d+", raw["name"]) or raw.get("contains_unlock", False) is not False:
                    continue
                key = identify("s", raw["id"])
                scenes[key] = {
                    "id": key,
                    "name": clipped(raw["name"]),
                    "_default": raw["name"].casefold().replace("’", "'") in DEFAULT_SCENES,
                }
        if len(accessories) > 1024 or len(rooms) > 1024 or len(scenes) > 1024:
            raise LinkError("too_large", "Home relay inventory exceeds its budget")
        self.accessories, self.rooms, self.scenes = accessories, rooms, scenes
        self._raw_ids, self._write_names = raw_ids, write_names
        # Topology churn must not retain every doorbell ever seen by this hub.
        self._last_doorbell = {key: value for key, value in self._last_doorbell.items() if key in accessories}

    async def _reload(self):
        started_at = time.time()
        homes = await self._json("GET", "/snapshot")
        try:
            self._load(homes, started_at=started_at)
        except (KeyError, TypeError, AttributeError, ValueError, RecursionError):
            raise LinkError("internal", "Home relay snapshot is invalid") from None

    async def _run(self):
        delay = self.reconnect_delay
        while True:
            try:
                if self.url is None:
                    self.url = self._validate_url(os.environ.get("HOMERELAY_URL") or await discover_url())
                else:
                    self.url = self._validate_url(self.url)
                token = await asyncio.to_thread(self._token)
                timeout = aiohttp.ClientTimeout(total=None, sock_connect=8, sock_read=self.stream_timeout)
                async with self._client.get(
                    self.url + "/events",
                    headers={"Authorization": f"Bearer {token}"},
                    allow_redirects=False,
                    timeout=timeout,
                ) as response:
                    if response.status != 200:
                        raise self._http_error(response.status)
                    if response.content_type != "text/event-stream":
                        raise LinkError("internal", "Home relay event stream is invalid")
                    # Establish notifications first; a fresh snapshot repairs every reconnect gap.
                    await self._reload()
                    self.available, self.error = True, None
                    delay = self.reconnect_delay
                    for link in list(self._subscribers):
                        await self._refresh(link, force=True)
                    data, event_type = [], ""
                    size = 0
                    while True:
                        line = await response.content.readline()
                        if not line:
                            raise LinkError("unsupported", "Home relay disconnected")
                        size += len(line)
                        if size > 65536:
                            raise LinkError("too_large", "Home relay event exceeds its budget")
                        line = line.decode("utf-8").rstrip("\r\n")
                        if not line:
                            if data:
                                value = json.loads("\n".join(data))
                                await self._event(event_type, value)
                            data, event_type, size = [], "", 0
                        elif line.startswith(":"):
                            size = 0 if not data else size
                        elif line.startswith("data:"):
                            data.append(line[5:].removeprefix(" "))
                        elif line.startswith("event:"):
                            event_type = line[6:].strip()
            except LinkError as exc:
                self.error = exc.code
            except (
                aiohttp.ClientError,
                OSError,
                ValueError,
                UnicodeError,
                TimeoutError,
                KeyError,
                TypeError,
                RecursionError,
            ):
                self.error = "internal"
            self.available = False
            for link in list(self._subscribers):
                await self._refresh(link, force=True)
            # A discovered address can change after a relay restart or DHCP
            # renewal. Only explicit owner configuration pins an address.
            if self._explicit_url is None and not os.environ.get("HOMERELAY_URL"):
                self.url = None
            await asyncio.sleep(delay)
            delay = min(max(delay * 2, self.reconnect_delay), 30)

    async def _event(self, event_type, value):
        if not isinstance(value, dict):
            raise LinkError("internal", "Home relay event is invalid")
        kind = value.get("type", event_type)
        if kind == "homes":
            await self._reload()
            return
        raw_id = value.get("accessoryId")
        if not isinstance(raw_id, str):
            return
        item = self.accessories.get(alias("a", raw_id))
        if item is None:
            return
        if kind == "reachability" and type(value.get("reachable")) is bool:
            item["reachable"] = value["reachable"]
        elif kind == "characteristic":
            characteristic = value.get("characteristic")
            if (
                item["kind"] == "doorbell"
                and characteristic == "buttonEvent"
                and type(value.get("value")) is int
                and value["value"] in (0, 1, 2)
            ):
                await self._doorbell(item, value)
            # /events opens before /snapshot is sampled, so a state observation
            # older than the snapshot is already reflected in it and must not
            # undo it. Presses are not state and always reach _doorbell above.
            observed, snapshot_at = event_observation(value), item.get("_snapshot_at")
            if observed and snapshot_at and observed.timestamp() < snapshot_at:
                return
            if characteristic in READ_KEYS:
                wire_key = READ_KEYS[characteristic]
                previous = state_source(item["_relay_state"], wire_key, item["_read_order"])
                reported = value.get("value")
                sources = item["_read_sources"].get(characteristic, [])
                if sources:
                    selected = next((s for s in sources if s["value"] is not None), None)
                    identity = {
                        k: value[v] for k, v in (("id", "characteristicId"), ("service", "serviceId")) if v in value
                    }
                    source = (
                        next((s for s in sources if all(s[k] == v for k, v in identity.items())), None)
                        if identity
                        else selected or sources[0]
                    )
                    if source is None:
                        return
                    source["value"] = reported
                    current = next((s for s in sources if s["value"] is not None), None)
                    # Match HomeRelay's primary-first snapshot summary. An
                    # unrelated secondary service must not replace its value,
                    # or supersede an in-flight primary write's accepted state.
                    if source is not selected and source is not current:
                        return
                    reported = current["value"] if current else None
                item["_relay_state"][characteristic] = reported
                item["state"] = compact_state(item["_relay_state"], order=item["_read_order"])
                current = state_source(item["_relay_state"], wire_key, item["_read_order"])
                if item["id"] in self._write_events and characteristic in (previous, current):
                    self._write_events[item["id"]].add(wire_key)

    async def _doorbell(self, item, event):
        observed = event_observation(event)
        identity = (observed, event.get("characteristicId"), event.get("value"))
        # A characteristic ID identifies a button, not an individual press.
        # Without a timestamp two identical values are distinct interactions.
        if observed is not None:
            if self._last_doorbell.get(item["id"]) == identity:
                return
            self._last_doorbell[item["id"]] = identity
        targets = [None]
        if item["_excluded"]:
            # The room exclusion also applies to attention. Explicitly pinned
            # recipients retain their card across a device reconnect.
            targets = [
                row[0]
                for row in self.db.execute("SELECT device FROM favourites WHERE id=? AND enabled=1", (item["id"],))
                if any(a["id"] == item["id"] for a in self.snapshot(row[0])["accessories"])
            ]
        for device_id in targets:
            try:
                await self.sessions.show_card(
                    {
                        "id": "home-doorbell-" + uuid.uuid4().hex,
                        "kind": "attention",
                        "title": "Front Door",
                        "body": "Doorbell rang at "
                        + (observed.strftime("%H:%M") if observed is not None else time.strftime("%H:%M")),
                        "priority": "high",
                        "reply": False,
                        "options": [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}],
                        "expires": time.time() + 120,
                    },
                    source="home",
                    device_id=device_id,
                )
            except LinkError as exc:
                if exc.code != "busy":
                    raise
                break

    async def request(self, device_id, method, params):
        if method not in ("home.set", "home.toggle", "home.scene"):
            raise LinkError("unknown_method", "unknown Home method")
        models.fields(
            params,
            {"id", "values"} if method == "home.set" else {"id"},
            {"id", "values"} if method == "home.set" else {"id"},
        )
        key = models.text(params["id"], 16)
        view = self.snapshot(device_id)
        items = {a["id"]: a for a in view["scenes" if method == "home.scene" else "accessories"]}
        if key not in items:
            raise LinkError("not_found", "unknown Home object")
        item = items[key]
        values = None
        if method == "home.set":
            values = params["values"]
            models.fields(values, STATE_KEYS)
            if not values or any(not valid_value(k, v) for k, v in values.items()):
                raise LinkError("bad_request", "invalid Home values")
            if values.keys() - set(item["writable"]):
                raise LinkError("unsupported", "Home control is read-only")
            values = dict(values)
        elif method == "home.toggle" and (item["kind"] == "lock" or "on" not in item["writable"]):
            raise LinkError("unsupported", "Home accessory does not support toggling")
        if not self.available or method != "home.scene" and not item["reachable"]:
            raise LinkError("unsupported", "Home accessory is unavailable")
        raw_id = self._raw_ids[key]
        path = (
            f"/scenes/{quote(raw_id, safe='')}/run"
            if method == "home.scene"
            else f"/accessories/{quote(raw_id, safe='')}" + ("/toggle" if method == "home.toggle" else "")
        )
        lock = self._locks.setdefault(key, asyncio.Lock())
        events = None
        try:
            # Queue at the accessory lock before the shared slot. Otherwise a
            # slot waiter can retain an orphaned lock after its predecessor
            # removes it, allowing two later commands to reach one accessory.
            async with asyncio.timeout(8), lock, self._write_slots:
                # State may have gone offline while another command held the lock.
                if not self.available or raw_id != self._raw_ids.get(key):
                    raise LinkError("unsupported", "Home accessory is unavailable")
                current = self.snapshot(device_id)
                current_items = {a["id"]: a for a in current["scenes" if method == "home.scene" else "accessories"]}
                if key not in current_items:
                    raise LinkError("not_found", "unknown Home object")
                if method != "home.scene" and not current_items[key]["reachable"]:
                    raise LinkError("unsupported", "Home accessory is unavailable")
                if method == "home.set":
                    if values.keys() - set(current_items[key]["writable"]):
                        raise LinkError("unsupported", "Home control is read-only")
                    names = self._write_names[key]
                    values = {
                        names[k]: int(v) if names[k] in ("active", "lockTarget") else v for k, v in values.items()
                    }
                elif method == "home.toggle" and (
                    current_items[key]["kind"] == "lock" or "on" not in current_items[key]["writable"]
                ):
                    raise LinkError("unsupported", "Home accessory does not support toggling")
                current_item = self.accessories.get(key)
                if method != "home.scene":
                    events = set()
                    self._write_events[key] = events
                result = await self._json("POST", path, values=values)
                if not isinstance(result, dict) or result.get("id") != raw_id:
                    raise LinkError("internal", "Home relay returned an invalid write result")
                if method == "home.scene":
                    return {"id": key, "accepted": True}
                if not isinstance(result.get("state"), dict):
                    raise LinkError("internal", "Home relay returned an invalid write result")
                state = compact_state(result["state"], order=current_item["_read_order"])
                if self.accessories.get(key) is current_item:
                    # A response can arrive after newer SSE fields, including
                    # a transitional lock state that deliberately omits locked.
                    # Return the actual accepted response, but never publish it
                    # as a stale follow-up update from our shared cache. A fresh
                    # snapshot replaces the item and supersedes this reply too.
                    reported = {k: v for k, v in result["state"].items() if k in READ_KEYS}
                    for relay_key, wire_key in READ_KEYS.items():
                        if wire_key in events:
                            reported.pop(relay_key, None)
                            if relay_key in current_item["_relay_state"]:
                                reported[relay_key] = current_item["_relay_state"][relay_key]
                    current_item["state"] = compact_state(reported, order=current_item["_read_order"])
                    current_item["_relay_state"] = reported
                    for relay_key, sources in current_item["_read_sources"].items():
                        if READ_KEYS[relay_key] in events:
                            continue
                        if relay_key not in reported:
                            for source in sources:
                                source["value"] = None
                        else:
                            source = next((s for s in sources if s["value"] is not None), sources[0])
                            source["value"] = reported[relay_key]
                return {"id": key, "state": state}
        except TimeoutError:
            raise LinkError("timeout", "Home relay request timed out") from None
        finally:
            if events is not None and self._write_events.get(key) is events:
                self._write_events.pop(key)
            if not lock.locked() and not getattr(lock, "_waiters", None):
                self._locks.pop(key, None)
