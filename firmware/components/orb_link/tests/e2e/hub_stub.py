"""End-to-end test of orb_link against a stand-in hub (docs/protocol.md).

Runs the linux-target build of the test app (the real orb_link.c) as a child
process and plays the hub to it on 127.0.0.1: the certificate on the pairing
port, then the pinned-TLS WebSocket. The script walks the device through
pairing (a PEM with a second certificate, refused; an expired attempt; then a
confirmed one, each with pair.begin's commitment and pair.reveal), hello,
every kind of request, event and failure, a hub with the wrong certificate,
an unauthorized hello that makes it pair again, and finally device.reboot.

The hub's key and certificate are throwaway ones made in a temporary
directory for this run.

Run it with ESP-IDF's Python, which has websockets and cryptography:
    $IDF_PYTHON_ENV_PATH/bin/python hub_stub.py build/orb_link_e2e.elf
"""

import argparse
import asyncio
import datetime
import hashlib
import ipaddress
import json
import os
import re
import secrets
import ssl
import sys
import tempfile

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from websockets.asyncio.server import serve

OFFLINE, SEARCHING, PAIRING, CONNECTING, ONLINE, UNAUTHORIZED = range(6)
STEP_TIMEOUT = 15


class Failure(Exception):
    pass


def ok(what):
    print(f"ok   {what}")


def check(cond, what):
    if not cond:
        raise Failure(what)
    ok(what)


def make_cert(directory):
    """A throwaway self-signed P-256 certificate, as the hub makes for itself.

    It names the SRV host orb_net reports, as the hub's does, and an IP the hub no longer has, as after a new DHCP
    lease: the device reaches it at 127.0.0.1 and must check the name, not the address.
    """
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "orb-hub")])
    now = datetime.datetime.now(datetime.timezone.utc)
    san = x509.SubjectAlternativeName(
        [
            x509.DNSName("orb-hub-e2e.local"),
            x509.IPAddress(ipaddress.ip_address("192.0.2.7")),
        ]
    )
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(minutes=5))
        .not_valid_after(now + datetime.timedelta(days=1))
        .add_extension(san, critical=False)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )
    cert_path = os.path.join(directory, "hub.pem")
    key_path = os.path.join(directory, "hub.key")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(
            key.private_bytes(
                serialization.Encoding.PEM,
                serialization.PrivateFormat.PKCS8,
                serialization.NoEncryption(),
            )
        )
    fp = hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).digest()
    return cert_path, key_path, fp


def sas(cert_fp, nonce_d, nonce_h):
    digest = hashlib.sha256(b"orb-pair-v1" + cert_fp + nonce_d + nonce_h).digest()
    return f"{int.from_bytes(digest[:4], 'big') % 1000000:06d}"


class Device:
    """The device process and its "E2E ..." lines."""

    def __init__(self):
        self.lines = []
        self.seen = 0
        self.changed = asyncio.Condition()
        self.proc = None

    async def start(self, elf, env):
        self.proc = await asyncio.create_subprocess_exec(
            elf,
            env=env,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        asyncio.create_task(self._read())

    async def _read(self):
        async for raw in self.proc.stdout:
            line = raw.decode(errors="replace").rstrip()
            print(f"  device| {line}")
            async with self.changed:
                self.lines.append(line)
                self.changed.notify_all()

    async def expect(self, pattern, timeout=STEP_TIMEOUT):
        """Waits for a line matching `pattern` after the last one matched."""
        regex = re.compile(pattern)

        def found():
            for i in range(self.seen, len(self.lines)):
                m = regex.search(self.lines[i])
                if m:
                    self.seen = i + 1
                    return m
            return None

        async with self.changed:
            m = await asyncio.wait_for(self.changed.wait_for(found), timeout)
        return m

    def stop(self):
        if self.proc and self.proc.returncode is None:
            self.proc.kill()


class Link:
    """One WebSocket connection, from the hub's side."""

    def __init__(self, ws):
        self.ws = ws
        self.next_id = 1
        self.backlog = []

    async def recv(self, timeout=STEP_TIMEOUT):
        if self.backlog:
            return self.backlog.pop(0)
        return json.loads(await asyncio.wait_for(self.ws.recv(), timeout))

    async def send(self, msg):
        await self.ws.send(json.dumps(msg))

    async def request(self, method, params=None, fragments=0):
        """Sends a request (split into fragments if asked) and returns its
        response; anything else that arrives meanwhile is kept."""
        rid = self.next_id
        self.next_id += 1
        text = json.dumps(
            {"t": "req", "id": rid, "m": method, "p": {} if params is None else params}
        )
        if fragments:
            cuts = list(range(0, len(text), len(text) // fragments + 1)) + [len(text)]
            await self.ws.send([text[a:b] for a, b in zip(cuts, cuts[1:])])
        else:
            await self.ws.send(text)
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), STEP_TIMEOUT))
            if m.get("t") == "res" and m.get("id") == rid:
                return m
            self.backlog.append(m)

    async def expect(self, kind, name, timeout=STEP_TIMEOUT):
        """The next message of this kind and name, keeping the others."""
        for i, m in enumerate(self.backlog):
            if m.get("t") == kind and m.get("m") == name:
                return self.backlog.pop(i)
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), timeout))
            if m.get("t") == kind and m.get("m") == name:
                return m
            self.backlog.append(m)


