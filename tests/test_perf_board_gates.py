"""Run only the board-script evaluator against throwaway receipts; no board I/O."""

import json
import re
import subprocess
import sys
from pathlib import Path

import pytest
import speed_ab

SCRIPT = Path(__file__).resolve().parents[1] / "docs/agent-briefs/wave2/perf-board-commands.sh"
EVALUATOR = re.findall(r"<<'PY'\n(.*?)\nPY", SCRIPT.read_text(), re.S)[-1]


def test_board_build_flags_match_real_harness():
    command = SCRIPT.read_text().split("scripts/speed_ab.py build", 1)[1].split("cp firmware", 1)[0]
    enabled = re.findall(r"--enable\s+(\w+)", command)
    assert enabled and all(option in speed_ab.BASE_OPTIONS for option in enabled)
    assert "--disable" not in command  # Unlisted experiments are off in this harness.


def test_round_two_uses_a_fresh_run_directory():
    assert "$PWD/work/perf/board-run-2}" in SCRIPT.read_text()


@pytest.fixture
def candidate(tmp_path):
    for phase in ("navigation-before", "navigation-after"):
        directory = tmp_path / phase
        directory.mkdir()
        for gesture in (
            "slide-left",
            "slide-right",
            "stack-up",
            "shade-down",
            "back",
            "iris",
            "rotary",
        ):
            for run in range(1, 4):
                (directory / f"nav-{gesture}-{run}.txt").write_text(
                    f"@@ORBDEV NAV gesture={gesture} fps=60 errors=0 failed_frames=0\n"
                )
        for name in ("heap-before.txt", "heap.txt"):
            (directory / name).write_text(
                "HEAP internal free 71000 largest 31000 min-free 29232\n"
                "HEAP dma free 63000 largest 31000 min-free 21444\n"
                "HEAP psram free 2200000 largest 1500000 min-free 1417596\n"
                "HEAP_OWNER HEAP_CACHE HEAP_PSRAM NAV_ADMISSION\n"
            )
    measurement = tmp_path / "measurement"
    measurement.mkdir()
    (measurement / "open.json").write_text(
        json.dumps(
            {
                "captures": [
                    {
                        "surface": surface,
                        "cold": 0,
                        "first_changed_band_us": 11000,
                        "first_changed_frame_us": 19000,
                    }
                    for surface in ("apps", "play", "agents", "dot", "back")
                    for _ in range(3)
                ]
            }
        )
    )
    (measurement / "surfaces.json").write_text(
        json.dumps(
            {
                "captures": [
                    {"capture": f"scroll-{surface}-{run}", "handler_max_us": 32999}
                    for surface in ("favourites", "rooms", "agents", "settings", "notifications")
                    for run in range(1, 4)
                ]
            }
        )
    )
    return tmp_path


def evaluate(candidate):
    result = subprocess.run(
        [sys.executable, "-", str(candidate)],
        input=EVALUATOR,
        text=True,
        capture_output=True,
        check=False,
    )
    report = json.loads((candidate / "perf-gates.json").read_text())
    assert (result.returncode == 0) == report["pass"]
    return report


def test_complete_receipts_pass(candidate):
    assert evaluate(candidate)["pass"]


@pytest.mark.parametrize("kind,floor", [("internal", 29232), ("dma", 21444), ("psram", 1417596)])
def test_lifetime_floor_not_steady_free_is_gated(candidate, kind, floor):
    heap = candidate / "navigation-after/heap.txt"
    heap.write_text(heap.read_text().replace(f"min-free {floor}", f"min-free {floor - 1}"))
    assert not evaluate(candidate)["pass"]


@pytest.mark.parametrize(
    "surface,metric,value",
    [
        ("apps", "first_changed_frame_us", 20001),
        ("apps", "first_changed_frame_us", 0),
        ("back", "first_changed_band_us", 30001),
        ("back", "first_changed_frame_us", 30001),
    ],
)
def test_changed_frame_and_band_are_separate_gates(candidate, surface, metric, value):
    path = candidate / "measurement/open.json"
    report = json.loads(path.read_text())
    next(row for row in report["captures"] if row["surface"] == surface)[metric] = value
    path.write_text(json.dumps(report))
    assert not evaluate(candidate)["pass"]


def test_iris_must_reach_fifty_five(candidate):
    path = candidate / "navigation-before/nav-iris-1.txt"
    path.write_text(path.read_text().replace("fps=60", "fps=54.99"))
    assert not evaluate(candidate)["pass"]


def test_iris_target_does_not_depend_on_faster_slides(candidate):
    path = candidate / "navigation-before/nav-iris-1.txt"
    path.write_text(path.read_text().replace("fps=60", "fps=55"))
    assert evaluate(candidate)["pass"]


def test_rotary_must_reach_fifty_five(candidate):
    path = candidate / "navigation-before/nav-rotary-1.txt"
    path.write_text(path.read_text().replace("fps=60", "fps=54.99"))
    assert not evaluate(candidate)["pass"]


@pytest.mark.parametrize("name", ["heap-before.txt", "heap.txt"])
def test_missing_heap_cannot_pass(candidate, name):
    (candidate / "navigation-after" / name).unlink()
    assert not evaluate(candidate)["pass"]


def test_missing_navigation_cannot_pass_empty_glob(candidate):
    (candidate / "navigation-after/nav-back-1.txt").unlink()
    assert not evaluate(candidate)["pass"]


def test_handler_threshold_is_strict(candidate):
    path = candidate / "measurement/surfaces.json"
    report = json.loads(path.read_text())
    report["captures"][0]["handler_max_us"] = 33000
    path.write_text(json.dumps(report))
    assert not evaluate(candidate)["pass"]


def test_incomplete_open_workload_fails(candidate):
    path = candidate / "measurement/open.json"
    report = json.loads(path.read_text())
    report["captures"].pop()
    path.write_text(json.dumps(report))
    assert not evaluate(candidate)["pass"]
