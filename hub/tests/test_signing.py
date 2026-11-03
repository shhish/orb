"""The signing key in the Keychain, against a fake security(1)."""

import base64
import json
import sys
from pathlib import Path

import pytest
from conftest import TESTS

from orb_hub import cli, signing
from orb_hub.signing import SigningError


@pytest.fixture
def keychain(short_tmp, monkeypatch) -> Path:
    root = short_tmp / "keychain"
    root.mkdir()
    tool = short_tmp / "security"
    tool.write_text(f"#!{sys.executable}\n" + (TESTS / "fake_security.py").read_text())
    tool.chmod(0o700)
    monkeypatch.setenv("FAKE_KEYCHAIN_DIR", str(root))
    monkeypatch.setenv("ORB_HUB_SECURITY", str(tool))
    return root


def items(keychain: Path) -> dict:
    return json.loads((keychain / "items.json").read_text())


def calls(keychain: Path) -> list[dict]:
    return [json.loads(line) for line in (keychain / "calls.jsonl").read_text().splitlines()]


def test_keys_init_stores_the_key_without_exposing_it(keychain, short_tmp):
    pub = short_tmp / "repo" / "firmware" / "keys" / "orb_signing_pub.pem"
    assert signing.keys_init(pub) is True

    item = items(keychain)[signing.KEYCHAIN_SERVICE]
    assert item["keychain"] == ["login.keychain"]
    pem = base64.b64decode(item["value"])
    key = signing.private_key(pem)
    assert key.key_size == 3072
    assert pub.read_bytes() == signing.public_pem(key)

    adds = [c for c in calls(keychain) if c["argv"] == ["-i"]]
    assert len(adds) == 1 and len(adds[0]["stdin"]) < 4096
    for call in calls(keychain):
        assert item["value"] not in " ".join(call["argv"])
        assert b"PRIVATE KEY" not in " ".join(call["argv"]).encode()

    assert signing.load_signing_key() == pem


def test_keys_init_keeps_an_existing_key(keychain, short_tmp):
    pub = short_tmp / "pub.pem"
    assert signing.keys_init(pub) is True
    first = pub.read_bytes()
    pub.unlink()
    assert signing.keys_init(pub) is False
    assert pub.read_bytes() == first
    assert len([c for c in calls(keychain) if c["argv"] == ["-i"]]) == 1


def test_keys_init_cli(keychain, short_tmp, capsys):
    pub = short_tmp / "pub.pem"
    assert cli.main(["keys", "init", "--pub-out", str(pub)]) == 0
    assert "Created the signing key in the login Keychain" in capsys.readouterr().out
    assert cli.main(["keys", "init", "--pub-out", str(pub)]) == 0
    assert "already has the signing key" in capsys.readouterr().out


def test_reads_a_plain_pem_item(keychain, signing_pem):
    # An item made by hand holds the PEM itself; security prints it as hex because of the line breaks.
    (keychain / "items.json").write_text(
        json.dumps({signing.KEYCHAIN_SERVICE: {"value": signing_pem.decode(), "keychain": []}})
    )
    assert signing.load_signing_key().strip() == signing_pem.strip()


def test_rejects_an_item_that_is_not_a_key(keychain):
    (keychain / "items.json").write_text(json.dumps({signing.KEYCHAIN_SERVICE: {"value": "hunter2", "keychain": []}}))
    with pytest.raises(SigningError, match="doesn't hold a PEM key"):
        signing.load_signing_key()


def test_missing_key(keychain):
    with pytest.raises(SigningError, match="orb-hub keys init"):
        signing.load_signing_key()


def test_env_key_wins(keychain, signing_key, signing_pem):
    assert signing.load_signing_key() == signing_pem
    assert not (keychain / "calls.jsonl").exists()


def test_release_with_the_keychain_key(keychain, short_tmp, image_file, capsys):
    signing.keys_init(short_tmp / "pub.pem")
    assert cli.main(["release", str(image_file()), "--channel", "dev"]) == 0
    assert "Released orb 0.2.1+e1f2a3b" in capsys.readouterr().out
