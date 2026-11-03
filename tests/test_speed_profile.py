"""Round-3 lead workflow, with synthetic profiles and fake external actions only."""

import io
import json
import os
import shutil
import signal
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

import orb_dev_remote as transport
import pytest
import speed_profile as speed

ROOT = Path(__file__).resolve().parents[1]


def profile_text(*, prefix="@@", overflow=0):
    lines = [
        f"ORBDEV DRAWPROFILE_META clock=wall_us slots=160 bytes=20000 overflow={overflow}",
        "ORBDEV BENCH scroll seconds=8.00 frames=100 fps=12.50 draw_ms=40.00",
        "ORBDEV BENCH glass seconds=8.00 frames=200 fps=25.00 draw_ms=15.00",
        "ORBDEV DRAWPROFILE phase=scroll draw_us=4000000 frames=100",
        "ORBDEV DRAWTASK phase=scroll type=IMAGE_TRANSFORM class=obj calls=400 elapsed_us=3000000",
        "ORBDEV DRAWTASK phase=scroll type=BLUR class=obj calls=300 elapsed_us=2000000",
        "ORBDEV RASTER phase=scroll builds=0 count=8 bytes=230000",
        "ORBDEV DRAWPROFILE phase=glass draw_us=3000000 frames=200",
        "ORBDEV DRAWTASK phase=glass type=BLUR class=obj calls=600 elapsed_us=1500000",
        "ORBDEV DRAWTASK phase=glass type=LABEL class=label calls=100 elapsed_us=200000",
        "ORBDEV RASTER phase=glass builds=2 count=8 bytes=230000",
        "ORBDEV OK bench 16000",
    ]
    return "\n".join(prefix + line for line in lines) + "\n"


@pytest.mark.parametrize("prefix", ["@@", ""])
def test_ranked_totals_keep_draw_denominator_and_real_transport_prefix(tmp_path, prefix):
    for index in range(1, 4):
        (tmp_path / f"bench-{index}.txt").write_text(profile_text(prefix=prefix))
    speed.ranked_report(tmp_path)
    report = json.loads((tmp_path / "ranked.json").read_text())
    scroll = report["scroll"]
    assert scroll["frames"] == 300
    assert scroll["draw_us"] == 12_000_000
    assert scroll["raster_builds"] == 0
    assert scroll["rows"][0]["type"] == "IMAGE_TRANSFORM"
    assert scroll["rows"][0]["calls"] == 1200
    assert scroll["rows"][0]["percent_draw"] == 75
    assert scroll["rows"][0]["us_per_call"] == 7500
    assert scroll["rows"][0]["ms_per_frame"] == 30
    assert sum(row["percent_draw"] for row in scroll["rows"]) == 125
    assert report["glass"]["raster_builds"] == 6
    text = (tmp_path / "ranked.txt").read_text()
    assert "exceed 100%" in text
    assert "NOT scheduler CPU time" in text


@pytest.mark.parametrize(
    "change",
    [
        lambda text: text.replace("overflow=0", "overflow=1"),
        lambda text: "\n".join(
            line for line in text.splitlines() if "DRAWPROFILE_META" not in line
        ),
        lambda text: text.replace("@@ORBDEV OK bench 16000", ""),
        lambda text: text.replace("phase=glass", "phase=unknown"),
        lambda text: text.replace("draw_us=4000000", "draw_us=0"),
        lambda text: text.replace("frames=100", "frames=0"),
        lambda text: text.replace("calls=400", "calls=0"),
        lambda text: text.replace("elapsed_us=3000000", "elapsed_us=-1"),
        lambda text: "\n".join(
            line for line in text.splitlines() if "RASTER phase=glass" not in line
        ),
        lambda text: text.replace("@@ORBDEV DRAWTASK phase=glass", "ignored phase=glass"),
        lambda text: text + "Guru Meditation Error: test\n",
    ],
)
def test_incomplete_or_invalid_profile_never_gets_success_table(tmp_path, change):
    (tmp_path / "bench-1.txt").write_text(change(profile_text()))
    with pytest.raises((RuntimeError, ValueError)):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


def test_scaled_variant_build_cost_remains_separate_from_draw(tmp_path):
    text = profile_text()
    text += (
        "@@ORBDEV SCALECACHE phase=scroll hits=5 misses=3 builds=3 evictions=1 "
        "alloc_fail=0 blits=15 build_us=25000 bytes=500 peak=600 budget=1024\n"
        "@@ORBDEV SCALECACHE phase=glass hits=5 misses=0 builds=0 evictions=0 "
        "alloc_fail=0 blits=15 build_us=0 bytes=500 peak=600 budget=1024\n"
    )
    (tmp_path / "bench-1.txt").write_text(text)
    speed.ranked_report(tmp_path)
    result = json.loads((tmp_path / "ranked.json").read_text())["scroll"]
    assert result["draw_us"] == 4_000_000
    assert result["scale_cache"]["build_us"] == 25000
    assert result["scale_cache"]["blits"] == 15
    assert "outside DRAW" in (tmp_path / "ranked.txt").read_text()


