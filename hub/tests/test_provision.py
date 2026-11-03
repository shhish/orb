"""BLE Wi-Fi provisioning: wire format, security 1, the flow against a simulated device, the BLE transport."""

import hashlib
import json
import logging
import os

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

from orb_hub import cli
from orb_hub.provision import ble, client, session
from orb_hub.provision.proto import ProtoError, decode, encode, get
from orb_hub.provision.session import ProvisioningError, Security1

K = bytes(range(32))
V = bytes(range(32, 64))
R = bytes(range(64, 80))

# Produced by protobuf 7 with ESP-IDF 6.1's protocomm *_pb2.py and network_provisioning 1.4's
# network_config_pb2.py (synthetic loopback network), i.e. what esp_prov sends and what the device's protobuf-c code expects.
REFERENCE = {
    "cmd0": "10015a25a201220a20000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
    "cmd1": "10015a270802b201221220202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
    "resp0": "10015a390801aa01341220000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
    "1a10404142434445464748494a4b4c4d4e4f",
    "resp1": "10015a270803ba01221a20202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f",
    "set": "080262130a07486f6d654e6574120868756e7465723232",
    "apply": "0804",
    "get_status": "5200",
    "resp_set": "08036a00",
    "resp_apply": "08057a00",
    "status_connected": "08015a225a200a093132372e302e302e3110031a07486f6d654e657422060000000000002806",
    "status_failed_auth": "08015a0410035000",
    "status_failed_not_found": "08015a0410035001",
    "status_connecting": "08015a06100162020802",
}


def ref(name: str) -> bytes:
    return bytes.fromhex(REFERENCE[name])


# -- wire format


def test_requests_match_the_reference_encoding():
    assert encode([(2, 1), (11, [(1, 0), (20, [(1, K)])])]) == ref("cmd0")
    assert encode([(2, 1), (11, [(1, 2), (22, [(2, V)])])]) == ref("cmd1")
    assert session.set_wifi_config(b"HomeNet", b"hunter22") == ref("set")
    assert session.apply_wifi_config() == ref("apply")
    assert session.get_wifi_status() == ref("get_status")


def test_responses_parse():
    assert session.config_status(ref("resp_set"), session.RESP_SET_WIFI_CONFIG) == 0
    assert session.config_status(ref("resp_apply"), session.RESP_APPLY_WIFI_CONFIG) == 0
    assert session.wifi_status(ref("status_connected")) == session.WifiStatus("connected", ip="127.0.0.1")
    assert session.wifi_status(ref("status_failed_auth")) == session.WifiStatus("failed", reason="wrong password")
    assert session.wifi_status(ref("status_failed_not_found")).reason == "network not found"
    assert session.wifi_status(ref("status_connecting")) == session.WifiStatus("connecting", attempts_remaining=2)
    with pytest.raises(ProvisioningError, match="unexpected"):
        session.config_status(ref("resp_set"), session.RESP_APPLY_WIFI_CONFIG)


def test_proto_round_trip_and_errors():
    data = encode([(1, 300), (2, b"abc"), (3, [(1, 1)]), (4, -1), (5, 0), (6, b"")])
    fields = decode(data)
    assert get(fields, 1) == 300 and get(fields, 2) == b"abc" and decode(get(fields, 3)) == {1: [1]}
    assert get(fields, 4) == (1 << 64) - 1
    assert 5 not in fields and 6 not in fields
    for bad in (b"\x0a\x05ab", b"\x08", b"\x0b"):
        with pytest.raises(ProtoError):
            decode(bad)


# -- a simulated device: protocomm security 1 and network_provisioning's config handler


