"""Run device assets verification and installed IDF SBv2 code on actual build outputs.

No development private key is needed or accessed. Real signed files are read only;
the freshly built app and the exact assets envelope are also signed with an ephemeral
RSA-3072 key under pytest's temporary directory. OpenSSL supplies host PSA primitives.
"""

import hashlib
import os
import struct
import subprocess
import zlib
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "firmware/components/orb_fonts/tests/artifacts"
BUILD = ROOT / "firmware/build"
IDF = Path(os.environ.get("IDF_PATH", Path.home() / "esp/esp-idf"))
ESPSECURE = Path(
    os.environ.get(
        "ESPSECURE", Path.home() / ".espressif/python_env/idf6.1_py3.13_env/bin/espsecure"
    )
)


def run(args, **kwargs):
    try:
        return subprocess.run(
            [str(arg) for arg in args], capture_output=True, text=True, check=True, **kwargs
        )
    except subprocess.CalledProcessError as error:
        pytest.fail(error.stdout + error.stderr, pytrace=False)


def idf_fragments():
    """Keep IDF's mmap SHA algorithm and 24-bit range check exact, not reimplemented.

    The rest of flash_mmap.c and bootloader_utility.c requires ESP32 hardware. These
    selected functions only need fake flash/MMU and SHA adapters. Fail loudly if IDF
    changes either source shape, so a host test cannot silently stop exercising it.
    """
    utility = (IDF / "components/bootloader_support/src/bootloader_utility.c").read_text()
    start = utility.index("static esp_err_t bootloader_sha_flash_contents(")
    end = utility.index(
        "\n#if SOC_SHA_SUPPORT_SHA384 && SOC_ECDSA_SUPPORT_CURVE_P384\n"
        "esp_err_t bootloader_sha384_flash_contents",
        start,
    )
    mmap = (IDF / "components/spi_flash/flash_mmap.c").read_text()
    start_guard = mmap.index("#if !CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH")
    end_guard = mmap.index("#endif", start_guard) + len("#endif")
    return (
        '#include "platform.h"\nstatic const char *TAG = "idf_sha";\n'
        + utility[start:end]
        + "\nesp_err_t artifact_mmap_address_guard(size_t src_addr, size_t size) {\n"
        + mmap[start_guard:end_guard]
        + "\nreturn ESP_OK;\n}\n"
    )


@pytest.fixture(scope="module")
def verifier(tmp_path_factory):
    scratch = tmp_path_factory.mktemp("storage-artifact-host")
    fragment = scratch / "idf_flash_hash.c"
    fragment.write_text(idf_fragments())
    openssl = Path(os.environ.get("OPENSSL_PREFIX", "/opt/homebrew/opt/openssl@3"))
    if openssl.exists():
        crypto_flags = [f"-I{openssl}/include", f"-L{openssl}/lib", "-lcrypto"]
    else:
        crypto_flags = run(["pkg-config", "--cflags", "--libs", "openssl"]).stdout.split()
    fonts = ROOT / "firmware/components/orb_fonts"
    output = scratch / "verify_artifacts"
    run(
        [
            os.environ.get("CC", "clang"),
            "-std=c11",
            "-D_DEFAULT_SOURCE",
            "-Wno-deprecated-declarations",
            "-Wno-format",
            "-O1",
            "-g",
            "-fsanitize=address,undefined",
            "-DLV_CONF_INCLUDE_SIMPLE",
            "-DLV_KCONFIG_IGNORE",
            f"-I{BUILD / 'config'}",
            f"-I{HOST / 'stubs'}",
            f"-I{fonts / 'tests/host'}",
            f"-I{ROOT / 'firmware/third_party/lvgl__lvgl'}",
            f"-I{ROOT / 'firmware/components/orb_lv_simd/include'}",
            f"-I{fonts / 'include'}",
            f"-I{fonts / 'src'}",
            HOST / "verify_artifacts.c",
            fonts / "port/orb_assets_esp.c",
            fragment,
            IDF / "components/bootloader_support/src/secure_boot_v2/secure_boot_signatures_app.c",
            IDF / "components/bootloader_support/src/secure_boot_v2/secure_boot_rsa_signature.c",
            *crypto_flags,
            "-o",
            output,
        ]
    )
    return output


