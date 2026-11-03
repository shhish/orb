"""Firmware images, signing, releases, OTA offers and downloads."""

import asyncio
import hashlib
import json
import logging
import ssl
import struct
import subprocess
import sys
import zlib

import aiohttp
import pytest
from conftest import elf_sha256, make_image
from fake_device import FakeDevice, paired_device

from orb_hub import cli, firmware
from orb_hub.firmware import FirmwareError, Releases, is_current, make_offer, read_app_info, signature_blocks
from orb_hub.signing import SigningError, ensure_signed, private_key


def modulus(pem: bytes) -> int:
    return private_key(pem).public_key().public_numbers().n


def signers(image: bytes) -> list[int | None]:
    """Each signature block's RSA modulus by position, None for an empty or invalid slot."""
    return [block and block.rsa_modulus for block in signature_blocks(image)]


# -- the image itself


def test_read_app_info():
    info = read_app_info(make_image(version="0.2.1+e1f2a3b"))
    assert info.project == "orb"
    # What the firmware's hello reports: the descriptor's version whole, and esp_app_get_elf_sha256(buf, 8).
    assert info.version == "0.2.1+e1f2a3b"
    assert info.build == elf_sha256("0.2.1+e1f2a3b").hex()[:7]
    assert info.built == "Oct 26 2026 18:09:41"
    assert info.idf == "v6.1"
    assert read_app_info(make_image(elf_sha=bytes(32))).build == ""


@pytest.mark.parametrize(
    "image, message",
    [
        (b"\xe9" + bytes(100), "not an ESP-IDF app image"),
        (b"\x00" + make_image()[1:], "not an ESP-IDF app image"),
        (make_image(chip_id=0), "not the ESP32-S3"),
        (make_image(desc_magic=0x12345678), "no app descriptor"),
        (make_image(version=""), "no version"),
    ],
)
def test_read_app_info_rejects(image, message):
    with pytest.raises(FirmwareError, match=message):
        read_app_info(image)


def test_unsigned_image_has_no_signature_blocks():
    image = make_image()
    assert signature_blocks(image) == [None, None, None]
    assert signature_blocks(image + b"\xff" * (4096 - len(image) % 4096)) == [None, None, None]


# -- signing (real espsecure, throwaway RSA-3072 keys)


def test_sign_and_verify(signing_pem, short_tmp):
    image = make_image()
    signed = ensure_signed(image, signing_pem, short_tmp)
    assert len(signed) % 4096 == 0
    assert signed.startswith(image)
    assert signers(signed) == [modulus(signing_pem), None, None]
    # Already signed with the key: kept as it is (and verified again).
    assert ensure_signed(signed, signing_pem, short_tmp) == signed
    assert [p.name for p in short_tmp.iterdir() if p.name.startswith(".sign-")] == []


def test_image_signed_with_another_key_is_refused(signing_pem, other_pem, short_tmp):
    foreign = ensure_signed(make_image(), other_pem, short_tmp)
    with pytest.raises(SigningError, match="different key"):
        ensure_signed(foreign, signing_pem, short_tmp)


def append_signature(image: bytes, pem: bytes, tmp) -> bytes:
    """espsecure sign-data --append-signatures: another block after the image's existing ones."""
    (tmp / "TEST-ONLY-append-key.pem").write_bytes(pem)
    (tmp / "in.bin").write_bytes(image)
    subprocess.run(
        [sys.executable, "-m", "espsecure", "sign-data", "--version", "2", "--append-signatures"]
        + ["--keyfile", str(tmp / "TEST-ONLY-append-key.pem"), "--output", str(tmp / "out.bin"), str(tmp / "in.bin")],
        check=True,
        capture_output=True,
    )
    return (tmp / "out.bin").read_bytes()


def test_only_the_first_signature_block_counts(signing_pem, other_pem, short_tmp):
    # ESP-IDF checks just the first block of an update; espsecure verify-signature accepts any of them.
    foreign = ensure_signed(make_image(), other_pem, short_tmp)
    ours_second = append_signature(foreign, signing_pem, short_tmp)
    assert signers(ours_second) == [modulus(other_pem), modulus(signing_pem), None]
    with pytest.raises(SigningError, match="different key"):
        ensure_signed(ours_second, signing_pem, short_tmp)

    ours = ensure_signed(make_image(), signing_pem, short_tmp)
    ours_first = append_signature(ours, other_pem, short_tmp)
    assert ensure_signed(ours_first, signing_pem, short_tmp) == ours_first

    broken_first = bytearray(append_signature(ours, signing_pem, short_tmp))
    broken_first[-4096] ^= 0xFF  # the first block's magic
    assert signers(bytes(broken_first)) == [None, modulus(signing_pem), None]
    with pytest.raises(SigningError, match="first signature block isn't valid"):
        ensure_signed(bytes(broken_first), signing_pem, short_tmp)


