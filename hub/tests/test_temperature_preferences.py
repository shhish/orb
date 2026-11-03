"""Per-device temperature units are resolved from isolated Mac preferences."""

import asyncio
import json
import plistlib
from pathlib import Path

import pytest

from orb_hub import temperature_preferences
from orb_hub.envelope import LinkError
from orb_hub.sessions import SessionHub

# conftest replaces the reader for all runtime tests so no owner plist is read.
READ_TEMPERATURE_UNIT = temperature_preferences.read_temperature_unit


@pytest.mark.parametrize(
    ("preferences", "expected"),
    [
        ({}, "C"),
        ({"AppleTemperatureUnit": "Fahrenheit", "AppleLocale": "en_GB"}, "F"),
        ({"AppleTemperatureUnit": "Celsius", "AppleLocale": "en_US"}, "C"),
        ({"AppleTemperatureUnit": "Kelvin", "AppleLocale": "en_US"}, "F"),
        ({"AppleTemperatureUnit": False}, "C"),
        ({"AppleLocale": "en_US"}, "F"),
        ({"AppleLocale": "en-US"}, "F"),
        ({"AppleLocale": "en_US@calendar=gregorian"}, "F"),
        ({"AppleLocale": "en_US_POSIX"}, "F"),
        ({"AppleLocale": "en_Latn_US"}, "F"),
        ({"AppleLocale": "en_GB"}, "C"),
        ({"AppleLocale": "hi_IN"}, "C"),
        ({"AppleLocale": None}, "C"),
        ({"AppleLocale": "es_419"}, "C"),
        ({"AppleLocale": "nonsense"}, "C"),
    ],
)
def test_mac_temperature_preference_parsing(preferences, expected):
    assert temperature_preferences.temperature_unit(preferences) == expected


@pytest.mark.parametrize("region", ["US", "LR", "MM", "BS", "BZ", "KY", "PW", "FM", "MH"])
def test_fahrenheit_regions(region):
    assert temperature_preferences.temperature_unit({"AppleLocale": f"en_{region}"}) == "F"


@pytest.mark.parametrize("binary", [False, True])
def test_reader_uses_only_temporary_preferences(monkeypatch, tmp_path, binary):
    monkeypatch.setattr(temperature_preferences.sys, "platform", "darwin")
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    preferences = tmp_path / "Library/Preferences/.GlobalPreferences.plist"
    preferences.parent.mkdir(parents=True)
    original = plistlib.dumps(
        {"AppleTemperatureUnit": "Fahrenheit"}, fmt=plistlib.FMT_BINARY if binary else plistlib.FMT_XML
    )
    preferences.write_bytes(original)
    assert READ_TEMPERATURE_UNIT() == "F"
    assert preferences.read_bytes() == original


@pytest.mark.parametrize("raw", [None, b"broken plist", plistlib.dumps(["Fahrenheit"])])
def test_missing_or_invalid_preferences_fall_back_to_celsius(monkeypatch, tmp_path, raw):
    monkeypatch.setattr(temperature_preferences.sys, "platform", "darwin")
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    if raw is not None:
        preferences = tmp_path / "Library/Preferences/.GlobalPreferences.plist"
        preferences.parent.mkdir(parents=True)
        preferences.write_bytes(raw)
    assert READ_TEMPERATURE_UNIT() == "C"


def test_non_mac_reader_never_reads_home(monkeypatch):
    monkeypatch.setattr(temperature_preferences.sys, "platform", "linux")

    def home():
        raise AssertionError("non-Mac runtime must not read global Mac preferences")

    monkeypatch.setattr(Path, "home", home)
    assert READ_TEMPERATURE_UNIT() == "C"


class CaptureLink:
    def __init__(self, device_id, caps):
        self.device_id = device_id
        self.caps = set(caps)
        self.events = []

    async def notify(self, method, params):
        self.events.append((method, params))


async def drain(coordinator, link):
    await asyncio.wait_for(coordinator._subscribers[link].queue.join(), 1)
    events, link.events = link.events, []
    return events