def verify(verifier, app, assets, *, valid=True, legacy=False, app_only=False):
    result = subprocess.run(
        [
            str(verifier),
            str(app),
            str(assets),
            *(["app"] if app_only else ["legacy"] if legacy else []),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == (0 if valid else 1), result.stdout + result.stderr
    assert "Sanitizer" not in result.stderr, result.stderr
    return result


def inspect_app(data):
    assert data[0] == 0xE9 and 0 < data[1] <= 16 and data[23] == 1
    pos = 24
    checksum = 0xEF
    for _ in range(data[1]):
        length = struct.unpack_from("<I", data, pos + 4)[0]
        assert length % 4 == 0 and pos + 8 + length <= len(data)
        for byte in data[pos + 8 : pos + 8 + length]:
            checksum ^= byte
        pos += 8 + length
    pos = (pos + 16) & ~15
    assert data[pos - 1] == checksum
    assert data[pos : pos + 32] == hashlib.sha256(data[:pos]).digest()
    return pos + 32


def inspect_signature(data):
    assert len(data) % 4096 == 0
    block = data[-4096 : -4096 + 1216]
    assert block[:4] == b"\xe7\x02\0\0"
    assert block[4:36] == hashlib.sha256(data[:-4096]).digest()
    assert struct.unpack_from("<I", block, 1196)[0] == zlib.crc32(block[:1196])
    assert int.from_bytes(block[36:420], "little").bit_length() == 3072
    return hashlib.sha256(block[36:812]).digest()


def test_exact_real_artifacts_and_legacy_16mib_failure(verifier):
    app = BUILD / "orb.signed.bin"
    assets = BUILD / "orb_assets-signed.bin"
    original = {path: hashlib.sha256(path.read_bytes()).digest() for path in (app, assets)}
    app_data = app.read_bytes()
    app_length = inspect_app(app_data)
    assert (app_length + 4095) // 4096 * 4096 == len(app_data) - 4096
    assert inspect_signature(app_data) == inspect_signature(assets.read_bytes())
    assert assets.read_bytes()[:8] == b"ORBASSET"
    table = (BUILD / "partition_table/partition-table.bin").read_bytes()
    partitions = {}
    for offset in range(0, len(table), 32):
        magic, _, _, address, size, name, _ = struct.unpack_from("<HBBII16sI", table, offset)
        if magic != 0x50AA:
            break
        partitions[name.rstrip(b"\0").decode()] = address, size
    assert partitions["ota_0"] == (0x20000, 0x600000)
    assert partitions["assets"] == (0xC20000, 0x800000)
    config = (BUILD / "config/sdkconfig.h").read_text()
    assert "#define CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME 1" in config
    assert "#define CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT 1" in config
    assert "#define CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH 1" not in config
    assert "#define CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_OCTAL_FLASH 1" not in config
    payload_size = struct.unpack_from("<I", assets.read_bytes(), 16)[0]
    assert (
        assets.read_bytes()[64 : 64 + payload_size]
        == (ROOT / "firmware/components/orb_fonts/pack/orb_fonts.pack").read_bytes()
    )
    verify(verifier, app, assets)
    verify(verifier, app, assets, app_only=True)
    failure = verify(verifier, app, assets, valid=False, legacy=True)
    assert "out of range for 24bit flash mapping" in failure.stderr
    assert all(
        hashlib.sha256(path.read_bytes()).digest() == digest for path, digest in original.items()
    )


@pytest.fixture(scope="module")
def throwaway_pair(tmp_path_factory):
    scratch = tmp_path_factory.mktemp("storage-rsa3072")
    key = scratch / "throwaway-key.pem"
    run(
        ["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:3072", "-out", key]
    )
    key.chmod(0o600)
    app = scratch / "app.signed.bin"
    assets = scratch / "assets.signed.bin"
    unsigned = scratch / "assets.bin"
    unsigned.write_bytes((BUILD / "orb_assets-signed.bin").read_bytes()[:-4096])
    assert inspect_app((BUILD / "orb.bin").read_bytes()) <= (BUILD / "orb.bin").stat().st_size
    for source, target in ((BUILD / "orb.bin", app), (unsigned, assets)):
        run(
            [ESPSECURE, "sign-data", "--version", "2", "--keyfile", key, "--output", target, source]
        )
    yield scratch, key, app, assets
    key.unlink()


def test_rebuilt_app_and_exact_envelope_with_ephemeral_rsa3072(verifier, throwaway_pair):
    _, _, app, assets = throwaway_pair
    assert inspect_signature(app.read_bytes()) == inspect_signature(assets.read_bytes())
    verify(verifier, app, assets)
    verify(verifier, app, assets, app_only=True)
    verify(verifier, app, BUILD / "orb_assets-signed.bin", valid=False)


@pytest.mark.parametrize("damage", ["payload", "signature", "header", "truncated"])
def test_device_rejects_corrupted_artifacts(verifier, throwaway_pair, tmp_path, damage):
    _, _, app, assets = throwaway_pair
    damaged = bytearray(assets.read_bytes())
    if damage == "payload":
        damaged[100] ^= 1
    elif damage == "signature":
        damaged[-4096 + 812] ^= 1
        struct.pack_into(
            "<I", damaged, len(damaged) - 4096 + 1196, zlib.crc32(damaged[-4096:-2900])
        )
    elif damage == "header":
        damaged[20] ^= 1
    else:
        damaged.pop()
    path = tmp_path / "damaged.bin"
    path.write_bytes(damaged)
    verify(verifier, app, path, valid=False)


def test_signed_envelope_with_wrong_payload_digest(verifier, throwaway_pair, tmp_path):
    _, key, app, assets = throwaway_pair
    damaged = bytearray(assets.read_bytes()[:-4096])
    damaged[32] ^= 1
    unsigned = tmp_path / "bad-digest.bin"
    signed = tmp_path / "bad-digest.signed.bin"
    unsigned.write_bytes(damaged)
    run([ESPSECURE, "sign-data", "--version", "2", "--keyfile", key, "--output", signed, unsigned])
    verify(verifier, app, signed, valid=False)