def hotspot_text():
    text = profile_text().replace("bytes=20000", "bytes=21296 stages=7")
    # Insert all records before each next phase: headers establish the phase
    # independently of task/stage activity, including valid zero-work stages.
    for phase in ("scroll", "glass"):
        records = []
        for stage in speed.HOTSPOT_STAGES:
            active = phase == "scroll" or stage == "GLASS_COMPOSE"
            records.append(
                f"@@ORBDEV HOTSPOT phase={phase} stage={stage} "
                f"calls={3 if active else 0} elapsed_us={900 if active else 0} "
                f"pixels={120 if active else 0}\n"
            )
        text = text.replace(
            f"@@ORBDEV RASTER phase={phase}", "".join(records) + f"@@ORBDEV RASTER phase={phase}"
        )
    return text


def test_hotspot_stages_keep_inclusive_prepare_outside_task_totals(tmp_path):
    for index in range(1, 4):
        (tmp_path / f"bench-{index}.txt").write_text(hotspot_text())
    speed.ranked_report(tmp_path)
    result = json.loads((tmp_path / "ranked.json").read_text())
    stages = {row["stage"]: row for row in result["scroll"]["hotspots"]}
    assert stages["ROWS_DRAW"]["calls"] == 9
    assert stages["ROWS_DRAW"]["elapsed_us"] == 2700
    assert stages["ROWS_DRAW"]["pixels"] == 360
    assert stages["ROWS_DRAW"]["ms_per_frame"] == 0.009
    assert stages["ROWS_DRAW"]["pixels_per_frame"] == 1.2
    assert result["scroll"]["draw_us"] == 12_000_000
    assert sum(row["percent_draw"] for row in result["scroll"]["rows"]) == 125
    glass = {row["stage"]: row for row in result["glass"]["hotspots"]}
    assert glass["GLASS_PREPARE"]["calls"] == 0
    assert glass["GLASS_COMPOSE"]["calls"] == 9
    assert stages["DRAW_EVENTS"]["calls"] == 9
    assert "never sum hotspot stages" in (tmp_path / "ranked.txt").read_text()
    assert "DRAW_EVENTS includes synchronous painting" in (tmp_path / "ranked.txt").read_text()


def test_archived_six_stage_profile_remains_supported(tmp_path):
    text = hotspot_text().replace("stages=7", "stages=6")
    text = "\n".join(line for line in text.splitlines() if "stage=DRAW_EVENTS" not in line)
    (tmp_path / "bench-1.txt").write_text(text)
    speed.ranked_report(tmp_path)
    result = json.loads((tmp_path / "ranked.json").read_text())
    assert len(result["scroll"]["hotspots"]) == 6
    assert all(row["stage"] != "DRAW_EVENTS" for row in result["scroll"]["hotspots"])


def test_mixed_stage_shapes_cannot_invent_missing_measurements(tmp_path):
    (tmp_path / "bench-1.txt").write_text(hotspot_text())
    legacy = hotspot_text().replace("stages=7", "stages=6")
    legacy = "\n".join(line for line in legacy.splitlines() if "stage=DRAW_EVENTS" not in line)
    (tmp_path / "bench-2.txt").write_text(legacy)
    with pytest.raises(RuntimeError, match="inconsistent.*hotspot"):
        speed.ranked_report(tmp_path)


@pytest.mark.parametrize(
    "change",
    [
        lambda text: text.replace("stages=7", "stages=5"),
        lambda text: text.replace("stages=7", ""),
        lambda text: text.replace("stage=ROWS_DRAW", "stage=UNKNOWN"),
        lambda text: text.replace("stage=ROWS_DRAW", "stage=ROWS_PREPARE"),
        lambda text: "\n".join(
            line for line in text.splitlines() if "HOTSPOT phase=glass stage=ROWS_DRAW" not in line
        ),
        lambda text: text.replace("elapsed_us=900", "elapsed_us=-1"),
        lambda text: text.replace("pixels=120", "pixels=4294967296"),
        lambda text: text.replace("calls=0 elapsed_us=0 pixels=0", "calls=0 elapsed_us=1 pixels=0"),
        lambda text: (
            "@@ORBDEV HOTSPOT phase=scroll stage=ROWS_DRAW calls=0 elapsed_us=0 pixels=0\n" + text
        ),
    ],
)
def test_hotspot_profile_rejects_missing_duplicate_or_invalid_stages(tmp_path, change):
    (tmp_path / "bench-1.txt").write_text(change(hotspot_text()))
    with pytest.raises(RuntimeError, match="hotspot"):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


