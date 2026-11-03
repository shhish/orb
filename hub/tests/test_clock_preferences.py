import pytest

from orb_hub import clock_preferences


@pytest.mark.parametrize(
    ("pattern", "expected"),
    [
        ("h:mm a", False),
        ("H:mm", True),
        ("KK.mm a", False),
        ("kk:mm", True),
        ("'h' HH:mm", True),
        ("'H' h:mm a", False),
        ("''H:mm", True),
        ("'hours' mm:ss", None),
    ],
)
def test_icu_pattern_uses_hour_fields_outside_literals(pattern, expected):
    assert clock_preferences.hour24_from_pattern(pattern) is expected


def test_mac_owner_hour_cycle_is_cached_without_changing_locale(monkeypatch):
    calls = []

    def current_pattern():
        calls.append(True)
        return "h:mm\u202fa"

    monkeypatch.setattr(clock_preferences.sys, "platform", "darwin")
    monkeypatch.setattr(clock_preferences, "_mac_time_pattern", current_pattern)
    clock_preferences.local_hour24.cache_clear()
    try:
        assert clock_preferences.local_hour24() is False
        assert clock_preferences.local_hour24() is False
        assert calls == [True]
    finally:
        clock_preferences.local_hour24.cache_clear()


@pytest.mark.parametrize(("pattern", "expected"), [("%H:%M:%S", True), ("%I:%M:%S %p", False), ("%r", False)])
def test_non_mac_process_locale_fallback(monkeypatch, pattern, expected):
    monkeypatch.setattr(clock_preferences.sys, "platform", "linux")
    monkeypatch.setattr(clock_preferences.locale, "nl_langinfo", lambda _: pattern)
    clock_preferences.local_hour24.cache_clear()
    try:
        assert clock_preferences.local_hour24() is expected
    finally:
        clock_preferences.local_hour24.cache_clear()
