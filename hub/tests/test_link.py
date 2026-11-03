"""The device ↔ hub flow over real TLS on 127.0.0.1: pairing, hello, the link's rules."""

import asyncio
import contextlib
import hashlib
import json
import re
import ssl
import time

import aiohttp
import pytest
from fake_device import FakeDevice, paired_device

from orb_hub import cli, control, pairing, server
from orb_hub.envelope import MAX_MESSAGE
from orb_hub.pairing import MAX_PENDING
from orb_hub.store import token_hash
from orb_hub.tls import ensure_cert


async def eventually(condition, timeout: float = 3.0) -> None:
    deadline = time.monotonic() + timeout
    while not condition():
        assert time.monotonic() < deadline, "condition not met in time"
        await asyncio.sleep(0.02)


async def run_cli(*argv: str) -> int:
    # The CLI blocks on the control socket, so it runs in a thread while the hub serves on this loop.
    return await asyncio.to_thread(cli.main, list(argv))


async def test_cert_endpoint_serves_only_the_pem(hub):
    async with aiohttp.ClientSession() as session:
        base = f"http://127.0.0.1:{hub.pair_port}"
        async with session.get(f"{base}/v1/hub-cert") as resp:
            assert resp.status == 200
            assert resp.headers["Content-Type"] == "application/x-pem-file"
            assert await resp.read() == hub.cert.pem
        for path in ("/", "/v1/link", "/v1/firmware/x.bin", "/v1/hub-cert/../hub-cert/x"):
            async with session.get(base + path) as resp:
                assert resp.status == 404, path
        async with session.post(f"{base}/v1/hub-cert") as resp:
            assert resp.status == 405


async def test_link_is_pinned_to_the_hub_certificate(hub, short_tmp):
    stranger = ensure_cert(short_tmp / "other-tls", ["evil.local"], ["127.0.0.1"])
    device = FakeDevice(hub)
    try:
        with pytest.raises(aiohttp.ClientConnectorCertificateError):
            await device.connect(pem=stranger.pem)
    finally:
        await device.aclose()


async def test_advertised_host_outlives_a_rename(make_hub, monkeypatch):
    # macOS renames the Mac after a Bonjour conflict ("Demo-Mac-2"), but the certificate keeps the names it was
    # made with, and devices check it against the SRV host name.
    monkeypatch.setattr(server, "local_host_name", lambda: "Demo-Mac")
    first = await make_hub()
    assert first.mdns_host == "orb-hub-demo-mac.local"
    await first.stop()

    monkeypatch.setattr(server, "local_host_name", lambda: "Demo-Mac-2")
    hub = await make_hub()
    assert hub.cert.der == first.cert.der
    assert hub.mdns_host == "orb-hub-demo-mac.local"
    # What orb_link does: connect to the address, check the certificate against the advertised host.
    ctx = ssl.create_default_context(cadata=hub.cert.pem.decode())
    _, writer = await asyncio.open_connection("127.0.0.1", hub.tls_port, ssl=ctx, server_hostname=hub.mdns_host)
    writer.close()


class FakeAdvertiser:
    def __init__(self):
        self.updates: list[list[str]] = []

    async def update(self, addresses: list[str]) -> None:
        self.updates.append(addresses)

    async def stop(self) -> None:
        pass


async def test_mdns_addresses_follow_the_lan(make_hub, monkeypatch):
    addresses: list[str] = []
    monkeypatch.setattr(server, "reachable_addresses", lambda bind: list(addresses))
    hub = await make_hub(address_poll=0.05)
    assert hub.addresses == []
    hub._advertiser = advertiser = FakeAdvertiser()
    # Wi-Fi joins after the hub started, then DHCP hands out another address.
    addresses.append("127.0.0.10")
    await eventually(lambda: advertiser.updates == [["127.0.0.10"]])
    addresses[0] = "127.0.0.23"
    await eventually(lambda: advertiser.updates[-1] == ["127.0.0.23"])
    assert hub.addresses == ["127.0.0.23"] and len(advertiser.updates) == 2


