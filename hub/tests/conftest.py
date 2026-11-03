import hashlib
import math
import shutil
import struct
import tempfile
from pathlib import Path

import pytest
import pytest_asyncio
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

from orb_hub import temperature_preferences, units
from orb_hub.config import Config
from orb_hub.server import Hub

TESTS = Path(__file__).parent


def voice_pcm() -> bytes:
    """300 ms of a modulated tone with quiet edges, standing in for speech energy."""
    samples = [
        int(6000 * (0.6 + 0.4 * math.sin(2 * math.pi * 10 * i / 16000)) * math.sin(2 * math.pi * 220 * i / 16000))
        if 800 <= i < 4000
        else 0
        for i in range(4800)
    ]
    return struct.pack(f"<{len(samples)}h", *samples)


@pytest.fixture
def short_tmp():
    # pytest's tmp_path is too long for a unix socket path on macOS (104 bytes).
    path = Path(tempfile.mkdtemp(prefix="orbhub-"))
    yield path
    shutil.rmtree(path, ignore_errors=True)


@pytest.fixture(autouse=True)
def isolated(short_tmp, monkeypatch):
    """Every test gets its own data and log directories, and a `security` that fails, so nothing can touch
    ~/.local/share/orb-hub or the real Keychain. Temperature settings are mocked
    so tests never depend on the host's global preferences."""
    monkeypatch.setenv("ORB_HUB_DATA", str(short_tmp / "data"))
    monkeypatch.setenv("ORB_HUB_LOGS", str(short_tmp / "logs"))
    monkeypatch.setenv("ORB_HUB_SECURITY", "/usr/bin/false")
    monkeypatch.delenv("ORB_SIGNING_KEY", raising=False)
    monkeypatch.setattr(units, "read_temperature_unit", lambda: "C")
    # Tests must not inherit the owner's temperature setting or read their preferences plist.
    monkeypatch.setattr(temperature_preferences, "read_temperature_unit", lambda: "C")
    return short_tmp


def _rsa_pem() -> bytes:
    key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    return key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )


@pytest.fixture(scope="session")
def signing_pem() -> bytes:
    """A throwaway RSA-3072 key: TEST-ONLY, generated per run, never written outside a temp dir."""
    return _rsa_pem()


@pytest.fixture(scope="session")
def other_pem() -> bytes:
    return _rsa_pem()


@pytest.fixture
def signing_key(signing_pem, short_tmp, monkeypatch) -> Path:
    path = short_tmp / "TEST-ONLY-signing-key.pem"
    path.write_bytes(signing_pem)
    path.chmod(0o600)
    monkeypatch.setenv("ORB_SIGNING_KEY", str(path))
    return path


def elf_sha256(version: str) -> bytes:
    """make_image's app_elf_sha256 for a version, standing in for the ELF's hash."""
    return hashlib.sha256(b"elf " + version.encode()).digest()


def make_image(
    version: str = "0.2.1+e1f2a3b",
    project: str = "orb",
    chip_id: int = 9,
    desc_magic: int = 0xABCD5432,
    body: int = 40000,
    elf_sha: bytes | None = None,
) -> bytes:
    """A minimal ESP-IDF app image: header, one segment opening with the app descriptor, filler."""
    desc = struct.pack(
        "<II8x32s32s16s16s32s32s",
        desc_magic,
        0,
        version.encode(),
        project.encode(),
        b"18:09:41",
        b"Oct 26 2026",
        b"v6.1",
        elf_sha256(version) if elf_sha is None else elf_sha,
    ).ljust(256, b"\0")
    filler = (hashlib.sha256(version.encode()).digest() * (body // 32 + 1))[:body]
    segment = desc + filler
    header = struct.pack(
        "<BBBBIB3sHBHH4sB", 0xE9, 1, 2, 0x2F, 0x40380000, 0xEE, bytes(3), chip_id, 0, 0, 0, bytes(4), 0
    )
    return header + struct.pack("<II", 0x3C000020, len(segment)) + segment


@pytest.fixture
def image_file(short_tmp):
    def write(name: str = "orb.bin", **kwargs) -> Path:
        path = short_tmp / name
        path.write_bytes(make_image(**kwargs))
        return path

    return write


@pytest_asyncio.fixture
async def make_hub(short_tmp):
    hubs = []

    async def start(**overrides) -> Hub:
        settings = {
            "bind": "127.0.0.1",
            "tls_port": 0,
            "pair_port": 0,
            "mdns": False,
            "hub_name": "Test Hub",
            "tz": "America/New_York",
            "request_timeout": 3.0,
        }
        settings.update(overrides)
        cfg = Config.load(data_dir=short_tmp / "data", log_dir=short_tmp / "logs", **settings)
        hub = Hub(cfg)
        await hub.start()
        hubs.append(hub)
        return hub

    yield start
    for hub in hubs:
        await hub.stop()


@pytest_asyncio.fixture
async def hub(make_hub):
    return await make_hub()
