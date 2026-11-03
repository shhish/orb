"""`orb-hub setup`: gives an unprovisioned Orb the home Wi-Fi over BLE (docs/protocol.md, "Wi-Fi setup").

The device runs espressif/network_provisioning with security 1 and shows its service name (Orb-XXXX) and an
8-digit proof of possession. The Wi-Fi password is read with getpass and only travels encrypted to the device:
nothing here prints, logs or stores it.
"""

import asyncio
import getpass
import json
import re
from collections.abc import Callable
from typing import Protocol

from orb_hub.provision import ble
from orb_hub.provision.session import (
    RESP_APPLY_WIFI_CONFIG,
    RESP_SET_WIFI_CONFIG,
    STATUS,
    ProvisioningError,
    Security1,
    TransportError,
    WifiStatus,
    apply_wifi_config,
    config_status,
    get_wifi_status,
    set_wifi_config,
    wifi_status,
)

POP = re.compile(r"^\d{8}$")


class Transport(Protocol):
    async def exchange(self, endpoint: str, data: bytes) -> bytes:
        """Sends a request to an endpoint and returns its response. TransportError or OSError on failure."""


async def read_version(transport: Transport) -> dict:
    """proto-ver's JSON ({"prov": {"ver", "sec_ver", "cap"}}), or {} from firmware that only sends a version."""
    try:
        info = json.loads(await transport.exchange("proto-ver", b"---"))
    except ValueError:
        return {}
    return info if isinstance(info, dict) else {}


def check_version(info: dict) -> None:
    prov = info.get("prov") if isinstance(info.get("prov"), dict) else {}
    sec_ver = prov.get("sec_ver")
    if sec_ver is not None and sec_ver != 1:
        raise ProvisioningError(f"the device uses security {sec_ver}; orb-hub setup speaks security 1")
    caps = prov.get("cap")
    if isinstance(caps, list) and caps and "wifi_prov" not in caps:
        raise ProvisioningError("the device isn't taking Wi-Fi settings right now")


async def establish(transport: Transport, sec: Security1) -> None:
    try:
        response0 = await transport.exchange("prov-session", sec.command0())
        response1 = await transport.exchange("prov-session", sec.command1(response0))
    except (TransportError, OSError) as exc:
        # A device whose PoP doesn't match drops the session, which surfaces as a BLE error.
        raise ProvisioningError(f"the device ended the session ({exc}); check the proof of possession") from None
    sec.finish(response1)


async def _config(transport: Transport, sec: Security1, request: bytes) -> bytes:
    return sec.decrypt(await transport.exchange("prov-config", sec.encrypt(request)))


async def provision(
    transport: Transport,
    pop: str,
    ssid: str,
    passphrase: str,
    *,
    poll_interval: float = 2.0,
    timeout: float = 60.0,
    on_status: Callable[[WifiStatus], None] | None = None,
) -> WifiStatus:
    """Sends the network to the device, applies it and waits until the device has joined or given up."""
    check_version(await read_version(transport))
    sec = Security1(pop)
    await establish(transport, sec)
    status = config_status(
        await _config(transport, sec, set_wifi_config(ssid.encode(), passphrase.encode())), RESP_SET_WIFI_CONFIG
    )
    if status:
        raise ProvisioningError(f"the device didn't take the network settings: {STATUS.get(status, status)}")
    status = config_status(await _config(transport, sec, apply_wifi_config()), RESP_APPLY_WIFI_CONFIG)
    if status:
        raise ProvisioningError(f"the device couldn't apply the network settings: {STATUS.get(status, status)}")

    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        await asyncio.sleep(poll_interval)
        state = wifi_status(await _config(transport, sec, get_wifi_status()))
        if on_status:
            on_status(state)
        if state.state in ("connected", "failed"):
            return state
        if loop.time() >= deadline:
            return WifiStatus(state="timeout")


def check_network(ssid: str, password: str) -> None:
    if not 1 <= len(ssid.encode()) <= 32:
        raise ProvisioningError("a network name (SSID) is 1-32 bytes")
    if len(password.encode()) > 64 or 0 < len(password) < 8:
        raise ProvisioningError("a Wi-Fi password is 8-63 characters (or 64 hex digits); empty for an open network")


def _choose(found: list[ble.Found], ask: Callable[[str], str], out: Callable[[str], None]) -> ble.Found:
    for i, device in enumerate(found, 1):
        out(f"  {i}. {device.name}")
    answer = ask(f"Which one (1-{len(found)})? ").strip()
    if not answer.isdigit() or not 1 <= int(answer) <= len(found):
        raise ProvisioningError("no device chosen")
    return found[int(answer) - 1]


async def _connect_and_provision(
    transport: ble.BleTransport, pop: str, ssid: str, password: str, out: Callable[[str], None], poll_interval: float
) -> WifiStatus:
    def report(state: WifiStatus) -> None:
        if state.state == "connecting" and state.attempts_remaining is not None:
            out(f"Joining… ({state.attempts_remaining} attempts left)")
        elif state.state in ("connecting", "disconnected"):
            out("Joining…")

    await transport.connect()
    try:
        return await provision(transport, pop, ssid, password, poll_interval=poll_interval, on_status=report)
    finally:
        await transport.close()


def run_setup(
    name: str | None = None,
    ssid: str | None = None,
    *,
    ask: Callable[[str], str] = input,
    ask_secret: Callable[[str], str] = getpass.getpass,
    out: Callable[[str], None] = print,
    scan=ble.scan,
    make_transport=ble.BleTransport,
    poll_interval: float = 2.0,
) -> tuple[str, WifiStatus]:
    """The interactive setup. Returns the device's name and how joining the network ended."""
    out("Looking for an Orb waiting for Wi-Fi…")
    found = asyncio.run(scan())
    if name:
        found = [d for d in found if d.name == name]
    if not found:
        raise ProvisioningError(f"no {name or 'Orb'} is waiting for Wi-Fi nearby (its screen shows Orb-XXXX)")
    device = found[0] if len(found) == 1 else _choose(found, ask, out)
    out(f"Found {device.name}.")

    pop = ask("Proof of possession (the 8 digits on its screen): ").strip()
    if not POP.match(pop):
        raise ProvisioningError("the proof of possession is the 8 digits on the Orb's screen")
    ssid = ssid or ask("Wi-Fi network: ")
    password = ask_secret(f"Password for {ssid} (not shown): ")
    check_network(ssid, password)
    transport = make_transport(device)
    return device.name, asyncio.run(_connect_and_provision(transport, pop, ssid, password, out, poll_interval))