def test_first_rsa_signature_cannot_be_rescued_by_a_later_block(signing_pem, short_tmp):
    signed = ensure_signed(make_image(), signing_pem, short_tmp)
    broken_first = bytearray(append_signature(signed, signing_pem, short_tmp))
    first = len(broken_first) - 4096
    broken_first[first + 812] ^= 1  # RSA signature, keeping the first block's key and image digest intact.
    struct.pack_into("<I", broken_first, first + 1196, zlib.crc32(broken_first[first : first + 1196]))
    assert signers(bytes(broken_first)) == [modulus(signing_pem), modulus(signing_pem), None]
    image_path = short_tmp / "bad-first-rsa.bin"
    image_path.write_bytes(broken_first)
    # Ordinary espsecure verification still passes using block one: that is the regression trigger.
    subprocess.run(
        [
            sys.executable,
            "-m",
            "espsecure",
            "verify-signature",
            "--version",
            "2",
            "--keyfile",
            str(short_tmp / "TEST-ONLY-append-key.pem"),
            str(image_path),
        ],
        check=True,
        capture_output=True,
    )
    with pytest.raises(SigningError, match="verify-signature"):
        ensure_signed(bytes(broken_first), signing_pem, short_tmp)


def test_tampered_signed_image_fails_verification(signing_pem, short_tmp):
    signed = bytearray(ensure_signed(make_image(), signing_pem, short_tmp))
    signed[1000] ^= 0xFF
    with pytest.raises(SigningError, match="verify-signature"):
        ensure_signed(bytes(signed), signing_pem, short_tmp)


def test_only_rsa_3072_keys(short_tmp):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import rsa

    small = rsa.generate_private_key(public_exponent=65537, key_size=2048).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )
    with pytest.raises(SigningError, match="RSA-3072"):
        ensure_signed(make_image(), small, short_tmp)


# -- releases and offers


def _manifest(**overrides):
    m = {
        "channel": "dev",
        "version": "0.2.1+e1f2a3b",
        "build": "5d1c0a9",
        "size": 1234,
        "sha256": "ab" * 32,
        "notes": "n",
    }
    m.update(overrides)
    return m


def test_is_current_compares_whole_versions():
    m = _manifest()
    assert is_current(m, "0.2.1+e1f2a3b")
    assert not is_current(m, "0.2.1")
    assert not is_current(m, "0.2.0+e1f2a3b")
    assert is_current(_manifest(version="9e80c7f-dirty"), "9e80c7f-dirty")


def test_make_offer_install_policy():
    base = "https://127.0.0.2:8443"
    assert make_offer(None, "0.2.0", None, base) == {"available": False}
    assert make_offer(_manifest(), "0.2.1+e1f2a3b", None, base) == {"available": False}
    offer = make_offer(_manifest(), "0.2.0", None, base)
    assert offer == {
        "available": True,
        "version": "0.2.1+e1f2a3b",
        "build": "5d1c0a9",
        "size": 1234,
        "sha256": "ab" * 32,
        "url": f"{base}/v1/firmware/{'ab' * 32}.bin",
        "install": "now",
        "notes": "n",
    }
    assert make_offer(_manifest(channel="stable"), "0.2.0", None, base)["install"] == "idle"
    # Re-publishing an older release must never silently roll the device back.
    assert make_offer(_manifest(version="0.1.9"), "0.2.0", None, base) == {"available": False}
    # A version that already failed on this device is offered, but not installed on its own.
    failed = {"version": "0.2.1+e1f2a3b", "result": "rolled_back"}
    assert make_offer(_manifest(), "0.2.0", failed, base)["install"] == "ask"
    assert make_offer(_manifest(), "0.2.0", {"version": "0.1.0", "result": "failed"}, base)["install"] == "now"


