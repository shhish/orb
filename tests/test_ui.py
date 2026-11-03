"""Offline visual comparisons and complete scenarios; never touch hardware."""

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest
import yaml
from orb_devtools.ui import (
    CommandBackend,
    FixtureBackend,
    ScenarioError,
    _atomic_png,
    _lab,
    compare_images,
    load_scenario,
    main,
    run_scenario,
    validate_scenario,
    write_report,
)
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def image(color="black", size=(4, 4)):
    return Image.new("RGBA", size, color)


def scenario(steps=None):
    return {
        "version": 1,
        "name": "test",
        "size": [4, 4],
        "steps": steps or [{"shot": "one"}, {"expect": "one.png"}],
    }


def test_identical_rgb_and_rgba_images_pass():
    result = compare_images(image("red"), image("red").convert("RGB"))
    assert result.passed
    assert result.compared == 16
    assert result.changed == 0
    assert result.mean_delta_e == 0


def test_lab_known_black_white_and_red_values():
    assert _lab((0, 0, 0, 255)) == pytest.approx((0, 0, 0))
    assert _lab((255, 255, 255, 255)) == pytest.approx((100, 0, 0), abs=0.0001)
    assert _lab((255, 0, 0, 255)) == pytest.approx((53.2408, 80.0925, 67.2032), abs=0.001)


def test_perceptual_tolerance_accepts_small_shading_but_rejects_large_change():
    a = image((100, 100, 100, 255))
    assert compare_images(a, image((101, 101, 101, 255)), tolerance=2).passed
    assert not compare_images(a, image((150, 150, 150, 255)), tolerance=2).passed


def test_fraction_threshold_and_diff_pixels():
    a, g = image(), image()
    a.putpixel((1, 2), (255, 255, 255, 255))
    strict = compare_images(a, g, tolerance=2)
    assert not strict.passed
    assert strict.changed == 1
    assert strict.fraction == 1 / 16
    assert strict.max_delta_e == pytest.approx(100, abs=0.001)
    assert strict.diff.getpixel((1, 2)) == (255, 32, 64, 255)
    assert compare_images(a, g, max_changed=1 / 16).passed
    assert not compare_images(a, g, max_changed=1 / 17).passed


def test_masks_are_exclusive_and_excluded_from_denominator():
    a, g = image(), image()
    a.putpixel((1, 1), (255, 255, 255, 255))
    a.putpixel((2, 1), (255, 255, 255, 255))
    result = compare_images(a, g, masks=[[1, 1, 1, 1]])
    assert not result.passed
    assert result.changed == 1
    assert result.compared == 15
    assert result.diff.getpixel((1, 1))[3] == 0
    assert compare_images(a, g, masks=[[1, 1, 2, 1]]).passed


def test_transparent_round_corners_ignored_but_alpha_loss_is_detected():
    a, g = image(), image()
    a.putpixel((0, 0), (255, 100, 23, 0))
    g.putpixel((0, 0), (0, 0, 0, 0))
    result = compare_images(a, g)
    assert result.passed
    assert result.compared == 15
    a.putpixel((0, 1), (0, 0, 0, 0))
    assert compare_images(a, g).changed == 1


def test_dimension_change_fails_instead_of_resizing():
    result = compare_images(image(size=(4, 4)), image(size=(5, 4)))
    assert not result.passed
    assert "size mismatch" in result.error
    assert result.diff.size == (5, 4)


@pytest.mark.parametrize(
    "masks", [[[0, 0, 4, 4]], [[-1, 0, 1, 1]], [[3, 3, 2, 2]], [[0, 0, 0, 1]], [[0, 1, 2]], "all"]
)
def test_invalid_or_fully_covering_masks_fail(masks):
    with pytest.raises(ScenarioError):
        compare_images(image(), image(), masks=masks)


def test_fully_transparent_images_cannot_pass_vacuously():
    with pytest.raises(ScenarioError, match="no visible"):
        compare_images(image((0, 0, 0, 0)), image((0, 0, 0, 0)))


@pytest.mark.parametrize(
    "options",
    [{"tolerance": -1}, {"tolerance": float("nan")}, {"max_changed": 1.1}, {"tolerance": True}],
)
def test_invalid_comparison_thresholds(options):
    with pytest.raises(ScenarioError):
        compare_images(image(), image(), **options)