async def test_full_pairing_flow(hub, capsys):
    device = FakeDevice(hub)
    try:
        pem = await device.fetch_cert()
        await device.connect()
        assert hashlib.sha256(device.peer_cert_der()).digest() == hub.cert.fingerprint

        res = await device.pair_begin()
        assert res["ok"], res
        assert re.fullmatch(r"[0-9a-f]{32}", res["r"]["nonce_h"])
        assert res["r"]["hub_name"] == "Test Hub"
        assert re.fullmatch(r"\d{6}", device.sas)
        # Both sides computed the same code: the hub holds the request under it.
        assert hub.pairing._pending[device.device_id].sas == device.sas

        # `orb-hub devices` lists the request but never its code.
        assert await run_cli("devices", "--json") == 0
        listing = json.loads(capsys.readouterr().out)
        assert [p["device_id"] for p in listing["pending"]] == [device.device_id]
        assert device.sas not in json.dumps(listing)

        assert await run_cli("pair", device.sas) == 0
        assert "Paired Orb (orb-020000abcdef), dev channel." in capsys.readouterr().out
        name, params = await device.event()
        assert name == "pair.done"
        assert re.fullmatch(r"[0-9a-f]{64}", params["token"])
        assert await device.closed() == 1000
        device.token = params["token"]

        record = hub.store.get(device.device_id)
        assert record.token_sha256 == token_hash(device.token) != device.token
        assert device.token not in hub.cfg.db_path.read_bytes().decode(errors="replace")

        # Reconnect with the pinned PEM and say hello.
        await device.connect(pem=pem)
        res = await device.hello()
        assert res["ok"], res
        r = res["r"]
        assert r["hub_name"] == "Test Hub"
        assert r["hub_version"] == "0.1.0"
        assert abs(r["time_ms"] - time.time() * 1000) < 5000
        assert r["tz"] == "America/New_York"
        assert r.get("tz_posix") == "EST5EDT,M3.2.0,M11.1.0"
        assert r["device"] == {"name": "Orb", "channel": "dev"}
        assert device.device_id in hub.online

        assert await run_cli("devices") == 0
        out = capsys.readouterr().out
        assert "orb-020000abcdef" in out and "online" in out and "87%" in out and "-54 dBm" in out
    finally:
        await device.aclose()


async def test_wrong_code_changes_nothing(hub, capsys):
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        await device.pair_begin()
        wrong = f"{(int(device.sas) + 1) % 1_000_000:06d}"
        assert await run_cli("pair", wrong) == 1
        assert "no device is waiting with code" in capsys.readouterr().err
        assert await run_cli("pair", "12ab56") == 1
        assert hub.store.get(device.device_id) is None
        assert device.device_id in hub.pairing._pending
        assert device.events.empty()
        # The right code still works afterwards.
        assert await run_cli("pair", device.sas) == 0
        assert (await device.event())[0] == "pair.done"
    finally:
        await device.aclose()


async def test_pending_request_expires(make_hub, capsys):
    hub = await make_hub(pair_ttl=0.3)
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        await device.pair_begin()
        assert await device.event() == ("pair.failed", {"reason": "expired"})
        assert await device.closed() == 1000
        assert hub.pairing.list() == []
        assert await run_cli("pair", device.sas) == 1
        assert "no device is waiting" in capsys.readouterr().err
        assert hub.store.get(device.device_id) is None
    finally:
        await device.aclose()


async def test_retrying_replaces_the_pending_request(hub):
    first, second = FakeDevice(hub), FakeDevice(hub)
    try:
        for d in (first, second):
            await d.fetch_cert()
            await d.connect()
            await d.pair_begin()
        assert await first.event() == ("pair.failed", {"reason": "rejected"})
        with pytest.raises(control.ControlError):
            await hub.handle_command("pair", {"code": first.sas})
        await hub.handle_command("pair", {"code": second.sas})
        assert (await second.event())[0] == "pair.done"
    finally:
        await first.aclose()
        await second.aclose()


