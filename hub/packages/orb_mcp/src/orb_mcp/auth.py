"""Per-client 32-byte bearer credentials in private files."""

import hmac
import ipaddress
import os
import secrets
import stat
from pathlib import Path
from urllib.parse import urlsplit

PRINCIPALS = ("dot", "claude", "codex")


def read_token(path: Path) -> str:
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if (
            not stat.S_ISREG(info.st_mode)
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_uid != os.getuid()
        ):
            raise ValueError("Token file must be an owned regular file with mode 0600")
        value = os.read(fd, 1024).decode("ascii").strip()
        if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
            raise ValueError("Token file must contain a 32-byte lowercase hex token")
        return value
    finally:
        os.close(fd)


def provision_tokens(directory: Path) -> dict[str, Path]:
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    paths = {p: directory / f"{p}.token" for p in PRINCIPALS}
    for path in paths.values():
        try:
            fd = os.open(
                path, os.O_CREAT | os.O_EXCL | os.O_WRONLY | os.O_NOFOLLOW, 0o600
            )
        except FileExistsError:
            read_token(path)
        else:
            with os.fdopen(fd, "w") as f:
                f.write(secrets.token_hex(32) + "\n")
                f.flush()
                os.fsync(f.fileno())
    return paths


class Tokens:
    def __init__(self, paths: dict[str, Path]):
        if not paths or not set(paths) <= set(PRINCIPALS):
            raise ValueError("Expected dot, claude, or codex client tokens")
        self.paths = paths
        values = [read_token(p) for p in paths.values()]
        if len(set(values)) != len(values):
            raise ValueError("Clients must have distinct tokens")

    def authenticate(self, authorization: str) -> str | None:
        if not authorization.startswith("Bearer "):
            return None
        supplied = authorization[7:].encode("utf-8")
        matched = None
        for principal, path in self.paths.items():
            try:
                expected = read_token(path).encode("ascii")
            except (OSError, ValueError):
                continue  # Removing a token revokes access immediately.
            if hmac.compare_digest(expected, supplied):
                matched = principal
        return matched

    def active(self, principal: str) -> bool:
        try:
            read_token(self.paths[principal])
            return True
        except (KeyError, OSError, ValueError):
            return False


def loopback_host(host: str) -> bool:
    try:
        parsed = urlsplit("http://" + host)
        if (
            parsed.username
            or parsed.password
            or parsed.path
            or parsed.query
            or parsed.fragment
        ):
            return False
        if parsed.port is not None and not 1 <= parsed.port <= 65535:
            return False
        # DNS names are never trusted, even if DNS currently resolves to loopback.
        return bool(
            parsed.hostname and ipaddress.ip_address(parsed.hostname).is_loopback
        )
    except ValueError:
        return False


def local_url(url: str) -> str:
    parsed = urlsplit(url)
    if (
        parsed.scheme != "http"
        or parsed.path != "/mcp"
        or parsed.query
        or parsed.fragment
        or not loopback_host(parsed.netloc)
    ):
        raise ValueError("MCP URL must be http://<loopback-IP>:<port>/mcp")
    return url
