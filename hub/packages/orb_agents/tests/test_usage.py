"""All quota records are synthetic; never inspect the owner's provider files."""

import asyncio
import contextlib
import json
import os
import stat
import sys
from datetime import UTC, datetime
from pathlib import Path
from zoneinfo import ZoneInfo

import pytest

from orb_agents.usage import (
    CLAUDE_USAGE_INTERVAL,
    CLAUDE_USAGE_TIMEOUT,
    ClaudeUsagePoller,
    UsageCollector,
    claude_usage_path,
    observation_time,
    parse_claude_reset,
    parse_claude_statusline,
    parse_claude_usage_output,
    parse_codex_rate_limits,
    poll_claude_usage,
    read_claude_usage,
    resolve_claude_binary,
    window_label,
    write_claude_statusline,
)

FIXTURE = Path(__file__).parent / "fixtures" / "usage-codex.jsonl"
USAGE_FIXTURES = Path(__file__).parent / "fixtures"
OBSERVED = 1761566400.0  # 2025-10-27 12:00 UTC


def test_synthetic_rollout_uses_newest_observation_not_token_totals():
    collector = UsageCollector()
    records = [json.loads(line) for line in FIXTURE.read_text().splitlines()]
    assert collector.observe_codex(records[0])
    observed = observation_time(records[0]["timestamp"])
    snapshot = collector.snapshot(observed + 1)
    assert snapshot["unknown"] == ["claude"]
    assert [w["label"] for w in snapshot["windows"]] == ["5 hours", "week"]
    assert [w["used_percent"] for w in snapshot["windows"]] == [23.5, 61]
    assert collector.observations["codex"].credits == {
        "has_credits": True,
        "unlimited": False,
        "balance": "12.50",
    }
    assert collector.observations["codex"].limit_name == "Codex"
    assert not collector.observe_codex(records[1])
    assert collector.observe_codex(records[2])
    assert not collector.observe_codex(records[0])
    assert [w["label"] for w in collector.snapshot(observed + 4)["windows"]] == [
        "15 minutes"
    ]
    assert "credits" not in collector.snapshot(observed + 4)


def test_app_server_camel_case_and_bucket_selection():
    collector = UsageCollector()
    window = {"usedPercent": 0.25, "windowDurationMins": 60, "resetsAt": 3000}
    assert collector.observe_app_server(
        {
            "rateLimits": {"primary": dict(window, usedPercent=99)},
            "rateLimitsByLimitId": {"codex": {"primary": window, "secondary": None}},
        },
        1000,
    )
    value = collector.snapshot(1001)["windows"][0]
    assert value["used_percent"] == 0.25  # Percent units, never multiplied by 100.
    assert value["label"] == "1 hour"
    assert value["resets_at"] == 3000


@pytest.mark.parametrize(
    ("minutes", "label"),
    [
        (300, "5 hours"),
        (10080, "week"),
        (15, "15 minutes"),
        (120, "2 hours"),
        (1, "1 minute"),
    ],
)
def test_duration_labels(minutes, label):
    assert window_label(minutes) == label


@pytest.mark.parametrize(
    "value", [-1, 101, 10**1000, True, float("inf"), float("nan"), "50", None]
)
def test_invalid_percentages_are_not_zero_meters(value):
    limits = {
        "primary": {"used_percent": value, "window_minutes": 300, "resets_at": 2000}
    }
    assert parse_codex_rate_limits(limits, 1000) is None
    assert (
        parse_claude_statusline(
            {
                "rate_limits": {
                    "five_hour": {"used_percentage": value, "resets_at": 2000}
                }
            },
            1000,
        )
        is None
    )


@pytest.mark.parametrize(
    "reset", [None, "2026-10-27T12:00:00Z", 1793102400000, -1, True]
)
def test_reset_units_are_unix_seconds_only(reset):
    assert (
        parse_codex_rate_limits(
            {
                "primary": {
                    "used_percent": 50,
                    "window_minutes": 300,
                    "resets_at": reset,
                }
            },
            1000,
        )
        is None
    )


