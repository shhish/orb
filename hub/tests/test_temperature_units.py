"""Host-independent unit resolution, persistence and device delivery."""

import asyncio
import json
import subprocess
import threading
from types import SimpleNamespace

import pytest
from fake_device import paired_device
from test_home import inventory
from test_session_focus import CaptureLink, flush
from test_sessions import VoiceSocket

from orb_hub import cli, units
from orb_hub.envelope import LinkError
from orb_hub.home import HomeBridge, alias
from orb_hub.link import Link
from orb_hub.sessions import SessionHub
from orb_hub.units import read_temperature_unit, temperature_unit


@pytest.mark.parametrize("region", ["US", "LR", "MM", "BS", "BZ", "KY", "PW", "FM", "MH"])
def test_fahrenheit_regions(region):
    assert temperature_unit(None, f"en_{region}") == "F"


@pytest.mark.parametrize(
    "explicit,locale,expected",
    [
        ("Celsius", "en_US", "C"),
        ("Fahrenheit", "en_GB", "F"),
        (None, "en_US@calendar=gregorian", "F"),
        (None, "zh-Hant-US", "F"),
        (None, "en_GB", "C"),
        (None, "my_MM", "F"),
        (None, "en", "C"),
        (None, None, "C"),
        ("invalid", "en_US", "F"),
    ],
)
def test_explicit_units_precede_region_with_safe_fallback(explicit, locale, expected):
    assert temperature_unit(explicit, locale) == expected


@pytest.mark.parametrize("explicit", [None, "Celsius", "Fahrenheit"])
def test_defaults_reads_are_bounded_and_read_only(monkeypatch, explicit):
    calls = []

    def run(args, **kwargs):
        calls.append(args)
        assert kwargs["timeout"] == 2 and kwargs["stderr"] == subprocess.DEVNULL
        value = explicit if args[-1] == "AppleTemperatureUnit" else "en_US"
        return SimpleNamespace(returncode=0 if value else 1, stdout=f"{value or ''}\n")

    monkeypatch.setattr(units.subprocess, "run", run)
    assert read_temperature_unit() == ("C" if explicit == "Celsius" else "F")
    assert calls[0] == ["/usr/bin/defaults", "read", "-g", "AppleTemperatureUnit"]
    assert len(calls) == (2 if explicit is None else 1)
    if len(calls) == 2:
        assert calls[1] == ["/usr/bin/defaults", "read", "-g", "AppleLocale"]


@pytest.mark.parametrize("error", [OSError(), subprocess.TimeoutExpired("defaults", 2), UnicodeError()])
def test_unavailable_defaults_fall_back_without_host_access(monkeypatch, error):
    def run(*args, **kwargs):
        raise error

    monkeypatch.setattr(units.subprocess, "run", run)
    assert read_temperature_unit() == "C"


async def test_legacy_saved_prefs_and_override_survive_restart(short_tmp):
    coordinator = SessionHub(short_tmp, temperature="F")
    try:
        old = {"sources": {"claude": False, "codex": True, "dot": True}, "idle_minutes": 20}
        with coordinator.db:
            coordinator.db.execute("INSERT INTO device_prefs VALUES (?,?)", ("legacy", json.dumps(old)))
        assert coordinator.get_prefs("legacy") == {**old, "units": {"temperature": "F", "source": "auto"}}
        result = await coordinator.set_prefs("legacy", {"units": {"temperature": "C"}})
        assert result == {**old, "units": {"temperature": "C", "source": "owner"}}
        await coordinator.set_prefs("legacy", {"idle_minutes": 5})
        result["units"]["temperature"] = "F"
        assert coordinator.get_prefs("legacy")["units"] == {"temperature": "C", "source": "owner"}
    finally:
        await coordinator.close()
    coordinator = SessionHub(short_tmp, temperature="F")
    try:
        assert coordinator.get_prefs("legacy")["units"] == {"temperature": "C", "source": "owner"}
        result = await coordinator.set_prefs("legacy", {"units": {"temperature": "auto"}})
        assert result["units"] == {"temperature": "F", "source": "auto"}
        assert result["idle_minutes"] == 5 and result["sources"] == old["sources"]
    finally:
        await coordinator.close()


@pytest.mark.parametrize(
    "unit",
    [
        {},
        None,
        [],
        "F",
        {"temperature": None},
        {"temperature": True},
        {"temperature": 0},
        {"temperature": []},
        {"temperature": "auto", "source": "auto"},
        {"temperature": "Fahrenheit"},
        {"temperature": "f"},
    ],
)
async def test_invalid_unit_patches_are_atomic(short_tmp, unit):
    coordinator = SessionHub(short_tmp)
    try:
        before = coordinator.get_prefs("device")
        with pytest.raises(LinkError, match="bad_request"):
            await coordinator.set_prefs("device", {"sources": {"codex": False}, "units": unit})
        assert coordinator.get_prefs("device") == before
    finally:
        await coordinator.close()


async def test_auto_changes_only_notify_auto_links_and_all_device_links(short_tmp):
    coordinator = SessionHub(short_tmp, temperature="F")
    first, same, owner, other = CaptureLink(), CaptureLink(), CaptureLink("owner"), CaptureLink("other")
    try:
        await coordinator.set_prefs("owner", {"units": {"temperature": "F"}})
        for link in (first, same, owner, other):
            coordinator.attach(link)
        coordinator.refresh_temperature("C")
        for link in (first, same, other):
            assert await flush(coordinator, link) == [("prefs.update", coordinator.get_prefs(link.device_id))]
        assert await flush(coordinator, owner) == []
        result = await coordinator.set_prefs(first.device_id, {"units": {"temperature": "F"}})
        for link in (first, same):
            assert await flush(coordinator, link) == [("prefs.update", result)]
        assert await flush(coordinator, other) == []
        await coordinator.set_prefs(first.device_id, {"units": {"temperature": "F"}})
        coordinator.refresh_temperature("C")
        assert await flush(coordinator, first) == []
    finally:
        await coordinator.close()