async def test_pending_requests_are_capped(hub):
    devices = [FakeDevice(hub, device_id=f"orb-00000000000{i:x}") for i in range(MAX_PENDING + 1)]
    try:
        for d in devices:
            await d.fetch_cert()
            await d.connect()
            if d is not devices[-1]:
                assert (await d.pair_begin())["ok"] and d.revealed["ok"]
        res = await devices[-1].pair_begin()
        assert res["e"]["code"] == "busy"
        assert await devices[-1].closed() == 1013
        # A device already waiting may still retry.
        await devices[0].connect()
        assert (await devices[0].pair_begin())["ok"]
    finally:
        for d in devices:
            await d.aclose()


async def test_the_code_exists_only_after_the_reveal(hub):
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        assert (await device.pair_begin(reveal=False))["ok"]
        # The hub has committed to nonce_h without knowing nonce_d: nothing is pending, there is no code yet.
        assert hub.pairing.list() == []
        res = await device.request("pair.reveal", {"nonce_d": device.nonce_d.hex()})
        assert res == {"t": "res", "id": 2, "ok": True, "r": {}}
        assert [p["device_id"] for p in hub.pairing.list()] == [device.device_id]
    finally:
        await device.aclose()


async def test_reveal_must_match_the_commitment(hub):
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        assert (await device.pair_begin(reveal=False))["ok"]
        res = await device.request("pair.reveal", {"nonce_d": "00" * 16})
        assert res["e"]["code"] == "bad_request"
        assert await device.closed() == 1008
        assert hub.pairing.list() == []
    finally:
        await device.aclose()


@pytest.mark.parametrize(
    "second", [("ota.check", {}), ("hello", {"device_id": "orb-020000abcdef"}), ("pair.begin", {})]
)
async def test_pair_begin_must_be_followed_by_pair_reveal(hub, second):
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        assert (await device.pair_begin(reveal=False))["ok"]
        res = await device.request(*second)
        assert res["e"]["code"] == "unauthorized"
        assert await device.closed() == 1008
        assert hub.pairing.list() == []
    finally:
        await device.aclose()


async def test_pairing_attempts_are_limited(hub, monkeypatch):
    monkeypatch.setattr(pairing, "MAX_ATTEMPTS", 3)
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        for _ in range(3):
            await device.connect()
            assert (await device.pair_begin())["ok"]
        await device.connect()
        res = await device.pair_begin()
        assert res["e"]["code"] == "busy"
        assert await device.closed() == 1013
        # Attempts age out of the window.
        monkeypatch.setattr(pairing, "ATTEMPT_WINDOW", 0.0)
        await device.connect()
        assert (await device.pair_begin())["ok"] and device.revealed["ok"]
    finally:
        await device.aclose()


@pytest.mark.parametrize("stage", ["nothing", "pair.begin"])
async def test_pings_dont_buy_time_to_pair_or_say_hello(make_hub, stage):
    hub = await make_hub(handshake_timeout=0.5, idle_timeout=5.0)
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        started = time.monotonic()  # the deadline starts once the hub has the connection, after this
        await device.connect()
        if stage == "pair.begin":
            assert (await device.pair_begin(reveal=False))["ok"]
        while not device.ws.closed and time.monotonic() - started < 3:
            with contextlib.suppress(ConnectionError):
                await device.ws.ping()
            await asyncio.sleep(0.1)
        assert await device.closed() == 1001
        assert 0.5 <= time.monotonic() - started < 2.5
        await eventually(lambda: not hub._links)
    finally:
        await device.aclose()


async def test_connections_still_connecting_are_capped(hub, monkeypatch):
    # The paired device's link is past its hello, so it doesn't count.
    device = await paired_device(hub)
    waiting = [FakeDevice(hub) for _ in range(server.MAX_HANDSHAKES_PER_PEER)]
    extra = FakeDevice(hub)
    try:
        for d in waiting:
            await d.connect(pem=device.pem)
        with pytest.raises(aiohttp.WSServerHandshakeError) as refused:
            await extra.connect(pem=device.pem)
        assert refused.value.status == 503
        await waiting[0].disconnect()
        await eventually(lambda: sum(link.in_handshake for link in hub._links) < len(waiting))
        await extra.connect(pem=device.pem)

        # Besides the cap per address, one counts them all.
        monkeypatch.setattr(server, "MAX_HANDSHAKES_PER_PEER", 99)
        monkeypatch.setattr(server, "MAX_HANDSHAKES", len(waiting))
        with pytest.raises(aiohttp.WSServerHandshakeError):
            await waiting[0].connect(pem=device.pem)
    finally:
        for d in (device, extra, *waiting):
            await d.aclose()


