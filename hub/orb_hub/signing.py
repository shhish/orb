"""The firmware signing key and espsecure (docs/protocol.md, "Releases").

The RSA-3072 private key lives in the login Keychain as generic password orb-hub-firmware-signing, written and
read with security(1) so that /usr/bin/security is the app the item trusts and reads never prompt. The PEM is
stored base64-encoded: `security -i` takes commands on stdin (so the key never shows in process arguments), one
line of at most 4 KB each, which a PEM's line breaks and a hex encoding (5 KB) wouldn't fit. Reads accept a
plain PEM too, for an item made by hand. ORB_SIGNING_KEY=<path to a PEM> replaces the Keychain, for tests.

Devices only install images signed with this key: ESP-IDF checks an update against the keys in the running
app's own signature block (Secure Boot v2 without hardware Secure Boot), so images flashed over USB need
signing too (`orb-hub sign`).
"""

import base64
import binascii
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa

from orb_hub.firmware import SECTOR, SIG_BLOCK_SIZE, signature_blocks

KEYCHAIN_SERVICE = "orb-hub-firmware-signing"
KEYCHAIN_ACCOUNT = "orb-hub"
KEYCHAIN = "login.keychain"
KEY_BITS = 3072
# security -i reads commands into a 4096-byte line buffer.
SECURITY_LINE_MAX = 4000
# security(1) exits with this when the item doesn't exist (errSecItemNotFound).
SECURITY_NOT_FOUND = 44


class SigningError(Exception):
    pass


def _security() -> str:
    return os.environ.get("ORB_HUB_SECURITY", "/usr/bin/security")


def _decode_item(raw: bytes) -> bytes:
    text = raw.strip()
    if text.startswith(b"-----BEGIN"):
        return text + b"\n"
    # security prints values with non-printable bytes (such as a PEM's line breaks) as hex.
    if re.fullmatch(rb"(?:[0-9a-fA-F]{2})+", text):
        decoded = bytes.fromhex(text.decode())
        if decoded.lstrip().startswith(b"-----BEGIN"):
            return decoded
    try:
        decoded = base64.b64decode(text, validate=True)
    except binascii.Error:
        decoded = b""
    if decoded.lstrip().startswith(b"-----BEGIN"):
        return decoded
    raise SigningError(f"the Keychain item {KEYCHAIN_SERVICE} doesn't hold a PEM key")


def keychain_read() -> bytes | None:
    """The signing key's PEM from the login Keychain, or None if there is no such item."""
    try:
        out = subprocess.run(
            [_security(), "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w", KEYCHAIN],
            capture_output=True,
            check=False,
            timeout=120,  # macOS may ask the user to unlock the Keychain
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SigningError(f"couldn't run security: {exc}") from None
    if out.returncode == SECURITY_NOT_FOUND:
        return None
    if out.returncode != 0:
        raise SigningError(f"security find-generic-password failed: {out.stderr.decode(errors='replace').strip()}")
    return _decode_item(out.stdout)


def keychain_store(pem: bytes) -> None:
    command = (
        f"add-generic-password -a {KEYCHAIN_ACCOUNT} -s {KEYCHAIN_SERVICE} -l {KEYCHAIN_SERVICE} "
        f"-w {base64.b64encode(pem).decode()} {KEYCHAIN}\n"
    )
    if len(command) > SECURITY_LINE_MAX:
        raise SigningError("the key is too long for security -i")
    try:
        out = subprocess.run([_security(), "-i"], input=command.encode(), capture_output=True, check=False, timeout=120)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise SigningError(f"couldn't run security: {exc}") from None
    if out.returncode != 0:
        raise SigningError(f"security add-generic-password failed: {out.stderr.decode(errors='replace').strip()}")
    # security -i doesn't always report a failed command in its exit status; reading back does.
    if keychain_read() != pem:
        raise SigningError("the key didn't read back from the Keychain")


def private_key(pem: bytes) -> rsa.RSAPrivateKey:
    try:
        key = serialization.load_pem_private_key(pem, password=None)
    except (ValueError, TypeError) as exc:
        raise SigningError(f"not a usable private key: {exc}") from None
    if not isinstance(key, rsa.RSAPrivateKey) or key.key_size != KEY_BITS:
        raise SigningError(f"the signing key must be RSA-{KEY_BITS}")
    return key


def public_pem(key: rsa.RSAPrivateKey) -> bytes:
    return key.public_key().public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)


