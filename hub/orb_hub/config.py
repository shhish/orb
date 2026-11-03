"""Where the hub keeps its files, its settings, and the facts about this Mac it hands out."""

import ipaddress
import os
import socket
import subprocess
from dataclasses import dataclass
from pathlib import Path

import ifaddr

DEFAULT_DATA_DIR = "~/.local/share/orb-hub"
DEFAULT_LOG_DIR = "~/Library/Logs/orb-hub"
CHANNELS = ("dev", "stable")


@dataclass
class Config:
    data_dir: Path
    log_dir: Path
    bind: str = "0.0.0.0"
    tls_port: int = 8443
    pair_port: int = 8080
    hub_name: str = ""
    tz: str = ""
    mdns: bool = True
    # docs/protocol.md: the hub closes a link that has been silent this long (the device pings every 15 s).
    idle_timeout: float = 45.0
    # A connection has this long to say hello or finish pair.begin and pair.reveal, pings or not; an HTTP
    # connection that sends no request this long is closed too.
    handshake_timeout: float = 10.0
    # Unconfirmed pairing requests expire after 5 minutes.
    pair_ttl: float = 300.0
    # Default timeout of a hub → device request.
    request_timeout: float = 10.0
    # How often the hub looks for changed LAN addresses to announce over mDNS.
    address_poll: float = 30.0
    # Newly paired devices follow dev builds; `orb-hub config <id> --channel stable` moves one.
    default_channel: str = "dev"
    # Provider observation/listeners are explicit serve options. Tests and a
    # foundation-only install never inspect the owner's running sessions.
    agents_enabled: bool = False
    enable_codex_queue: bool = False
    hooks_port: int = 8447
    mcp_enabled: bool = False
    mcp_port: int = 8445
    home_enabled: bool = False
    speech_enabled: bool = False
    nowplaying_enabled: bool = False

    @classmethod
    def load(cls, data_dir: Path | None = None, log_dir: Path | None = None, **overrides) -> "Config":
        """Settings from ORB_HUB_DATA / ORB_HUB_LOGS and the given overrides (None keeps the default)."""
        cfg = cls(
            data_dir=Path(data_dir or os.environ.get("ORB_HUB_DATA", DEFAULT_DATA_DIR)).expanduser(),
            log_dir=Path(log_dir or os.environ.get("ORB_HUB_LOGS", DEFAULT_LOG_DIR)).expanduser(),
        )
        for name, value in overrides.items():
            if value is not None:
                setattr(cfg, name, value)
        if not cfg.hub_name:
            cfg.hub_name = computer_name()
        if not cfg.tz:
            cfg.tz = local_timezone()
        return cfg

    @property
    def tls_dir(self) -> Path:
        return self.data_dir / "tls"

    @property
    def firmware_dir(self) -> Path:
        return self.data_dir / "firmware"

    @property
    def db_path(self) -> Path:
        return self.data_dir / "devices.db"

    @property
    def control_socket(self) -> Path:
        return self.data_dir / "control.sock"

    @property
    def device_log_dir(self) -> Path:
        return self.log_dir / "devices"


def ensure_private_dir(path: Path) -> Path:
    """Creates path (mode 700), tightening it if it already exists: it holds keys and tokens."""
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(path, 0o700)
    return path


def _scutil(key: str) -> str:
    try:
        out = subprocess.run(["/usr/sbin/scutil", "--get", key], capture_output=True, text=True, timeout=5, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


def computer_name() -> str:
    """The Mac's display name ("Mac mini"), used as hub_name and the mDNS instance name."""
    return _scutil("ComputerName") or socket.gethostname().split(".")[0]


def local_host_name() -> str:
    """The Mac's Bonjour host label, without ".local"."""
    return _scutil("LocalHostName") or socket.gethostname().split(".")[0]


def local_timezone() -> str:
    """The Mac's IANA zone name. macOS links /etc/localtime to .../zoneinfo/<Area>/<City>."""
    tz = os.environ.get("TZ", "").lstrip(":")
    if "/" in tz and not tz.startswith("/"):
        return tz
    try:
        target = os.readlink("/etc/localtime")
    except OSError:
        return "UTC"
    _, found, name = target.partition("zoneinfo/")
    return name if found and name else "UTC"


def lan_ipv4() -> list[str]:
    """Private IPv4 addresses of this Mac. VPN ranges such as Tailscale's 100.64/10 aren't private, so they're
    left out: a device on the LAN can't reach them."""
    found = set()
    for adapter in ifaddr.get_adapters():
        for ip in adapter.ips:
            if not ip.is_IPv4:
                continue
            addr = ipaddress.IPv4Address(ip.ip)
            if addr.is_private and not addr.is_loopback and not addr.is_link_local:
                found.add(addr)
    return [str(a) for a in sorted(found)]
