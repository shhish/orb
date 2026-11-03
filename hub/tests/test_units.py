"""Pure pieces: the pairing code, the certificate, envelopes, mDNS records, settings, the device store."""

import datetime
import hashlib
import ipaddress
import os
import socket
import stat

import pytest
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from orb_hub import config, envelope, mdns
from orb_hub.devlog import clean_line
from orb_hub.pairing import compute_sas
from orb_hub.store import Store, token_hash
from orb_hub.tls import ensure_cert


def test_sas_test_vector():
    # docs/protocol.md, "Pairing": both sides unit-test against this.
    cert_fp = hashlib.sha256(b"test-cert").digest()
    assert cert_fp.hex() == "ac978e63f273ba9ee1132bfecc103c767c52512fd5b065cc0ee224f9ec49d6a2"
    nonce_d = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
    nonce_h = bytes.fromhex("101112131415161718191a1b1c1d1e1f")
    # commit_d: pair.begin carries it, and pair.reveal the nonce, once nonce_h is known.
    commit_d = hashlib.sha256(nonce_d).hexdigest()
    assert commit_d == "be45cb2605bf36bebde684841a28f0fd43c69850a3dce5fedba69928ee3a8991"
    digest = hashlib.sha256(b"orb-pair-v1" + cert_fp + nonce_d + nonce_h).hexdigest()
    assert digest == "acbc0186c13758df7488335a8e95f3eba38e7818a36dad14e3be0940ebc3f554"
    assert compute_sas(cert_fp, nonce_d, nonce_h) == "002310"


def test_sas_changes_with_the_certificate():
    nonce = bytes(16)
    assert compute_sas(b"\x01" * 32, nonce, nonce) != compute_sas(b"\x02" * 32, nonce, nonce)


def test_certificate(short_tmp):
    tls_dir = short_tmp / "tls"
    cert = ensure_cert(tls_dir, ["orb.local", "orb-hub-demo-mac.local"], ["127.0.0.10", "127.0.0.1"])
    parsed = x509.load_pem_x509_certificate(cert.pem)
    assert parsed.subject.get_attributes_for_oid(NameOID.COMMON_NAME)[0].value == "orb-hub"
    assert parsed.issuer == parsed.subject
    assert isinstance(parsed.public_key(), ec.EllipticCurvePublicKey)
    assert parsed.public_key().curve.name == "secp256r1"
    lifetime = parsed.not_valid_after_utc - parsed.not_valid_before_utc
    assert datetime.timedelta(days=3650) <= lifetime <= datetime.timedelta(days=3652)
    san = parsed.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
    assert san.get_values_for_type(x509.DNSName) == ["orb.local", "orb-hub-demo-mac.local"]
    assert [str(ip) for ip in san.get_values_for_type(x509.IPAddress)] == ["127.0.0.10", "127.0.0.1"]
    assert parsed.extensions.get_extension_for_class(x509.BasicConstraints).value.ca
    eku = parsed.extensions.get_extension_for_class(x509.ExtendedKeyUsage).value
    assert ExtendedKeyUsageOID.SERVER_AUTH in eku
    assert cert.fingerprint == hashlib.sha256(cert.der).digest()
    assert stat.S_IMODE(os.stat(tls_dir / "hub.key").st_mode) == 0o600
    assert stat.S_IMODE(os.stat(tls_dir).st_mode) == 0o700
    # Created once: later runs load the same certificate whatever the addresses are now.
    assert ensure_cert(tls_dir, ["other.local"], ["172.16.0.3"]).der == cert.der


