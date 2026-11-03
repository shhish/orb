"""A Python stand-in for the Orb firmware's orb_link: real TLS on 127.0.0.1, pinned to the PEM it fetched."""

import asyncio
import hashlib
import json
import secrets
import ssl

import aiohttp

from orb_hub.pairing import compute_sas


class FakeDevice:
    def __init__(self, hub, device_id: str = "orb-020000abcdef", name: str = "Orb"):
        self.hub = hub
        self.device_id = device_id
        self.name = name
        self.pem: bytes | None = None
        self.token: str | None = None
        self.nonce_d: bytes | None = None
        self.revealed: dict | None = None
        self.sas: str | None = None
        self.session = aiohttp.ClientSession()
        self.ws: aiohttp.ClientWebSocketResponse | None = None
        self.events: asyncio.Queue = asyncio.Queue()
        self.hub_requests: asyncio.Queue = asyncio.Queue()
        self._waiting: dict[int, asyncio.Future] = {}
        self._next_id = 1
        self._reader: asyncio.Task | None = None

    @property
    def base(self) -> str:
        return f"https://127.0.0.1:{self.hub.tls_port}"

    async def fetch_cert(self) -> bytes:
        async with self.session.get(f"http://127.0.0.1:{self.hub.pair_port}/v1/hub-cert") as resp:
            assert resp.status == 200
            assert resp.content_type == "application/x-pem-file"
            self.pem = await resp.read()
        return self.pem

    def ssl_context(self, pem: bytes | None = None) -> ssl.SSLContext:
        # Like esp-tls given one PEM: the hub's certificate is the only trusted root.
        return ssl.create_default_context(cadata=(pem or self.pem).decode())

    async def connect(self, pem: bytes | None = None) -> None:
        self.ws = await self.session.ws_connect(
            f"wss://127.0.0.1:{self.hub.tls_port}/v1/link", ssl=self.ssl_context(pem), autoping=True, heartbeat=None
        )
        self._reader = asyncio.create_task(self._read())

    def peer_cert_der(self) -> bytes:
        return self.ws.get_extra_info("ssl_object").getpeercert(binary_form=True)

    async def _read(self) -> None:
        async for msg in self.ws:
            if msg.type is not aiohttp.WSMsgType.TEXT:
                continue
            obj = json.loads(msg.data)
            if obj["t"] == "res":
                future = self._waiting.pop(obj["id"], None)
                if future and not future.done():
                    future.set_result(obj)
            elif obj["t"] == "evt":
                await self.events.put((obj["m"], obj["p"]))
            else:
                await self.hub_requests.put(obj)
                result = {"uptime_s": 12} if obj["m"] == "device.ping" else {}
                await self.ws.send_str(json.dumps({"t": "res", "id": obj["id"], "ok": True, "r": result}))

    async def request(self, method: str, params: dict | None = None, timeout: float = 5.0) -> dict:
        req_id = self._next_id
        self._next_id += 1
        future = asyncio.get_running_loop().create_future()
        self._waiting[req_id] = future
        await self.send({"t": "req", "id": req_id, "m": method, "p": params or {}})
        return await asyncio.wait_for(future, timeout)

    async def send(self, obj: dict) -> None:
        await self.ws.send_str(json.dumps(obj))

    async def raw_request(self, obj: dict, timeout: float = 5.0) -> dict:
        """Sends obj as it is and waits for the response to its id."""
        future = asyncio.get_running_loop().create_future()
        self._waiting[obj["id"]] = future
        await self.send(obj)
        return await asyncio.wait_for(future, timeout)

    async def event(self, timeout: float = 5.0) -> tuple[str, dict]:
        return await asyncio.wait_for(self.events.get(), timeout)

    async def closed(self, timeout: float = 5.0) -> int:
        """Waits for the hub to close the link; returns the close code."""
        await asyncio.wait_for(asyncio.shield(self._reader), timeout)
        return self.ws.close_code

    async def pair_begin(self, reveal: bool = True) -> dict:
        """pair.begin with a commitment to a fresh nonce_d; once the hub answers, pair.reveal (unless reveal is
        False) and the code. Returns pair.begin's response; pair.reveal's is in self.revealed."""
        self.nonce_d = secrets.token_bytes(16)
        res = await self.request(
            "pair.begin",
            {
                "device_id": self.device_id,
                "name": self.name,
                "model": "waveshare-s3-amoled-1.75c",
                "fw": "0.2.0",
                "commit_d": hashlib.sha256(self.nonce_d).hexdigest(),
            },
        )
        if res["ok"] and reveal:
            self.revealed = await self.request("pair.reveal", {"nonce_d": self.nonce_d.hex()})
            if self.revealed["ok"]:
                # The device hashes the certificate it actually connected with.
                cert_fp = hashlib.sha256(self.peer_cert_der()).digest()
                self.sas = compute_sas(cert_fp, self.nonce_d, bytes.fromhex(res["r"]["nonce_h"]))
        return res

    async def hello(self, token: str | None = None, **extra) -> dict:
        params = {
            "device_id": self.device_id,
            "token": token or self.token,
            "fw": "0.2.0",
            "build": "d7b73f7",
            "idf": "v6.1",
            "boot_slot": "ota_0",
            "reset_reason": "poweron",
            "uptime_s": 12,
            "power": {"battery_pct": 87, "charging": False, "usb": True},
            "rssi": -54,
        }
        params.update(extra)
        return await self.request("hello", params)

    async def disconnect(self) -> None:
        if self.ws is not None and not self.ws.closed:
            await self.ws.close()
        if self._reader:
            await asyncio.gather(self._reader, return_exceptions=True)

    async def aclose(self) -> None:
        await self.disconnect()
        await self.session.close()


async def paired_device(hub, **kwargs) -> FakeDevice:
    """A device that has gone through pairing and is back online after hello."""
    device = FakeDevice(hub, **kwargs)
    await device.fetch_cert()
    await device.connect()
    assert (await device.pair_begin())["ok"] and device.revealed["ok"]
    await hub.handle_command("pair", {"code": device.sas})
    name, params = await device.event()
    assert name == "pair.done"
    device.token = params["token"]
    await device.closed()
    await device.connect()
    assert (await device.hello())["ok"]
    return device
