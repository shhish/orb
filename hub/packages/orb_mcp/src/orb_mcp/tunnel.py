"""Pinned tunnel-client installation and offline, reviewable service setup.

No command in this module starts a tunnel, invokes launchctl, or sends a key
over the network. The JSON profile is also valid YAML, as accepted upstream.
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import io
import json
import os
import platform
import plistlib
import re
import shlex
import stat
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import warnings
import zipfile
from pathlib import Path, PurePosixPath
from typing import Callable

from .auth import read_token

VERSION = "v0.0.18"
ASSET = f"tunnel-client-{VERSION}-darwin-arm64.zip"
RELEASE_URL = f"https://github.com/openai/tunnel-client/releases/download/{VERSION}"
# Both values checked against the GitHub release API and SHA256SUMS.txt,
# 2026-10-26. Upgrades require an intentional code change and renewed review.
ASSET_SHA256 = "ba7a0994f7dd15bc8708aa1602ecf54371a492319a641ab9c33dc00f1fd56bfe"
MANIFEST_SHA256 = "63f9996246424301c8b0680c943c67d0ea7fe2f0853a234649104f8062c67329"
HEALTH_ADDRESS = "127.0.0.1:8446"
LABEL = "com.orb.mcp-tunnel"
MAX_ARCHIVE_BYTES = 100 * 1024 * 1024
MAX_UNPACKED_BYTES = 300 * 1024 * 1024
TUNNEL_ID = re.compile(r"tunnel_(?:[a-z0-9]{4}_)?[a-z0-9]{32}\Z")


class TunnelError(ValueError):
    """A safe-to-display setup failure; never include credential contents."""


class _ReleaseRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if target.scheme != "https" or target.hostname not in {
            "github.com",
            "release-assets.githubusercontent.com",
            "objects.githubusercontent.com",
        }:
            raise TunnelError("Release download redirected outside GitHub HTTPS")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class _NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _download(url: str, limit: int) -> bytes:
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), _ReleaseRedirects()
    )
    request = urllib.request.Request(url, headers={"User-Agent": "orb-mcp/0.1"})
    with opener.open(request, timeout=30) as response:
        result = response.read(limit + 1)
    if len(result) > limit:
        raise TunnelError("Release download exceeds the size limit")
    return result


def _private_directory(path: Path) -> Path:
    if path.is_symlink():
        raise TunnelError("Refusing a symlink directory")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not path.is_dir() or path.stat().st_uid != os.getuid():
        raise TunnelError("Setup directory must be owned by the current user")
    if stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise TunnelError("Setup directory must have mode 0700")
    return path.resolve()


def _new_file(path: Path, data: bytes, mode: int = 0o600) -> None:
    """Create without following symlinks, replacing files, or a public window."""
    try:
        descriptor = os.open(
            path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode
        )
    except FileExistsError as exc:
        raise TunnelError("Refusing to overwrite an existing setup file") from exc
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def _private_file(path: Path, *, executable: bool = False) -> bool:
    try:
        info = path.lstat()
        return (
            stat.S_ISREG(info.st_mode)
            and info.st_uid == os.getuid()
            and stat.S_IMODE(info.st_mode) == (0o700 if executable else 0o600)
            and info.st_size > 0
        )
    except OSError:
        return False


def _verify_archive(archive: bytes, manifest: bytes) -> None:
    if hashlib.sha256(manifest).hexdigest() != MANIFEST_SHA256:
        raise TunnelError("SHA256SUMS.txt does not match the pinned release")
    digest = hashlib.sha256(archive).hexdigest()
    if digest != ASSET_SHA256:
        raise TunnelError("Release archive does not match its pinned SHA256")
    try:
        matches = []
        for line in manifest.decode("ascii").splitlines():
            fields = line.split()
            if len(fields) == 2 and fields[1].lstrip("*") == ASSET:
                matches.append(fields[0])
    except UnicodeDecodeError as exc:
        raise TunnelError("Invalid SHA256SUMS.txt") from exc
    if matches != [digest]:
        raise TunnelError("Release archive does not match SHA256SUMS.txt")


def _extract_archive(archive: bytes, destination: Path) -> dict[str, str]:
    """Only upstream's flat, regular-file archive format is accepted."""
    hashes = {}
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            members = bundle.infolist()
            if (
                not members
                or sum(item.file_size for item in members) > MAX_UNPACKED_BYTES
            ):
                raise TunnelError("Release archive exceeds the unpacked size limit")
            for member in members:
                name = member.filename
                mode = member.external_attr >> 16
                if (
                    name in {"", ".", ".."}
                    or PurePosixPath(name).name != name
                    or "\\" in name
                    or "\x00" in name
                    or name in hashes
                    or member.is_dir()
                    or stat.S_IFMT(mode) not in {0, stat.S_IFREG}
                ):
                    raise TunnelError("Release archive contains an unsafe member")
                data = bundle.read(member)
                _new_file(
                    destination / name,
                    data,
                    0o700 if name in {"tunnel-client", "cloudflared"} else 0o600,
                )
                hashes[name] = hashlib.sha256(data).hexdigest()
    except zipfile.BadZipFile as exc:
        raise TunnelError("Invalid release archive") from exc
    if "tunnel-client" not in hashes:
        raise TunnelError("Release archive has no tunnel-client binary")
    return hashes