def row_atlas_record(phase, *, draws=15, size=500):
    return (
        f"@@ORBDEV ROWATLAS phase={phase} builds=2 fallbacks=1 draws={draws} pixels=240 "
        f"rows=3 glyphs=30 bytes={size} peak=600 budget=1024\n"
    )


def test_row_atlas_reports_cumulative_work_and_peak_residency(tmp_path):
    for index in range(1, 4):
        (tmp_path / f"bench-{index}.txt").write_text(
            hotspot_text() + row_atlas_record("scroll") + row_atlas_record("glass", draws=3)
        )
    speed.ranked_report(tmp_path)
    result = json.loads((tmp_path / "ranked.json").read_text())
    assert result["scroll"]["row_atlas"] == {
        "builds": 6,
        "fallbacks": 3,
        "draws": 45,
        "pixels": 720,
        "rows": 3,
        "glyphs": 30,
        "bytes": 500,
        "peak": 600,
        "budget": 1024,
    }
    assert result["glass"]["row_atlas"]["draws"] == 9
    assert "resident 3 rows/30 glyphs" in (tmp_path / "ranked.txt").read_text()


@pytest.mark.parametrize("fault", ["missing", "duplicate", "negative", "oversize", "unknown"])
def test_row_atlas_rejects_incomplete_or_invalid_memory_records(tmp_path, fault):
    text = hotspot_text() + row_atlas_record("scroll")
    if fault != "missing":
        text += row_atlas_record(
            "unknown" if fault == "unknown" else "glass",
            size=-1 if fault == "negative" else 700 if fault == "oversize" else 500,
        )
    if fault == "duplicate":
        text += row_atlas_record("scroll")
    (tmp_path / "bench-1.txt").write_text(text)
    with pytest.raises(RuntimeError, match="row atlas"):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


def fused_glass_record(phase, *, draws=15, size=500):
    return (
        f"@@ORBDEV FUSEDGLASS phase={phase} prepares=3 draws={draws} fallbacks=2 "
        f"maps=3 bytes={size} peak=600 budget=12288\n"
    )


def test_fused_glass_reports_cumulative_work_and_peak_internal_payload(tmp_path):
    for index in range(1, 4):
        (tmp_path / f"bench-{index}.txt").write_text(
            hotspot_text() + fused_glass_record("scroll") + fused_glass_record("glass", draws=3)
        )
    speed.ranked_report(tmp_path)
    result = json.loads((tmp_path / "ranked.json").read_text())
    assert result["scroll"]["fused_glass"] == {
        "prepares": 9,
        "draws": 45,
        "fallbacks": 6,
        "maps": 3,
        "bytes": 500,
        "peak": 600,
        "budget": 12288,
    }
    assert result["glass"]["fused_glass"]["draws"] == 9
    assert "resident 3 maps" in (tmp_path / "ranked.txt").read_text()
    assert "exclude temporary" in (tmp_path / "ranked.txt").read_text()


@pytest.mark.parametrize("fault", ["missing", "duplicate", "negative", "oversize", "unknown"])
def test_fused_glass_rejects_incomplete_or_invalid_memory_records(tmp_path, fault):
    text = hotspot_text() + fused_glass_record("scroll")
    if fault != "missing":
        text += fused_glass_record(
            "unknown" if fault == "unknown" else "glass",
            size=-1 if fault == "negative" else 700 if fault == "oversize" else 500,
        )
    if fault == "duplicate":
        text += fused_glass_record("scroll")
    (tmp_path / "bench-1.txt").write_text(text)
    with pytest.raises(RuntimeError, match="fused glass"):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


@pytest.mark.parametrize("record", [row_atlas_record, fused_glass_record])
def test_memory_receipts_cannot_silently_omit_a_capture_from_work_totals(tmp_path, record):
    (tmp_path / "bench-1.txt").write_text(hotspot_text() + record("scroll") + record("glass"))
    (tmp_path / "bench-2.txt").write_text(hotspot_text())
    with pytest.raises(RuntimeError, match="across captures"):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


@pytest.mark.parametrize("enabled", [False, True])
def test_raster_mips_report_actual_work_and_budget_without_double_counting(tmp_path, enabled):
    fields = (
        f"mip_builds={2 if enabled else 0} mip_bytes={800 if enabled else 0} "
        f"mip_budget={196608 if enabled else 0}"
    )
    for index in range(1, 4):
        (tmp_path / f"bench-{index}.txt").write_text(
            profile_text().replace("bytes=230000", f"bytes=230000 {fields}")
        )
    speed.ranked_report(tmp_path)
    result = json.loads((tmp_path / "ranked.json").read_text())
    assert result["scroll"]["raster_mips"] == {
        "builds": 6 if enabled else 0,
        "bytes": 800 if enabled else 0,
        "budget": 196608 if enabled else 0,
    }
    assert result["scroll"]["raster_builds"] == 0
    assert "included in raster bytes" in (tmp_path / "ranked.txt").read_text()