def test_nullable_windows_unknown_empty_expiry_and_staleness():
    collector = UsageCollector()
    assert collector.snapshot(1000) == {"windows": [], "unknown": ["claude", "codex"]}
    record = {
        "type": "event_msg",
        "timestamp": 1000,
        "payload": {
            "type": "token_count",
            "rate_limits": {"primary": None, "secondary": None},
        },
    }
    assert collector.observe_codex(record)
    assert collector.snapshot(1000) == {"windows": [], "unknown": ["claude"]}
    record["payload"]["rate_limits"]["primary"] = {
        "used_percent": 100,
        "window_minutes": 300,
        "resets_at": 3000,
    }
    assert collector.observe_codex(record)
    assert not collector.snapshot(1900)["windows"][0]["stale"]
    assert collector.snapshot(1901)["windows"][0]["stale"]
    assert collector.snapshot(3000)["windows"] == []


def test_codex_missing_timestamp_does_not_refresh_quota_age():
    collector = UsageCollector()
    assert not collector.observe_codex(
        {
            "type": "event_msg",
            "payload": {"type": "token_count", "rate_limits": {"primary": None}},
        }
    )
    assert not collector.observe_codex(
        {
            "type": "response_item",
            "timestamp": 1000,
            "payload": {"rate_limits": {"primary": None}},
        }
    )
    assert observation_time("2026-10-27T12:00:00") is None
    assert observation_time("not a timestamp") is None


def test_statusline_file_private_sanitized_atomic_and_stale(tmp_path):
    directory = tmp_path / "hub"
    payload = {
        "session_id": "SYNTHETIC-DO-NOT-STORE",
        "workspace": {"current_dir": "private"},
        "rate_limits": {
            "five_hour": {"used_percentage": 23, "resets_at": 5000},
            "seven_day": None,
        },
    }
    write_claude_statusline(directory, payload, now=1000)
    path = claude_usage_path(directory)
    assert stat.S_IMODE(path.stat().st_mode) == 0o600
    saved = json.loads(path.read_text())
    assert saved == {
        "observed_at": 1000,
        "rate_limits": {"five_hour": {"used_percentage": 23, "resets_at": 5000}},
    }
    assert not list(path.parent.glob("*.tmp"))
    collector = UsageCollector(path)
    assert collector.refresh_claude()
    assert not collector.refresh_claude()
    assert collector.snapshot(1001)["windows"][0]["label"] == "5 hours"
    assert collector.snapshot(1901)["windows"][0]["stale"]
    assert collector.snapshot(1001)["unknown"] == ["codex"]
    write_claude_statusline(directory, {}, now=2000)
    assert json.loads(path.read_text()) == saved
    path.unlink()
    assert collector.refresh_claude()
    assert collector.snapshot(2000)["unknown"] == ["claude", "codex"]


@pytest.mark.parametrize(
    "content",
    [
        "{",
        "[]",
        '{"observed_at":1000,"rate_limits":{"five_hour":42}}',
        '{"observed_at":NaN,"rate_limits":{}}',
    ],
)
def test_corrupt_statusline_file_is_unknown(tmp_path, content):
    tmp_path.chmod(0o700)
    path = tmp_path / "usage.json"
    path.write_text(content)
    path.chmod(0o600)
    assert read_claude_usage(path) is None


def test_statusline_rejects_symlinks_shared_files_and_nonregular_state(tmp_path):
    tmp_path.chmod(0o700)
    target = tmp_path / "target"
    target.write_text('{"observed_at":1000,"rate_limits":{}}')
    target.chmod(0o600)
    link = tmp_path / "link"
    link.symlink_to(target)
    assert read_claude_usage(link) is None
    target.chmod(0o644)
    assert read_claude_usage(target) is None
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo, 0o600)
    assert read_claude_usage(fifo) is None
    directory = tmp_path / "hub"
    directory.symlink_to(tmp_path, target_is_directory=True)
    with pytest.raises(OSError):
        write_claude_statusline(directory, {"rate_limits": {}}, now=1000)