@pytest.mark.parametrize(
    "steps",
    [
        [{"tap": [4, 1]}, {"shot": "one"}, {"expect": "one.png"}],
        [{"hold": [1, 1, -5]}],
        [{"swipe": [1, 1, 2, 2, 60001]}],
        [{"wait": float("inf")}],
        [{"tap": [True, 2]}],
        [{"shot": "../escape"}],
        [{"shot": "one"}, {"shot": "one"}],
        [{"shot": "one"}],
        [{"expect": "one.png"}],
        [{"shot": "one"}, {"expect": "../escape.png"}],
        [{"shot": "one"}, {"expect": "/tmp/escape.png"}],
        [{"shot": "one"}, {"expect": {"golden": "one.png", "typo": 1}}],
        [{"shell": "echo unsafe"}],
        [{"shot": "one", "wait": 1}],
    ],
)
def test_invalid_scenarios_rejected(steps):
    with pytest.raises(ScenarioError):
        validate_scenario(scenario(steps))


def test_example_scenarios_validate():
    examples = sorted((ROOT / "tests/ui/scenarios").glob("*.yaml"))
    assert len(examples) == 2
    for path in examples:
        assert load_scenario(path)["example"] is True


def test_unsafe_yaml_loader_disabled(tmp_path):
    file = tmp_path / "bad.yaml"
    file.write_text("!!python/object/apply:os.system ['touch should-not-exist']")
    with pytest.raises(ScenarioError, match="invalid YAML"):
        load_scenario(file)


def test_atomic_update_preserves_old_file_when_replace_fails(tmp_path, monkeypatch):
    destination = tmp_path / "golden.png"
    _atomic_png(image("red"), destination)
    before = destination.read_bytes()

    def fail(*args):
        raise OSError("simulated disk failure")

    monkeypatch.setattr(os, "replace", fail)
    with pytest.raises(OSError):
        _atomic_png(image("blue"), destination)
    assert destination.read_bytes() == before
    assert list(tmp_path.glob(".orb-ui-*")) == []


def test_fixture_update_compare_failure_and_report(tmp_path):
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    image("red").save(fixtures / "01.png")
    goldens, output = tmp_path / "goldens", tmp_path / "report"
    data = validate_scenario(scenario())
    updated = run_scenario(data, FixtureBackend(fixtures), goldens, output / "updated", update=True)
    assert updated["passed"]
    assert updated["expectations"][0]["status"] == "updated"
    compared = run_scenario(data, FixtureBackend(fixtures), goldens, output / "same")
    assert compared["passed"]
    image("blue").save(fixtures / "01.png")
    different = run_scenario(data, FixtureBackend(fixtures), goldens, output / "different")
    assert not different["passed"]
    different["directory"] = "different"
    different["description"] = "<script>alert(1)</script>"
    write_report([different], output, update=False)
    report = (output / "index.html").read_text()
    assert "&lt;script&gt;" in report and "<script>" not in report
    assert "different/expect-2-diff.png" in report
    assert json.loads((output / "results.json").read_text())["scenarios"][0]["passed"] is False


def test_missing_golden_failure_and_capture_error_are_reported(tmp_path):
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    image().save(fixtures / "1.png")
    result = run_scenario(
        validate_scenario(scenario()),
        FixtureBackend(fixtures),
        tmp_path / "missing",
        tmp_path / "out",
    )
    assert not result["passed"]
    assert "missing golden" in result["expectations"][0]["status"]
    fixtures.joinpath("1.png").unlink()
    failed = run_scenario(
        validate_scenario(scenario()),
        FixtureBackend(fixtures),
        tmp_path / "missing",
        tmp_path / "out2",
    )
    assert not failed["passed"]
    assert "step 1 (shot)" in failed["error"]


def test_golden_symlink_cannot_escape_root(tmp_path):
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    image().save(fixtures / "1.png")
    goldens = tmp_path / "goldens"
    goldens.mkdir()
    outside = tmp_path / "outside.png"
    goldens.joinpath("one.png").symlink_to(outside)
    result = run_scenario(
        validate_scenario(scenario()),
        FixtureBackend(fixtures),
        goldens,
        tmp_path / "out",
        update=True,
    )
    assert not result["passed"]
    assert "escapes" in result["error"]
    assert not outside.exists()


