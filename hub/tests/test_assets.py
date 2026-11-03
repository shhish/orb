"""Signed data releases are independent of application updates and old clients."""

import asyncio
import hashlib
import struct

import aiohttp
import pytest
from conftest import make_image
from fake_device import paired_device

from orb_hub import cli, firmware
from orb_hub.firmware import FirmwareError, Releases, read_assets_info
from orb_hub.signing import SigningError, ensure_signed


def font_pack():
    payload = bytearray(b"ORBFONTS" + bytes(40) + b"test-only-font-record")
    struct.pack_into("<I", payload, 8, 1)
    struct.pack_into("<I", payload, 32, len(payload))
    return bytes(payload)


def assets_image(version=1, payload=None, schema=1):
    payload = font_pack() if payload is None else payload
    digest = hashlib.sha256(payload).digest()
    size = (64 + len(payload) + 4095) // 4096 * 4096
    header = firmware.ASSETS_HEADER.pack(b"ORBASSET", schema, 64, len(payload), size, version, schema, digest)
    return (header + payload).ljust(size, b"\xff")


def test_assets_header_and_version():
    info = read_assets_info(assets_image(27))
    assert info.project == "orb-assets" and info.version == "27"
    assert info.build == hashlib.sha256(font_pack()).hexdigest()[:7]
    assert firmware.MAX_IMAGE == 6 * 1024 * 1024


@pytest.mark.parametrize(
    "offset,value,message",
    [
        (0, 0, "unsupported"),
        (8, 2, "unsupported"),
        (12, 63, "unsupported"),
        (16, 0, "size"),
        (20, 8192, "size"),
        (24, 0, "version"),
        (28, 2, "unsupported"),
        (112, 0, "SHA-256"),
        (4095, 0, "padding"),
    ],
)
def test_assets_header_corruption(offset, value, message):
    image = bytearray(assets_image())
    if 8 <= offset <= 28:
        struct.pack_into("<I", image, offset, value)
    else:
        image[offset] = value
    with pytest.raises(FirmwareError, match=message):
        read_assets_info(bytes(image))


@pytest.mark.parametrize("image", [b"", assets_image()[:-1], assets_image() + b"x"])
def test_assets_truncated_or_trailing_data(image):
    with pytest.raises(FirmwareError):
        read_assets_info(image)


@pytest.mark.parametrize("corruption", ["magic", "embedded_size"])
def test_assets_rejects_non_font_payload_before_signing(short_tmp, corruption):
    payload = bytearray(font_pack())
    if corruption == "magic":
        payload[:8] = b"NOTFONTS"
    else:
        struct.pack_into("<I", payload, 32, len(payload) - 1)
    image = assets_image(payload=bytes(payload))

    # The outer payload hash is correct: this must fail structural validation.
    def must_not_sign(data):
        raise AssertionError("invalid payload reached signing")

    with pytest.raises(FirmwareError, match="complete font pack"):
        Releases(short_tmp / "firmware").publish(image, "dev", "", must_not_sign, assets=True)


def test_assets_releases_are_signed_and_independent(signing_pem, other_pem, short_tmp):
    releases = Releases(short_tmp / "firmware")
    sign = lambda data: ensure_signed(data, signing_pem, short_tmp)
    app = releases.publish(make_image(), "dev", "app", sign)
    assets = releases.publish(assets_image(), "dev", "assets", sign, assets=True)
    assert releases.current("dev") == app
    assert releases.current("dev", assets=True) == assets
    assert assets["kind"] == "assets" and assets["version"] == "1"
    assert read_assets_info(releases.image_path(assets["sha256"] + ".bin").read_bytes()).version == "1"
    foreign = ensure_signed(assets_image(2), other_pem, short_tmp)
    with pytest.raises(SigningError, match="different key"):
        releases.publish(foreign, "dev", "", sign, assets=True)
    assert releases.current("dev", assets=True) == assets
    corrupt = bytearray(ensure_signed(assets_image(2), signing_pem, short_tmp))
    corrupt[-4096 + 812] ^= 1
    # Keep the block CRC valid: the RSA verification, not just parsing, must fail.
    import zlib

    struct.pack_into("<I", corrupt, len(corrupt) - 4096 + 1196, zlib.crc32(corrupt[-4096 : -4096 + 1196]))
    with pytest.raises(SigningError):
        releases.publish(bytes(corrupt), "dev", "", sign, assets=True)
    assert releases.current("dev", assets=True) == assets


