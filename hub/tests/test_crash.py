"""Panic receipts survive the next normal state sample in the device's log."""

import pytest
from fake_device import paired_device

from orb_hub.crash import crash_note

SUMMARY = {"task": "sound_prefs", "pc": 0x42000000, "backtrace": [0x42000000, 0x42001000], "corrupted": False}


def test_crash_note_is_compact_and_bounded():
    assert crash_note(SUMMARY) == ("panic: task=sound_prefs pc=0x42000000 bt=0x42000000 0x42001000 corrupted=0")
    assert crash_note({**SUMMARY, "corrupted": True}).endswith("corrupted=1")
    assert crash_note({"error": "ESP_ERR_NOT_FOUND"}) == "panic: crash summary unavailable: ESP_ERR_NOT_FOUND"


@pytest.mark.parametrize(
    "value",
    [
        None,
        [],
        {},
        {**SUMMARY, "pc": True},
        {**SUMMARY, "pc": -1},
        {**SUMMARY, "pc": 0x100000000},
        {**SUMMARY, "backtrace": []},
        {**SUMMARY, "backtrace": [True]},
        {**SUMMARY, "backtrace": [0x42000000] * 17},
        {**SUMMARY, "task": "x" * 17},
        {**SUMMARY, "task": "\nforged log"},
        {**SUMMARY, "task": "é" * 9},
        {**SUMMARY, "task": "\ud800"},
        {**SUMMARY, "corrupted": 1},
        {"error": "ESP_FAIL\nforged log"},
    ],
)
def test_malformed_crash_does_not_forge_a_log_entry(value):
    assert crash_note(value) is None


async def test_first_state_crash_is_retained_after_later_state(hub):
    device = await paired_device(hub)
    try:
        await device.send({"t": "evt", "m": "state.report", "p": {"crash": SUMMARY}})
        await device.send({"t": "evt", "m": "state.report", "p": {"uptime_s": 90}})
        await device.request("ota.check")
        assert hub.store.get(device.device_id).state == {"uptime_s": 90}
        text = hub.devlogs.path(device.device_id).read_text()
        assert "[hub] panic: task=sound_prefs pc=0x42000000 bt=0x42000000 0x42001000 corrupted=0" in text
        assert text.count("[hub] panic:") == 1
    finally:
        await device.aclose()