async def test_owner_unit_override_survives_patches_restart_and_auto_refresh(short_tmp, monkeypatch):
    coordinator = SessionHub(short_tmp)
    try:
        result = await coordinator.set_prefs("orb-first", {"units": {"temperature": "F"}})
        assert result["units"] == {"temperature": "F", "source": "owner"}
        await coordinator.set_prefs("orb-first", {"sources": {"codex": False}, "idle_minutes": 0})
        result["units"]["temperature"] = "C"
        assert coordinator.get_prefs("orb-first")["units"] == {"temperature": "F", "source": "owner"}
        monkeypatch.setattr(temperature_preferences, "read_temperature_unit", lambda: "C")
        await coordinator.refresh_units()
        assert coordinator.get_prefs("orb-first")["units"] == {"temperature": "F", "source": "owner"}
    finally:
        await coordinator.close()
    coordinator = SessionHub(short_tmp)
    try:
        await coordinator.start()
        assert coordinator.get_prefs("orb-first") == {
            "sources": {"claude": True, "codex": False, "dot": True},
            "idle_minutes": 0,
            "units": {"temperature": "F", "source": "owner"},
        }
        reset = await coordinator.set_prefs("orb-first", {"units": {"temperature": "auto"}})
        assert reset["units"] == {"temperature": "C", "source": "auto"}
        monkeypatch.setattr(temperature_preferences, "read_temperature_unit", lambda: "F")
        await coordinator.refresh_units()
        assert coordinator.get_prefs("orb-first")["units"] == {"temperature": "F", "source": "auto"}
    finally:
        await coordinator.close()


async def test_legacy_saved_prefs_without_units_use_auto(short_tmp, monkeypatch):
    coordinator = SessionHub(short_tmp)
    legacy = {"sources": {"claude": False, "codex": True, "dot": True}, "idle_minutes": 3}
    coordinator.db.execute("INSERT INTO device_prefs VALUES (?,?)", ("orb-first", json.dumps(legacy)))
    try:
        monkeypatch.setattr(temperature_preferences, "read_temperature_unit", lambda: "F")
        await coordinator.refresh_units()
        assert coordinator.get_prefs("orb-first") == {**legacy, "units": {"temperature": "F", "source": "auto"}}
        updated = await coordinator.set_prefs("orb-first", {"idle_minutes": 5})
        assert updated["units"] == {"temperature": "F", "source": "auto"}
        monkeypatch.setattr(temperature_preferences, "read_temperature_unit", lambda: "C")
        await coordinator.refresh_units()
        assert coordinator.get_prefs("orb-first")["units"] == {"temperature": "C", "source": "auto"}
    finally:
        await coordinator.close()


@pytest.mark.parametrize(
    "units",
    [
        {},
        None,
        [],
        True,
        {"temperature": "c"},
        {"temperature": "Kelvin"},
        {"temperature": True},
        {"temperature": 0},
        {"temperature": None},
        {"temperature": "F", "source": "owner"},
        {"source": "auto"},
        {"temperature": "F", "other": 1},
    ],
)
async def test_invalid_units_patch_is_atomic(short_tmp, units):
    coordinator = SessionHub(short_tmp)
    try:
        before = coordinator.get_prefs("orb-first")
        with pytest.raises(LinkError, match="bad_request"):
            await coordinator.set_prefs("orb-first", {"sources": {"codex": False}, "idle_minutes": 0, "units": units})
        assert coordinator.get_prefs("orb-first") == before
        assert coordinator.db.execute("SELECT COUNT(*) FROM device_prefs").fetchone()[0] == 0
    finally:
        await coordinator.close()


async def test_auto_refresh_notifies_only_capable_links_and_keeps_owner_overrides(short_tmp, monkeypatch):
    coordinator = SessionHub(short_tmp)
    links = [
        CaptureLink(f"orb-{name}", caps)
        for name, caps in [("cards", ["cards"]), ("home", ["home"]), ("weather", ["weather"]), ("old", [])]
    ]
    owner = CaptureLink("orb-owner", ["weather"])
    try:
        await coordinator.set_prefs(owner.device_id, {"units": {"temperature": "C"}})
        for link in [*links, owner]:
            coordinator.attach(link)
        monkeypatch.setattr(temperature_preferences, "read_temperature_unit", lambda: "F")
        await coordinator.refresh_units()
        for link in links:
            events = await drain(coordinator, link)
            if link.caps:
                assert events == [("prefs.update", coordinator.get_prefs(link.device_id))]
                assert events[0][1]["units"] == {"temperature": "F", "source": "auto"}
                assert coordinator.delivery_allowed(link, "prefs.update", events[0][1])
            else:
                assert events == []
        assert await drain(coordinator, owner) == []
        await coordinator.refresh_units()
        for link in [*links, owner]:
            assert await drain(coordinator, link) == []
        old = coordinator.get_prefs(links[0].device_id)
        await coordinator.set_prefs(links[0].device_id, {"units": {"temperature": "C"}})
        assert not coordinator.delivery_allowed(links[0], "prefs.update", old)
    finally:
        await coordinator.close()