def test_statusline_empty_windows_are_known_and_clock_skew_unknown(tmp_path):
    directory = tmp_path / "hub"
    write_claude_statusline(directory, {"rate_limits": {}}, now=1000)
    collector = UsageCollector(claude_usage_path(directory))
    assert collector.refresh_claude()
    assert collector.snapshot(1000) == {"windows": [], "unknown": ["codex"]}
    assert collector.snapshot(0) == {"windows": [], "unknown": ["claude", "codex"]}


def _synthetic(name):
    return (USAGE_FIXTURES / name).read_text()


def _fake_claude(directory, body):
    """A local executable that can be the only `claude` on PATH."""
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "claude"
    path.write_text(f"#!{sys.executable}\n{body}")
    path.chmod(0o755)
    return path


def _usage_body(source):
    return (
        "import pathlib, sys\n"
        "if sys.argv[1:] != ['-p', '/usage']:\n"
        "    sys.exit(9)\n"
        f"sys.stdout.write(pathlib.Path({str(source)!r}).read_text())\n"
    )


def _count(path):
    try:
        return int(path.read_text())
    except (OSError, ValueError):
        return 0


async def _eventually(predicate):
    async with asyncio.timeout(5):
        for _ in range(500):
            if predicate():
                return
            await asyncio.sleep(0.01)
    raise TimeoutError("synthetic condition did not settle")


def test_parse_synthetic_usage_example_maps_session_and_week():
    observation = parse_claude_usage_output(_synthetic("usage-claude.txt"), OBSERVED)
    assert observation is not None
    assert observation.provider == "claude"
    assert observation.source == "usage"
    assert [
        (window.label, window.minutes, window.used_percent, window.resets_at)
        for window in observation.windows
    ] == [
        (None, 300, 2, 1761640740),
        (None, 10080, 76, 1761681540),
        ("Fable", 10080, 0, 1761681600),
    ]


def test_parse_missing_and_garbage_lines_is_defensive():
    missing = parse_claude_usage_output(
        _synthetic("usage-claude-missing.txt"), OBSERVED
    )
    assert missing is not None
    assert [
        (window.label, window.minutes, window.used_percent)
        for window in missing.windows
    ] == [(None, 10080, 41)]
    assert (
        parse_claude_usage_output(_synthetic("usage-claude-garbage.txt"), OBSERVED)
        is None
    )
    assert (
        parse_claude_usage_output("You are currently using your subscription", OBSERVED)
        is None
    )
    assert parse_claude_usage_output("", OBSERVED) is None
    assert (
        parse_claude_usage_output(_synthetic("usage-claude.txt"), "not a time") is None
    )


def test_parse_reworded_lines_and_year_rollover():
    observation = parse_claude_usage_output(
        _synthetic("usage-claude-reworded.txt"), 1767211200.0
    )
    assert observation is not None
    assert [
        (window.label, window.minutes, window.used_percent, window.resets_at)
        for window in observation.windows
    ] == [
        (None, 300, 12.5, 1767243540),
        (None, 10080, 45, 1767330000),
        ("Sonnet", 10080, 3, 1767474000),
    ]
    # The same October date after Oct 28 must roll into the next year.
    rolled = parse_claude_usage_output(_synthetic("usage-claude.txt"), 1761998400.0)
    assert rolled is not None
    assert rolled.windows[0].resets_at == 1793176740


def test_reset_text_requires_a_valid_zone_and_clock():
    assert (
        parse_claude_reset("resets Oct 28 at 4:39am (America/New_York)", OBSERVED)
        == 1761640740
    )
    assert (
        parse_claude_reset("resets Oct 28 at 4pm (America/New_York)", OBSERVED)
        == 1761681600
    )
    assert parse_claude_reset("resets Oct 28 at 4:39am", OBSERVED) is None
    assert parse_claude_reset("resets Oct 28 at 4:39am (Not/AZone)", OBSERVED) is None
    assert parse_claude_reset("resets someday", OBSERVED) is None