def test_publish_writes_image_and_manifest(signing_pem, short_tmp):
    releases = Releases(short_tmp / "firmware")
    manifest = releases.publish(
        make_image(), "stable", "Fixes", lambda data: ensure_signed(data, signing_pem, short_tmp)
    )
    stored = (short_tmp / "firmware" / f"{manifest['sha256']}.bin").read_bytes()
    assert hashlib.sha256(stored).hexdigest() == manifest["sha256"]
    assert manifest["size"] == len(stored)
    assert releases.current("stable") == manifest
    assert json.loads((short_tmp / "firmware" / "stable.json").read_text())["notes"] == "Fixes"
    assert releases.current("dev") is None
    assert releases.image_path(f"{manifest['sha256']}.bin") is not None
    for bad in ("stable.json", f"{manifest['sha256'].upper()}.bin", f"../{manifest['sha256']}.bin", "x.bin"):
        assert releases.image_path(bad) is None


@pytest.mark.parametrize(
    "kwargs, message",
    [
        ({"channel": "beta"}, "channel"),
        ({"notes": "x" * 501}, "notes"),
    ],
)
def test_publish_rejects(kwargs, message, short_tmp):
    args = {"image": make_image(), "channel": "dev", "notes": "", "sign": lambda data: data} | kwargs
    with pytest.raises(FirmwareError, match=message):
        Releases(short_tmp / "firmware").publish(**args)


def test_publish_rejects_oversized_images(short_tmp):
    with pytest.raises(FirmwareError, match="OTA slot"):
        Releases(short_tmp / "firmware").publish(make_image(body=firmware.MAX_IMAGE), "dev", "", lambda data: data)


# -- over the link


async def run_cli(*argv: str) -> int:
    return await asyncio.to_thread(cli.main, list(argv))


async def test_release_offers_and_serves_the_image(hub, signing_key, image_file, capsys):
    device = await paired_device(hub)
    try:
        assert (await device.request("ota.check"))["r"] == {"available": False}

        assert (
            await run_cli("release", str(image_file(version="0.2.1+e1f2a3b")), "--channel", "dev", "--notes", "Hi") == 0
        )
        out = capsys.readouterr().out
        assert "Released orb 0.2.1+e1f2a3b" in out and "Offered to orb-020000abcdef." in out

        name, offer = await device.event()
        assert name == "ota.available"
        assert offer["install"] == "now" and offer["notes"] == "Hi"
        assert offer["url"] == f"https://127.0.0.1:{hub.tls_port}/v1/firmware/{offer['sha256']}.bin"
        assert (await device.request("ota.check"))["r"] == offer

        async with aiohttp.ClientSession() as session:
            ctx = device.ssl_context()
            async with session.get(offer["url"], ssl=ctx, headers={"Authorization": f"Bearer {device.token}"}) as r:
                assert r.status == 200
                assert r.headers["Content-Type"] == "application/octet-stream"
                assert int(r.headers["Content-Length"]) == offer["size"]
                body = await r.read()
            for headers in ({}, {"Authorization": "Bearer " + "0" * 64}, {"Authorization": f"Basic {device.token}"}):
                async with session.get(offer["url"], ssl=ctx, headers=headers) as r:
                    assert r.status == 401
                    assert r.headers["WWW-Authenticate"] == "Bearer"
        assert hashlib.sha256(body).hexdigest() == offer["sha256"]
        assert signers(body)[0] is not None
        # orb_ota refuses an image whose descriptor version isn't the offer's version, exactly.
        info = read_app_info(body)
        assert (offer["version"], offer["build"]) == (info.version, info.build)
        assert info.version == "0.2.1+e1f2a3b"

        # After installing, the device's hello reports the new version as the firmware does: nothing more to offer.
        await device.disconnect()
        await device.connect()
        res = await device.hello(fw=info.version, build=info.build, last_ota={"version": info.version, "result": "ok"})
        assert res["ok"]
        assert (await device.request("ota.check"))["r"] == {"available": False}
        assert hub.store.get(device.device_id).ota["last"]["result"] == "ok"
    finally:
        await device.aclose()


