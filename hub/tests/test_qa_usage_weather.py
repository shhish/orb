"""Hub QA regressions use provider fixtures and never contact a service."""

import copy
import json
import threading
import uuid
from datetime import UTC, datetime
from pathlib import Path
from zoneinfo import ZoneInfo

import pytest
from orb_agents.claude import ClaudeAdapter, _transcript_metadata
from orb_agents.codex import CodexAdapter, RolloutObserver, _status, _time
from orb_agents.store import StateStore
from orb_agents.usage import UsageCollector, UsageObservation, UsageWindow

from orb_hub.weather import WeatherLocation, geocode, parse_forecast


@pytest.mark.parametrize("query", ["Columbia, MD 29201", "Columbia, OR 21044"])
async def test_geocoding_requires_every_explicit_qualifier(query):
    async def fetch(_url, _params):
        return {
            "results": [
                {
                    "name": "Columbia",
                    "admin1": "South Carolina",
                    "country_code": "US",
                    "postcodes": ["29201"],
                    "latitude": 34,
                    "longitude": -81,
                },
                {
                    "name": "Columbia",
                    "admin1": "Maryland",
                    "country_code": "US",
                    "postcodes": ["21044"],
                    "latitude": 39,
                    "longitude": -76,
                },
            ]
        }

    with pytest.raises(ValueError, match="not found"):
        await geocode(query, fetch_json=fetch)


def test_daily_range_uses_location_zone_across_dst_fallback():
    raw = json.loads((Path(__file__).parent / "fixtures/weather/berlin_forecast_c.json").read_text())
    zone = ZoneInfo("America/New_York")
    now = int(datetime(2026, 11, 1, 12, tzinfo=zone).timestamp())
    raw["timezone"] = "America/New_York"
    raw["utc_offset_seconds"] = -5 * 3600
    raw["current"]["time"] = now
    # Midnight preceded the fall transition and therefore had the EDT offset.
    raw["daily"]["time"] = [int(datetime(2026, 11, day, tzinfo=zone).timestamp()) for day in (1, 2)]
    raw["daily"]["temperature_2m_max"] = [21, 12]
    raw["daily"]["temperature_2m_min"] = [10, 3]
    raw["hourly"]["time"] = [now + (index + 1) * 3600 for index in range(len(raw["hourly"]["time"]))]
    value = parse_forecast(raw, WeatherLocation("New York", 40, -74), "C", now=now)
    assert (value["high"], value["low"]) == (21, 10)


@pytest.mark.parametrize("flags", [None, 42, "waitingOnApproval"])
def test_codex_malformed_status_flags_never_invent_approval_or_crash(flags):
    assert _status({"type": "active", "activeFlags": flags}) == ("working", None)
    assert _status({"type": {}, "activeFlags": flags}) == ("idle", None)


def test_codex_naive_timestamp_is_utc_and_independent_of_host_timezone():
    assert _time("2026-10-28T12:00:00") == datetime(2026, 10, 28, 12, tzinfo=UTC)


def test_codex_malformed_event_does_not_hide_valid_discovery(tmp_path):
    path = tmp_path / "rollout-fixture.jsonl"
    stamp = datetime.now(UTC).isoformat()
    records = [
        {"type": "session_meta", "timestamp": stamp, "payload": {"id": "fixture", "title": "Fixture"}},
        {"type": "event_msg", "timestamp": stamp, "payload": {"type": {"invalid": "event"}}},
        {"type": "event_msg", "timestamp": stamp, "payload": {"type": "user_message", "message": "Work"}},
    ]
    path.write_text("".join(json.dumps(record) + "\n" for record in records))
    store = StateStore()
    RolloutObserver(store, tmp_path).scan()
    assert store.get("codex:fixture").state == "working"