def test_larger_app_fits_new_six_megabyte_slot(signing_pem, short_tmp):
    manifest = Releases(short_tmp / "firmware").publish(
        make_image(body=5 * 1024 * 1024), "dev", "", lambda data: ensure_signed(data, signing_pem, short_tmp)
    )
    assert 5 * 1024 * 1024 < manifest["size"] < firmware.MAX_IMAGE


async def test_assets_cli_download_offer_failure_and_old_client(hub, signing_key, short_tmp):
    old_device = await paired_device(hub)
    image_path = short_tmp / "assets.bin"
    image_path.write_bytes(assets_image(2))
    app = await asyncio.to_thread(
        hub.releases.publish,
        make_image(version="0.2.0"),
        "dev",
        "Fixture app",
        lambda data: ensure_signed(data, signing_key.read_bytes(), short_tmp),
    )
    try:
        assert await asyncio.to_thread(cli.main, ["release", str(image_path), "--assets", "--channel", "dev"]) == 0
        assert (await old_device.request("ota.check", {"kind": "assets"}))["r"] == {"available": False}
        assert hub.releases.current("dev") == app
        await old_device.disconnect()
        await old_device.connect()
        assert (await old_device.hello(fw=app["version"], build=app["build"], assets=""))["ok"]
        name, offer = await old_device.event()
        assert name == "ota.available" and offer["kind"] == "assets" and offer["version"] == "2"
        assert (await old_device.request("ota.check"))["r"] == {"available": False}
        assert (await old_device.request("ota.check", {"kind": "assets"}))["r"] == offer
        async with (
            aiohttp.ClientSession() as session,
            session.get(
                offer["url"], ssl=old_device.ssl_context(), headers={"Authorization": f"Bearer {old_device.token}"}
            ) as response,
        ):
            assert response.status == 200
            body = await response.read()
            assert hashlib.sha256(body).hexdigest() == offer["sha256"]
            assert firmware.signature_blocks(body)[0] is not None
        await old_device.send(
            {
                "t": "evt",
                "m": "ota.report",
                "p": {"kind": "assets", "version": "2", "state": "failed", "error": "power interrupted"},
            }
        )
        assert (await old_device.request("ota.check", {"kind": "assets"}))["r"]["install"] == "ask"
        record = hub.store.get(old_device.device_id)
        assert "failed" not in record.ota and record.ota["assets_failed"]["version"] == "2"
        await old_device.disconnect()
        await old_device.connect()
        assert (await old_device.hello(fw=app["version"], build=app["build"], assets="2"))["ok"]
        assert (await old_device.request("ota.check", {"kind": "assets"}))["r"] == {"available": False}
    finally:
        await old_device.aclose()


def sound_bundle():
    fonts = font_pack()
    so = (32 + len(fonts) + 3) & ~3
    sounds = struct.pack("<8s4I", b"ORBSOUND", 1, 32000, 0, 24)
    header = struct.pack("<8s6I", b"ORBBNDL2", 2, 32, len(fonts), so, len(sounds), so + len(sounds))
    return (header + fonts).ljust(so, b"\0") + sounds


def test_assets_accepts_font_sound_bundle():
    assert read_assets_info(assets_image(28, sound_bundle(), schema=2)).version == "28"


@pytest.mark.parametrize("offset", [0, 8, 12, 16, 20, 24, 28])
def test_assets_bundle_rejects_overlaps_and_corrupt_bounds(offset):
    payload = bytearray(sound_bundle())
    payload[offset] ^= 0x80
    with pytest.raises(FirmwareError):
        read_assets_info(assets_image(28, payload, schema=2))