@pytest.mark.parametrize(
    "fields",
    [
        "mip_builds=2",
        "mip_builds=-1 mip_bytes=0 mip_budget=196608",
        "mip_builds=2 mip_bytes=196609 mip_budget=196608",
    ],
)
def test_raster_mip_diagnostics_reject_invalid_shapes_or_memory(tmp_path, fields):
    (tmp_path / "bench-1.txt").write_text(
        profile_text().replace("bytes=230000", f"bytes=230000 {fields}")
    )
    with pytest.raises(RuntimeError, match="raster mip"):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


@pytest.mark.parametrize("fault", ["missing", "duplicate", "negative", "oversize"])
def test_variant_diagnostics_reject_incomplete_or_invalid_accounting(tmp_path, fault):
    def record(phase, *, size=0):
        return (
            f"@@ORBDEV SCALECACHE phase={phase} hits=0 misses=0 builds=0 evictions=0 "
            f"alloc_fail=0 blits=0 build_us=0 bytes={size} peak=0 budget=0\n"
        )

    text = profile_text() + record("scroll")
    if fault != "missing":
        text += record("glass", size=-1 if fault == "negative" else 1 if fault == "oversize" else 0)
    if fault == "duplicate":
        text += record("scroll")
    (tmp_path / "bench-1.txt").write_text(text)
    with pytest.raises(RuntimeError, match="scale cache"):
        speed.ranked_report(tmp_path)
    assert not (tmp_path / "ranked.txt").exists()


def native_text():
    # First and every transition sample deliberately have absurd FPS. These
    # should remain available as raw evidence, but never enter phase means.
    phases = ["scroll"] * 4 + ["glass"] * 4 + ["scroll"] * 4 + ["glass"] * 3
    previous = None
    lines = []
    for index, phase in enumerate(phases):
        fps = 999.0 if previous != phase else (50.0 if phase == "scroll" else 58.0)
        lines.append(
            f"2026-10-27T00:00:{index:02d}Z I ({30000 + index * 2000}) main: "
            f"{phase}: {fps:.1f} fps, frame 20.0 ms (draw 13.0, wait 5.0; "
            "hold 1.0, wire 10.0), 6.0 bands, 300 KB, torn 0"
        )
        previous = phase
    return "\n".join(lines) + "\n"


def test_native_renderer_uses_fresh_windows_excluding_phase_transitions():
    data, table = speed.renderer_report(native_text())
    assert len(data["all_samples"]) == 15
    assert len(data["stable_samples"]) == 11
    assert all(row["fps"] != 999 for row in data["stable_samples"])
    assert "50.00" in table and "58.00" in table
    assert "Historical DRAW/frame accounting differs" in table


@pytest.mark.parametrize(
    "text",
    [
        native_text() + "ESP-ROM:esp32s3\n",
        native_text() + "Guru Meditation Error: fake\n",
        native_text() + native_text(),
        "\n".join(native_text().splitlines()[:5]),
        native_text().replace("glass:", "scroll:"),
    ],
)
def test_native_renderer_rejects_reset_duplicate_or_short_capture(text):
    with pytest.raises(RuntimeError):
        speed.renderer_report(text)


def test_artifact_tampering_stops_resume(tmp_path):
    artifact = tmp_path / "orb.bin"
    artifact.write_bytes(b"archived build")
    receipt = {"artifacts": {"orb.bin": speed.sha(artifact)}}
    speed.save(tmp_path / "built.json", receipt)
    assert speed.verify_artifacts(tmp_path) == receipt
    artifact.write_bytes(b"unexpected later build")
    with pytest.raises(RuntimeError, match="archived build changed"):
        speed.verify_artifacts(tmp_path)


def test_build_resume_rejects_changed_sources_without_running_anything(tmp_path, monkeypatch):
    speed.save(tmp_path / "built.json", {"source": {"commit": "old"}, "artifacts": {}})
    monkeypatch.setattr(speed, "source_identity", lambda _: {"commit": "new"})

    def forbidden(*args, **kwargs):
        pytest.fail("source mismatch reached build command")

    monkeypatch.setattr(speed, "run", forbidden)
    with pytest.raises(RuntimeError, match="source changed"):
        speed.build(tmp_path, tmp_path, "0.3.0-control", profile=True)


