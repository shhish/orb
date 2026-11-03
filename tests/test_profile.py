"""The fixture is synthetic data in the exact formatter's recorded text shape."""

import json
import subprocess
import sys
from pathlib import Path

import pytest
from orb_devtools.profile import convert, main, parse, summary

FIXTURE = Path(__file__).parent / "fixtures" / "profile-scrolling.ftrace"
ROOT = Path(__file__).parents[1]


def record(phase, name, ms, tid=1, cpu=0):
    return f"   LVGL-{tid} [{cpu}] {ms / 1000:.9f}: tracing_mark_write: {phase}|1|{name}\n"


def test_nested_and_concurrent_totals():
    trace = convert(FIXTURE.read_text())
    rows = {row["name"]: row for row in trace["otherData"]["functions"]}
    assert trace["otherData"]["warnings"] == []
    assert rows["lv_display_refr_timer"] == {
        "name": "lv_display_refr_timer",
        "calls": 2,
        "self_us": 11000.0,
        "inclusive_us": 18000.0,
    }
    assert rows["lv_refr_area"]["self_us"] == 3000
    assert rows["lv_draw_sw_blend"]["inclusive_us"] == 4000
    frames = trace["otherData"]["frames"]
    assert [frame["wall_us"] for frame in frames] == [10000, 8000]
    assert [frame["scoped_self_us"] for frame in frames] == [14000, 8000]
    assert "11000" not in summary(trace)  # summary is milliseconds
    assert "11.000" in summary(trace)


def test_core_tracks_use_original_task_ids():
    trace = convert(FIXTURE.read_text())
    spans = [event for event in trace["traceEvents"] if event["ph"] == "X"]
    assert {(span["pid"], span["tid"]) for span in spans} == {(1, 33), (2, 17)}
    assert min(span["ts"] for span in spans) == 0
    assert all(span["dur"] >= 0 for span in spans)
    assert len([event for event in trace["traceEvents"] if event["name"] == "process_name"]) == 2


def test_orb_logd_jsonl_console_is_decoded_before_parsing():
    text = FIXTURE.read_text()
    jsonl = "\n".join(
        json.dumps({"time": "2026-10-26Z", "boot": "fake", "text": line})
        for line in text.splitlines()
    )
    assert convert(jsonl) == convert(text)


def test_recursive_scopes_subtract_direct_children_once():
    scopes, warnings = parse(
        record("B", "a", 0) + record("B", "a", 2) + record("E", "a", 4) + record("E", "a", 10)
    )
    assert not warnings
    assert [scope.self_ns for scope in scopes] == [8_000_000, 2_000_000]
    row = convert(
        record("B", "a", 0) + record("B", "a", 2) + record("E", "a", 4) + record("E", "a", 10)
    )["otherData"]["functions"][0]
    assert row["self_us"] == 10000
    assert row["inclusive_us"] == 12000


def test_scopes_are_matched_by_task_even_when_same_cpu():
    trace = (
        record("B", "a", 0, 1)
        + record("B", "b", 1, 2)
        + record("E", "a", 2, 1)
        + record("E", "b", 3, 2)
    )
    scopes, warnings = parse(trace)
    assert not warnings
    assert [s.self_ns for s in scopes] == [2_000_000, 2_000_000]


@pytest.mark.parametrize(
    "text, expected",
    [
        (record("E", "a", 1), "unmatched end"),
        (record("B", "a", 1), "unclosed scope"),
        (record("B", "a", 1) + record("E", "b", 2), "mismatches"),
        (record("B", "a", 2) + record("E", "a", 1), "time moved backwards"),
        ("LVGL-x [0] broken: tracing_mark_write: B|1|name", "malformed"),
        (record("B", "a", 1) + "=== PROFILE END ===\n", "capture boundary"),
    ],
)
def test_invalid_or_truncated_scopes_never_get_invented_duration(text, expected):
    scopes, warnings = parse(text)
    assert scopes == []
    assert any(expected in warning for warning in warnings)


def test_completed_children_survive_truncated_parent():
    scopes, warnings = parse(
        record("B", "frame", 0) + record("B", "draw", 1) + record("E", "draw", 2)
    )
    assert [scope.name for scope in scopes] == ["draw"]
    assert len(warnings) == 1


def test_migration_records_uncertainty_and_entry_core():
    trace = convert(record("B", "a", 0, cpu=0) + record("E", "a", 2, cpu=1))
    span = [item for item in trace["traceEvents"] if item["ph"] == "X"][0]
    assert span["pid"] == 1
    assert span["args"]["exit_cpu"] == 1
    assert "transition time unavailable" in trace["otherData"]["warnings"][0]


def test_per_frame_clips_worker_scopes_and_exclusive_intervals():
    text = (
        record("B", "worker", 0, tid=2)
        + record("B", "frame", 2)
        + record("B", "nested", 3, tid=2)
        + record("E", "frame", 5)
        + record("E", "nested", 6, tid=2)
        + record("E", "worker", 8, tid=2)
    )
    frame = convert(text, "frame")["otherData"]["frames"][0]
    rows = {row["name"]: row for row in frame["functions"]}
    assert rows["worker"]["inclusive_us"] == 3000
    assert rows["worker"]["self_us"] == 1000
    assert rows["nested"]["self_us"] == 2000
    assert frame["scoped_self_us"] == 6000


def test_nanosecond_precision_and_host_timestamp():
    text = (
        "2026-10-26T12:00:00Z LVGL-1 [0] 10000.000000001: tracing_mark_write: B|1|a\n"
        + "LVGL-1 [0] 10000.000000011: tracing_mark_write: E|1|a\n"
    )
    scopes, warnings = parse(text)
    assert not warnings
    assert scopes[0].inclusive_ns == 10


def test_no_frames_explains_configuration():
    trace = convert(record("B", "a", 0) + record("E", "a", 1))
    assert "--frame NAME" in summary(trace)


def test_cli_writes_trace_and_strict_reports_warning(tmp_path, capsys):
    source = tmp_path / "profile.txt"
    output = tmp_path / "trace.json"
    source.write_text(FIXTURE.read_text() + record("B", "unfinished", 2000))
    assert main([str(source), str(output), "--strict"]) == 1
    assert json.loads(output.read_text())["otherData"]["warnings"]
    assert "WARNING:" in capsys.readouterr().out


def test_empty_capture_fails(tmp_path):
    path = tmp_path / "empty"
    path.write_text("console only")
    with pytest.raises(SystemExit, match="2"):
        main([str(path), str(tmp_path / "trace")])


def test_entrypoint_subprocess(tmp_path):
    output = tmp_path / "trace.json"
    result = subprocess.run(
        [sys.executable, str(ROOT / "scripts/orb-profile-convert.py"), str(FIXTURE), str(output)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert "Frames: lv_display_refr_timer" in result.stdout
    assert len(json.loads(output.read_text())["otherData"]["frames"]) == 2


def test_shell_existing_capture_path_with_spaces(tmp_path):
    output = tmp_path / "trace with spaces.json"
    result = subprocess.run(
        [str(ROOT / "scripts/orb-profile.sh"), "--input", str(FIXTURE), str(output)],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert output.exists()
    assert output.with_suffix(".summary.txt").exists()
