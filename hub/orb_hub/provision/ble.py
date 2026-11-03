"""The BLE side of provisioning, with bleak (CoreBluetooth on macOS).

protocomm_ble exposes each endpoint (proto-ver, prov-session, prov-config, ...) as a characteristic of one
service: a request is a write, its response the following read. Characteristics are named by their user
description descriptor (0x2901); without one, an endpoint's UUID is the service UUID with the endpoint's 16-bit id
in place of characters 4-7 (protocomm_nimble stores it in bytes 12-13 of the little-endian UUID).
"""

from dataclasses import dataclass, field

from orb_hub.provision.session import ProvisioningError, TransportError

# espressif/network_provisioning's default service UUID (scheme_ble.c), unless the firmware sets its own.
DEFAULT_SERVICE_UUID = "1775244d-6b43-439b-877c-060f2d9bed07"
ENDPOINT_IDS = {
    "prov-ctrl": 0xFF4F,
    "prov-scan": 0xFF50,
    "prov-session": 0xFF51,
    "prov-config": 0xFF52,
    "proto-ver": 0xFF53,
}
USER_DESCRIPTION = "00002901-0000-1000-8000-00805f9b34fb"
NAME_PREFIX = "Orb-"


@dataclass
class Found:
    name: str
    address: str
    service_uuids: list[str] = field(default_factory=list)


def characteristic_uuid(service_uuid: str, endpoint_id: int) -> str:
    return f"{service_uuid[:4]}{endpoint_id:04x}{service_uuid[8:]}".lower()


async def scan(prefix: str = NAME_PREFIX, timeout: float = 6.0) -> list[Found]:
    """Devices advertising a name starting with prefix (an unprovisioned Orb advertises Orb-XXXX)."""
    from bleak import BleakScanner

    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    devices = []
    for device, adv in found.values():
        name = adv.local_name or device.name or ""
        if name.startswith(prefix):
            devices.append(Found(name=name, address=device.address, service_uuids=list(adv.service_uuids)))
    return sorted(devices, key=lambda d: d.name)


class BleTransport:
    def __init__(self, found: Found, client=None):
        self.found = found
        self._client = client
        self._endpoints: dict[str, str] = {}

    async def connect(self) -> None:
        if self._client is None:
            from bleak import BleakClient

            self._client = BleakClient(self.found.address, timeout=20)
        await self._client.connect()
        self._endpoints = await self._resolve()

    async def _resolve(self) -> dict[str, str]:
        # An Orb advertises exactly its provisioning service.
        uuids = self.found.service_uuids
        service = self._client.services.get_service(uuids[0] if len(uuids) == 1 else DEFAULT_SERVICE_UUID)
        if service is None:
            raise ProvisioningError(f"{self.found.name} doesn't offer the provisioning service")
        endpoints = {}
        for characteristic in service.characteristics:
            for descriptor in characteristic.descriptors:
                if descriptor.uuid.lower() == USER_DESCRIPTION:
                    raw = await self._client.read_gatt_descriptor(descriptor.handle)
                    endpoints[bytes(raw).decode(errors="replace").lower()] = characteristic.uuid
        for name, endpoint_id in ENDPOINT_IDS.items():
            endpoints.setdefault(name, characteristic_uuid(service.uuid, endpoint_id))
        return endpoints

    async def exchange(self, endpoint: str, data: bytes) -> bytes:
        from bleak.exc import BleakError

        uuid = self._endpoints[endpoint]
        try:
            await self._client.write_gatt_char(uuid, data, response=True)
            return bytes(await self._client.read_gatt_char(uuid))
        except BleakError as exc:
            raise TransportError(str(exc)) from exc

    async def close(self) -> None:
        if self._client is not None and self._client.is_connected:
            await self._client.disconnect()