async def test_offer_after_hello_and_ask_after_rollback(hub, signing_key, image_file, capsys):
    assert await run_cli("release", str(image_file(version="0.3.0")), "--channel", "dev") == 0
    assert "devices get the offer when they next connect" not in capsys.readouterr().out  # the hub is running
    device = FakeDevice(hub)
    try:
        await device.fetch_cert()
        await device.connect()
        await device.pair_begin()
        await hub.handle_command("pair", {"code": device.sas})
        device.token = (await device.event())[1]["token"]
        await device.closed()

        await device.connect()
        assert (await device.hello())["ok"]
        name, offer = await device.event()
        assert (name, offer["version"], offer["install"]) == ("ota.available", "0.3.0", "now")

        await device.disconnect()
        await device.connect()
        last_ota = {"version": "0.3.0", "result": "rolled_back", "error": "no hello within 60 s"}
        assert (await device.hello(last_ota=last_ota))["ok"]
        name, offer = await device.event()
        assert offer["install"] == "ask"
        assert hub.store.get(device.device_id).ota["failed"]["result"] == "rolled_back"

        # Moved to stable, which has no release yet, the device has nothing to install.
        await hub.handle_command("config", {"device_id": device.device_id, "channel": "stable"})
        await device.hub_requests.get()
        assert (await device.request("ota.check"))["r"] == {"available": False}
    finally:
        await device.aclose()


async def test_ota_report_failure_downgrades_to_ask(hub, signing_key, image_file):
    device = await paired_device(hub)
    try:
        assert await run_cli("release", str(image_file(version="0.4.0")), "--channel", "dev") == 0
        assert (await device.event())[1]["install"] == "now"
        await device.send(
            {"t": "evt", "m": "ota.report", "p": {"version": "0.4.0", "state": "failed", "error": "sha256 mismatch"}}
        )
        assert (await device.request("ota.check"))["r"]["install"] == "ask"
        assert "OTA 0.4.0 failed sha256 mismatch" in hub.devlogs.path(device.device_id).read_text()
    finally:
        await device.aclose()


def test_release_without_a_running_hub(signing_key, image_file, capsys):
    assert cli.main(["release", str(image_file()), "--channel", "stable"]) == 0
    assert "devices get the offer when they next connect" in capsys.readouterr().out


def test_release_warns_about_a_reused_version(signing_key, image_file, capsys):
    assert cli.main(["release", str(image_file(version="0.5.0")), "--channel", "dev"]) == 0
    assert "Warning" not in capsys.readouterr().out
    rebuilt = image_file("rebuilt.bin", version="0.5.0", elf_sha=bytes(range(32)))
    assert cli.main(["release", str(rebuilt), "--channel", "dev"]) == 0
    assert "Warning: the channel's previous release was also 0.5.0" in capsys.readouterr().out


async def test_release_rejects_foreign_signature(hub, signing_key, other_pem, image_file, short_tmp, capsys):
    foreign = short_tmp / "foreign.bin"
    foreign.write_bytes(ensure_signed(make_image(), other_pem, short_tmp))
    assert await run_cli("release", str(foreign), "--channel", "dev") == 1
    assert "signed with a different key" in capsys.readouterr().err
    assert hub.releases.current("dev") is None


def test_sign_command(signing_key, image_file, capsys):
    source = image_file()
    assert cli.main(["sign", str(source)]) == 0
    signed = source.with_name("orb-signed.bin").read_bytes()
    assert signers(signed)[0] is not None and signed.startswith(source.read_bytes())
    assert cli.main(["sign", str(source.with_name("orb-signed.bin")), "-o", str(source.with_name("again.bin"))]) == 0
    assert source.with_name("again.bin").read_bytes() == signed


def test_release_needs_a_key(image_file, capsys):
    # conftest points security(1) at /usr/bin/false: no Keychain, no ORB_SIGNING_KEY.
    assert cli.main(["release", str(image_file()), "--channel", "dev"]) == 1
    assert "security find-generic-password failed" in capsys.readouterr().err


async def _raw_get(hub, pem: bytes, target: str, token: str | None) -> tuple[int, bytes]:
    """Sends the request line exactly as given, so no client library normalises the path first."""
    ctx = ssl.create_default_context(cadata=pem.decode())
    reader, writer = await asyncio.open_connection("127.0.0.1", hub.tls_port, ssl=ctx)
    auth = f"Authorization: Bearer {token}\r\n" if token else ""
    writer.write(f"GET {target} HTTP/1.1\r\nHost: 127.0.0.1\r\n{auth}Connection: close\r\n\r\n".encode())
    await writer.drain()
    data = await reader.read()
    writer.close()
    status = int(data.split(b" ", 2)[1])
    return status, data.partition(b"\r\n\r\n")[2]