@pytest.mark.parametrize("profile", [True, False])
@pytest.mark.parametrize("enabled", [(), ("ORB_DISPLAY_RASTER_SCALE_CACHE",)])
def test_build_uses_fresh_case_config_and_renderer_own_defaults(
    tmp_path, monkeypatch, profile, enabled
):
    if enabled and not profile:
        return
    tree, case = tmp_path / "tree with spaces", tmp_path / "case with spaces"
    tree.mkdir()
    case.mkdir()
    (case / "sdkconfig").write_text("stale config must be removed")
    source = {"commit": "fake", "files": {}}
    monkeypatch.setattr(speed, "source_identity", lambda _tree: source)
    monkeypatch.setattr(speed, "git", lambda *_args: "")
    commands = []

    def fake_build(command, log, *, cwd):
        commands.append(command)
        assert cwd == tree
        assert not (case / "sdkconfig").exists()
        (case / "build/config").mkdir(parents=True)
        header = "#define CONFIG_ORB_DISPLAY_DRAW_PROFILE 1\n" if profile else ""
        header += "".join(f"#define CONFIG_{name} 1\n" for name in enabled)
        (case / "build/config/sdkconfig.h").write_text(header)
        for name in (
            "sdkconfig",
            "build/orb.bin",
            "build/orb.elf",
            "build/orb.map",
            "build/ota_data_initial.bin",
            "build/CMakeCache.txt",
            "build/compile_commands.json",
            "build/project_description.json",
        ):
            (case / name).write_text("synthetic host fixture")
        log.write_text("synthetic success\n")

    monkeypatch.setattr(speed, "run", fake_build)
    monkeypatch.setattr(
        speed.ab,
        "image_identity",
        lambda _path: {"fw": "0.3.0-control", "elf_sha256": speed.sha(case / "build/orb.elf")},
    )
    receipt = speed.build(case, tree, "0.3.0-control", profile=profile, enabled=enabled)
    command = [str(arg) for arg in commands[0]]
    assert command[:3] == [str(tree / "scripts/idf.sh"), "-B", str(case / "build")]
    assert f"SDKCONFIG={case / 'sdkconfig'}" in command
    assert command[-2:] == ["reconfigure", "build"]
    defaults = str(tree / "firmware/sdkconfig.defaults")
    if profile:
        defaults += f";{case / 'profile.defaults'}"
        overlay = (case / "profile.defaults").read_text()
        assert 'CONFIG_APP_PROJECT_VER="0.3.0-control"' in overlay
        assert "CONFIG_ORB_DISPLAY_DRAW_PROFILE=y" in overlay
        assert overlay.count("CONFIG_ORB_DISPLAY_DRAW_PROFILE=") == 1
        assert "CONFIG_LV_USE_PROFILER_BUILTIN=n" in overlay
        assert all(
            f"CONFIG_{name}={'y' if name in enabled or name == 'ORB_DISPLAY_DRAW_PROFILE' else 'n'}"
            in overlay
            for name in speed.ab.BASE_OPTIONS
        )
    else:
        assert not (case / "profile.defaults").exists()
    assert f"SDKCONFIG_DEFAULTS={defaults}" in command
    assert receipt["source"] == source
    assert speed.verify_artifacts(case) == receipt


@pytest.mark.parametrize("stubborn", [False, True])
def test_offline_capture_terminates_entire_local_process_group(tmp_path, monkeypatch, stubborn):
    class Process:
        pid = 123456789
        returncode = None

        def poll(self):
            return self.returncode

        def wait(self, timeout=None):
            if stubborn and timeout is not None:
                raise subprocess.TimeoutExpired("fake", timeout)
            return self.returncode

    process = Process()
    signals = []

    def spawn(_command, **kwargs):
        assert kwargs["start_new_session"] is True
        return process

    def kill_group(pid, sig):
        assert pid == process.pid
        signals.append(sig)
        if not stubborn or sig == signal.SIGKILL:
            process.returncode = -sig

    probe_results = iter([True, False])
    monkeypatch.setattr(speed.subprocess, "Popen", spawn)
    monkeypatch.setattr(speed.os, "killpg", kill_group)
    monkeypatch.setattr(speed.time, "sleep", lambda _delay: None)
    with pytest.raises(RuntimeError, match="offline"):
        speed.run(["never executed"], tmp_path / "command.log", probe=lambda: next(probe_results))
    assert signals == [signal.SIGTERM] + ([signal.SIGKILL] if stubborn else [])


def test_offline_before_capture_does_not_launch_child(tmp_path, monkeypatch):
    def forbidden(*_args, **_kwargs):
        pytest.fail("offline preflight must not launch a command")

    monkeypatch.setattr(speed.subprocess, "Popen", forbidden)
    with pytest.raises(RuntimeError, match="offline before"):
        speed.run(["never executed"], tmp_path / "command.log", probe=lambda: False)
    assert not (tmp_path / "command.log").exists()