def load_signing_key() -> bytes:
    """The signing key's PEM: ORB_SIGNING_KEY's file if set, else the Keychain item."""
    path = os.environ.get("ORB_SIGNING_KEY")
    if path:
        pem = Path(path).expanduser().read_bytes()
    else:
        pem = keychain_read()
        if pem is None:
            raise SigningError(f"no signing key in the Keychain ({KEYCHAIN_SERVICE}): run `orb-hub keys init`")
    private_key(pem)
    return pem


def keys_init(pub_out: Path) -> bool:
    """Creates the signing key in the Keychain unless it already exists, and writes its public key to pub_out.
    Returns whether a key was created. An existing key is never replaced: devices trust only the key their
    running image was signed with."""
    existing = keychain_read()
    if existing is None:
        key = rsa.generate_private_key(public_exponent=65537, key_size=KEY_BITS)
        keychain_store(
            key.private_bytes(
                serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()
            )
        )
    else:
        key = private_key(existing)
    pub_out.parent.mkdir(parents=True, exist_ok=True)
    pub_out.write_bytes(public_pem(key))
    return existing is None


def _espsecure(args: list[str], pass_fds: tuple[int, ...] = ()) -> None:
    # The venv's espsecure, whatever is on PATH.
    out = subprocess.run(
        [sys.executable, "-m", "espsecure", *args],
        capture_output=True,
        text=True,
        check=False,
        timeout=300,
        pass_fds=pass_fds,
    )
    if out.returncode != 0:
        detail = (out.stderr.strip() or out.stdout.strip()).splitlines()[-3:]
        raise SigningError(f"espsecure {args[0]} failed: {' '.join(detail)}")


def ensure_signed(image: bytes, key_pem: bytes, workdir: Path) -> bytes:
    """Returns image signed with the key (espsecure sign-data --version 2), signing it unless its first signature
    block already carries the key, and checks the signature with espsecure verify-signature.

    Only the first block counts: without hardware Secure Boot, ESP-IDF checks just that one block of an update
    against the running app's key, while verify-signature accepts any block. So an image whose first block holds
    another key, or isn't valid while a later one is, is refused rather than published for devices to reject."""
    key = private_key(key_pem)
    first, *later = signature_blocks(image)
    if first is not None and first.rsa_modulus != key.public_key().public_numbers().n:
        raise SigningError("the image is signed with a different key")
    if first is None and any(later):
        raise SigningError("the image's first signature block isn't valid, and devices check only that one")

    with tempfile.TemporaryDirectory(dir=workdir, prefix=".sign-") as tmp:
        unsigned_path = Path(tmp) / "unsigned.bin"
        signed_path = Path(tmp) / "signed.bin"
        if first is not None:
            signed_path.write_bytes(image)
        else:
            unsigned_path.write_bytes(image)
            # The private key reaches espsecure through a pipe, never a file.
            read_end, write_end = os.pipe()
            try:
                with os.fdopen(write_end, "wb") as w:
                    w.write(key_pem)  # a few KB: fits the pipe buffer, so this can't block
                _espsecure(
                    [
                        "sign-data",
                        "--version",
                        "2",
                        "--keyfile",
                        f"/dev/fd/{read_end}",
                        "--output",
                        str(signed_path),
                        str(unsigned_path),
                    ],
                    pass_fds=(read_end,),
                )
            finally:
                os.close(read_end)
        pub_path = Path(tmp) / "pub.pem"
        pub_path.write_bytes(public_pem(key))
        signed_image = signed_path.read_bytes()
        # espsecure accepts any valid signature block; the device accepts only block zero.
        # Erase later blocks in a verification copy so they cannot hide a broken first RSA signature.
        first_end = len(signed_image) - SECTOR + SIG_BLOCK_SIZE
        verify_path = Path(tmp) / "block-zero.bin"
        verify_path.write_bytes(signed_image[:first_end] + b"\xff" * (SECTOR - SIG_BLOCK_SIZE))
        _espsecure(["verify-signature", "--version", "2", "--keyfile", str(pub_path), str(verify_path)])
        return signed_image