async def test_firmware_path_traversal(hub, signing_key, image_file):
    device = await paired_device(hub)
    try:
        assert await run_cli("release", str(image_file()), "--channel", "dev") == 0
        _, offer = await device.event()
        sha = offer["sha256"]
        db = hub.cfg.db_path.read_bytes()[:16]
        targets = [
            "/v1/firmware/../devices.db",
            "/v1/firmware/..%2Fdevices.db",
            "/v1/firmware/..%2F..%2Fdata%2Fdevices.db",
            "/v1/firmware/%2e%2e/%2e%2e/devices.db",
            "/v1/firmware/%2e%2e%2fdev.json",
            "/v1/firmware/dev.json",
            f"/v1/firmware/{sha.upper()}.bin",
            f"/v1/firmware/{sha}.bin/../dev.json",
            f"/v1/firmware/{sha}.bin%00",
            f"/v1/firmware/{sha}",
            "/v1/firmware/" + "0" * 64 + ".bin",
            "/v1/firmware//etc/passwd",
            "/v1/firmware/%2Fetc%2Fpasswd",
            "/v1/../v1/firmware/../../tls/hub.key",
            "/v1/firmware/..\\..\\tls\\hub.key",
        ]
        for target in targets:
            for token in (device.token, None):
                status, body = await _raw_get(hub, device.pem, target, token)
                assert status in (400, 401, 404), (target, status)
                assert db not in body and b"PRIVATE KEY" not in body and b'"sha256"' not in body, target
        status, body = await _raw_get(hub, device.pem, f"/v1/firmware/{sha}.bin", device.token)
        assert status == 200 and hashlib.sha256(body).hexdigest() == sha
    finally:
        await device.aclose()


async def test_firmware_download_abort_does_not_crash_the_hub(hub, caplog):
    """A device that hangs up mid-download must not crash the handler.

    web.FileResponse used loop.sendfile(), which over TLS falls back to asyncio's _sendfile_fallback; when the
    client disconnects, that path's restore() calls set_protocol() on the closed _SSLProtocolTransport and raises
    AttributeError ('NoneType' object has no attribute '_set_app_protocol'). Streaming the image instead keeps the
    hub serving after an abort.
    """
    caplog.set_level(logging.DEBUG)
    # Big enough that the download is still streaming (not buffered) when the client hangs up.
    manifest = hub.releases.publish(make_image(body=3 * 1024 * 1024), "dev", "", lambda data: data)
    token = "f" * 64
    hub.store.pair("orb-abort-test", "Abort", "waveshare-s3-amoled-1.75c", "0.2.0", token, "dev")

    async with aiohttp.ClientSession() as session, session.get(f"http://127.0.0.1:{hub.pair_port}/v1/hub-cert") as resp:
        pem = await resp.read()
    ctx = ssl.create_default_context(cadata=pem.decode())
    reader, writer = await asyncio.open_connection("127.0.0.1", hub.tls_port, ssl=ctx)
    writer.write(
        f"GET /v1/firmware/{manifest['sha256']}.bin HTTP/1.1\r\n"
        f"Host: 127.0.0.1\r\nAuthorization: Bearer {token}\r\n\r\n".encode()
    )
    await writer.drain()
    # Read the response head and a little of the image, then reset the connection mid-transfer.
    assert await reader.read(4096)
    writer.transport.abort()
    # Wait for the hub to notice the reset and handle it; the fixed handler logs a clean abort quickly, while
    # the old FileResponse path raises and logs the AttributeError a moment later.
    for _ in range(150):
        if "aborted the download" in caplog.text:
            break
        await asyncio.sleep(0.02)

    # The disconnect was seen and handled cleanly: no sendfile AttributeError in the log, hub still serving.
    assert "aborted the download" in caplog.text
    assert "Unhandled exception" not in caplog.text
    assert "AttributeError" not in caplog.text and "_set_app_protocol" not in caplog.text

    async with (
        aiohttp.ClientSession() as session,
        session.get(
            f"https://127.0.0.1:{hub.tls_port}/v1/firmware/{manifest['sha256']}.bin",
            ssl=ctx,
            headers={"Authorization": f"Bearer {token}"},
        ) as resp,
    ):
        assert resp.status == 200
        assert int(resp.headers["Content-Length"]) == manifest["size"]
        body = await resp.read()
    assert hashlib.sha256(body).hexdigest() == manifest["sha256"]


def test_descriptor_offset_matches_esp_idf_layout():
    # esp_image_header_t is 24 bytes and esp_image_segment_header_t 8: the descriptor starts at 0x20.
    assert struct.calcsize("<BBBBIB3sHBHH4sB") + struct.calcsize("<II") == firmware.APP_DESC_OFFSET