def test_command_backend_runs_touch_shot_contract_without_shell(tmp_path):
    calls = tmp_path / "calls.jsonl"
    source = tmp_path / "source.png"
    image("red").save(source)
    fake = tmp_path / "fake device; not a shell.py"
    fake.write_text(
        "import json, pathlib, shutil, sys\n"
        f"with pathlib.Path({str(calls)!r}).open('a') as f: f.write(json.dumps(sys.argv[1:])+'\\n')\n"
        f"if sys.argv[1] == 'shot': shutil.copyfile({str(source)!r}, sys.argv[2])\n"
    )
    data = validate_scenario(
        scenario(
            [
                {"tap": [1, 2]},
                {"hold": [2, 1, 100]},
                {"swipe": [1, 1, 2, 2, 150]},
                {"wait": 0},
                {"shot": "one"},
                {"expect": "one.png"},
            ]
        )
    )
    result = run_scenario(
        data,
        CommandBackend([sys.executable, str(fake)]),
        tmp_path / "goldens",
        tmp_path / "out",
        update=True,
    )
    assert result["passed"]
    actions = [json.loads(line) for line in calls.read_text().splitlines()]
    assert actions[:3] == [
        ["tap", "1", "2"],
        ["hold", "2", "1", "100"],
        ["swipe", "1", "1", "2", "2", "150"],
    ]
    assert actions[3][0] == "shot"
    assert len(actions) == 4


def test_cli_end_to_end_fixture_update_pass_fail(tmp_path):
    fixtures = tmp_path / "fixtures"
    fixtures.mkdir()
    image("red").save(fixtures / "1.png")
    path = tmp_path / "scenario.yaml"
    path.write_text(yaml.safe_dump(scenario()))
    args = [
        sys.executable,
        str(ROOT / "scripts/orb-ui-test.py"),
        str(path),
        "--backend",
        "fixture",
        "--fixtures",
        str(fixtures),
        "--goldens",
        str(tmp_path / "goldens"),
        "--output",
        str(tmp_path / "report"),
    ]
    assert subprocess.run([*args, "--update"], capture_output=True).returncode == 0
    assert subprocess.run(args, capture_output=True).returncode == 0
    image("blue").save(fixtures / "1.png")
    assert subprocess.run(args, capture_output=True).returncode == 1
    assert (tmp_path / "report/index.html").is_file()


def test_cli_validates_all_inputs_before_invoking_backend(tmp_path, monkeypatch):
    good = tmp_path / "good.yaml"
    good.write_text(yaml.safe_dump(scenario()))
    bad = tmp_path / "bad.yaml"
    bad.write_text("version: 7")
    calls = []
    monkeypatch.setattr(CommandBackend, "shot", lambda *args: calls.append(args))
    assert main([str(good), str(bad), "--output", str(tmp_path / "report")]) == 2
    assert calls == []


def test_expect_shot_type_and_version_type_rejected():
    data = scenario([{"shot": "one"}, {"expect": {"shot": [], "golden": "one.png"}}])
    with pytest.raises(ScenarioError):
        validate_scenario(data)
    data = scenario()
    data["version"] = True
    with pytest.raises(ScenarioError):
        validate_scenario(data)


def test_update_report_preserves_previous_baseline_and_diff(tmp_path):
    fixtures = tmp_path / "fixtures"
    goldens = tmp_path / "goldens"
    fixtures.mkdir()
    goldens.mkdir()
    image("red").save(goldens / "one.png")
    image("blue").save(fixtures / "1.png")
    output = tmp_path / "out"
    result = run_scenario(
        validate_scenario(scenario()), FixtureBackend(fixtures), goldens, output, update=True
    )
    assert result["passed"]
    expectation = result["expectations"][0]
    with Image.open(output / expectation["previous"]) as previous:
        assert previous.getpixel((0, 0)) == (255, 0, 0, 255)
    with Image.open(goldens / "one.png") as golden:
        assert golden.getpixel((0, 0)) == (0, 0, 255, 255)
    assert (output / expectation["diff"]).is_file()


def test_preflight_rejects_custom_golden_symlink_before_capture(tmp_path, monkeypatch):
    goldens = tmp_path / "goldens"
    goldens.mkdir()
    (goldens / "one.png").symlink_to(tmp_path / "outside.png")
    path = tmp_path / "scenario.yaml"
    path.write_text(yaml.safe_dump(scenario()))
    calls = []
    monkeypatch.setattr(CommandBackend, "shot", lambda *args: calls.append(args))
    assert (
        main(
            [str(path), "--goldens", str(goldens), "--output", str(tmp_path / "report"), "--update"]
        )
        == 2
    )
    assert calls == []
