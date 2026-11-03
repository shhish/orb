"""CLI surfaces route to the hub and never install owner configuration."""

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

from orb_hub import cli
from orb_hub.control import ControlError


def test_hooks_snippet_is_print_safe_and_does_not_modify_owner_files(short_tmp, monkeypatch, capsys):
    monkeypatch.setattr(Path, "home", lambda: short_tmp)
    assert cli.main(["agents", "hooks-snippet", "--port", "12345"]) == 0
    output = capsys.readouterr().out
    value = json.loads(output)
    assert "hooks" in value
    assert "REPLACE_WITH_PRIVATE_ORB_HOOK_TOKEN" in output
    assert "127.0.0.1:12345" in output
    assert not (short_tmp / ".claude").exists()
    assert not (short_tmp / ".codex").exists()


def test_status_demo_and_start_route_to_control(short_tmp, monkeypatch, capsys):
    calls = []

    def call(command, **params):
        calls.append((command, params))
        return {"ok": True}

    monkeypatch.setattr(cli, "_call", call)
    assert cli.main(["agents", "status"]) == 0
    assert cli.main(["dot", "status"]) == 0
    assert cli.main(["demo-cards", "orb-001122334455"]) == 0
    assert cli.main(["agents", "start", "codex", "--cwd", str(short_tmp), "--prompt", "Test"]) == 0
    assert [command for command, _ in calls] == ["agents.status", "dot.status", "demo-cards", "agents.start"]
    assert calls[-1][1]["cwd"] == str(short_tmp.resolve())
    assert capsys.readouterr().err == ""


def test_dot_forwards_stdio_and_tunnel_without_side_effects(monkeypatch):
    calls = []
    monkeypatch.setitem(sys.modules, "orb_mcp.cli", SimpleNamespace(main=lambda args: calls.append(args) or 0))
    assert cli.main(["dot", "tunnel", "doctor", "--profile", "example"]) == 0
    assert calls == [["tunnel", "doctor", "--profile", "example"]]


def test_serve_integration_flags_are_explicit():
    parser = cli.build_parser()
    defaults = parser.parse_args(["serve"])
    assert defaults.agents is False and defaults.dot is False
    options = parser.parse_args(["serve", "--agents", "--hooks-port", "0", "--dot", "--mcp-port", "0"])
    assert options.agents and options.dot and options.hooks_port == options.mcp_port == 0


def test_prefs_cli_routes_partial_patch_and_prints_complete_settings(monkeypatch, capsys):
    calls = []
    prefs = {
        "sources": {"claude": True, "codex": False, "dot": True},
        "idle_minutes": 5,
        "units": {"temperature": "C", "source": "auto"},
    }

    def call(command, **params):
        calls.append((command, params))
        return prefs

    monkeypatch.setattr(cli, "_call", call)
    device_id = "orb-001122334455"
    assert cli.main(["prefs", "show", device_id, "--json"]) == 0
    assert json.loads(capsys.readouterr().out) == prefs
    assert cli.main(["prefs", "set", device_id, "--codex", "off", "--idle-minutes", "5", "--json"]) == 0
    assert json.loads(capsys.readouterr().out) == prefs
    assert cli.main(["prefs", "set", device_id, "--claude", "on"]) == 0
    assert "claude on, codex off, dot on" in capsys.readouterr().out
    assert calls == [
        ("prefs.show", {"device_id": device_id}),
        ("prefs.set", {"device_id": device_id, "prefs": {"sources": {"codex": False}, "idle_minutes": 5}}),
        ("prefs.set", {"device_id": device_id, "prefs": {"sources": {"claude": True}}}),
    ]


@pytest.mark.parametrize("options", [[], ["--idle-minutes", "-1"], ["--idle-minutes", "1441"]])
def test_prefs_cli_rejects_empty_or_invalid_patch_before_call(monkeypatch, capsys, options):
    calls = []
    monkeypatch.setattr(cli, "_call", lambda *args, **kwargs: calls.append((args, kwargs)))
    assert cli.main(["prefs", "set", "orb-001122334455", *options]) == 1
    assert capsys.readouterr().err.startswith("orb-hub:")
    assert calls == []


async def test_prefs_control_supports_offline_paired_devices_and_rejects_unknown(hub):
    device_id = "orb-001122334455"
    hub.store.pair(device_id, "Orb", "test", "1.1", "ab" * 32, "dev")
    defaults = {
        "sources": {"claude": True, "codex": True, "dot": True},
        "idle_minutes": 10,
        "units": {"temperature": "C", "source": "auto"},
    }
    assert await hub.handle_command("prefs.show", {"device_id": device_id}) == defaults
    expected = {**defaults, "sources": {"claude": True, "codex": False, "dot": True}, "idle_minutes": 0}
    assert (
        await hub.handle_command(
            "prefs.set", {"device_id": device_id, "prefs": {"sources": {"codex": False}, "idle_minutes": 0}}
        )
        == expected
    )
    assert await hub.handle_command("prefs.show", {"device_id": device_id}) == expected
    with pytest.raises(ControlError, match="no paired device"):
        await hub.handle_command("prefs.set", {"device_id": "orb-aabbccddeeff", "prefs": {"idle_minutes": 20}})
    with pytest.raises(ControlError):
        await hub.handle_command(
            "prefs.set", {"device_id": device_id, "prefs": {"sources": {"dot": False}, "idle_minutes": True}}
        )
    assert await hub.handle_command("prefs.show", {"device_id": device_id}) == expected
