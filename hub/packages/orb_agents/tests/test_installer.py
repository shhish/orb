import json
import os
import stat
from pathlib import Path

import pytest

from orb_agents.installer import EVENTS, HEADER, generate_hooks, main, write_snippet


def test_generated_secret_is_unique_and_every_hook_uses_same_secret():
    first = generate_hooks("http://127.0.0.1:8447/hooks/claude")
    second = generate_hooks("http://127.0.0.1:8447/hooks/claude")
    hooks = first["hooks"]
    assert set(hooks) == set(EVENTS)
    values = {entries[0]["hooks"][0]["headers"][HEADER] for entries in hooks.values()}
    assert len(values) == 1
    assert len(next(iter(values))) >= 32
    assert first != second
    assert hooks["PermissionRequest"][0]["hooks"][0]["timeout"] == 130
    assert hooks["Stop"][0]["hooks"][0]["timeout"] == 10
    assert all("command" not in entries[0]["hooks"][0] for entries in hooks.values())


@pytest.mark.parametrize(
    "url",
    [
        "http://localhost:8447/hooks/claude",
        "https://127.0.0.1:8447/hooks/claude",
        "http://0.0.0.0:8447/hooks/claude",
        "http://[::1]:8447/hooks/claude",
        "http://127.0.0.1/hooks/claude",
        "http://127.0.0.1:0/hooks/claude",
        "http://127.0.0.1:65536/hooks/claude",
        "http://user@127.0.0.1:8447/hooks/claude",
        "http://127.0.0.1:8447/hooks/claude?secret=bad",
        "http://127.0.0.1:8447/hooks/claude#fragment",
        "http://127.0.0.1:8447/other",
        " http://127.0.0.1:8447/hooks/claude",
        "http://127.0.0.1:8447/hooks/claude\n",
    ],
)
def test_rejects_noncanonical_endpoint(url):
    with pytest.raises(ValueError):
        generate_hooks(url)


@pytest.mark.parametrize(
    "secret",
    [
        "",
        "short",
        "x" * 257,
        "x" * 32 + "\n",
        "x" * 32 + "ü",
        "x" * 32 + " ",
        "x" * 32 + "$TOKEN",
    ],
)
def test_rejects_unsafe_header_secret(secret):
    with pytest.raises(ValueError):
        generate_hooks("http://127.0.0.1:8447/hooks/claude", secret)


@pytest.mark.parametrize("timeout", [0, -1, 601, float("nan")])
def test_rejects_invalid_timeout(timeout):
    with pytest.raises(ValueError):
        generate_hooks("http://127.0.0.1:8447/hooks/claude", timeout=timeout)


def test_write_private_new_file_without_overwriting_or_following_symlink(tmp_path):
    path = tmp_path / "hooks.json"
    data = generate_hooks("http://127.0.0.1:8447/hooks/claude")
    write_snippet(path, data)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    assert json.loads(path.read_text()) == data
    with pytest.raises(FileExistsError):
        write_snippet(path, {})
    assert json.loads(path.read_text()) == data
    link = tmp_path / "link"
    link.symlink_to(path)
    with pytest.raises(FileExistsError):
        write_snippet(link, {})


def test_cli_never_changes_config_or_prints_secret(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("HOME", str(tmp_path))
    config = tmp_path / ".claude" / "settings.json"
    config.parent.mkdir()
    config.write_text('{"keep":true}\n')
    output = tmp_path / "snippet.json"
    assert (
        main(["--url", "http://127.0.0.1:8447/hooks/claude", "--output", str(output)])
        == 0
    )
    data = json.loads(output.read_text())
    secret = data["hooks"]["PermissionRequest"][0]["hooks"][0]["headers"][HEADER]
    captured = capsys.readouterr()
    assert secret not in captured.out + captured.err
    assert config.read_text() == '{"keep":true}\n'
    with pytest.raises(SystemExit) as error:
        main(["--url", "http://127.0.0.1:8447/hooks/claude", "--output", str(output)])
    assert error.value.code == 2


def test_write_respects_more_restrictive_umask(tmp_path):
    previous = os.umask(0o777)
    try:
        path = Path(tmp_path) / "snippet.json"
        write_snippet(path, {})
        assert stat.S_IMODE(path.stat().st_mode) == 0
    finally:
        os.umask(previous)
        path.chmod(0o600)
