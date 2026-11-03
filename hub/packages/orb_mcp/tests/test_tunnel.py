"""Tunnel tooling never starts processes or uses a real runtime credential."""

import argparse
import hashlib
import io
import json
import plistlib
import shlex
import stat
import threading
import urllib.request
import warnings
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

from orb_mcp import tunnel

TUNNEL_ID = "tunnel_" + "0" * 32


def make_zip(members=None):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as bundle:
        for name, data, mode in members or [
            ("tunnel-client", b"a test binary, never executed", stat.S_IFREG | 0o755),
            ("LICENSE", b"test license", stat.S_IFREG | 0o644),
        ]:
            info = zipfile.ZipInfo(name)
            info.external_attr = mode << 16
            bundle.writestr(info, data)
    return stream.getvalue()


@pytest.fixture
def release(monkeypatch):
    archive = make_zip()
    digest = hashlib.sha256(archive).hexdigest()
    manifest = f"{digest}  {tunnel.ASSET}\n".encode()
    monkeypatch.setattr(tunnel, "ASSET_SHA256", digest)
    monkeypatch.setattr(tunnel, "MANIFEST_SHA256", hashlib.sha256(manifest).hexdigest())
    monkeypatch.setattr(tunnel.platform, "system", lambda: "Darwin")
    monkeypatch.setattr(tunnel.platform, "machine", lambda: "arm64")
    calls = []

    def fetch(url, limit):
        calls.append((url, limit))
        return manifest if url.endswith("SHA256SUMS.txt") else archive

    return archive, manifest, calls, fetch


@pytest.fixture
def prepared(tmp_path, release):
    directory = tmp_path / "tunnel"
    tunnel.install(directory, download=release[3])
    executable = tmp_path / "orb mcp"
    executable.write_text("unused fake executable")
    executable.chmod(0o700)
    token = tmp_path / "dot.token"
    token.write_text("ab" * 32)
    token.chmod(0o600)
    return directory, executable, token


def setup(prepared, **kwargs):
    directory, executable, token = prepared
    return tunnel.setup(
        directory,
        tunnel_id=TUNNEL_ID,
        orb_command=executable,
        token_file=token,
        password_prompt=lambda _: "throwaway-runtime-key",
        **kwargs,
    )


def test_installs_only_pinned_release_and_keeps_private_files(tmp_path, release):
    _, _, calls, fetch = release
    result = tunnel.install(tmp_path / "tunnel", download=fetch)
    binary = Path(result["binary"])
    assert binary.read_bytes() == b"a test binary, never executed"
    assert stat.S_IMODE(binary.stat().st_mode) == 0o700
    assert stat.S_IMODE(binary.parent.stat().st_mode) == 0o700
    assert stat.S_IMODE((binary.parent / "LICENSE").stat().st_mode) == 0o600
    assert [url for url, _ in calls] == [
        f"{tunnel.RELEASE_URL}/SHA256SUMS.txt",
        f"{tunnel.RELEASE_URL}/{tunnel.ASSET}",
    ]
    assert not list((tmp_path / "tunnel").glob(".install-*"))
    with pytest.raises(tunnel.TunnelError, match="already installed"):
        tunnel.install(tmp_path / "tunnel", download=fetch)


def test_manifest_and_archive_pins_are_independently_verified(release):
    archive, manifest, _, _ = release
    tunnel._verify_archive(archive, manifest)
    with pytest.raises(tunnel.TunnelError, match="SHA256SUMS"):
        tunnel._verify_archive(archive, manifest + b"\n")
    with pytest.raises(tunnel.TunnelError, match="pinned SHA256"):
        tunnel._verify_archive(archive + b"bad", manifest)


@pytest.mark.parametrize("suffix", ["", "  wrong.zip\n", " duplicate"])
def test_manifest_requires_exactly_one_matching_checksum(release, monkeypatch, suffix):
    archive = release[0]
    manifest = f"{tunnel.ASSET_SHA256}{suffix}".encode()
    monkeypatch.setattr(tunnel, "MANIFEST_SHA256", hashlib.sha256(manifest).hexdigest())
    with pytest.raises(tunnel.TunnelError, match="SHA256SUMS"):
        tunnel._verify_archive(archive, manifest)


def test_duplicate_checksum_entry_rejected(release, monkeypatch):
    archive, manifest, _, _ = release
    manifest *= 2
    monkeypatch.setattr(tunnel, "MANIFEST_SHA256", hashlib.sha256(manifest).hexdigest())
    with pytest.raises(tunnel.TunnelError, match="SHA256SUMS"):
        tunnel._verify_archive(archive, manifest)


