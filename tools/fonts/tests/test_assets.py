"""Signed envelope boundaries and real Secure Boot v2 rejection with disposable keys."""

import os
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

import assets


@pytest.fixture
def font_pack():
    pack = bytearray(48)
    pack[:8] = b"ORBFONTS"
    struct.pack_into("<I", pack, 32, len(pack))
    return bytes(pack)


def test_versioned_envelope(font_pack):
    image = assets.envelope(font_pack, 0xFFFFFFFF)
    info = assets.inspect(image)
    assert info["payload_size"] == 48
    assert info["signed_size"] == 4096
    assert info["image_size"] == 8192
    assert info["content_version"] == 0xFFFFFFFF


@pytest.mark.parametrize("version", [0, -1, 0x100000000])
def test_invalid_version_rejected(font_pack, version):
    with pytest.raises(ValueError, match="version"):
        assets.envelope(font_pack, version)


@pytest.mark.parametrize("offset", [0, 8, 12, 16, 20, 24, 28, 32, 64, 4095])
def test_header_payload_padding_corruption_rejected(font_pack, offset):
    data = bytearray(assets.envelope(font_pack, 1))
    data[offset] ^= 1
    with pytest.raises(ValueError):
        assets.inspect(data)


@pytest.mark.parametrize("length", [0, 63, 4095, 4097, 8192])
def test_truncated_or_extra_image_rejected(font_pack, length):
    data = assets.envelope(font_pack, 1)
    data = data[:length] + b"\xff" * max(0, length - len(data))
    with pytest.raises(ValueError):
        assets.inspect(data)


@pytest.fixture(scope="module")
def signing_tools(tmp_path_factory):
    executable = os.environ.get("ESPSECURE") or str(
        Path.home() / ".espressif/python_env/idf6.1_py3.13_env/bin/espsecure"
    )
    if not Path(executable).is_file() or not shutil.which("openssl"):
        pytest.skip("espsecure and openssl are required for real signature checks")
    folder = tmp_path_factory.mktemp("assets-keys")
    keys = []
    for name in ("release", "untrusted"):
        key = folder / f"{name}.pem"
        subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "RSA",
                "-pkeyopt",
                "rsa_keygen_bits:3072",
                "-out",
                str(key),
            ],
            check=True,
            capture_output=True,
        )
        keys.append(key)
    return executable, keys


def test_actual_sbv2_signature_and_wrong_key_rejected(font_pack, signing_tools, tmp_path):
    executable, (key, other_key) = signing_tools
    unsigned = tmp_path / "envelope.bin"
    signed = tmp_path / "signed.bin"
    unsigned.write_bytes(assets.envelope(font_pack, 3))
    subprocess.run(
        [
            executable,
            "sign-data",
            "--version",
            "2",
            "--keyfile",
            str(key),
            "--output",
            str(signed),
            str(unsigned),
        ],
        check=True,
        capture_output=True,
    )
    assert assets.inspect(signed.read_bytes(), signed=True)["content_version"] == 3
    verify = [executable, "verify-signature", "--version", "2", "--keyfile"]
    assert subprocess.run([*verify, str(key), str(signed)], capture_output=True).returncode == 0
    assert (
        subprocess.run([*verify, str(other_key), str(signed)], capture_output=True).returncode != 0
    )
    data = bytearray(signed.read_bytes())
    data[64] ^= 1
    signed.write_bytes(data)
    assert subprocess.run([*verify, str(key), str(signed)], capture_output=True).returncode != 0


def test_small_builtin_rescue_and_all_default_faces_in_pack():
    import build

    fonts, sets = build.plan()
    builtin = {name for name, font in fonts.items() if font.home == "app" and font.kind != "status"}
    assert builtin == {"orb_inter_28_regular", "orb_icons_28_regular"}
    assert all(fonts[name].home == "pack" for name in sets[0].font_names() if name not in builtin)


@pytest.mark.parametrize("missing", [True, False])
def test_actual_loader_rescue_for_missing_and_corrupt_pack(tmp_path, missing):
    import lvgl_check

    executable = lvgl_check.compile_all("clang", 4)
    pack = tmp_path / "pack.bin"
    if not missing:
        pack.write_bytes(b"untrusted and corrupted" + b"\xff" * 64)
    result = subprocess.run(
        [str(executable), "--rescue-tests", str(pack)],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert "rescue fonts: 165 checks, 0 failures" in result.stdout


def test_sound_bundle_envelope_and_bounds(font_pack):
    sound = struct.pack("<8sIIII", b"ORBSOUND", 1, 32000, 0, 24)
    image = assets.envelope(font_pack, 7, sound)
    info = assets.inspect(image)
    assert info["schema"] == 2
    assert info["font_bytes"] == 48 and info["sound_bytes"] == 24
    assert info["content_version"] == 7
    payload = bytearray(assets.bundle(font_pack, sound))
    for offset in (0, 8, 12, 16, 20, 24, 28):
        damaged = payload.copy()
        damaged[offset] ^= 0x80
        with pytest.raises(ValueError):
            assets.sections(damaged, 2)
    struct.pack_into("<I", payload, 20, 76)  # overlapping font/sound sections
    with pytest.raises(ValueError):
        assets.sections(payload, 2)


def test_signed_bundle_authenticates_sound_bytes(font_pack, signing_tools, tmp_path):
    executable, (key, _) = signing_tools
    sound = struct.pack("<8sIIII", b"ORBSOUND", 1, 32000, 0, 24)
    unsigned = tmp_path / "bundle.bin"
    signed = tmp_path / "bundle-signed.bin"
    unsigned.write_bytes(assets.envelope(font_pack, 8, sound))
    subprocess.run(
        [
            executable,
            "sign-data",
            "--version",
            "2",
            "--keyfile",
            str(key),
            "--output",
            str(signed),
            str(unsigned),
        ],
        check=True,
        capture_output=True,
    )
    assert assets.inspect(signed.read_bytes(), signed=True)["sound_bytes"] == 24
    damaged = bytearray(signed.read_bytes())
    damaged[64 + 80 + 12] ^= 1
    signed.write_bytes(damaged)
    result = subprocess.run(
        [executable, "verify-signature", "--version", "2", "--keyfile", str(key), str(signed)],
        capture_output=True,
        check=False,
    )
    assert result.returncode != 0
