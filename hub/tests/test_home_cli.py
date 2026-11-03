"""The Home CLI uses local control commands and never handles relay credentials."""

import json

import pytest

from orb_hub import cli
from orb_hub.config import Config
from orb_hub.control import ControlError


def test_home_serve_flag_is_explicit(short_tmp):
    parser = cli.build_parser()
    assert parser.parse_args(["serve"]).home is False
    assert parser.parse_args(["serve", "--home", "--agents", "--dot"]).home is True
    assert Config(data_dir=short_tmp, log_dir=short_tmp).home_enabled is False


def test_home_serve_flag_reaches_runtime(monkeypatch):
    configs = []

    async def serve(cfg):
        configs.append(cfg)

    monkeypatch.setattr(cli, "_serve", serve)
    monkeypatch.setattr(cli, "raise_fd_limit", lambda: None)
    monkeypatch.setattr(cli.logging, "basicConfig", lambda **kwargs: None)
    assert cli.main(["serve", "--home", "--bind", "127.0.0.1", "--no-mdns"]) == 0
    assert len(configs) == 1 and configs[0].home_enabled is True


def test_home_cli_routes_only_expected_parameters(monkeypatch, capsys):
    calls = []

    def call(command, **params):
        calls.append((command, params))
        return {"enabled": True, "favourites": ["a123"]}

    monkeypatch.setattr(cli, "_call", call)
    device_id = "orb-001122334455"
    for arguments in (
        ["home", "status"],
        ["home", "favourites", "list", device_id],
        ["home", "favourites", "add", device_id, "a123"],
        ["home", "favourites", "remove", device_id, "s456"],
    ):
        assert cli.main(arguments) == 0
        assert json.loads(capsys.readouterr().out) == {"enabled": True, "favourites": ["a123"]}
    assert calls == [
        ("home.status", {}),
        ("home.favourites.list", {"device_id": device_id}),
        ("home.favourites.add", {"device_id": device_id, "id": "a123"}),
        ("home.favourites.remove", {"device_id": device_id, "id": "s456"}),
    ]


def test_home_cli_reports_control_error(monkeypatch, capsys):
    def call(*args, **kwargs):
        raise ControlError("start the hub with --home first")

    monkeypatch.setattr(cli, "_call", call)
    assert cli.main(["home", "status"]) == 1
    assert capsys.readouterr().err == "orb-hub: start the hub with --home first\n"


@pytest.mark.parametrize(
    "arguments",
    [
        ["home"],
        ["home", "favourites"],
        ["home", "favourites", "list"],
        ["home", "favourites", "add", "orb-001122334455"],
        ["home", "favourites", "remove", "orb-001122334455"],
    ],
)
def test_home_cli_requires_a_complete_command(arguments):
    with pytest.raises(SystemExit) as exc:
        cli.build_parser().parse_args(arguments)
    assert exc.value.code == 2