@pytest.mark.parametrize(
    "name,mode",
    [
        ("../outside", stat.S_IFREG),
        ("/outside", stat.S_IFREG),
        ("dir/file", stat.S_IFREG),
        ("dir\\file", stat.S_IFREG),
        (".", stat.S_IFREG),
        ("link", stat.S_IFLNK),
        ("fifo", stat.S_IFIFO),
        ("dir/", stat.S_IFDIR),
    ],
)
def test_unsafe_archive_members_rejected(tmp_path, name, mode):
    with pytest.raises(tunnel.TunnelError, match="unsafe"):
        tunnel._extract_archive(make_zip([(name, b"bad", mode | 0o644)]), tmp_path)
    assert not list(tmp_path.iterdir())


def test_archive_limits_and_missing_binary(tmp_path, monkeypatch):
    with pytest.raises(tunnel.TunnelError, match="no tunnel-client"):
        tunnel._extract_archive(make_zip([("LICENSE", b"x", stat.S_IFREG)]), tmp_path)
    monkeypatch.setattr(tunnel, "MAX_UNPACKED_BYTES", 1)
    with pytest.raises(tunnel.TunnelError, match="size limit"):
        tunnel._extract_archive(make_zip(), tmp_path)


def test_install_rejects_other_platforms(tmp_path, release, monkeypatch):
    monkeypatch.setattr(tunnel.platform, "machine", lambda: "x86_64")
    with pytest.raises(tunnel.TunnelError, match="Apple Silicon"):
        tunnel.install(tmp_path / "tunnel", download=release[3])
    assert release[2] == []


def test_setup_uses_file_reference_quotes_stdio_and_only_generates_plist(prepared):
    result = setup(prepared)
    directory, executable, token = prepared
    profile = json.loads(Path(result["profile"]).read_text())
    plist = plistlib.loads(Path(result["launchagent"]).read_bytes())
    assert result["loaded"] is False
    assert result["network_verified"] is False
    assert profile["control_plane"]["api_key"] == f"file:{directory}/runtime-key"
    assert profile["control_plane"]["tunnel_id"] == TUNNEL_ID
    assert profile["health"]["listen_addr"] == "127.0.0.1:8446"
    assert shlex.split(profile["mcp"]["commands"][0]["command"]) == [
        str(executable),
        "stdio",
        "--url",
        "http://127.0.0.1:8445/mcp",
        "--token-file",
        str(token),
    ]
    assert plist["ProgramArguments"] == [
        str(directory / tunnel.VERSION / "tunnel-client"),
        "run",
        "--profile",
        "orb",
        "--profile-dir",
        str(directory / "profiles"),
        "--health.listen-addr",
        "127.0.0.1:8446",
    ]
    assert plist["Umask"] == 0o077
    assert "throwaway-runtime-key" not in json.dumps(profile)
    assert "throwaway-runtime-key" not in json.dumps(plist)
    assert "throwaway-runtime-key" not in json.dumps(result)
    for name in ("runtime-key", "profiles/orb.yaml", f"{tunnel.LABEL}.plist"):
        assert stat.S_IMODE((directory / name).stat().st_mode) == 0o600
    assert tunnel.doctor(directory)["ok"]


def test_setup_does_not_overwrite_or_reprompt(prepared):
    setup(prepared)
    key = prepared[0] / "runtime-key"
    with pytest.raises(tunnel.TunnelError, match="already exists"):
        setup(prepared)
    assert key.read_text() == "throwaway-runtime-key\n"


@pytest.mark.parametrize(
    "secret", ["", "sk-admin-secret", "has space", "has\nnewline", "ü", "x" * 4097]
)
def test_invalid_keys_never_written_or_reported(prepared, secret):
    directory, executable, token = prepared
    with pytest.raises(tunnel.TunnelError, match="runtime key") as error:
        tunnel.setup(
            directory,
            tunnel_id=TUNNEL_ID,
            orb_command=executable,
            token_file=token,
            password_prompt=lambda _: secret,
        )
    assert not (directory / "runtime-key").exists()
    if secret:
        assert secret not in str(error.value)


def test_hidden_input_does_not_fall_back_to_echo(prepared):
    directory, executable, token = prepared

    def insecure_prompt(_):
        warnings.warn("fallback", tunnel.getpass.GetPassWarning)
        pytest.fail("must fail before fallback reads input")

    with pytest.raises(tunnel.TunnelError, match="interactive terminal"):
        tunnel.setup(
            directory,
            tunnel_id=TUNNEL_ID,
            orb_command=executable,
            token_file=token,
            password_prompt=insecure_prompt,
        )


@pytest.mark.parametrize(
    "url",
    [
        "https://127.0.0.1:8445/mcp",
        "http://localhost:8445/mcp",
        "http://orb.local:8445/mcp",
        "http://example.com:8445/mcp",
        "http://127.0.0.1:8445/mcp?secret=x",
        "http://token@127.0.0.1:8445/mcp",
        "http://127.0.0.1:8445/mcp#fragment",
        "http://127.0.0.1:99999/mcp",
        "http://127.0.0.1/mcp",
    ],
)
def test_setup_rejects_nonlocal_or_credentialed_mcp_urls(prepared, url):
    with pytest.raises(tunnel.TunnelError, match="MCP URL"):
        setup(prepared, mcp_url=url)