class Hub:
    def __init__(self, directory):
        self.cert_path, self.key_path, self.fp = make_cert(directory)
        with open(self.cert_path, "rb") as f:
            self.cert_pem = f.read()
        self.served_pem = self.cert_pem
        self.tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.tls.load_cert_chain(self.cert_path, self.key_path)
        self.links = asyncio.Queue()
        self.port = 0

    def serve_second_cert(self, directory):
        """Serve the real certificate followed by another, as a man in the middle would to get its own trusted
        while the pairing code still covers the real one."""
        cert_path, _, _ = make_cert(directory)
        with open(cert_path, "rb") as f:
            self.served_pem = self.cert_pem + f.read()

    def impersonate(self, directory):
        """From the next handshake on, present another certificate, as a man in the middle would."""
        cert_path, key_path, _ = make_cert(directory)
        self.tls.load_cert_chain(cert_path, key_path)

    def restore(self):
        self.tls.load_cert_chain(self.cert_path, self.key_path)

    async def serve_cert(self, reader, writer):
        request = await reader.readline()
        while (await reader.readline()).strip():
            pass
        if request.split()[1:2] == [b"/v1/hub-cert"]:
            head = "HTTP/1.1 200 OK\r\nContent-Type: application/x-pem-file\r\n"
            body = self.served_pem
        else:
            head, body = "HTTP/1.1 404 Not Found\r\n", b""
        head += f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n"
        writer.write(head.encode() + body)
        await writer.drain()
        writer.close()

    async def serve_link(self, ws):
        if ws.request.path != "/v1/link":
            await ws.close(4404)
            return
        await self.links.put(Link(ws))
        await ws.wait_closed()

    async def next_link(self):
        return await asyncio.wait_for(self.links.get(), 40)


async def pair(hub, device, outcome, token=None):
    """One pairing connection; outcome is "expired" or "done". Returns nonce_d."""
    link = await hub.next_link()
    first = await link.recv()
    p = first.get("p", {})
    check(
        first.get("t") == "req" and first.get("m") == "pair.begin",
        "first message is pair.begin",
    )
    check(p.get("device_id") == "orb-e2e000000000", "pair.begin device_id")
    check(
        p.get("name") == "Orb"
        and p.get("model") == "waveshare-s3-amoled-1.75c"
        and p.get("fw") == "0.2.1",
        "pair.begin name, model, fw",
    )
    commit_d = p.get("commit_d", "")
    check(
        re.fullmatch(r"[0-9a-f]{64}", commit_d) is not None and "nonce_d" not in p,
        "pair.begin commits to nonce_d without revealing it",
    )
    nonce_h = secrets.token_bytes(16)
    await link.send(
        {
            "t": "res",
            "id": first["id"],
            "ok": True,
            "r": {"nonce_h": nonce_h.hex(), "hub_name": "E2E hub"},
        }
    )
    reveal = await link.recv()
    check(
        reveal.get("t") == "req" and reveal.get("m") == "pair.reveal",
        "second message is pair.reveal",
    )
    nonce_d = reveal.get("p", {}).get("nonce_d", "")
    check(
        re.fullmatch(r"[0-9a-f]{32}", nonce_d) is not None
        and hashlib.sha256(bytes.fromhex(nonce_d)).hexdigest() == commit_d,
        "pair.reveal's nonce_d matches commit_d",
    )
    await link.send({"t": "res", "id": reveal["id"], "ok": True, "r": {}})
    code = sas(hub.fp, bytes.fromhex(nonce_d), nonce_h)
    m = await device.expect(rf"E2E state={PAIRING} paired=0 sas=(\d{{6}}) hub=E2E hub")
    check(m.group(1) == code, f"device shows the hub's code {code}")
    if outcome == "expired":
        await link.send({"t": "evt", "m": "pair.failed", "p": {"reason": "expired"}})
    else:
        await link.send({"t": "evt", "m": "pair.done", "p": {"token": token}})
        await device.expect(r"E2E state=\d paired=1")
    await asyncio.wait_for(link.ws.wait_closed(), STEP_TIMEOUT)
    ok(f"device closed after pairing {outcome}")
    return nonce_d