async def test_codex_discovery_reads_in_worker_and_publishes_on_event_loop(tmp_path):
    path = tmp_path / "rollout-fixture.jsonl"
    path.write_text(json.dumps({"type": "session_meta", "payload": {"id": "fixture"}}) + "\n")
    store = StateStore()
    adapter = CodexAdapter(store, sessions_root=tmp_path)
    event_thread = threading.get_ident()
    scan = adapter.observer.scan
    update = store.update

    def checked_scan(*args, **kwargs):
        assert threading.get_ident() != event_thread
        return scan(*args, **kwargs)

    def checked_update(session):
        assert threading.get_ident() == event_thread
        return update(session)

    adapter.observer.scan = checked_scan
    store.update = checked_update
    try:
        await adapter.refresh()
        assert store.get("codex:fixture") is not None
    finally:
        await adapter.close()
        await store.close()


@pytest.mark.parametrize("provider", ["claude", "codex"])
def test_deeply_nested_json_does_not_hide_valid_discovery(tmp_path, provider):
    nested = "[" * 20000 + "0" + "]" * 20000 + "\n"
    if provider == "claude":
        native = str(uuid.uuid4())
        folder = tmp_path / "projects" / "fixture"
        folder.mkdir(parents=True)
        (folder / f"{native}.jsonl").write_text(
            nested + json.dumps({"type": "assistant", "message": {"content": "Visible answer"}}) + "\n" + nested
        )
        assert _transcript_metadata(tmp_path, native)[1] == "Visible answer"
    else:
        (tmp_path / "rollout-fixture.jsonl").write_text(
            nested + json.dumps({"type": "session_meta", "payload": {"id": "fixture"}}) + "\n"
        )
        store = StateStore()
        RolloutObserver(store, tmp_path).scan()
        assert store.get("codex:fixture") is not None


async def test_claude_malformed_pid_does_not_hide_other_sessions(tmp_path):
    store = StateStore()
    adapter = ClaudeAdapter(store, config_dir=tmp_path)

    async def capture(*_args, command=None):
        if command:
            return b""
        return json.dumps([{"sessionId": "bad-pid", "pid": {}}, {"sessionId": "good", "name": "Good session"}]).encode()

    adapter._capture = capture
    try:
        await adapter.refresh()
        assert store.get("claude:good").title == "Good session"
    finally:
        await adapter.close()
        await store.close()


def test_weekly_only_codex_primary_survives_both_source_channels():
    collector = UsageCollector()
    limits = {"primary": {"used_percent": 31, "window_minutes": 10080, "resets_at": 20000}, "secondary": None}
    record = {"type": "event_msg", "timestamp": 1000, "payload": {"type": "token_count", "rate_limits": limits}}
    assert collector.observe_codex(record)
    assert collector.snapshot(1001)["unknown"] == ["claude"]
    assert [(window["label"], window["used_percent"]) for window in collector.snapshot(1001)["windows"]] == [
        ("week", 31)
    ]
    app_limits = {"primary": {"usedPercent": 32, "windowDurationMins": 10080, "resetsAt": 20000}, "secondary": None}
    assert collector.observe_app_server({"rateLimits": copy.deepcopy(app_limits)}, 1002)
    assert [(window["label"], window["used_percent"]) for window in collector.snapshot(1003)["windows"]] == [
        ("week", 32)
    ]


@pytest.mark.parametrize("source", ["rollout", "app-server", "usage", "statusline"])
def test_future_quota_record_cannot_poison_later_valid_observations(monkeypatch, source):
    monkeypatch.setattr("orb_agents.usage.time.time", lambda: 1000)
    collector = UsageCollector()

    def observe(at, used):
        if source == "rollout":
            return collector.observe_codex(
                {
                    "type": "event_msg",
                    "timestamp": at,
                    "payload": {
                        "type": "token_count",
                        "rate_limits": {"primary": {"used_percent": used, "window_minutes": 300, "resets_at": 5000}},
                    },
                }
            )
        if source == "app-server":
            return collector.observe_app_server(
                {"rateLimits": {"primary": {"usedPercent": used, "windowDurationMins": 300, "resetsAt": 5000}}}, at
            )
        observation = UsageObservation("claude", at, (UsageWindow(300, used, 5000),), source=source)
        return collector._set_claude(source, observation)

    assert observe(1000, 10)
    assert not observe(90000, 99)
    assert observe(1001, 11)
    assert collector.snapshot(1002)["windows"][0]["used_percent"] == 11