async def test_poll_uses_only_a_fake_claude_from_path(tmp_path, monkeypatch):
    binary_dir = tmp_path / "bin"
    binary = _fake_claude(binary_dir, _usage_body(USAGE_FIXTURES / "usage-claude.txt"))
    monkeypatch.setenv("PATH", str(binary_dir))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    assert resolve_claude_binary() == binary
    observation = await poll_claude_usage(
        now=OBSERVED, timeout_seconds=CLAUDE_USAGE_TIMEOUT
    )
    assert observation is not None
    assert observation.source == "usage"
    assert [window.used_percent for window in observation.windows] == [2, 76, 0]


async def test_poll_falls_back_to_the_user_local_binary(tmp_path, monkeypatch):
    monkeypatch.setenv("PATH", str(tmp_path / "empty"))
    home = tmp_path / "home"
    binary = _fake_claude(
        home / ".local" / "bin",
        _usage_body(USAGE_FIXTURES / "usage-claude-missing.txt"),
    )
    monkeypatch.setenv("HOME", str(home))
    assert resolve_claude_binary() == binary
    observation = await poll_claude_usage(
        now=OBSERVED, timeout_seconds=CLAUDE_USAGE_TIMEOUT
    )
    assert observation is not None
    assert [window.used_percent for window in observation.windows] == [41]


@pytest.mark.parametrize(
    ("body", "timeout_seconds"),
    [
        ("import sys\nsys.exit(1)\n", 5),
        ("import sys\nsys.stdout.write('synthetic garbage')\n", 5),
        ("import time\ntime.sleep(2)\n", 0.1),
    ],
)
async def test_poll_failure_is_unknown_and_never_logged(
    tmp_path, monkeypatch, capsys, body, timeout_seconds
):
    binary_dir = tmp_path / "bin"
    _fake_claude(binary_dir, body)
    monkeypatch.setenv("PATH", str(binary_dir))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    assert (
        await poll_claude_usage(now=OBSERVED, timeout_seconds=timeout_seconds) is None
    )
    captured = capsys.readouterr()
    assert captured.out == ""
    assert captured.err == ""


def test_default_polling_is_at_most_every_five_minutes_with_a_30_second_timeout():
    assert CLAUDE_USAGE_INTERVAL == 300
    assert CLAUDE_USAGE_TIMEOUT == 30
    poller = ClaudeUsagePoller(UsageCollector())
    assert poller.interval == 300
    assert poller.timeout == 30


async def test_poller_polls_immediately_without_waiting_for_the_interval(
    tmp_path, monkeypatch
):
    binary_dir = tmp_path / "bin"
    _fake_claude(binary_dir, _usage_body(USAGE_FIXTURES / "usage-claude.txt"))
    monkeypatch.setenv("PATH", str(binary_dir))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    collector = UsageCollector()
    poller = ClaudeUsagePoller(collector, interval=60, timeout=5, now=lambda: OBSERVED)
    task = asyncio.create_task(poller.run())
    try:
        await _eventually(lambda: "claude" in collector.observations)
        snapshot = collector.snapshot(OBSERVED + 1)
        assert snapshot["windows"][0]["source"] == "usage"
        assert [window["label"] for window in snapshot["windows"]] == [
            "5 hours",
            "week",
            "Fable",
        ]
    finally:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task