class SimulatedOrb:
    def __init__(self, pop: str, outcome: str = "connected", polls_before_outcome: int = 1):
        self.pop = pop.encode()
        self.key = X25519PrivateKey.generate()
        self.public = self.key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.random = os.urandom(16)
        self.cipher = None
        self.client_public = None
        self.outcome = outcome
        self.polls = polls_before_outcome
        self.config: dict = {}
        self.applied = False
        self.connected = False
        self.closed = False

    async def connect(self):
        self.connected = True

    async def close(self):
        self.closed = True

    async def exchange(self, endpoint: str, data: bytes) -> bytes:
        if endpoint == "proto-ver":
            assert data == b"---"
            return json.dumps({"prov": {"ver": "v1.1", "sec_ver": 1, "cap": ["wifi_prov", "wifi_scan"]}}).encode()
        if endpoint == "prov-session":
            return self._session(data)
        assert endpoint == "prov-config"
        return self.cipher.update(self._config(self.cipher.update(data)))

    def _session(self, data: bytes) -> bytes:
        sec1 = decode(get(decode(data), 11))
        if get(sec1, 1) == session.SESSION_COMMAND0:
            self.client_public = get(decode(get(sec1, 20)), 1)
            shared = self.key.exchange(X25519PublicKey.from_public_bytes(self.client_public))
            shared = bytes(a ^ b for a, b in zip(shared, hashlib.sha256(self.pop).digest()))
            self.cipher = Cipher(algorithms.AES(shared), modes.CTR(self.random)).encryptor()
            return encode([(2, 1), (11, [(1, 1), (21, [(2, self.public), (3, self.random)])])])
        if self.cipher.update(get(decode(get(sec1, 22)), 2)) != self.public:
            # protocomm drops the session; over BLE the write fails.
            raise OSError("ATT error: unlikely error")
        return encode([(2, 1), (11, [(1, 3), (23, [(3, self.cipher.update(self.client_public))])])])

    def _config(self, request: bytes) -> bytes:
        payload = decode(request)
        msg = get(payload, 1)
        if msg == session.CMD_SET_WIFI_CONFIG:
            body = decode(get(payload, 12))
            self.config = {"ssid": get(body, 1, b""), "passphrase": get(body, 2, b"")}
            return encode([(1, session.RESP_SET_WIFI_CONFIG), (13, [])])
        if msg == session.CMD_APPLY_WIFI_CONFIG:
            self.applied = True
            return encode([(1, session.RESP_APPLY_WIFI_CONFIG), (15, [])])
        assert msg == session.CMD_GET_WIFI_STATUS and 10 in payload
        if self.polls > 0:
            self.polls -= 1
            return ref("status_connecting")
        return ref({"connected": "status_connected", "failed": "status_failed_auth"}[self.outcome])


async def test_security1_session_and_provisioning():
    orb = SimulatedOrb("12345678", polls_before_outcome=2)
    seen = []
    result = await client.provision(orb, "12345678", "HomeNet", "hunter22", poll_interval=0, on_status=seen.append)
    assert result == session.WifiStatus("connected", ip="127.0.0.1")
    assert orb.config == {"ssid": b"HomeNet", "passphrase": b"hunter22"}
    assert orb.applied
    assert [s.state for s in seen] == ["connecting", "connecting", "connected"]


async def test_wrong_pop_is_reported():
    with pytest.raises(ProvisioningError, match="proof of possession"):
        await client.provision(SimulatedOrb("12345678"), "87654321", "HomeNet", "hunter22", poll_interval=0)


async def test_device_that_cannot_join():
    result = await client.provision(SimulatedOrb("1", outcome="failed"), "1", "HomeNet", "wrongpass", poll_interval=0)
    assert result == session.WifiStatus("failed", reason="wrong password")


async def test_gives_up_waiting():
    result = await client.provision(
        SimulatedOrb("1", polls_before_outcome=1000), "1", "Net", "password", poll_interval=0.01, timeout=0.05
    )
    assert result.state == "timeout"


async def test_device_proof_is_checked():
    sec = Security1("12345678")
    orb = SimulatedOrb("12345678")
    response1 = await orb.exchange("prov-session", sec.command1(await orb.exchange("prov-session", sec.command0())))
    tampered = bytearray(response1)
    tampered[-1] ^= 1
    with pytest.raises(ProvisioningError, match="couldn't prove"):
        sec.finish(bytes(tampered))


def test_version_checks():
    client.check_version({})
    client.check_version({"prov": {"sec_ver": 1, "cap": ["wifi_prov"]}})
    with pytest.raises(ProvisioningError, match="security 2"):
        client.check_version({"prov": {"sec_ver": 2}})
    with pytest.raises(ProvisioningError, match="isn't taking"):
        client.check_version({"prov": {"sec_ver": 1, "cap": ["thread_prov"]}})


@pytest.mark.parametrize(
    "ssid, password", [("", "password"), ("x" * 33, "password"), ("Net", "short"), ("Net", "p" * 65)]
)
def test_network_checks(ssid, password):
    with pytest.raises(ProvisioningError):
        client.check_network(ssid, password)


# -- the interactive command


def _setup(monkeypatch, orb, answers, found=None, **kwargs):
    async def scan():
        return found if found is not None else [ble.Found("Orb-CDEF", "uuid-1", [])]

    prompts, secrets_asked, printed = [], [], []

    def ask(prompt):
        prompts.append(prompt)
        return answers.pop(0)

    def ask_secret(prompt):
        secrets_asked.append(prompt)
        return "correct horse battery"

    result = client.run_setup(
        ask=ask,
        ask_secret=ask_secret,
        out=printed.append,
        scan=scan,
        make_transport=lambda found: orb,
        poll_interval=0,
        **kwargs,
    )
    return result, prompts, secrets_asked, printed