def test_envelope_parse_and_encode():
    assert envelope.parse('{"t":"req","id":3,"m":"ota.check"}').p == {}
    assert envelope.parse('{"t":"res","id":3,"ok":true,"r":{"a":1}}').r == {"a": 1}
    assert envelope.parse('{"t":"evt","m":"x","p":{"b":2}}').p == {"b": 2}
    for text, req_id in (
        ("[]", None),
        ("{", None),
        ('{"t":"req","id":0,"m":"x"}', None),
        ('{"t":"req","id":true,"m":"x"}', None),
        ('{"t":"req","id":4,"m":7}', 4),
        ('{"t":"req","id":5,"m":"x","p":[]}', 5),
        ('{"t":"res","id":1}', None),
        ('{"t":"evt","p":{}}', None),
        ('{"t":"bogus"}', None),
    ):
        with pytest.raises(envelope.EnvelopeError) as info:
            envelope.parse(text)
        assert info.value.req_id == req_id, text
    assert envelope.encode(envelope.success(1, {"é": "ü"})) == '{"t":"res","id":1,"ok":true,"r":{"é":"ü"}}'
    with pytest.raises(ValueError):
        envelope.encode(envelope.event("x", {"s": "a" * envelope.MAX_MESSAGE}))


def test_mdns_service_info():
    info = mdns.service_info("Mac mini", mdns.mdns_host("Demo-Mac"), 8443, 8080, "ab" * 32, ["127.0.0.10"])
    assert info.type == "_orb-hub._tcp.local."
    assert info.name == "Mac mini._orb-hub._tcp.local."
    assert info.server == "orb-hub-demo-mac.local."
    assert info.port == 8443
    assert info.properties == {b"v": b"1", b"pair": b"8080", b"fp": b"abababababababab"}
    assert info.addresses == [socket.inet_aton("127.0.0.10")]


@pytest.mark.parametrize("payload", ["NaN", "1e999", '"\\ud800"', "[" * 33 + "0" + "]" * 33])
def test_envelope_rejects_unrenderable_or_unbounded_values(payload):
    with pytest.raises(envelope.EnvelopeError) as error:
        envelope.parse('{"t":"req","id":1,"m":"message.send","p":{"text":' + payload + "}}")
    assert error.value.req_id == 1


def test_timezone_and_addresses(monkeypatch):
    monkeypatch.setenv("TZ", "Europe/London")
    assert config.local_timezone() == "Europe/London"
    monkeypatch.setenv("TZ", "EST5EDT")
    zone = config.local_timezone()
    assert "/" in zone or zone == "UTC"
    for ip in config.lan_ipv4():
        address = ipaddress.IPv4Address(ip)
        assert address.is_private and not address.is_loopback and not address.is_link_local


def test_private_dirs(short_tmp):
    loose = short_tmp / "loose"
    loose.mkdir(mode=0o755)
    config.ensure_private_dir(loose)
    assert stat.S_IMODE(os.stat(loose).st_mode) == 0o700


def test_store(short_tmp):
    store = Store(short_tmp / "devices.db")
    token = "ab" * 32
    device = store.pair("orb-020000abcdef", "Orb", "model", "0.2.0", token, "dev")
    assert device.token_sha256 == token_hash(token) == hashlib.sha256(bytes.fromhex(token)).hexdigest()
    assert device.token_matches(token.upper()) and not device.token_matches("cd" * 32)
    assert store.find_by_token(token).device_id == "orb-020000abcdef"
    store.set_config("orb-020000abcdef", name="Desk", channel="stable")
    # Pairing again replaces the token but keeps what the hub set.
    store.pair("orb-020000abcdef", "Orb", "model", "0.2.1", "cd" * 32, "dev")
    device = store.get("orb-020000abcdef")
    assert (device.name, device.channel, device.fw) == ("Desk", "stable", "0.2.1")
    assert store.find_by_token(token) is None
    store.update_ota("orb-020000abcdef", failed={"version": "1"}, last={"result": "ok"})
    store.update_ota("orb-020000abcdef", failed=None)
    assert store.get("orb-020000abcdef").ota == {"last": {"result": "ok"}}
    assert stat.S_IMODE(os.stat(short_tmp / "devices.db").st_mode) == 0o600
    assert store.unpair("orb-020000abcdef") and not store.unpair("orb-020000abcdef")
    store.close()


def test_clean_log_line():
    assert clean_line("\x1b[0;32mI (12) main: up\x1b[0m\r\n") == "I (12) main: up"
    assert clean_line("a\nb\x07") == "a b?"
    assert len(clean_line("x" * 5000)) == 1024