async def test_repeated_failures_back_off_and_mark_claude_unknown(
    tmp_path, monkeypatch
):
    counter = tmp_path / "count"
    body = (
        "import pathlib, sys\n"
        f"path = pathlib.Path({str(counter)!r})\n"
        "current = path.read_text() if path.exists() else '0'\n"
        "path.write_text(str(int(current or '0') + 1))\n"
        "sys.exit(1)\n"
    )
    binary_dir = tmp_path / "bin"
    _fake_claude(binary_dir, body)
    monkeypatch.setenv("PATH", str(binary_dir))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    collector = UsageCollector()
    poller = ClaudeUsagePoller(
        collector, interval=0.02, timeout=1, max_backoff=0.05, now=lambda: OBSERVED
    )
    task = asyncio.create_task(poller.run())
    try:
        await _eventually(lambda: _count(counter) >= 3)
    finally:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task
    assert poller.failure_delay(1) == 0.02
    assert poller.failure_delay(2) == 0.04
    assert poller.failure_delay(3) == 0.05
    assert poller.failure_delay(99) == 0.05
    assert collector.snapshot(OBSERVED)["unknown"] == ["claude", "codex"]


def test_freshest_claude_source_wins_and_unknown_on_failure(tmp_path):
    directory = tmp_path / "hub"
    statusline = {
        "rate_limits": {"five_hour": {"used_percentage": 11, "resets_at": 5000}}
    }
    write_claude_statusline(directory, statusline, now=1000)
    collector = UsageCollector(claude_usage_path(directory))
    assert collector.refresh_claude()
    assert collector.snapshot(1001)["windows"][0]["source"] == "statusline"

    usage = parse_claude_usage_output(_synthetic("usage-claude.txt"), 2000)
    assert collector.observe_claude_usage(usage)
    assert collector.snapshot(2001)["windows"][0]["source"] == "usage"

    statusline["rate_limits"]["five_hour"] = {
        "used_percentage": 22,
        "resets_at": 6000,
    }
    write_claude_statusline(directory, statusline, now=3000)
    assert collector.refresh_claude()
    assert collector.snapshot(3001)["windows"][0]["source"] == "statusline"

    # Losing the poll does not erase a fresher statusline reading.
    assert not collector.fail_claude_usage()
    assert collector.snapshot(3001)["windows"][0]["source"] == "statusline"
    claude_usage_path(directory).unlink()
    assert collector.refresh_claude()
    assert collector.snapshot(3001) == {"windows": [], "unknown": ["claude", "codex"]}


def test_multiline_cli_usage_ansi_and_clock_only_reset_keep_owner_timezone():
    observed = datetime(2026, 10, 28, 16, tzinfo=UTC).timestamp()
    text = _synthetic("usage-claude-multiline.txt")
    observation = parse_claude_usage_output("\x1b[1m" + text + "\x1b[0m", observed)
    assert observation is not None
    assert [(w.minutes, w.used_percent, w.label) for w in observation.windows] == [
        (300, 28, None),
        (10080, 41.5, None),
        (10080, 5, "Sonnet"),
    ]
    zone = ZoneInfo("America/New_York")
    assert [
        datetime.fromtimestamp(w.resets_at, zone).isoformat()
        for w in observation.windows
    ] == [
        "2026-10-28T16:39:00-04:00",
        "2026-11-02T00:00:00-05:00",
        "2026-11-02T09:00:00-05:00",
    ]


def test_clock_only_reset_after_midnight_and_dst_repeated_wall_time():
    zone = ZoneInfo("America/New_York")
    before = datetime(2026, 10, 28, 23, 59, tzinfo=zone).timestamp()
    reset = parse_claude_reset("Resets 12:30am (America/New_York)", before)
    assert (
        datetime.fromtimestamp(reset, zone).isoformat() == "2026-10-29T00:30:00-04:00"
    )
    before = datetime(2026, 11, 1, 1, 45, tzinfo=zone, fold=0).timestamp()
    reset = parse_claude_reset("Resets Nov 1 at 1:30am (America/New_York)", before)
    assert (
        datetime.fromtimestamp(reset, zone).isoformat() == "2026-11-01T01:30:00-05:00"
    )
    assert parse_claude_reset("Resets 4pm (unknown/zone)", before) is None
    assert parse_claude_reset("Resets 4pm", before) is None