@pytest.mark.parametrize(
    "name,value",
    [
        ("ESP_REMOTE_HOST", "another-host"),
        ("ORB_LOGD_SOCKET", "~/.local/share/second-board/orb.sock"),
        ("ESP_REMOTE_PORT", "/dev/cu.second-board"),
        ("ORB_DEV_LOCAL", "1"),
    ],
)
def test_other_board_selection_stops_before_workflow(tmp_path, monkeypatch, name, value):
    for variable in ("ESP_REMOTE_HOST", "ORB_LOGD_SOCKET", "ESP_REMOTE_PORT", "ORB_DEV_LOCAL"):
        monkeypatch.delenv(variable, raising=False)
    key = tmp_path / "throwaway-key"
    key.write_text("not a real signing key")
    monkeypatch.setenv("ORB_SIGNING_KEY", str(key))
    monkeypatch.setenv(name, value)

    def forbidden(*_args):
        pytest.fail("unexpected board selector reached workflow")

    monkeypatch.setattr(speed, "orchestrate", forbidden)
    output = tmp_path / "must-not-be-created"
    with pytest.raises(SystemExit, match="2"):
        speed.main(["--bench-version", "0.3.0-control", "--output", str(output)])
    assert not output.exists()


def test_main_records_failure_and_does_not_claim_success(tmp_path, monkeypatch):
    for variable in ("ESP_REMOTE_HOST", "ORB_LOGD_SOCKET", "ESP_REMOTE_PORT", "ORB_DEV_LOCAL"):
        monkeypatch.delenv(variable, raising=False)
    key = tmp_path / "throwaway-key"
    key.write_text("not a real signing key")
    monkeypatch.setenv("ORB_SIGNING_KEY", str(key))
    monkeypatch.setattr(speed, "ROOT", tmp_path)
    monkeypatch.setattr(speed.os, "umask", lambda _value: None)

    def fail(*_args):
        raise RuntimeError("synthetic recovery failure")

    monkeypatch.setattr(speed, "orchestrate", fail)
    output = tmp_path / "output"
    assert speed.main(["--bench-version", "0.3.0-control", "--output", str(output)]) == 1
    assert (output / "LAST_ERROR.txt").read_text() == "synthetic recovery failure\n"
    assert not (output / "RESULTS.txt").exists()


def workflow_mocks(tmp_path, monkeypatch):
    output, renderer = tmp_path / "output", tmp_path / "renderer"
    output.mkdir()
    renderer.mkdir()
    events = []

    def fake_git(tree, *args):
        if args == ("branch", "--show-current"):
            return "orb/render" if tree == renderer else "orb/w2-speed"
        if args == ("status", "--porcelain"):
            return ""
        pytest.fail(f"unexpected git command: {args}")

    def build(case, *_args, **_kwargs):
        events.append(f"build {case.name}")
        case.mkdir(exist_ok=True)

    def capture(case, *_args):
        events.append(f"capture {case.name}")
        attempt = case / "capture-001"
        attempt.mkdir()
        name = "ranked.txt" if case.name == "profile" else "native-stats.txt"
        (attempt / name).write_text(f"{case.name} report\n")
        speed.save(case / "captured.json", {"attempt": attempt.name})

    monkeypatch.setattr(speed, "git", fake_git)
    monkeypatch.setattr(speed, "build", build)
    monkeypatch.setattr(speed, "profile_capture", capture)
    monkeypatch.setattr(speed, "renderer_capture", capture)
    monkeypatch.setattr(speed, "restore", lambda *_args: events.append("restore"))
    return output, renderer, events


@pytest.mark.parametrize("error", [RuntimeError("capture failed"), KeyboardInterrupt()])
def test_renderer_failure_always_restores_profiling_build(tmp_path, monkeypatch, error):
    output, renderer, events = workflow_mocks(tmp_path, monkeypatch)

    def fail(_case):
        events.append("renderer interrupted")
        assert json.loads((output / "state.json").read_text())["restore_pending"]
        raise error

    monkeypatch.setattr(speed, "renderer_capture", fail)
    with pytest.raises(type(error)):
        speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))
    assert events[-2:] == ["renderer interrupted", "restore"]
    state = json.loads((output / "state.json").read_text())
    assert not state["restore_pending"]
    assert not state.get("complete")
    assert not (output / "RESULTS.txt").exists()


def test_failed_restore_remains_pending_and_resume_restores_first(tmp_path, monkeypatch):
    output, renderer, events = workflow_mocks(tmp_path, monkeypatch)

    def fail_restore(*_args):
        events.append("failed restore")
        raise RuntimeError("USB disconnected")

    monkeypatch.setattr(speed, "restore", fail_restore)
    with pytest.raises(RuntimeError, match="USB disconnected"):
        speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))
    assert json.loads((output / "state.json").read_text())["restore_pending"]
    events.clear()
    monkeypatch.setattr(speed, "restore", lambda *_args: events.append("restore"))
    speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))
    assert events[0] == "restore"
    assert not any(item.startswith("capture") for item in events)
    state = json.loads((output / "state.json").read_text())
    assert state["complete"] and not state["restore_pending"]
    assert "Profiling image restored" in (output / "RESULTS.txt").read_text()


def test_completed_workflow_is_idempotent(tmp_path, monkeypatch):
    output, renderer, events = workflow_mocks(tmp_path, monkeypatch)
    speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))
    assert events == [
        "build profile",
        "capture profile",
        "build render",
        "capture render",
        "restore",
    ]
    events.clear()
    speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))
    assert events == []
    with pytest.raises(RuntimeError, match="resume options differ"):
        speed.orchestrate(output, renderer, "new-feed", Path("fake-hub"))


