"""mDNS advertisement of _orb-hub._tcp (docs/protocol.md, "Discovery").

The service's SRV record names its own host, orb-hub-<LocalHostName>.local, with A records for the LAN
addresses. Announcing the Mac's own host name instead would compete with mDNSResponder, which owns that name:
an A record set that differs from its own (one interface's address versus all of them) reads as a conflict and
can make macOS rename the Mac ("Demo-Mac-2").

Devices check the certificate against the SRV host name, and the certificate never changes, so the name is the one
the certificate was made with even after the Mac is renamed; the addresses follow the LAN.
"""

import ipaddress
import logging
import socket

from zeroconf import IPVersion
from zeroconf.asyncio import AsyncServiceInfo, AsyncZeroconf

SERVICE_TYPE = "_orb-hub._tcp.local."
PROTOCOL_VERSION = "1"
HOST_PREFIX = "orb-hub-"

log = logging.getLogger(__name__)


def mdns_host(local_host_name: str) -> str:
    return f"{HOST_PREFIX}{local_host_name.lower()}.local"


def advertised_host(cert_names: list[str]) -> str | None:
    """The SRV target: the orb-hub-*.local name the certificate was made with, or None if it has none."""
    return next((n for n in cert_names if n.startswith(HOST_PREFIX) and n.endswith(".local")), None)


def packed(addresses: list[str]) -> list[bytes]:
    return [socket.inet_aton(str(ipaddress.IPv4Address(a))) for a in addresses]


def service_info(
    hub_name: str, host: str, port: int, pair_port: int, cert_fp_hex: str, addresses: list[str]
) -> AsyncServiceInfo:
    return AsyncServiceInfo(
        SERVICE_TYPE,
        f"{hub_name}.{SERVICE_TYPE}",
        port=port,
        properties={"v": PROTOCOL_VERSION, "pair": str(pair_port), "fp": cert_fp_hex[:16]},
        server=f"{host}.",
        addresses=packed(addresses),
    )


class Advertiser:
    def __init__(self, info: AsyncServiceInfo):
        self.info = info
        self._zc: AsyncZeroconf | None = None

    async def start(self) -> None:
        self._zc = AsyncZeroconf(ip_version=IPVersion.V4Only)
        # Another hub with the same name gets "Mac mini (2)" rather than a failure.
        await self._zc.async_register_service(self.info, allow_name_change=True)
        log.info("advertising %s on port %d", self.info.name, self.info.port)

    async def update(self, addresses: list[str]) -> None:
        """Re-announces the host's A records with the hub's current LAN addresses."""
        self.info.addresses = packed(addresses)
        if self._zc:
            await self._zc.async_update_service(self.info)

    async def stop(self) -> None:
        if self._zc:
            await self._zc.async_unregister_all_services()
            await self._zc.async_close()
            self._zc = None