async def hello(hub, device, token, answer):
    link = await hub.next_link()
    first = await link.recv()
    p = first.get("p", {})
    kind = (first.get("t"), first.get("m"))
    check(kind == ("req", "hello"), "first message is hello")
    check(p.get("token") == token, "hello carries the token from pair.done")
    build = p.get("build", "")
    check(
        p.get("fw") == "0.2.1" and re.fullmatch(r"[0-9a-f]{7}", build) is not None,
        "hello fw is the version, build 7 hex digits",
    )
    check(
        p.get("power")
        == {
            "battery_pct": 87,
            "battery_mv": 4010,
            "display_state": "always_on",
            "charging": True,
            "usb": True,
        },
        "hello power from the provider",
    )
    check(
        p.get("rssi") == -50 and p.get("last_ota", {}).get("result") == "ok",
        "hello rssi and last_ota",
    )
    check(
        all(k in p for k in ("fw", "idf", "boot_slot", "reset_reason", "uptime_s")),
        "hello has the other fields",
    )
    await link.send({"t": "res", "id": first["id"], **answer})
    return link


async def scenario(hub, device, tmp):
    await device.expect(r"link: 127\.0\.0\.1 didn't serve exactly one certificate")
    check(
        hub.links.empty(),
        "a PEM with a second certificate is refused before connecting",
    )
    hub.served_pem = hub.cert_pem

    first_nonce = await pair(hub, device, "expired")
    token = secrets.token_hex(32)
    check(
        await pair(hub, device, "done", token) != first_nonce,
        "a new attempt has new nonces",
    )

    result = {
        "hub_name": "E2E hub",
        "hub_version": "0.1.0",
        "time_ms": 1793050000000,
        "tz": "America/New_York",
        "device": {"name": "Desk Orb", "channel": "dev"},
    }
    link = await hello(hub, device, token, {"ok": True, "r": result})
    await device.expect(r"E2E tz=America/New_York")
    await device.expect(rf"E2E state={ONLINE} paired=1 .* name=Desk Orb channel=dev")
    # Although the certificate doesn't name 127.0.0.1: the device checks the host name.
    ok("online with the hub's name, channel and time zone")
    m = await device.expect(r"E2E https url=(\S+) auth=(.+) pem_sha256=([0-9a-f]{64})")
    url, auth, pem_sha256 = m.groups()
    check(
        url == f"https://127.0.0.1:{hub.port}" and auth == f"Bearer {token}",
        "orb_link_hub_https gives the hub's address and the token",
    )
    check(
        pem_sha256 == hashlib.sha256(hub.cert_pem).hexdigest(),
        "orb_link_hub_https gives the pinned certificate alone",
    )
    batch = await link.expect("evt", "log.batch")
    check(batch["p"] == {}, "event from the app arrives")

    r = await link.request("device.ping")
    check(
        r["ok"] and isinstance(r["r"].get("uptime_s"), int),
        "device.ping answers uptime_s",
    )
    r = await link.request("no.such.method")
    check(
        not r["ok"] and r["e"]["code"] == "unknown_method",
        "unknown method gets unknown_method",
    )
    r = await link.request("device.config", {"name": 5})
    check(
        not r["ok"] and r["e"]["code"] == "bad_request",
        "ESP_ERR_INVALID_ARG maps to bad_request",
    )
    r = await link.request("device.config", {"name": "Desk Orb"})
    check(r["ok"] and r["r"] == {}, "app method answers {}")
    r = await link.request("device.ping", [1])
    check(
        not r["ok"] and r["e"]["code"] == "bad_request",
        "params that aren't an object get bad_request",
    )

    await link.send({"t": "evt", "m": "unknown.event", "p": {}})
    await link.send({"t": "evt", "m": "test.event", "p": {"x": 1}})
    await device.expect(r'E2E event=test.event params=\{"x":1\}')
    ok("events reach their handler; unknown ones are ignored")

    big = {"blob": "z" * 10000}
    await link.send({"t": "evt", "m": "test.event", "p": big})
    await device.expect(r'E2E event=test.event params=\{"blob":"z{10000}"\}')
    ok("a 10 KB message arrives whole across buffer-sized reads")
    await link.send({"t": "evt", "m": "test.event", "p": {"blob": "y" * 17000}})
    r = await link.request("device.ping", fragments=3)
    check(
        r["ok"],
        "a fragmented message is reassembled; one over 16 KB is dropped without harm",
    )

    # 900 levels: cJSON alone would take it, recursing far past the device's 8 KB link task stack.
    deep = '{"t":"evt","m":"test.event","p":{"a":' + "[" * 900 + "]" * 900 + "}}"
    await link.ws.send(deep)
    await device.expect(r"link: dropped a message that isn't a valid envelope")
    r = await link.request("device.ping")
    check(
        r["ok"] and not any('params={"a":[[' in line for line in device.lines),
        "a message nested too deep is dropped unparsed",
    )

    # device.identify makes the app send ota.check (answered) and slow.method (left to time out).
    r = await link.request("device.identify")
    check(r["ok"], "device.identify answered")
    check_req = await link.expect("req", "ota.check")
    slow = await link.expect("req", "slow.method")
    check(check_req["id"] != slow["id"], "device numbers its requests")
    offer = {"available": False}
    await link.send({"t": "res", "id": check_req["id"], "ok": True, "r": offer})
    await device.expect(r'E2E reply=ESP_OK body=\{"available":false\}')
    await device.expect(r"E2E reply=ESP_ERR_TIMEOUT body=null", timeout=5)
    ok("replies reach the caller; an unanswered request times out")

    await link.ws.close()
    await device.expect(rf"E2E state={SEARCHING}")
    ok("a dropped link goes back to searching")

    mitm = os.path.join(tmp, "mitm")
    os.mkdir(mitm)
    hub.impersonate(mitm)
    await device.expect(r"link: couldn't reach wss://127\.0\.0\.1")
    check(hub.links.empty(), "a hub with another certificate is refused (pinned TLS)")
    hub.restore()

    link = await hello(
        hub,
        device,
        token,
        {"ok": False, "e": {"code": "unauthorized", "msg": "unknown device"}},
    )
    await device.expect(rf"E2E state={UNAUTHORIZED} paired=0")
    ok("unauthorized hello forgets the pairing")
    await asyncio.wait_for(link.ws.wait_closed(), STEP_TIMEOUT)

    token = secrets.token_hex(32)
    await pair(hub, device, "done", token)
    link = await hello(hub, device, token, {"ok": True, "r": result})
    await device.expect(rf"E2E state={ONLINE}")
    r = await link.request("device.reboot")
    check(r["ok"] and r["r"] == {}, "device.reboot answers before rebooting")
    await asyncio.wait_for(link.ws.wait_closed(), STEP_TIMEOUT)
    await asyncio.wait_for(device.proc.wait(), STEP_TIMEOUT)
    await device.expect(r"restart triggered")
    ok("device restarted after the response went out")


async def main(elf):
    with tempfile.TemporaryDirectory() as tmp:
        hub = Hub(tmp)
        device = Device()
        cert_server = await asyncio.start_server(hub.serve_cert, "127.0.0.1", 0)
        async with serve(hub.serve_link, "127.0.0.1", 0, ssl=hub.tls) as link_server:
            hub.port = link_server.sockets[0].getsockname()[1]
            extra = os.path.join(tmp, "extra")
            os.mkdir(extra)
            hub.serve_second_cert(extra)
            env = dict(
                os.environ,
                ORB_E2E_PORT=str(hub.port),
                ORB_E2E_PAIR_PORT=str(cert_server.sockets[0].getsockname()[1]),
                ORB_E2E_FP=hub.fp.hex()[:16],
            )
            await device.start(elf, env)
            try:
                await asyncio.wait_for(scenario(hub, device, tmp), 180)
            finally:
                device.stop()
                cert_server.close()
                await cert_server.wait_closed()
    print("PASS")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", help="the linux build of tests/e2e")
    try:
        asyncio.run(main(parser.parse_args().elf))
    except (Failure, asyncio.TimeoutError) as e:
        print(f"FAIL {type(e).__name__}: {e}")
        sys.exit(1)