def test_run_setup_reads_the_password_with_getpass_and_never_shows_it(monkeypatch, caplog):
    caplog.set_level(logging.DEBUG)
    orb = SimulatedOrb("12345678")
    (name, status), prompts, secrets_asked, printed = _setup(monkeypatch, orb, ["12345678", "HomeNet"])
    assert (name, status.state, status.ip) == ("Orb-CDEF", "connected", "127.0.0.1")
    assert orb.config["passphrase"] == b"correct horse battery"
    assert len(secrets_asked) == 1 and "HomeNet" in secrets_asked[0]
    assert all("Password" not in p for p in prompts)
    assert orb.connected and orb.closed
    shown = "\n".join(printed + prompts + secrets_asked + [r.getMessage() for r in caplog.records])
    assert "correct horse battery" not in shown


def test_run_setup_choices_and_errors(monkeypatch):
    two = [ble.Found("Orb-0001", "a"), ble.Found("Orb-0002", "b")]
    (name, _), _, _, _ = _setup(monkeypatch, SimulatedOrb("12345678"), ["2", "12345678", "Net"], found=two)
    assert name == "Orb-0002"
    (name, _), _, _, _ = _setup(
        monkeypatch, SimulatedOrb("12345678"), ["12345678"], found=two, name="Orb-0001", ssid="Net"
    )
    assert name == "Orb-0001"
    with pytest.raises(ProvisioningError, match="no Orb is waiting"):
        _setup(monkeypatch, SimulatedOrb("1"), [], found=[])
    with pytest.raises(ProvisioningError, match="8 digits"):
        _setup(monkeypatch, SimulatedOrb("1"), ["1234"])


def test_setup_command_reports_failure(monkeypatch, capsys):
    def fake_run_setup(name=None, ssid=None):
        return "Orb-CDEF", session.WifiStatus("failed", reason="network not found")

    monkeypatch.setattr(client, "run_setup", fake_run_setup)
    assert cli.main(["setup"]) == 1
    assert "Orb-CDEF couldn't join: network not found" in capsys.readouterr().err


# -- the BLE transport, against a stand-in for bleak's client


class FakeDescriptor:
    def __init__(self, handle, uuid=ble.USER_DESCRIPTION):
        self.handle, self.uuid = handle, uuid


class FakeCharacteristic:
    def __init__(self, uuid, descriptors):
        self.uuid, self.descriptors = uuid, descriptors


class FakeService:
    def __init__(self, uuid, characteristics):
        self.uuid, self.characteristics = uuid, characteristics


class FakeBleakClient:
    """Named characteristics for proto-ver and prov-session only; prov-config must come from the UUID rule."""

    SERVICE = "0000ffff-0000-1000-8000-00805f9b34fb"

    def __init__(self, orb):
        self.orb = orb
        self.is_connected = False
        self.names = {1: b"proto-ver", 2: b"prov-session"}
        self.pending = {}
        service = FakeService(
            self.SERVICE,
            [
                FakeCharacteristic("0000ff53-0000-1000-8000-00805f9b34fb", [FakeDescriptor(1)]),
                FakeCharacteristic("0000ff51-0000-1000-8000-00805f9b34fb", [FakeDescriptor(2)]),
                FakeCharacteristic("0000ff52-0000-1000-8000-00805f9b34fb", [FakeDescriptor(3, uuid="00002902-x")]),
            ],
        )
        self.services = type(
            "Services", (), {"get_service": lambda _, uuid: service if uuid == self.SERVICE else None}
        )()
        self.by_uuid = {
            "0000ff53-0000-1000-8000-00805f9b34fb": "proto-ver",
            "0000ff51-0000-1000-8000-00805f9b34fb": "prov-session",
            "0000ff52-0000-1000-8000-00805f9b34fb": "prov-config",
        }

    async def connect(self):
        self.is_connected = True

    async def disconnect(self):
        self.is_connected = False

    async def read_gatt_descriptor(self, handle):
        return bytearray(self.names[handle])

    async def write_gatt_char(self, uuid, data, response):
        assert response
        self.pending[uuid] = await self.orb.exchange(self.by_uuid[uuid], bytes(data))

    async def read_gatt_char(self, uuid):
        return bytearray(self.pending.pop(uuid))


def test_characteristic_uuid_rule():
    assert ble.characteristic_uuid(ble.DEFAULT_SERVICE_UUID, 0xFF52) == "1775ff52-6b43-439b-877c-060f2d9bed07"


async def test_ble_transport_end_to_end():
    orb = SimulatedOrb("12345678")
    fake = FakeBleakClient(orb)
    transport = ble.BleTransport(ble.Found("Orb-CDEF", "uuid-1", [FakeBleakClient.SERVICE]), client=fake)
    await transport.connect()
    result = await client.provision(transport, "12345678", "HomeNet", "hunter22", poll_interval=0)
    assert result.state == "connected"
    await transport.close()
    assert not fake.is_connected


async def test_ble_transport_needs_the_service():
    transport = ble.BleTransport(ble.Found("Orb-CDEF", "uuid-1", []), client=FakeBleakClient(SimulatedOrb("1")))
    with pytest.raises(ProvisioningError, match="provisioning service"):
        await transport.connect()