def test_cached_usage_output_never_renews_freshness_or_turns_cost_into_quota():
    text = _synthetic("usage-claude.txt")
    assert (
        parse_claude_usage_output(
            text + "\nShowing last-known usage (5m old)", OBSERVED
        )
        is None
    )
    assert (
        parse_claude_usage_output(
            "Session\nTotal cost: $0.50\nTotal API duration: 5h\nUsage by model: 20%",
            OBSERVED,
        )
        is None
    )


def test_codex_weekly_primary_and_unrelated_named_buckets_do_not_mix():
    collector = UsageCollector()
    weekly = {"used_percent": 31.5, "window_minutes": 10080, "resets_at": 20000}
    record = {
        "type": "event_msg",
        "timestamp": 1000,
        "payload": {
            "type": "token_count",
            "info": {"total_token_usage": {"total_tokens": 999999999}},
            "rate_limits": {
                "limit_id": "codex",
                "primary": weekly,
                "secondary": None,
                "plan_type": "pro",
            },
        },
    }
    assert collector.observe_codex(record)
    assert collector.snapshot(1001)["windows"][0]["label"] == "week"
    record["timestamp"] = 1002
    record["payload"]["rate_limits"]["limit_id"] = "codex_other_model"
    record["payload"]["rate_limits"]["primary"]["used_percent"] = 99
    assert not collector.observe_codex(record)
    assert collector.snapshot(1003)["windows"][0]["used_percent"] == 31.5
    assert not collector.observe_app_server(
        {
            "rateLimits": {
                "limitId": "codex_other_model",
                "primary": {
                    "usedPercent": 99,
                    "windowDurationMins": 300,
                    "resetsAt": 20000,
                },
            }
        },
        1004,
    )


def test_model_label_is_bounded_in_utf8_bytes():
    observation = parse_claude_usage_output(
        f"Current week ({'界' * 40}): 5% used · resets Oct 28 at 4pm (America/New_York)",
        OBSERVED,
    )
    assert observation is not None
    assert 0 < len(observation.windows[0].label.encode()) <= 32


def test_codex_on_credits_when_limit_at_100_and_positive_balance():
    collector = UsageCollector()
    weekly = {"used_percent": 100, "window_minutes": 10080, "resets_at": 20000}
    record = {
        "type": "event_msg",
        "timestamp": 1000,
        "payload": {
            "type": "token_count",
            "info": {},
            "rate_limits": {
                "limit_id": "codex",
                "primary": weekly,
                "secondary": None,
                "credits": {"balance": "15.50", "has_credits": True},
            },
        },
    }
    assert collector.observe_codex(record)
    snap = collector.snapshot(1001)
    win = snap["windows"][0]
    assert win["used_percent"] == 100
    assert win["on_credits"] is True
    # The balance number itself is never included in the window payload
    assert "balance" not in win


def test_codex_on_credits_not_set_when_under_100_or_zero_balance():
    collector = UsageCollector()
    # 1. 99% used with positive balance -> no on_credits
    record_under = {
        "type": "event_msg",
        "timestamp": 1000,
        "payload": {
            "type": "token_count",
            "info": {},
            "rate_limits": {
                "limit_id": "codex",
                "primary": {
                    "used_percent": 99,
                    "window_minutes": 10080,
                    "resets_at": 20000,
                },
                "secondary": None,
                "credits": {"balance": "15.50", "has_credits": True},
            },
        },
    }
    assert collector.observe_codex(record_under)
    snap = collector.snapshot(1001)
    assert "on_credits" not in snap["windows"][0]

    # 2. 100% used with 0 balance -> no on_credits
    record_zero = {
        "type": "event_msg",
        "timestamp": 1002,
        "payload": {
            "type": "token_count",
            "info": {},
            "rate_limits": {
                "limit_id": "codex",
                "primary": {
                    "used_percent": 100,
                    "window_minutes": 10080,
                    "resets_at": 20000,
                },
                "secondary": None,
                "credits": {"balance": "0.00", "has_credits": False},
            },
        },
    }
    assert collector.observe_codex(record_zero)
    snap = collector.snapshot(1003)
    assert "on_credits" not in snap["windows"][0]