def install(
    directory: Path,
    *,
    download: Callable[[str, int], bytes] | None = None,
) -> dict:
    """Install only the pinned darwin-arm64 release, without executing it."""
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise TunnelError("The pinned release requires macOS on Apple Silicon")
    directory = _private_directory(directory)
    release = directory / VERSION
    if release.exists() or release.is_symlink():
        raise TunnelError("This release is already installed; refusing to overwrite it")
    fetch = download or _download
    manifest = fetch(f"{RELEASE_URL}/SHA256SUMS.txt", 512 * 1024)
    archive = fetch(f"{RELEASE_URL}/{ASSET}", MAX_ARCHIVE_BYTES)
    _verify_archive(archive, manifest)
    with tempfile.TemporaryDirectory(prefix=".install-", dir=directory) as staging:
        stage = Path(staging)
        hashes = _extract_archive(archive, stage)
        _new_file(stage / "SHA256SUMS.txt", manifest)
        _new_file(
            stage / "orb-install.json",
            json.dumps(
                {"version": VERSION, "asset_sha256": ASSET_SHA256, "files": hashes},
                indent=2,
            ).encode(),
        )
        # os.rename is same-filesystem; the final directory only appears complete.
        if release.exists() or release.is_symlink():
            raise TunnelError("Release destination appeared during installation")
        os.rename(stage, release)
    return {"version": VERSION, "binary": str(release / "tunnel-client")}


def _validate_mcp_url(url: str) -> str:
    try:
        parts = urllib.parse.urlsplit(url)
        port = parts.port
    except ValueError as exc:
        raise TunnelError("Invalid local MCP URL") from exc
    if (
        parts.scheme != "http"
        or parts.hostname != "127.0.0.1"
        or not port
        or parts.path != "/mcp"
        or parts.username is not None
        or parts.password is not None
        or parts.query
        or parts.fragment
    ):
        raise TunnelError("MCP URL must be http://127.0.0.1:<port>/mcp")
    return url


