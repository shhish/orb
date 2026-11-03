"""The hub's one self-signed certificate (docs/protocol.md, "Identity and TLS").

Devices pin it: they get the PEM once over plain HTTP while pairing, check the 6-digit code that hashes its
fingerprint, and from then on trust nothing else. So the certificate must never change after devices pair; it
is created on first run and reused as long as both files exist.
"""

import datetime
import hashlib
import ipaddress
import os
import secrets
import ssl
from dataclasses import dataclass
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from orb_hub.config import ensure_private_dir

COMMON_NAME = "orb-hub"
VALIDITY = datetime.timedelta(days=3650)
# Covers a Mac clock that is slightly ahead of the device's; the device doesn't check dates (no RTC before SNTP).
BACKDATE = datetime.timedelta(days=1)


@dataclass(frozen=True)
class HubCert:
    pem: bytes
    der: bytes
    key_path: Path
    pem_path: Path

    @property
    def fingerprint(self) -> bytes:
        """cert_fp: SHA-256 of the DER certificate (32 bytes)."""
        return hashlib.sha256(self.der).digest()

    @property
    def fingerprint_hex(self) -> str:
        return self.fingerprint.hex()

    @property
    def dns_names(self) -> list[str]:
        """The host names in the certificate's SAN, as they were when it was made."""
        cert = x509.load_der_x509_certificate(self.der)
        try:
            san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
        except x509.ExtensionNotFound:
            return []
        return san.get_values_for_type(x509.DNSName)


def ensure_cert(tls_dir: Path, hostnames: list[str], ips: list[str]) -> HubCert:
    """Loads the hub certificate, creating it (and hub.key, mode 600) on first run."""
    ensure_private_dir(tls_dir)
    key_path = tls_dir / "hub.key"
    pem_path = tls_dir / "hub.pem"
    if key_path.exists() and pem_path.exists():
        return _load(key_path, pem_path)

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, COMMON_NAME)])
    now = datetime.datetime.now(datetime.UTC)
    san = [x509.DNSName(h) for h in dict.fromkeys(hostnames)]
    san += [x509.IPAddress(ipaddress.ip_address(ip)) for ip in dict.fromkeys(ips)]
    public = key.public_key()
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(public)
        .serial_number(int.from_bytes(secrets.token_bytes(16), "big") >> 1)
        .not_valid_before(now - BACKDATE)
        .not_valid_after(now + VALIDITY)
        # A self-signed certificate is its own CA: devices load it as their only trusted root.
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .add_extension(x509.SubjectAlternativeName(san), critical=False)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(public), critical=False)
        .add_extension(x509.AuthorityKeyIdentifier.from_issuer_public_key(public), critical=False)
        .sign(key, hashes.SHA256())
    )

    key_pem = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
    )
    _write_new(key_path, key_pem, 0o600)
    _write_new(pem_path, cert.public_bytes(serialization.Encoding.PEM), 0o644)
    return _load(key_path, pem_path)


def server_context(cert: HubCert) -> ssl.SSLContext:
    ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(cert.pem_path, cert.key_path)
    return ctx


def _load(key_path: Path, pem_path: Path) -> HubCert:
    pem = pem_path.read_bytes()
    der = x509.load_pem_x509_certificate(pem).public_bytes(serialization.Encoding.DER)
    return HubCert(pem=pem, der=der, key_path=key_path, pem_path=pem_path)


def _write_new(path: Path, data: bytes, mode: int) -> None:
    # Opened with its final mode so the key is never readable by others, even briefly.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    with os.fdopen(fd, "wb") as f:
        f.write(data)
    os.chmod(path, mode)
