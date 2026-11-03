"""protocomm security 1 and the network_provisioning messages (ESP-IDF protocomm, espressif/network_provisioning
1.4: session.proto, sec1.proto, network_config.proto).

Security 1: X25519 key exchange, the shared key XORed with SHA-256 of the proof of possession, then AES-256-CTR
with the device's 16-byte random as the counter block. One CTR stream carries both directions in turn, exactly as
the device keeps it, so every request and response must pass through the same cipher in order.
"""

import hashlib
from dataclasses import dataclass

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

from orb_hub.provision.proto import ProtoError, decode, encode, get

# SessionData: sec_ver = 2, sec1 = 11. Sec1Payload: msg = 1, sc0 = 20, sr0 = 21, sc1 = 22, sr1 = 23.
SEC_SCHEME_1 = 1
SESSION_COMMAND0, SESSION_RESPONSE0, SESSION_COMMAND1, SESSION_RESPONSE1 = range(4)

# NetworkConfigPayload: msg = 1, then one of the oneof fields below.
CMD_GET_WIFI_STATUS, RESP_GET_WIFI_STATUS = 0, 1
CMD_SET_WIFI_CONFIG, RESP_SET_WIFI_CONFIG = 2, 3
CMD_APPLY_WIFI_CONFIG, RESP_APPLY_WIFI_CONFIG = 4, 5
PAYLOAD_FIELD = {
    CMD_GET_WIFI_STATUS: 10,
    RESP_GET_WIFI_STATUS: 11,
    CMD_SET_WIFI_CONFIG: 12,
    RESP_SET_WIFI_CONFIG: 13,
    CMD_APPLY_WIFI_CONFIG: 14,
    RESP_APPLY_WIFI_CONFIG: 15,
}

# constants.proto Status
STATUS = {
    0: "success",
    1: "invalid security scheme",
    2: "invalid proto",
    3: "too many sessions",
    4: "invalid argument",
    5: "internal error",
    6: "crypto error",
    7: "invalid session",
}
WIFI_STATES = {0: "connected", 1: "connecting", 2: "disconnected", 3: "failed"}
WIFI_FAIL_REASONS = {0: "wrong password", 1: "network not found"}


class ProvisioningError(Exception):
    pass


class TransportError(Exception):
    """A request to the device failed below the protocol (a BLE error)."""


class Security1:
    def __init__(self, pop: str, private_key: X25519PrivateKey | None = None):  # gitleaks:allow
        self._pop = pop.encode()
        self._key = private_key or X25519PrivateKey.generate()
        self._public = self._key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self._cipher = None

    def command0(self) -> bytes:
        return encode([(2, SEC_SCHEME_1), (11, [(1, SESSION_COMMAND0), (20, [(1, self._public)])])])

    def command1(self, response0: bytes) -> bytes:
        """Takes the device's public key and random, derives the session key and proves it to the device."""
        sr0 = _sec1_part(response0, SESSION_RESPONSE0, 21)
        device_public, device_random = get(sr0, 2, b""), get(sr0, 3, b"")
        if len(device_public) != 32 or len(device_random) != 16:
            raise ProvisioningError("the device sent a malformed session response")
        shared = self._key.exchange(X25519PublicKey.from_public_bytes(device_public))
        if self._pop:
            shared = bytes(a ^ b for a, b in zip(shared, hashlib.sha256(self._pop).digest()))
        self._cipher = Cipher(algorithms.AES(shared), modes.CTR(device_random)).encryptor()
        verify = self._cipher.update(device_public)
        return encode([(2, SEC_SCHEME_1), (11, [(1, SESSION_COMMAND1), (22, [(2, verify)])])])

    def finish(self, response1: bytes) -> None:
        """Checks the device's proof: our public key, encrypted with the same session key."""
        sr1 = _sec1_part(response1, SESSION_RESPONSE1, 23)
        if self._cipher.update(get(sr1, 3, b"")) != self._public:
            raise ProvisioningError("the device couldn't prove the session key: is the proof of possession right?")

    def encrypt(self, data: bytes) -> bytes:
        return self._cipher.update(data)

    def decrypt(self, data: bytes) -> bytes:
        return self._cipher.update(data)


def _sec1_part(data: bytes, expected_msg: int, field: int) -> dict:
    try:
        session = decode(data)
        if get(session, 2) != SEC_SCHEME_1:
            raise ProvisioningError("the device isn't using security 1")
        sec1 = decode(get(session, 11, b""))
        if get(sec1, 1) != expected_msg:
            raise ProvisioningError("unexpected session message from the device")
        part = decode(get(sec1, field, b""))
    except ProtoError as exc:
        raise ProvisioningError(f"malformed session message: {exc}") from None
    status = get(part, 1)
    if status:
        raise ProvisioningError(f"the device refused the session: {STATUS.get(status, status)}")
    return part


def set_wifi_config(ssid: bytes, passphrase: bytes) -> bytes:
    return encode([(1, CMD_SET_WIFI_CONFIG), (PAYLOAD_FIELD[CMD_SET_WIFI_CONFIG], [(1, ssid), (2, passphrase)])])


def apply_wifi_config() -> bytes:
    return encode([(1, CMD_APPLY_WIFI_CONFIG)])


def get_wifi_status() -> bytes:
    return encode([(1, CMD_GET_WIFI_STATUS), (PAYLOAD_FIELD[CMD_GET_WIFI_STATUS], [])])


def _config_payload(data: bytes, expected_msg: int) -> dict:
    try:
        payload = decode(data)
        if get(payload, 1) != expected_msg:
            raise ProvisioningError("unexpected answer from the device")
        return decode(get(payload, PAYLOAD_FIELD[expected_msg], b""))
    except ProtoError as exc:
        raise ProvisioningError(f"malformed answer from the device: {exc}") from None


def config_status(data: bytes, expected_msg: int) -> int:
    """The Status of a RespSetWifiConfig / RespApplyWifiConfig."""
    return get(_config_payload(data, expected_msg), 1)


@dataclass
class WifiStatus:
    state: str  # connected, connecting, disconnected, failed, or timeout (we stopped waiting)
    reason: str = ""  # why it failed
    ip: str = ""  # once connected
    attempts_remaining: int | None = None


def wifi_status(data: bytes) -> WifiStatus:
    resp = _config_payload(data, RESP_GET_WIFI_STATUS)
    status = get(resp, 1)
    if status:
        raise ProvisioningError(f"the device couldn't report its Wi-Fi state: {STATUS.get(status, status)}")
    result = WifiStatus(state=WIFI_STATES.get(get(resp, 2), "unknown"))
    try:
        if 10 in resp:
            result.reason = WIFI_FAIL_REASONS.get(get(resp, 10), "unknown reason")
        if 11 in resp:
            result.ip = get(decode(get(resp, 11, b"")), 1, b"").decode(errors="replace")
        if 12 in resp:
            result.attempts_remaining = get(decode(get(resp, 12, b"")), 1)
    except ProtoError as exc:
        raise ProvisioningError(f"malformed answer from the device: {exc}") from None
    return result