def test_profile_candidate_does_not_build_or_flash_historical_renderer(tmp_path, monkeypatch):
    output, renderer, events = workflow_mocks(tmp_path, monkeypatch)
    selected = ("ORB_DISPLAY_RASTER_SCALE_CACHE",)
    speed.orchestrate(
        output,
        renderer,
        "0.3.0-control",
        Path("fake-hub"),
        enabled=selected,
        profile_only=True,
    )
    assert events == ["build profile", "capture profile"]
    request = json.loads((output / "state.json").read_text())["request"]
    assert request["enabled"] == list(selected) and request["profile_only"]
    assert "Candidate remains installed" in (output / "RESULTS.txt").read_text()
    with pytest.raises(RuntimeError, match="resume options differ"):
        speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))


def test_build_resume_rejects_changed_experiment_before_external_actions(tmp_path, monkeypatch):
    source = {"commit": "same"}
    speed.save(tmp_path / "built.json", {"source": source, "artifacts": {}, "enabled": []})
    monkeypatch.setattr(speed, "source_identity", lambda _: source)
    with pytest.raises(RuntimeError, match="experiment changed"):
        speed.build(
            tmp_path,
            tmp_path,
            "0.3.0-control",
            profile=True,
            enabled=("ORB_DISPLAY_RASTER_SCALE_CACHE",),
        )


def test_dirty_old_renderer_stops_before_any_build(tmp_path, monkeypatch):
    output, renderer, events = workflow_mocks(tmp_path, monkeypatch)
    original = speed.git
    monkeypatch.setattr(
        speed,
        "git",
        lambda tree, *args: " M source.c" if args[0] == "status" else original(tree, *args),
    )
    with pytest.raises(RuntimeError, match="clean orb/render"):
        speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"))
    assert events == []


def test_logs_new_requests_no_replayed_tail(monkeypatch, capsys):
    requests = []

    class Socket:
        def settimeout(self, value):
            assert value is None

    @contextmanager
    def daemon(request):
        requests.append(request)
        yield Socket(), io.BytesIO(b'{"type":"done"}\n')

    monkeypatch.setattr(transport, "daemon", daemon)
    monkeypatch.setattr(sys, "argv", ["orb_dev_remote.py", "/fake", "--logs-new", "30", "main:"])
    transport.main()
    assert requests == [
        {"op": "logs", "tail": 0, "follow": True, "seconds": 30, "filters": ["main:"]}
    ]
    assert capsys.readouterr().out == ""


def make_script(path, contents):
    path.write_text(contents)
    path.chmod(0o755)


def flash_app_fixture(tmp_path):
    root = tmp_path / "fake repository"
    scripts, tools, build, copied = (
        root / "scripts",
        tmp_path / "tools",
        tmp_path / "build with spaces",
        tmp_path / "copied",
    )
    for path in (scripts, tools, build, copied, root / "firmware"):
        path.mkdir(parents=True)
    shutil.copyfile(ROOT / "scripts/remote.sh", scripts / "remote.sh")
    (scripts / "remote.sh").chmod(0o755)
    (build / "orb.bin").write_bytes(b"unsigned image")
    (build / "ota_data_initial.bin").write_bytes(b"\xff" * 8192)
    make_script(
        scripts / "sign-app.sh",
        '#!/bin/bash\nset -eu\nprintf "%s" "$1" >"$TEST_SIGNED_PATH"\nprintf signed >>"$1"\n',
    )
    make_script(
        scripts / "orb-transport.sh",
        '#!/bin/bash\nset -eu\nprintf "%s\\n" "$@" >"$TEST_TRANSPORT_ARGS"\n',
    )
    make_script(tools / "ssh", "#!/bin/bash\nexit 0\n")
    make_script(tools / "rsync", '#!/bin/bash\nset -eu\ncp -R "$2/." "$TEST_COPIED/"\n')
    environment = {
        **os.environ,
        "PATH": str(tools) + os.pathsep + os.environ["PATH"],
        "TEST_SIGNED_PATH": str(tmp_path / "signed-path"),
        "TEST_TRANSPORT_ARGS": str(tmp_path / "transport-args"),
        "TEST_COPIED": str(copied),
    }
    return scripts / "remote.sh", build, copied, environment