def test_setup_rejects_insecure_or_symlink_token(prepared, tmp_path):
    directory, executable, token = prepared
    token.chmod(0o644)
    with pytest.raises(tunnel.TunnelError, match="0600"):
        setup(prepared)
    token.chmod(0o600)
    link = tmp_path / "token-link"
    link.symlink_to(token)
    with pytest.raises(tunnel.TunnelError, match="0600"):
        setup((directory, executable, link))


def test_private_directory_rejects_symlinks_and_public_modes(tmp_path):
    actual = tmp_path / "actual"
    actual.mkdir(mode=0o755)
    link = tmp_path / "link"
    link.symlink_to(actual)
    with pytest.raises(tunnel.TunnelError, match="symlink"):
        tunnel._private_directory(link)
    with pytest.raises(tunnel.TunnelError, match="0700"):
        tunnel._private_directory(actual)


def test_doctor_reports_tampering_without_secret_or_process_execution(
    prepared, monkeypatch
):
    setup(prepared)
    directory = prepared[0]
    monkeypatch.setattr(
        urllib.request, "build_opener", lambda *_: pytest.fail("doctor must be offline")
    )
    assert tunnel.doctor(directory)["ok"]
    (directory / tunnel.VERSION / "tunnel-client").write_text("tampered")
    (directory / "runtime-key").chmod(0o644)
    report = tunnel.doctor(directory)
    assert not report["ok"]
    assert not report["checks"]["runtime_key"]
    assert not report["checks"]["release_integrity"]
    assert "throwaway-runtime-key" not in json.dumps(report)
    assert report["network_verified"] is False


def test_doctor_handles_missing_and_malformed_files(tmp_path, prepared):
    assert not tunnel.doctor(tmp_path / "missing")["ok"]
    setup(prepared)
    (prepared[0] / "profiles" / "orb.yaml").write_text("bad json with a secret")
    report = tunnel.doctor(prepared[0])
    assert not report["ok"]
    assert "secret" not in json.dumps(report)


@pytest.mark.parametrize(
    "receipt",
    [[], {"version": tunnel.VERSION, "asset_sha256": tunnel.ASSET_SHA256, "files": []}],
)
def test_doctor_handles_wrong_receipt_types(prepared, receipt):
    setup(prepared)
    (prepared[0] / tunnel.VERSION / "orb-install.json").write_text(json.dumps(receipt))
    assert not tunnel.doctor(prepared[0])["ok"]


def test_setup_rejects_malformed_local_token(prepared):
    prepared[2].write_text("invalid-token")
    with pytest.raises(tunnel.TunnelError, match="32-byte"):
        setup(prepared)


def test_status_is_loopback_only_no_redirects_and_does_not_expose_bodies(monkeypatch):
    hits = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append(self.path)
            if self.path == "/healthz":
                self.send_response(200)
            else:
                self.send_response(302)
                self.send_header("Location", "http://example.com/secret")
            self.end_headers()
            self.wfile.write(b"secret body must not be reported")

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    monkeypatch.setenv("HTTP_PROXY", "http://127.0.0.1:1")
    try:
        report = tunnel.status(health_address=f"127.0.0.1:{server.server_port}")
        assert report["healthy"] is True
        assert report["ready"] is False
        assert hits == ["/healthz", "/readyz"]
        assert "secret" not in json.dumps(report)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


@pytest.mark.parametrize(
    "address", ["localhost:8446", "0.0.0.0:8446", "example.com:8446", "127.0.0.1:99999"]
)
def test_status_rejects_any_nonloopback_override(address):
    with pytest.raises(tunnel.TunnelError):
        tunnel.status(health_address=address)


def test_release_redirects_reject_non_github_https():
    handler = tunnel._ReleaseRedirects()
    request = urllib.request.Request(f"{tunnel.RELEASE_URL}/{tunnel.ASSET}")
    for url in (
        "http://github.com/file",
        "https://evil.example/file",
        "http://127.0.0.1/file",
    ):
        with pytest.raises(tunnel.TunnelError, match="outside GitHub"):
            handler.redirect_request(request, None, 302, "Found", {}, url)


def test_cli_registers_actions_and_redacts_os_errors(tmp_path, monkeypatch, capsys):
    parser = argparse.ArgumentParser()
    tunnel.register_parser(parser.add_subparsers(required=True))
    args = parser.parse_args(["tunnel", "doctor", "--directory", str(tmp_path)])
    assert args.handler(args) == 1
    assert json.loads(capsys.readouterr().out)["network_verified"] is False

    def fail(_):
        raise OSError("do not expose this secret")

    monkeypatch.setattr(tunnel, "install", fail)
    args = parser.parse_args(["tunnel", "install", "--directory", str(tmp_path)])
    assert args.handler(args) == 1
    assert "secret" not in capsys.readouterr().out