async def test_stale_preference_event_is_suppressed_after_send_lock(short_tmp):
    coordinator = SessionHub(short_tmp)
    hub = SimpleNamespace(sessions=coordinator, cfg=SimpleNamespace(request_timeout=2))
    link = Link(hub, VoiceSocket(), "test-only", "127.0.0.1")
    link.state, link.device_id, link.caps = "online", "device", {"home"}
    try:
        coordinator.attach(link)
        async with link._send_lock:
            coordinator.refresh_temperature("F")
            await asyncio.sleep(0)
            coordinator.refresh_temperature("C")
        await coordinator._subscribers[link].queue.join()
        message = await link.ws.messages.get()
        assert message["m"] == "prefs.update" and message["p"]["units"]["temperature"] == "C"
        assert link.ws.messages.empty()
    finally:
        await coordinator.close()


@pytest.mark.parametrize("periodic", [False, True])
async def test_startup_and_refresh_read_off_event_loop(make_hub, monkeypatch, periodic):
    from orb_hub import server

    current = ["F"]
    threads = []

    def read():
        threads.append(threading.get_ident())
        return current[0]

    monkeypatch.setattr(units, "read_temperature_unit", read)
    if periodic:
        monkeypatch.setattr(server, "PREFERENCES_POLL", 0.01)
    hub = await make_hub()
    assert hub.sessions.get_prefs("device")["units"]["temperature"] == "F"
    updated = asyncio.Event()
    refresh = hub.sessions.refresh_temperature

    def refreshed(value):
        refresh(value)
        if value == "C":
            updated.set()

    monkeypatch.setattr(hub.sessions, "refresh_temperature", refreshed)
    current[0] = "C"
    if not periodic:
        hub.request_preferences_refresh()
    async with asyncio.timeout(2):
        await updated.wait()
    assert hub.sessions.get_prefs("device")["units"]["temperature"] == "C"
    assert len(threads) >= 2 and threading.get_ident() not in threads
    watcher = hub._preferences_watch
    await hub.stop()
    assert watcher.done()


@pytest.mark.parametrize("caps", [[], ["cards"], ["home"], ["cards", "home"]])
async def test_tls_prefs_hello_patch_update_and_legacy_isolation(hub, caps):
    home = HomeBridge(hub.sessions, hub.cfg.data_dir, url="http://127.0.0.1:1/api/v1")
    home._load(inventory())
    home.available = True
    hub.home = home
    hub.sessions.refresh_temperature("F")
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        hello = (await device.hello(caps=caps))["r"]
        if not caps:
            assert "prefs" not in hello
            assert (await device.request("prefs.set", {"units": {"temperature": "C"}}))["e"]["code"] == "unsupported"
            hub.sessions.refresh_temperature("C")
            assert device.events.empty()
            return
        assert hello["prefs"]["units"] == {"temperature": "F", "source": "auto"}
        if "home" in caps:
            while True:
                method, part = await device.event()
                if method == "home.snapshot":
                    thermostat = next((a for a in part["accessories"] if a["id"] == alias("a", "thermostat")), None)
                    if thermostat:
                        assert thermostat["state"]["temp"] == 21.5 and thermostat["state"]["target"] == 22
                    if not part["more"]:
                        break
        result = (await device.request("prefs.set", {"units": {"temperature": "C"}}))["r"]
        assert result["units"] == {"temperature": "C", "source": "owner"}
        while (event := await device.event())[0] != "prefs.update":
            pass
        assert event[1] == result
        await device.disconnect()
        await device.connect()
        assert (await device.hello(caps=caps))["r"]["prefs"] == result
    finally:
        await device.aclose()


async def test_refresh_during_hello_is_repaired_after_subscription(hub, monkeypatch):
    send = Link._send

    async def refresh_before_hello_write(link, obj, **kwargs):
        if obj.get("r", {}).get("prefs"):
            hub.sessions.refresh_temperature("F")
        await send(link, obj, **kwargs)

    monkeypatch.setattr(Link, "_send", refresh_before_hello_write)
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        hello = await device.hello(caps=["cards"])
        assert hello["r"]["prefs"]["units"]["temperature"] == "C"
        while (event := await device.event())[0] != "prefs.update":
            pass
        assert event[1]["units"] == {"temperature": "F", "source": "auto"}
    finally:
        await device.aclose()


@pytest.mark.parametrize("temperature", ["auto", "F", "C"])
def test_temperature_cli_routes_override_and_prints_source(monkeypatch, capsys, temperature):
    calls = []
    prefs = {
        "sources": {},
        "idle_minutes": 10,
        "units": {"temperature": "F", "source": "auto" if temperature == "auto" else "owner"},
    }

    def call(command, **params):
        calls.append((command, params))
        return prefs

    monkeypatch.setattr(cli, "_call", call)
    assert cli.main(["prefs", "set", "device", "--temperature", temperature]) == 0
    assert calls == [("prefs.set", {"device_id": "device", "prefs": {"units": {"temperature": temperature}}})]
    assert f"°F ({prefs['units']['source']})" in capsys.readouterr().out
