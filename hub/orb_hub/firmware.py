"""Firmware images, releases and OTA offers (docs/protocol.md, "Releases" and "OTA offer").

Images are stored once as firmware/<sha256>.bin; each channel's current release is firmware/<channel>.json.
"""

import datetime
import hashlib
import json
import os
import re
import struct
import tempfile
import zlib
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from orb_hub.config import CHANNELS, ensure_private_dir

IMAGE_MAGIC = 0xE9
CHIP_ID_ESP32S3 = 9
APP_DESC_MAGIC = 0xABCD5432
# The app descriptor opens the first segment: after the 24-byte image header and the 8-byte segment header.
APP_DESC_OFFSET = 0x20
APP_DESC_SIZE = 256
# hello's build is esp_app_get_elf_sha256(buf, 8): the first 7 hex digits of the descriptor's app_elf_sha256.
BUILD_DIGITS = 7

SECTOR = 4096
SIG_BLOCK_SIZE = 1216
SIG_BLOCK_MAGIC = 0xE7
SIG_BLOCK_RSA = 0x02
SIG_BLOCK_ECDSA = 0x03
SIG_BLOCKS_MAX = 3

# One ota_N slot in firmware/partitions.csv; the signature sector has to fit too.
MAX_IMAGE = 6 * 1024 * 1024
MAX_ASSETS = 8 * 1024 * 1024
ASSETS_HEADER = struct.Struct("<8s6I32s")
ASSETS_MAGIC = b"ORBASSET"
MAX_NOTES = 500
INSTALL_POLICY = {"dev": "now", "stable": "idle"}
IMAGE_NAME = re.compile(r"^([0-9a-f]{64})\.bin$")
APP_VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:[-+][0-9A-Za-z][0-9A-Za-z.+-]*)?")


class FirmwareError(Exception):
    pass


@dataclass(frozen=True)
class AppInfo:
    project: str
    # The descriptor's version, whole ("0.2.1+e1f2a3b"): what hello's fw and offers' version are, and what the
    # device compares with the version of the image it downloads.
    version: str
    build: str  # hello's build: the first 7 hex digits of app_elf_sha256, or "" if esptool left it empty
    built: str  # compile date and time, e.g. "Oct 26 2026 18:09:41"
    idf: str


@dataclass(frozen=True)
class SignatureBlock:
    rsa_modulus: int | None  # the signer's RSA modulus; None in an ECDSA block