def setup(
    directory: Path,
    *,
    tunnel_id: str,
    orb_command: Path,
    token_file: Path,
    mcp_url: str = "http://127.0.0.1:8445/mcp",
    password_prompt: Callable[[str], str] | None = None,
) -> dict:
    """Write a profile, secret reference and unloaded plist into one directory."""
    if not TUNNEL_ID.fullmatch(tunnel_id):
        raise TunnelError("Provide a complete service-issued tunnel ID")
    _validate_mcp_url(mcp_url)
    directory = _private_directory(directory)
    binary = directory / VERSION / "tunnel-client"
    if not _private_file(binary, executable=True):
        raise TunnelError("Install the pinned tunnel-client release first")
    orb_command = orb_command.absolute()
    if not orb_command.is_file() or not os.access(orb_command, os.X_OK):
        raise TunnelError("orb-command must name an executable orb-mcp installation")
    token_file = token_file.absolute()
    if not _private_file(token_file):
        raise TunnelError("The local MCP bearer token must be a nonempty 0600 file")
    try:
        read_token(token_file)
    except (OSError, ValueError) as exc:
        raise TunnelError(
            "The local MCP bearer token must contain a 32-byte hex token"
        ) from exc
    profiles = _private_directory(directory / "profiles")
    logs = _private_directory(directory / "logs")
    profile_path = profiles / "orb.yaml"
    key_path = directory / "runtime-key"
    plist_path = directory / f"{LABEL}.plist"
    destinations = (key_path, profile_path, plist_path)
    if any(path.exists() or path.is_symlink() for path in destinations):
        raise TunnelError(
            "Setup already exists; refusing to overwrite keys or profiles"
        )
    prompt = password_prompt or getpass.getpass
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            secret = prompt("Tunnel runtime key (Read + Use only; input hidden): ")
    except (getpass.GetPassWarning, EOFError) as exc:
        raise TunnelError(
            "An interactive terminal is required for hidden key entry"
        ) from exc
    if (
        not secret
        or len(secret) > 4096
        or not secret.isascii()
        or any(char.isspace() or ord(char) < 33 for char in secret)
        or secret.startswith("sk-admin-")
    ):
        raise TunnelError(
            "Enter a runtime key without whitespace; admin keys are rejected"
        )
    command = shlex.join(
        [str(orb_command), "stdio", "--url", mcp_url, "--token-file", str(token_file)]
    )
    profile = {
        "config_version": 1,
        "control_plane": {
            "base_url": "https://api.openai.com",
            "tunnel_id": tunnel_id,
            "api_key": f"file:{key_path}",
        },
        "health": {"listen_addr": HEALTH_ADDRESS},
        "admin_ui": {"open_browser": False},
        "log": {"level": "info", "format": "json"},
        "mcp": {"commands": [{"channel": "main", "command": command}]},
    }
    plist = {
        "Label": LABEL,
        "ProgramArguments": [
            str(binary),
            "run",
            "--profile",
            "orb",
            "--profile-dir",
            str(profiles),
            "--health.listen-addr",
            HEALTH_ADDRESS,
        ],
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 10,
        "WorkingDirectory": str(directory),
        "StandardOutPath": str(logs / "stdout.log"),
        "StandardErrorPath": str(logs / "stderr.log"),
        "Umask": 0o077,
    }
    created = []
    try:
        for path, content in (
            (key_path, (secret + "\n").encode()),
            (profile_path, (json.dumps(profile, indent=2) + "\n").encode()),
            (plist_path, plistlib.dumps(plist)),
        ):
            _new_file(path, content)
            created.append(path)
    except BaseException:
        for path in created:
            path.unlink(missing_ok=True)
        raise
    return {
        "profile": str(profile_path),
        "launchagent": str(plist_path),
        "loaded": False,
        "health_url": f"http://{HEALTH_ADDRESS}",
        "network_verified": False,
    }


def doctor(directory: Path) -> dict:
    """Inspect only generated local files; never resolve or submit credentials."""
    directory = directory.resolve()
    release = directory / VERSION
    checks = {
        "binary": _private_file(release / "tunnel-client", executable=True),
        "runtime_key": _private_file(directory / "runtime-key"),
        "profile": _private_file(directory / "profiles" / "orb.yaml"),
        "launchagent": _private_file(directory / f"{LABEL}.plist"),
    }
    try:
        receipt = json.loads((release / "orb-install.json").read_text())
        checks["release_integrity"] = (
            receipt["version"] == VERSION
            and receipt["asset_sha256"] == ASSET_SHA256
            and all(
                PurePosixPath(name).name == name
                and not (release / name).is_symlink()
                and hashlib.sha256((release / name).read_bytes()).hexdigest() == digest
                for name, digest in receipt["files"].items()
            )
            and "tunnel-client" in receipt["files"]
        )
    except (OSError, ValueError, TypeError, KeyError, AttributeError):
        checks["release_integrity"] = False
    try:
        profile = json.loads((directory / "profiles" / "orb.yaml").read_text())
        plane = profile["control_plane"]
        command = shlex.split(profile["mcp"]["commands"][0]["command"])
        checks["profile_settings"] = (
            profile["config_version"] == 1
            and plane["base_url"] == "https://api.openai.com"
            and bool(TUNNEL_ID.fullmatch(plane["tunnel_id"]))
            and plane["api_key"] == f"file:{directory / 'runtime-key'}"
            and profile["health"]["listen_addr"] == HEALTH_ADDRESS
            and len(command) == 6
            and command[1:3] == ["stdio", "--url"]
            and _validate_mcp_url(command[3]) == command[3]
            and command[4] == "--token-file"
            and _private_file(Path(command[5]))
            and os.access(command[0], os.X_OK)
        )
        plist = plistlib.loads((directory / f"{LABEL}.plist").read_bytes())
        checks["launchagent_settings"] = plist["Label"] == LABEL and plist[
            "ProgramArguments"
        ] == [
            str(release / "tunnel-client"),
            "run",
            "--profile",
            "orb",
            "--profile-dir",
            str(directory / "profiles"),
            "--health.listen-addr",
            HEALTH_ADDRESS,
        ]
    except (
        OSError,
        ValueError,
        TypeError,
        KeyError,
        IndexError,
        plistlib.InvalidFileException,
    ):
        checks["profile_settings"] = False
        checks["launchagent_settings"] = False
    return {
        "ok": all(checks.values()),
        "checks": checks,
        "network_verified": False,
        "service_started": False,
    }