async def test_a_connection_that_sends_no_request_is_closed(make_hub):
    hub = await make_hub(handshake_timeout=0.3)
    tls = ssl.create_default_context(cadata=hub.cert.pem.decode())
    # Plain HTTP, stalled in the TLS handshake, and past it: aiohttp and asyncio would wait an hour or a minute.
    for port, ctx in ((hub.pair_port, None), (hub.tls_port, None), (hub.tls_port, tls)):
        reader, writer = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)
        try:
            assert await asyncio.wait_for(reader.read(), 3) == b""
        finally:
            writer.close()


@pytest.mark.parametrize(
    "first",
    [
        {"t": "req", "id": 1, "m": "ota.check", "p": {}},
        {"t": "evt", "m": "state.report", "p": {}},
        "not json",
        b"\x00binary",
    ],
)
async def test_anything_but_pair_begin_or_hello_first_is_unauthorized(hub, first):
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        if isinstance(first, dict) and first["t"] == "req":
            res = await device.request(first["m"], first["p"])
            assert res == {"t": "res", "id": 1, "ok": False, "e": {"code": "unauthorized", "msg": res["e"]["msg"]}}
        elif isinstance(first, dict):
            await device.send(first)
        elif isinstance(first, str):
            await device.ws.send_str(first)
        else:
            await device.ws.send_bytes(first)
        assert await device.closed() == 1008
    finally:
        await device.aclose()


async def test_wrong_token_and_unknown_device_are_unauthorized(hub):
    device = await paired_device(hub)
    try:
        await device.disconnect()
        near_miss = device.token[:-1] + ("1" if device.token[-1] != "1" else "2")
        for kwargs in ({"token": "00" * 32}, {"token": near_miss}, {"device_id": "orb-000000000000"}):
            await device.connect()
            res = await device.hello(**kwargs)
            assert not res["ok"] and res["e"]["code"] == "unauthorized", kwargs
            assert await device.closed() == 1008
        # The right token still works.
        await device.connect()
        assert (await device.hello())["ok"]
    finally:
        await device.aclose()


async def test_bad_pair_begin_is_refused(hub):
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        res = await device.request("pair.begin", {"device_id": "orb-020000ABCDEF", "name": "Orb", "nonce_d": "00" * 16})
        assert res["e"]["code"] == "bad_request"
        assert await device.closed() == 1008
    finally:
        await device.aclose()


async def test_unpair_sends_the_device_back_to_pairing(hub, capsys):
    device = await paired_device(hub)
    try:
        assert await run_cli("unpair", device.device_id) == 0
        assert await device.closed() == 1008
        assert hub.store.get(device.device_id) is None
        await device.connect()
        res = await device.hello()
        assert res["e"]["code"] == "unauthorized"
        assert await run_cli("unpair", device.device_id) == 1
    finally:
        await device.aclose()


async def test_requests_events_and_errors_on_the_link(hub):
    device = await paired_device(hub)
    try:
        res = await device.request("no.such.method")
        assert res["e"]["code"] == "unknown_method"
        res = await device.request("hello", {})
        assert res["e"]["code"] == "bad_request"
        res = await device.raw_request({"t": "req", "id": 77, "m": "ota.check", "p": "not an object"})
        assert res["e"]["code"] == "bad_request"
        # Unknown events and binary frames are ignored; the link stays up.
        await device.send({"t": "evt", "m": "future.thing", "p": {"x": 1}})
        await device.ws.send_bytes(b"\x01\x02")
        assert (await device.request("ota.check"))["r"] == {"available": False}
    finally:
        await device.aclose()