def _cstr(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def read_app_info(image: bytes) -> AppInfo:
    """Reads the app descriptor (esp_app_desc_t), checking that this is an ESP32-S3 app image."""
    if len(image) < APP_DESC_OFFSET + APP_DESC_SIZE or image[0] != IMAGE_MAGIC:
        raise FirmwareError("not an ESP-IDF app image")
    (chip_id,) = struct.unpack_from("<H", image, 12)
    if chip_id != CHIP_ID_ESP32S3:
        raise FirmwareError(f"image is for chip id {chip_id}, not the ESP32-S3 ({CHIP_ID_ESP32S3})")
    (magic,) = struct.unpack_from("<I", image, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        raise FirmwareError("no app descriptor at 0x20: is this the app .bin rather than the bootloader?")
    # esp_app_desc_t: version at 16, project_name at 48, time at 80, date at 96, idf_ver at 112, app_elf_sha256 at 144.
    desc = image[APP_DESC_OFFSET : APP_DESC_OFFSET + APP_DESC_SIZE]
    version = _cstr(desc[16:48])
    if not version:
        raise FirmwareError("the app descriptor has no version")
    elf_sha256 = desc[144:176]
    return AppInfo(
        project=_cstr(desc[48:80]),
        version=version,
        build=elf_sha256.hex()[:BUILD_DIGITS] if any(elf_sha256) else "",
        built=f"{_cstr(desc[96:112])} {_cstr(desc[80:96])}".strip(),
        idf=_cstr(desc[112:144]),
    )


def read_assets_info(image: bytes) -> AppInfo:
    """Validate the versioned assets envelope before signing or publishing it.

    This checks structure and payload integrity, not authenticity: publish() must
    still sign and verify the entire padded envelope with the release key.
    """
    if len(image) < ASSETS_HEADER.size:
        raise FirmwareError("truncated assets header")
    magic, version, header_size, payload_size, signed_size, content_version, kind, digest = ASSETS_HEADER.unpack_from(
        image
    )
    if magic != ASSETS_MAGIC or version not in (1, 2) or header_size != ASSETS_HEADER.size or kind != version:
        raise FirmwareError("unsupported assets header")
    expected_size = (header_size + payload_size + SECTOR - 1) // SECTOR * SECTOR
    if (
        payload_size < 48
        or payload_size > MAX_ASSETS - 2 * SECTOR
        or not content_version
        or signed_size != expected_size
        or signed_size + SECTOR > MAX_ASSETS
    ):
        raise FirmwareError("invalid assets size or version")
    if len(image) not in (signed_size, signed_size + SECTOR):
        raise FirmwareError("assets image length does not match its header")
    payload_end = header_size + payload_size
    payload = image[header_size:payload_end]
    fonts = payload
    if kind == 2:
        magic, schema, fo, fs, so, ss, total = struct.unpack_from("<8s6I", payload)
        if (
            magic != b"ORBBNDL2"
            or schema != 2
            or total != payload_size
            or fo != 32
            or fs < 48
            or so < fo + fs
            or so % 4
            or ss < 24
            or so > total
            or ss != total - so
        ):
            raise FirmwareError("invalid assets section bounds")
        fonts = payload[fo : fo + fs]
        sounds = payload[so : so + ss]
        if (
            sounds[:8] != b"ORBSOUND"
            or struct.unpack_from("<I", sounds, 8)[0] != 1
            or struct.unpack_from("<I", sounds, 20)[0] != ss
        ):
            raise FirmwareError("invalid assets sound section")
    if fonts[:8] != b"ORBFONTS" or struct.unpack_from("<I", fonts, 32)[0] != len(fonts):
        raise FirmwareError("assets payload is not a complete font pack")
    if hashlib.sha256(payload).digest() != digest:
        raise FirmwareError("assets payload SHA-256 mismatch")
    if image[payload_end:signed_size] != b"\xff" * (signed_size - payload_end):
        raise FirmwareError("invalid assets padding")
    return AppInfo("orb-assets", str(content_version), digest.hex()[:BUILD_DIGITS], "", "")


def signature_blocks(image: bytes) -> list[SignatureBlock | None]:
    """An image's Secure Boot v2 signature blocks by position, None where a slot holds no valid block (all None when
    the image isn't signed). Position matters: ESP-IDF checks only the first block of an update. The blocks fill
    the last 4 KB sector (espsecure's layout: magic, version, 2 pad, 32-byte digest, 384-byte little-endian
    modulus, ... CRC32 at 1196)."""
    blocks: list[SignatureBlock | None] = [None] * SIG_BLOCKS_MAX
    if len(image) < 2 * SECTOR or len(image) % SECTOR:
        return blocks
    sector = image[-SECTOR:]
    for i in range(SIG_BLOCKS_MAX):
        block = sector[i * SIG_BLOCK_SIZE : (i + 1) * SIG_BLOCK_SIZE]
        if block[0] != SIG_BLOCK_MAGIC or block[1] not in (SIG_BLOCK_RSA, SIG_BLOCK_ECDSA):
            continue
        (crc,) = struct.unpack_from("<I", block, 1196)
        if zlib.crc32(block[:1196]) != crc:
            continue
        modulus = int.from_bytes(block[36 : 36 + 384], "little") if block[1] == SIG_BLOCK_RSA else None
        blocks[i] = SignatureBlock(modulus)
    return blocks


def is_current(manifest: dict, fw: str) -> bool:
    """Whether a device whose hello reports fw already runs the release. Versions alone decide, as on the device
    (orb_ota ignores an offer of the version it runs): build is only shown."""
    return fw == manifest["version"]


def is_newer(manifest: dict, fw: str) -> bool:
    """Order release numbers, never build IDs or timestamps.

    Bench and feed builds of the same app release must coexist without the
    feed silently replacing the bench. Unknown versions are incomparable;
    replacing those or rolling back requires an explicit recovery workflow.
    Empty assets means no pack is installed, so any valid content version wins.
    """
    version = manifest["version"]
    if manifest.get("kind") == "assets":
        if not re.fullmatch(r"[1-9][0-9]*", version):
            return False
        return fw == "" or bool(re.fullmatch(r"[0-9]+", fw) and int(version) > int(fw))
    release = APP_VERSION.fullmatch(version)
    running = APP_VERSION.fullmatch(fw)
    return bool(release and running and tuple(map(int, release.groups())) > tuple(map(int, running.groups())))


def make_offer(manifest: dict | None, fw: str, failed: dict | None, base_url: str) -> dict:
    """ota.check's answer and ota.available's params for a device on manifest's channel.

    install follows the channel (dev: now, stable: idle), except that a version this device already failed to
    install (it rolled back, or the download or check failed) is offered with "ask": installing it again on
    its own would just loop.
    """
    if manifest is None or not is_newer(manifest, fw):
        return {"available": False}
    install = INSTALL_POLICY[manifest["channel"]]
    if failed and failed.get("version") == manifest["version"]:
        install = "ask"
    return {
        **({"kind": "assets"} if manifest.get("kind") == "assets" else {}),
        "available": True,
        "version": manifest["version"],
        "build": manifest["build"],
        "size": manifest["size"],
        "sha256": manifest["sha256"],
        "url": f"{base_url}/v1/firmware/{manifest['sha256']}.bin",
        "install": install,
        "notes": manifest["notes"],
    }


class Releases:
    def __init__(self, directory: Path):
        self.directory = directory

    def current(self, channel: str, *, assets: bool = False) -> dict | None:
        try:
            return json.loads((self.directory / f"{channel}{'-assets' if assets else ''}.json").read_text())
        except FileNotFoundError:
            return None

    def image_path(self, name: str) -> Path | None:
        """The stored image for a request path's last part ("<sha256>.bin"), or None. Only names that are
        exactly 64 lowercase hex digits plus ".bin" are looked up, so nothing outside the directory can be."""
        if not IMAGE_NAME.match(name):
            return None
        path = self.directory / name
        return path if path.is_file() else None

    def publish(
        self, image: bytes, channel: str, notes: str, sign: Callable[[bytes], bytes], *, assets: bool = False
    ) -> dict:
        """Signs (via sign), stores and makes image the channel's current release. Returns the manifest."""
        if channel not in CHANNELS:
            raise FirmwareError(f"channel must be one of {', '.join(CHANNELS)}")
        if len(notes) > MAX_NOTES:
            raise FirmwareError(f"notes are limited to {MAX_NOTES} characters")
        limit = MAX_ASSETS if assets else MAX_IMAGE
        if len(image) > limit:
            raise FirmwareError(f"image is {len(image)} bytes; an OTA slot holds {limit}")
        info = read_assets_info(image) if assets else read_app_info(image)
        signed = sign(image)
        if len(signed) > limit:
            raise FirmwareError(f"signed image is {len(signed)} bytes; an OTA slot holds {limit}")
        if assets:
            read_assets_info(signed)
            if signature_blocks(signed)[0] is None:
                raise FirmwareError("assets signing did not produce a valid first signature block")
        digest = hashlib.sha256(signed).hexdigest()
        ensure_private_dir(self.directory)
        path = self.directory / f"{digest}.bin"
        if not path.exists():
            _write_atomic(path, signed)
        manifest = {
            **({"kind": "assets"} if assets else {}),
            "channel": channel,
            "project": info.project,
            "version": info.version,
            "build": info.build,
            "built": info.built,
            "idf": info.idf,
            "size": len(signed),
            "sha256": digest,
            "notes": notes,
            "released_at": datetime.datetime.now(datetime.UTC).isoformat(timespec="seconds"),
        }
        _write_atomic(
            self.directory / f"{channel}{'-assets' if assets else ''}.json",
            json.dumps(manifest, indent=2).encode() + b"\n",
        )
        return manifest


def _write_atomic(path: Path, data: bytes) -> None:
    # The server reads these while the CLI writes them: readers see the old file or the new one, never half.
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise
