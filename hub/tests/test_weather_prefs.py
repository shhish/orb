"""Weather CLI and persistence use synthetic location data only."""

import json
import stat
import sys

import pytest

from orb_hub import cli
from orb_hub.weather_prefs import LOCATION_FILE, load_location, parse_latlon, save_location, validate_location

LOCATION = {"name": "Example City", "latitude": 0.0000, "longitude": 0.0}


def test_location_persistence_and_off(tmp_path):
    assert load_location(tmp_path) is None
    save_location(tmp_path, LOCATION)
    assert load_location(tmp_path) == LOCATION
    assert stat.S_IMODE((tmp_path / LOCATION_FILE).stat().st_mode) == 0o600
    save_location(tmp_path, None)
    assert load_location(tmp_path) is None
    assert json.loads((tmp_path / LOCATION_FILE).read_text()) == {"location": None}
    assert list(tmp_path.glob(".weather-*")) == []


@pytest.mark.parametrize(
    "raw",
    [
        b"not json",
        b"[]",
        b"{}",
        b'{"location": true}',
        b'{"location": {"name":"Place", "latitude":true,"longitude":12}}',
        b'{"location": {"name":"Place", "latitude":91,"longitude":12}}',
        b'{"location": null, "extra":true}',
        b"\xff",
        b" " * 1025,
    ],
)
def test_corrupt_location_is_idle(tmp_path, raw, caplog):
    (tmp_path / LOCATION_FILE).write_bytes(raw)
    assert load_location(tmp_path) is None
    assert "weather is idle" in caplog.text


@pytest.mark.parametrize(
    "value",
    [
        True,
        [],
        {},
        {**LOCATION, "latitude": True},
        {**LOCATION, "latitude": float("nan")},
        {**LOCATION, "latitude": float("inf")},
        {**LOCATION, "latitude": 10**1000},
        {**LOCATION, "latitude": -90.1},
        {**LOCATION, "longitude": 180.1},
        {**LOCATION, "longitude": "12"},
        {**LOCATION, "name": ""},
        {**LOCATION, "name": "é" * 33},
        {**LOCATION, "name": "New\nYork"},
        {**LOCATION, "extra": 1},
    ],
)
def test_invalid_location_cannot_replace_saved_value(tmp_path, value):
    save_location(tmp_path, LOCATION)
    with pytest.raises(ValueError):
        save_location(tmp_path, value)
    assert load_location(tmp_path) == LOCATION


def test_latlon_validates_ranges_and_records_readable_name():
    assert parse_latlon("0.0000,0.0000") == {**LOCATION, "name": "0.0000, 0.0000"}
    assert parse_latlon(" -90 , +180 ") == {"name": "-90.0000, 180.0000", "latitude": -90, "longitude": 180}
    assert validate_location({**LOCATION, "name": " Example City "}) == LOCATION


@pytest.mark.parametrize("value", ["", "1", "1,2,3", "nan,2", "1,inf", "91,2", "1,-181", "True,False"])
def test_invalid_latlon(value):
    with pytest.raises(ValueError):
        parse_latlon(value)


@pytest.mark.parametrize(
    ("options", "command", "params"),
    [
        ([], "weather.prefs.show", {}),
        (["--weather", "Example City"], "weather.prefs.set", {"weather": "Example City"}),
        (["--weather", "off"], "weather.prefs.set", {"weather": "off"}),
        (["--weather-latlon", "0.0000,0.0000"], "weather.prefs.set", {"latlon": [0.0000, 0.0]}),
        (["--weather-latlon=-33,151"], "weather.prefs.set", {"latlon": [-33, 151]}),
        (["--weather-latlon", "-33.8,151.2"], "weather.prefs.set", {"latlon": [-33.8, 151.2]}),
        (["--weather-latlon", "-90,-180"], "weather.prefs.set", {"latlon": [-90, -180]}),
    ],
)
def test_weather_cli_controls(monkeypatch, capsys, options, command, params):
    calls = []

    def call(method, **values):
        calls.append((method, values))
        return {"location": LOCATION}

    monkeypatch.setattr(cli, "_call", call)
    assert cli.main(["prefs", *options, "--json"]) == 0
    assert calls == [(command, params)]
    assert json.loads(capsys.readouterr().out) == {"location": LOCATION}


def test_weather_cli_prints_place_and_disabled_hint(monkeypatch, capsys):
    monkeypatch.setattr(cli, "_call", lambda *args, **kwargs: {"location": LOCATION})
    assert cli.main(["prefs", "--weather", "Example City"]) == 0
    assert "Example City (0.0000, 0.0000)" in capsys.readouterr().out
    monkeypatch.setattr(cli, "_call", lambda *args, **kwargs: {"location": None})
    assert cli.main(["prefs", "--weather", "off"]) == 0
    assert 'orb-hub prefs --weather "<city>"' in capsys.readouterr().out


@pytest.mark.parametrize(
    "options",
    [
        ["--weather", ""],
        ["--weather", "x" * 129],
        ["--weather", "New\nYork"],
        ["--weather-latlon", "91,0"],
        ["--weather-latlon", "nan,0"],
        ["--weather", "Example City", "show", "orb-001122334455"],
    ],
)
def test_weather_cli_invalid_input_never_calls_control(monkeypatch, capsys, options):
    calls = []
    monkeypatch.setattr(cli, "_call", lambda *args, **kwargs: calls.append((args, kwargs)))
    assert cli.main(["prefs", *options]) == 1
    assert calls == []
    assert capsys.readouterr().err.startswith("orb-hub:")


def test_weather_cli_mutually_exclusive_modes():
    with pytest.raises(SystemExit) as exc:
        cli.build_parser().parse_args(["prefs", "--weather", "Example City", "--weather-latlon", "0,0"])
    assert exc.value.code == 2


def test_negative_latitude_normalization_preserves_sys_argv(monkeypatch, capsys):
    arguments = ["orb-hub", "prefs", "--weather-latlon", "-33.8,151.2", "--json"]
    monkeypatch.setattr(sys, "argv", arguments)
    calls = []

    def call(command, **params):
        calls.append((command, params))
        return {"location": LOCATION}

    monkeypatch.setattr(cli, "_call", call)
    assert cli.main() == 0
    assert calls == [("weather.prefs.set", {"latlon": [-33.8, 151.2]})]
    assert sys.argv is arguments
    assert arguments == ["orb-hub", "prefs", "--weather-latlon", "-33.8,151.2", "--json"]
    assert json.loads(capsys.readouterr().out) == {"location": LOCATION}


@pytest.mark.parametrize("following", ["--json", "--weather", "--not-an-option", "-91,0"])
def test_latitude_normalization_never_consumes_invalid_values(monkeypatch, following):
    calls = []
    monkeypatch.setattr(cli, "_call", lambda *args, **kwargs: calls.append((args, kwargs)))
    with pytest.raises(SystemExit) as exc:
        cli.main(["prefs", "--weather-latlon", following])
    assert exc.value.code == 2
    assert calls == []


@pytest.mark.parametrize("unit", ["auto", "F", "C"])
def test_device_temperature_cli(monkeypatch, capsys, unit):
    calls = []
    prefs = {
        "sources": {"claude": True, "codex": True, "dot": True},
        "idle_minutes": 10,
        "units": {"temperature": "F", "source": "auto" if unit == "auto" else "owner"},
    }

    def call(command, **params):
        calls.append((command, params))
        return prefs

    monkeypatch.setattr(cli, "_call", call)
    assert cli.main(["prefs", "set", "orb-001122334455", "--temperature", unit]) == 0
    assert calls == [("prefs.set", {"device_id": "orb-001122334455", "prefs": {"units": {"temperature": unit}}})]
    assert "Temperature: °F" in capsys.readouterr().out