async def test_state_report_and_log_batch(hub):
    device = await paired_device(hub)
    try:
        state = {"power": {"battery_pct": 41, "charging": True, "usb": True}, "rssi": -60, "heap_internal": 78000}
        await device.send({"t": "evt", "m": "state.report", "p": state})
        lines = ["\x1b[0;32mI (1234) main: hello\x1b[0m", "W (1300) net: weak signal"]
        await device.send({"t": "evt", "m": "log.batch", "p": {"lines": lines}})
        await device.request("ota.check")  # the hub handles messages in order, so both events are in by now

        assert hub.store.get(device.device_id).state == state
        log_text = hub.devlogs.path(device.device_id).read_text()
        assert "I (1234) main: hello" in log_text and "\x1b" not in log_text
        assert "W (1300) net: weak signal" in log_text
        listing = await hub.handle_command("devices", {})
        assert listing["devices"][0]["battery_pct"] == 41 and listing["devices"][0]["charging"] is True
    finally:
        await device.aclose()


async def test_identify_reboot_and_config_reach_the_device(hub, capsys):
    device = await paired_device(hub)
    try:
        assert await run_cli("identify", device.device_id) == 0
        assert (await device.hub_requests.get())["m"] == "device.identify"
        assert await run_cli("reboot", device.device_id) == 0
        assert (await device.hub_requests.get())["m"] == "device.reboot"
        argv = ("config", device.device_id, "--name", "Desk", "--channel", "stable", "--brightness", "40")
        assert await run_cli(*argv) == 0
        req = await device.hub_requests.get()
        assert req["m"] == "device.config"
        assert req["p"] == {"name": "Desk", "channel": "stable", "brightness": 40}
        record = hub.store.get(device.device_id)
        assert (record.name, record.channel) == ("Desk", "stable")

        assert await run_cli("config", device.device_id, "--brightness", "101") == 1
        await device.disconnect()
        await asyncio.sleep(0.1)
        assert await run_cli("identify", device.device_id) == 1
        assert "offline" in capsys.readouterr().err
        assert await run_cli("config", device.device_id, "--brightness", "10") == 1
        assert await run_cli("config", device.device_id, "--name", "Kitchen") == 0
        assert "on next connect" in capsys.readouterr().out
        await device.connect()
        assert (await device.hello())["r"]["device"] == {"name": "Kitchen", "channel": "stable"}
    finally:
        await device.aclose()


async def test_a_new_connection_replaces_the_old_one(hub):
    device = await paired_device(hub)
    twin = FakeDevice(hub)
    try:
        twin.pem, twin.token = device.pem, device.token
        await twin.connect()
        assert (await twin.hello())["ok"]
        assert await device.closed() == 1001
        assert hub.online[device.device_id].ws is not None
        assert (await twin.request("ota.check"))["ok"]
    finally:
        await device.aclose()
        await twin.aclose()


async def test_message_size_limit(hub):
    device = await paired_device(hub)
    try:
        envelope = {"t": "evt", "m": "pad", "p": {"x": ""}}
        exact = json.dumps(envelope, separators=(",", ":"))
        envelope["p"]["x"] = "a" * (MAX_MESSAGE - len(exact))
        await device.ws.send_str(json.dumps(envelope, separators=(",", ":")))
        assert (await device.request("ota.check"))["ok"]  # exactly 16 KB is fine

        envelope["p"]["x"] += "a"
        await device.ws.send_str(json.dumps(envelope, separators=(",", ":")))
        assert await device.closed() == 1009
    finally:
        await device.aclose()


async def test_idle_link_is_closed_but_pings_keep_it_open(make_hub):
    hub = await make_hub(idle_timeout=0.6)
    device = await paired_device(hub)
    try:
        for _ in range(6):
            await asyncio.sleep(0.2)
            await device.ws.ping()
        assert not device.ws.closed
        started = time.monotonic()
        assert await device.closed(timeout=5) == 1001
        assert time.monotonic() - started < 3
        await eventually(lambda: device.device_id not in hub.online)
    finally:
        await device.aclose()


async def test_a_silent_first_connection_is_closed(make_hub):
    hub = await make_hub(idle_timeout=0.3)
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        assert await device.closed() == 1001
    finally:
        await device.aclose()