def status(*, health_address: str = HEALTH_ADDRESS) -> dict:
    """Read local health only. Do not relay arbitrary health-body content."""
    # health_address is injectable for ephemeral-port tests, never user-configured.
    host, separator, port = health_address.partition(":")
    if (
        host != "127.0.0.1"
        or not separator
        or not port.isdigit()
        or not 0 < int(port) < 65536
    ):
        raise TunnelError("Health address must be a literal IPv4 loopback endpoint")
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}), _NoRedirects()
    )
    result = {
        "health_url": f"http://{health_address}",
        "healthy": False,
        "ready": False,
    }
    for route, key in (("healthz", "healthy"), ("readyz", "ready")):
        try:
            with opener.open(f"http://{health_address}/{route}", timeout=2) as response:
                result[key] = response.status == 200
        except (OSError, urllib.error.URLError):
            pass
    return result


def register_parser(subparsers: argparse._SubParsersAction) -> None:
    parser = subparsers.add_parser(
        "tunnel", help="Prepare a tunnel without starting it"
    )
    commands = parser.add_subparsers(dest="tunnel_command", required=True)
    for name, help_text in (
        ("install", "Download and verify pinned darwin-arm64 tunnel-client"),
        ("setup", "Write a profile, runtime key file and unloaded LaunchAgent"),
        ("doctor", "Check local installation and generated files offline"),
        ("status", "Read tunnel-client health at 127.0.0.1:8446"),
    ):
        command = commands.add_parser(name, help=help_text)
        if name != "status":
            command.add_argument("--directory", type=Path, required=True)
        command.set_defaults(handler=run_cli)
        if name == "setup":
            command.add_argument("--tunnel-id", required=True)
            command.add_argument("--orb-command", type=Path, required=True)
            command.add_argument("--token-file", type=Path, required=True)
            command.add_argument("--mcp-url", default="http://127.0.0.1:8445/mcp")


def run_cli(args: argparse.Namespace) -> int:
    try:
        match args.tunnel_command:
            case "install":
                result = install(args.directory)
            case "setup":
                result = setup(
                    args.directory,
                    tunnel_id=args.tunnel_id,
                    orb_command=args.orb_command,
                    token_file=args.token_file,
                    mcp_url=args.mcp_url,
                )
            case "doctor":
                result = doctor(args.directory)
            case "status":
                result = status()
            case _:
                raise TunnelError("Unknown tunnel command")
    except TunnelError as exc:
        print(json.dumps({"ok": False, "error": str(exc)}))
        return 1
    except (OSError, urllib.error.URLError):
        # Exception strings may contain URL query credentials or secret input.
        print(
            json.dumps(
                {
                    "ok": False,
                    "error": "Tunnel operation failed; check local paths and network",
                }
            )
        )
        return 1
    print(json.dumps(result, indent=2))
    if args.tunnel_command == "doctor":
        return 0 if result["ok"] else 1
    if args.tunnel_command == "status":
        return 0 if result["ready"] else 1
    return 0