def test_flash_app_signs_copy_and_never_stages_nvs_table_or_bootloader(tmp_path):
    script, build, copied, environment = flash_app_fixture(tmp_path)
    result = subprocess.run(
        [str(script), "flash-app", str(build)], env=environment, capture_output=True, text=True
    )
    assert result.returncode == 0, result.stderr
    assert (build / "orb.bin").read_bytes() == b"unsigned image"
    assert (copied / "orb.bin").read_bytes() == b"unsigned imagesigned"
    assert sorted(path.name for path in copied.iterdir()) == [
        "flash_args",
        "orb.bin",
        "ota_data_initial.bin",
    ]
    assert (copied / "flash_args").read_text().splitlines() == [
        "--flash-mode keep --flash-freq keep --flash-size keep",
        "0xf000 ota_data_initial.bin",
        "0x20000 orb.bin",
    ]
    args = Path(environment["TEST_TRANSPORT_ARGS"]).read_text().splitlines()
    assert args[0] == "--exec"
    assert args[-2:] == ["write-flash", "@flash_args"]
    assert not any("erase" in arg for arg in args)
    signed_path = Path(Path(environment["TEST_SIGNED_PATH"]).read_text())
    assert signed_path != build / "orb.bin"
    assert not signed_path.exists()


@pytest.mark.parametrize("contents", [b"\xff" * 4096, b"\xff" * 8191 + b"\x00"])
def test_flash_app_invalid_selector_fails_before_sign_or_transport(tmp_path, contents):
    script, build, copied, environment = flash_app_fixture(tmp_path)
    (build / "ota_data_initial.bin").write_bytes(contents)
    result = subprocess.run(
        [str(script), "flash-app", str(build)], env=environment, capture_output=True, text=True
    )
    assert result.returncode != 0
    assert not Path(environment["TEST_SIGNED_PATH"]).exists()
    assert not Path(environment["TEST_TRANSPORT_ARGS"]).exists()
    assert list(copied.iterdir()) == []


def test_shell_help_is_host_only():
    result = subprocess.run(
        [str(ROOT / "scripts/speed_profile.sh"), "--help"],
        env={**os.environ, "ORB_PYTHON": sys.executable},
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert "--bench-version" in result.stdout and "--output" in result.stdout


def test_architecture_profile_runs_from_render_branch_and_records_fixture_request(
    tmp_path, monkeypatch
):
    output, renderer, events = workflow_mocks(tmp_path, monkeypatch)
    original = speed.git
    monkeypatch.setattr(
        speed,
        "git",
        lambda tree, *args: (
            "orb/w2-render"
            if tree != renderer and args == ("branch", "--show-current")
            else original(tree, *args)
        ),
    )
    captures = []

    def capture(case, hub, *, fixtures=False):
        captures.append(fixtures)
        attempt = case / "capture-001"
        attempt.mkdir()
        (attempt / "ranked.txt").write_text("profile report\n")
        speed.save(case / "captured.json", {"attempt": attempt.name})

    monkeypatch.setattr(speed, "profile_capture", capture)
    speed.orchestrate(
        output, renderer, "0.3.0-control", Path("fake-hub"), profile_only=True, fixtures=True
    )
    assert captures == [True]
    assert json.loads((output / "state.json").read_text())["request"]["fixtures"]
    with pytest.raises(RuntimeError, match="resume options differ"):
        speed.orchestrate(output, renderer, "0.3.0-control", Path("fake-hub"), profile_only=True)


def test_profile_fixture_capture_preserves_original_three_benches_and_archives_receipts(
    tmp_path, monkeypatch
):
    case = tmp_path / "profile"
    case.mkdir()
    monkeypatch.setattr(speed, "flash", lambda *_: {"fw": "test", "build": "1234567"})
    monkeypatch.setattr(speed, "probe_for", lambda *_: lambda: True)
    monkeypatch.setattr(speed.ab, "wait_settled", lambda probe: probe())
    monkeypatch.setattr(speed, "verify_artifacts", lambda _: {"enabled": []})
    commands = []

    def capture(command, destination, **kwargs):
        commands.append([str(arg) for arg in command[1:]])
        destination.write_text(
            profile_text() if destination.name.startswith("bench-") else "fixture\n"
        )

    monkeypatch.setattr(speed, "run", capture)
    speed.profile_capture(case, Path("fake-hub"), fixtures=True)
    assert commands.count(["bench", "16000"]) == 3
    assert ["bench_transition", "16000"] in commands
    assert ["bench_quiet", "25000"] in commands
    receipt = json.loads((case / "captured.json").read_text())
    assert receipt["fixtures"]
    attempt = case / receipt["attempt"]
    assert (attempt / "ranked.json").exists()
    assert (attempt / "accounting.json").exists()
    assert (
        json.loads((attempt / "fixture-quiet.json").read_text())["command"] == "bench_quiet 25000"
    )


def test_profile_build_rejects_unknown_experiment_without_actions(tmp_path, monkeypatch):
    monkeypatch.setattr(
        speed, "source_identity", lambda *_: pytest.fail("unknown option reached git")
    )
    with pytest.raises(ValueError, match="unsupported"):
        speed.build(tmp_path, tmp_path, "test", profile=True, enabled=("ORB_DISPLAY_TYPO",))
