"""Speed harness failure boundaries; no builds, board, hub, or wall-clock waits."""

import copy
import json
import re
import struct
from pathlib import Path

import pytest
import speed_ab as speed


@pytest.fixture
def identity():
    return {"fw": "0.3.0-control", "build": "1234567", "flashed_at_ms": 1000}


@pytest.fixture
def snapshot(identity):
    return {
        "devices": [
            {
                "device_id": speed.DEVICE,
                "online": True,
                "fw": identity["fw"],
                "build": identity["build"],
                "ota": {},
            }
        ]
    }


def test_exact_image_descriptor(tmp_path):
    image = bytearray(288)
    image[0] = 0xE9
    struct.pack_into("<H", image, 12, 9)
    struct.pack_into("<I", image, 32, 0xABCD5432)
    image[48:61] = b"0.3.0-control"
    image[176:208] = bytes.fromhex("1234567a" + "00" * 28)
    path = tmp_path / "orb.bin"
    path.write_bytes(image)
    assert speed.image_identity(path) == {
        "fw": "0.3.0-control",
        "build": "1234567",
        "elf_sha256": image[176:208].hex(),
    }
    image[12] = 0
    path.write_bytes(image)
    with pytest.raises(ValueError, match="ESP32-S3"):
        speed.image_identity(path)


@pytest.mark.parametrize("state", ["downloading", "verifying", "ready", "failed"])
def test_active_ota_invalidates_even_while_online(identity, snapshot, state):
    snapshot["devices"][0]["ota"] = {"report": {"state": state, "at": 1000}}
    with pytest.raises(RuntimeError, match="OTA active"):
        speed.device_status(snapshot, identity)


def test_hub_retains_previous_boot_report(identity, snapshot):
    snapshot["devices"][0]["ota"] = {"report": {"state": "ready", "at": 999}}
    assert speed.device_status(snapshot, identity)


@pytest.mark.parametrize("key", ["fw", "build"])
def test_version_reuse_requires_exact_build(identity, snapshot, key):
    snapshot["devices"][0][key] = "other"
    with pytest.raises(RuntimeError, match="differs"):
        speed.device_status(snapshot, identity)


def test_offline_is_waitable_but_wrong_device_is_not(identity, snapshot):
    snapshot["devices"][0]["online"] = False
    assert not speed.device_status(snapshot, identity)
    snapshot["devices"][0]["device_id"] = "second-board-must-not-be-used"
    with pytest.raises(RuntimeError, match="absent"):
        speed.device_status(snapshot, identity)


class Clock:
    now = 0

    def __call__(self):
        return self.now

    def sleep(self, delay):
        assert 0 < delay <= 2
        self.now += delay


def test_settle_is_full_90_seconds_after_online():
    clock = Clock()
    speed.wait_settled(lambda: clock.now >= 20, clock=clock, sleep=clock.sleep)
    assert clock.now == 110


def test_disconnect_resets_whole_settle_period():
    clock = Clock()
    speed.wait_settled(lambda: clock.now != 60, clock=clock, sleep=clock.sleep)
    assert clock.now == 152


def test_never_online_times_out():
    clock = Clock()
    with pytest.raises(TimeoutError, match="90 seconds"):
        speed.wait_settled(lambda: False, clock=clock, sleep=clock.sleep, timeout=100)
    assert clock.now == 100


def test_guard_stops_command_if_candidate_changes(monkeypatch, tmp_path):
    class Process:
        returncode = None
        terminated = False

        def poll(self):
            return self.returncode

        def terminate(self):
            self.terminated = True
            self.returncode = -15

        def wait(self, **kwargs):
            return self.returncode

    process = Process()
    monkeypatch.setattr(speed.subprocess, "Popen", lambda *a, **kw: process)
    results = iter([True, False])
    with pytest.raises(RuntimeError, match="during"):
        speed.guarded_command(["never-executed"], tmp_path / "bench.txt", lambda: next(results))
    assert process.terminated


def test_failed_flash_invalidates_previous_success(monkeypatch, tmp_path, identity):
    candidate = identity | {"elf_sha256": "a" * 64}
    speed.write_json(tmp_path / "identity.json", candidate)
    (tmp_path / "sdkconfig.h").write_text("config")
    build = tmp_path / "build"
    (build / "config").mkdir(parents=True)
    (build / "config/sdkconfig.h").write_text("config")
    monkeypatch.setattr(speed, "BUILD", build)
    monkeypatch.setattr(speed, "image_identity", lambda p: candidate)

    def failed(*args, **kwargs):
        raise RuntimeError("flash failed")

    monkeypatch.setattr(speed, "run_log", failed)
    with pytest.raises(RuntimeError, match="flash failed"):
        speed.flash_case(tmp_path)
    assert "flashed_at_ms" not in json.loads((tmp_path / "identity.json").read_text())


def test_build_refuses_reused_sdkconfig_before_running_anything(tmp_path, monkeypatch):
    def forbidden(*args, **kwargs):
        pytest.fail("reused case reached external command")

    monkeypatch.setattr(speed, "run_log", forbidden)
    with pytest.raises(FileExistsError):
        speed.build_case(tmp_path, "0.3.0-control", [])


def test_measure_never_benches_wrong_or_ota_candidate(tmp_path, monkeypatch, identity, snapshot):
    speed.write_json(tmp_path / "identity.json", identity)
    snapshot = copy.deepcopy(snapshot)
    snapshot["devices"][0]["ota"] = {"report": {"state": "downloading", "at": 1100}}
    monkeypatch.setattr(speed.subprocess, "check_output", lambda *a, **kw: json.dumps(snapshot))

    def forbidden(*args, **kwargs):
        pytest.fail("contaminated candidate reached bench")

    monkeypatch.setattr(speed, "guarded_command", forbidden)
    with pytest.raises(RuntimeError, match="OTA active"):
        speed.measure_case(tmp_path, Path("fake-hub"))
    assert (tmp_path / "measurement/INVALID").exists()
    assert not (tmp_path / "measurement/COMPLETE").exists()


def test_config_difference_normalizes_order_and_comments():
    assert (
        speed.config_difference(
            "#define CONFIG_B 2\n#define CONFIG_A 1\n/* one */\n",
            "/* two */\n#define CONFIG_A 1\n#define CONFIG_B 2\n",
        )
        == {}
    )
    assert speed.config_difference("#define CONFIG_CLOCK 80\n", "#define CONFIG_CLOCK 40\n") == {
        "CONFIG_CLOCK": ["80", "40"]
    }


@pytest.mark.parametrize("open_control", [False, True])
def test_build_uses_fresh_explicit_config_and_records_provenance(
    tmp_path, monkeypatch, open_control
):
    root = tmp_path / "project with spaces"
    build = root / "firmware/build"
    (build / "config").mkdir(parents=True)
    for name in (
        "orb.bin",
        "orb.map",
        "CMakeCache.txt",
        "compile_commands.json",
        "project_description.json",
        "config/sdkconfig.h",
    ):
        (build / name).write_text("mock")
    (build / "config/sdkconfig.h").write_text(
        "#define CONFIG_ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH 1\n"
        + ("" if open_control else "#define CONFIG_ORB_OPEN_LATENCY_FIXES 1\n")
    )
    commands = []

    def capture(command, destination):
        commands.append(command)
        destination.write_text("mock log")

    monkeypatch.setattr(speed, "ROOT", root)
    monkeypatch.setattr(speed, "BUILD", build)
    monkeypatch.setattr(speed, "run_log", capture)
    monkeypatch.setattr(
        speed,
        "image_identity",
        lambda path: {"fw": "0.3.0-control", "build": "1234567", "elf_sha256": "a" * 64},
    )
    monkeypatch.setattr(
        speed.subprocess,
        "check_output",
        lambda args, **kwargs: "commit\n" if "rev-parse" in args else b"",
    )
    directory = tmp_path / "new case"
    comparison = tmp_path / "control-sdkconfig.h"
    comparison.write_text(
        "#define CONFIG_ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH 1\n"
        + ("#define CONFIG_ORB_OPEN_LATENCY_FIXES 1\n" if open_control else "")
    )
    speed.build_case(
        directory,
        "0.3.0-control",
        ["ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH"],
        comparison,
        open_control=open_control,
    )
    assert commands[0][1:3] == ["-B", "build"]
    assert f"SDKCONFIG={directory / 'sdkconfig'}" in commands[0]
    assert commands[0][-2:] == ["reconfigure", "build"]
    assert (
        'CONFIG_APP_PROJECT_VER="0.3.0-control"' in (directory / "experiment.defaults").read_text()
    )
    assert json.loads((directory / "identity.json").read_text())["build"] == "1234567"
    assert (directory / "compile_commands.json").exists()
    expected = "n" if open_control else "y"
    assert (
        f"CONFIG_ORB_OPEN_LATENCY_FIXES={expected}\n"
        in (directory / "experiment.defaults").read_text()
    )
    assert (
        json.loads((directory / "identity.json").read_text())["options"]["ORB_OPEN_LATENCY_FIXES"]
        == expected
    )
    assert "ORB_OPEN_LATENCY_FIXES" not in speed.BASE_OPTIONS
    for name in ("ORB_RENDER_FUSED_GLASS", "ORB_RENDER_ROWS_AT_SIZE"):
        assert f"CONFIG_{name}=n\n" in (directory / "experiment.defaults").read_text()
        assert json.loads((directory / "identity.json").read_text())["options"][name] == "n"


def test_measure_captures_diagnostics_after_bench_and_shot(
    tmp_path, monkeypatch, identity, snapshot
):
    speed.write_json(tmp_path / "identity.json", identity)
    monkeypatch.setattr(speed.subprocess, "check_output", lambda *a, **kw: json.dumps(snapshot))
    monkeypatch.setattr(speed, "wait_settled", lambda probe: probe())
    commands = []

    def capture(command, destination, probe, **kwargs):
        assert probe()
        commands.append((command, destination.name, kwargs.get("timeout")))

    monkeypatch.setattr(speed, "guarded_command", capture)
    speed.measure_case(tmp_path, Path("fake-hub"))
    assert commands[-2][0][1] == "shot"
    assert commands[-1][0][1:3] == ["--logs", "3"]
    assert commands[-1][1:] == ("display-after.log", 15)
    assert sum("bench" in command[0] for command in commands) == 3
    assert (tmp_path / "measurement/COMPLETE").exists()


def accounting_text(phases=("scroll", "glass"), command="bench 16000", prefix="@@"):
    lines = [
        "ORBDEV ACCOUNT_META clock=wall_us interval_bucket_us=1000 buckets=128 "
        "last_unbounded=1 trace_capacity=8 overflow=0 active=1 cold_prepare_us=1234",
    ]
    for phase in phases:
        lines += [
            f"ORBDEV BENCH {phase} seconds=8.00 frames=2 fps=0.25 bytes=400 bands=2",
            f"ORBDEV ACCOUNT phase={phase} scheduled=2 completed=2 dropped=0 "
            "preparation_us=250 scene_prepare_us=100 whole_update_us=12500 "
            "source_builds=0 mip_builds=0 largest_internal=30000 largest_dma=30000 "
            "largest_psram=2000000 scratch_peak=4096",
            f"ORBDEV CADENCE phase={phase} bucket=34 intervals=1 whole_updates=2",
            f"ORBDEV MATERIAL phase={phase} map_hits=2 map_builds=0 base_hits=2 "
            "base_builds=0 recaptures=0 kernels=0 fallbacks=0 bytes=100 peak=100 budget=192000 entries=1",
            f"ORBDEV BAND phase={phase} seq=2 draw_us=100 queue_us=101 sender_us=102 "
            "dma_us=103 done_us=104 bytes=200 y1=0 y2=1 ok=1",
        ]
    lines.append(f"ORBDEV OK {command}")
    return "\n".join(prefix + line for line in lines) + "\n"


@pytest.mark.parametrize("prefix", ["", "@@"])
def test_accounting_retains_cold_preparation_whole_update_cadence_and_bands(tmp_path, prefix):
    path = tmp_path / "bench-1.txt"
    path.write_text(accounting_text(prefix=prefix))
    speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    capture = json.loads((tmp_path / "accounting.json").read_text())["captures"][0]
    assert capture["account_meta"][0]["cold_prepare_us"] == 1234
    assert capture["account"][0]["whole_update_us"] == 12500
    assert capture["account"][0]["preparation_us"] == 250
    assert capture["cadence"][0]["bucket"] == 34
    assert capture["band"][0]["dma_us"] == 103
    assert capture["material"][0]["kernels"] == 0
    assert capture["bench"][0]["bytes"] == 400


@pytest.mark.parametrize("enabled", [False, True])
def test_accounting_archives_real_mip_work_and_memory_when_available(tmp_path, enabled):
    path = tmp_path / "bench-1.txt"
    text = accounting_text().replace(
        "cold_prepare_us=1234", f"cold_prepare_us=1234 cold_mip_builds={7 if enabled else 0}"
    )
    path.write_text(
        text.replace(
            "mip_builds=0 largest_internal",
            f"mip_builds={2 if enabled else 0} mip_bytes={800 if enabled else 0} "
            f"mip_budget={196608 if enabled else 0} largest_internal",
        )
    )
    speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    capture = json.loads((tmp_path / "accounting.json").read_text())["captures"][0]
    assert capture["account_meta"][0]["cold_mip_builds"] == (7 if enabled else 0)
    assert capture["account"][0]["mip_builds"] == (2 if enabled else 0)
    assert capture["account"][0]["mip_bytes"] == (800 if enabled else 0)
    assert capture["account"][0]["mip_budget"] == (196608 if enabled else 0)


@pytest.mark.parametrize(
    "fields",
    [
        "mip_bytes=2",
        "mip_budget=100",
        "mip_bytes=-1 mip_budget=100",
        "mip_bytes=101 mip_budget=100",
        "mip_bytes=word mip_budget=100",
    ],
)
def test_accounting_rejects_incomplete_or_overbudget_mip_payloads(tmp_path, fields):
    path = tmp_path / "bench-1.txt"
    path.write_text(accounting_text().replace("mip_builds=0", f"mip_builds=0 {fields}"))
    with pytest.raises(RuntimeError, match="mip memory"):
        speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    assert not (tmp_path / "accounting.json").exists()


@pytest.mark.parametrize(
    "change",
    [
        lambda s: s.replace("overflow=0", "overflow=1"),
        lambda s: s.replace("active=1", "active=0"),
        lambda s: s + "Guru Meditation Error: synthetic\n",
        lambda s: s.replace("clock=wall_us", "clock=cpu"),
        lambda s: s.replace("interval_bucket_us=1000", "interval_bucket_us=4000"),
        lambda s: s.replace("buckets=128", "buckets=64"),
        lambda s: s.replace("phase=glass", "phase=scroll"),
        lambda s: s.replace("scheduled=2", "scheduled=1"),
        lambda s: s.replace("source_builds=0", "source_builds=-1"),
        lambda s: s.replace("bucket=34", "bucket=128"),
        lambda s: s.replace("@@ORBDEV OK bench 16000", ""),
    ],
)
def test_accounting_rejects_incomplete_or_misleading_receipts(tmp_path, change):
    path = tmp_path / "bench-1.txt"
    path.write_text(change(accounting_text()))
    with pytest.raises(RuntimeError):
        speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    assert not (tmp_path / "accounting.json").exists()


def test_accounting_archives_transition_and_quiet_separately(tmp_path):
    paths = []
    for phase, duration in (("transition", 16000), ("quiet", 25000)):
        path = tmp_path / f"fixture-{phase}.txt"
        path.write_text(accounting_text((phase,), f"bench_{phase} {duration}"))
        paths.append(path)
    speed.accounting_report(paths, tmp_path / "accounting.json", required=True)
    captures = json.loads((tmp_path / "accounting.json").read_text())["captures"]
    assert [row["command"] for row in captures] == ["bench_transition 16000", "bench_quiet 25000"]
    assert [row["account"][0]["phase"] for row in captures] == ["transition", "quiet"]


def test_accounting_archives_pacing_and_backdrop_shortcuts(tmp_path):
    path = tmp_path / "bench-1.txt"
    text = accounting_text().replace(
        "@@ORBDEV OK bench 16000",
        "@@ORBDEV PACING phase=scroll scheduled=200 on_slot=199 late=1 skipped=0 "
        "boundaries=1 unpaced=0 admitted_30=0 period_us=16800 divisor=2 max_error_us=1100\n"
        "@@ORBDEV OK bench 16000",
    )
    path.write_text(text.replace("kernels=0", "kernels=0 solid_fills=10 moving_bypasses=20"))
    speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    capture = json.loads((tmp_path / "accounting.json").read_text())["captures"][0]
    assert capture["pacing"][0]["period_us"] == 16800
    assert capture["pacing"][0]["on_slot"] == 199
    assert capture["pacing"][0]["max_error_us"] == 1100
    assert capture["material"][0]["solid_fills"] == 10
    assert capture["material"][0]["moving_bypasses"] == 20


@pytest.mark.parametrize("field", ["late=-1", "divisor=3", "period_us=invalid"])
def test_accounting_rejects_malformed_pacing_receipts(tmp_path, field):
    path = tmp_path / "bench-1.txt"
    row = (
        "@@ORBDEV PACING phase=scroll scheduled=1 on_slot=1 late=0 skipped=0 "
        "boundaries=1 unpaced=0 admitted_30=0 period_us=16800 divisor=2 max_error_us=1\n"
    )
    key = field.split("=")[0]
    row = re.sub(rf"\b{key}=\S+", field, row)
    path.write_text(accounting_text() + row)
    with pytest.raises(RuntimeError, match="invalid pacing"):
        speed.accounting_report([path], tmp_path / "accounting.json", required=True)


@pytest.mark.parametrize("required", [True, False])
def test_accounting_option_cannot_silently_lose_instrumentation(tmp_path, required):
    path = tmp_path / "bench-1.txt"
    path.write_text("@@ORBDEV OK bench 16000\n")
    if required:
        with pytest.raises(RuntimeError, match="missing accounting"):
            speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    else:
        speed.accounting_report([path], tmp_path / "accounting.json")
        assert json.loads((tmp_path / "accounting.json").read_text()) == {
            "enabled": False,
            "captures": [],
        }


def test_build_refuses_unknown_experiment_before_external_action(tmp_path, monkeypatch):
    monkeypatch.setattr(speed, "run_log", lambda *_: pytest.fail("unknown option reached build"))
    with pytest.raises(ValueError, match="supported"):
        speed.build_case(tmp_path / "candidate", "0.3.0-control", ["ORB_DISPLAY_TYPO"])


def test_measure_fixtures_have_independent_capture_receipts(
    tmp_path, monkeypatch, identity, snapshot
):
    speed.write_json(tmp_path / "identity.json", identity)
    monkeypatch.setattr(speed.subprocess, "check_output", lambda *a, **kw: json.dumps(snapshot))
    monkeypatch.setattr(speed, "wait_settled", lambda probe: probe())
    commands = []

    def capture(command, destination, probe, **kwargs):
        assert probe()
        commands.append(command[1:])
        destination.write_text("fake capture\n")

    monkeypatch.setattr(speed, "guarded_command", capture)
    speed.measure_case(tmp_path, Path("fake-hub"), fixtures=True)
    assert ["bench_transition", "16000"] in commands
    assert ["bench_quiet", "25000"] in commands
    result = tmp_path / "measurement"
    assert json.loads((result / "fixture-quiet.json").read_text())["command"] == "bench_quiet 25000"
    assert (
        json.loads((result / "fixture-transition.json").read_text())["capture"]
        == "fixture-transition.txt"
    )


def open_text(surface="play", cold=1):
    return (
        f"@@ORBDEV OPEN surface={surface} origin=injected_navigation cold={cold} sequence=9 causal=1 "
        "first_changed_band_us=10000 first_changed_frame_us=19000 transition_end_us=560000 "
        "motion_end_us=550000 motion_duration_us=550000 panel_period_us=16800 source_prepare_us=650000 object_us=6000 max_slice_us=2000 build_slices=3 "
        "font_loads=0 font_failures=0 font_us=0 raster_builds=2 raster_us=6000 scale_raster_us=0 raster_timing=1 material_base_builds=0 glass_mask_builds=2 glass_mask_failures=0 glass_mask_us=1200 glass_us=1800 glass_timing=1 material_counters=1 blur_kernels=4 accounting_timing=1 preparation_us=9000 scene_prepare_us=6000 whole_update_us=45000 "
        "draw_us=17000 wire_us=7000 bytes=32000 frames=3 errors=0 failed_frames=0 "
        "minimum_internal=15800 cache_bytes=400000 cache_entries=2\n"
        f"@@ORBDEV OK bench_open {surface}\n"
    )


def test_open_receipts_preserve_dma_model_end_and_actual_cold_classification(tmp_path):
    paths = []
    for index, cold in enumerate((1, 0, 0)):
        path = tmp_path / f"open-play-{index}.txt"
        path.write_text(open_text(cold=cold))
        paths.append(path)
    speed.open_report(paths, tmp_path / "open.json")
    result = json.loads((tmp_path / "open.json").read_text())
    assert [row["cold"] for row in result["captures"]] == [1, 0, 0]
    assert result["captures"][0]["transition_end_us"] == 560000
    assert result["captures"][0]["motion_end_us"] == 550000
    assert [group["samples"] for group in result["groups"]] == [1, 2]
    assert [group["first_frame_target_us"] for group in result["groups"]] == [35000, 20000]
    assert all(group["first_frame_pass"] for group in result["groups"])
    assert all(group["transition_pass"] for group in result["groups"])
    assert result["captures"][0]["transition_target_us"] == 566800
    assert result["captures"][0]["raster_us"] == 6000
    assert result["captures"][0]["glass_mask_us"] == 1200
    assert result["captures"][0]["glass_us"] == 1800


@pytest.mark.parametrize(
    "change",
    [
        lambda s: s.replace("causal=1", "causal=0"),
        lambda s: s.replace("first_changed_band_us=10000", "first_changed_band_us=20000"),
        lambda s: s.replace("errors=0", "errors=1"),
        lambda s: s.replace("cold=1", "cold=2"),
        lambda s: s.replace("first_changed_frame_us=19000", "first_changed_frame_us=-1"),
        lambda s: s.replace("font_us=0", "font_us=unknown"),
        lambda s: s.replace("origin=injected_navigation", "origin=physical_touch"),
        lambda s: s.replace("raster_timing=1", "raster_timing=0"),
        lambda s: s.replace("glass_timing=1", "glass_timing=0"),
        lambda s: s.replace("accounting_timing=1", "accounting_timing=0"),
        lambda s: s.replace("material_counters=1", "material_counters=0"),
        lambda s: s.replace("motion_duration_us=550000", "motion_duration_us=50000"),
        lambda s: s.replace("@@ORBDEV OK bench_open play", ""),
        lambda s: s + "Guru Meditation Error synthetic\n",
    ],
)
def test_open_receipts_reject_stale_failed_or_untruthful_completion(tmp_path, change):
    path = tmp_path / "open-play-1.txt"
    path.write_text(change(open_text()))
    with pytest.raises(RuntimeError):
        speed.open_report([path], tmp_path / "open.json")
    assert not (tmp_path / "open.json").exists()


@pytest.mark.parametrize("period, end, expected", [(16800, 580000, False), (0, 560000, None)])
def test_open_transition_gate_keeps_failures_and_unknown_panel_intervals(
    tmp_path, period, end, expected
):
    path = tmp_path / "open-play-1.txt"
    path.write_text(
        open_text()
        .replace("panel_period_us=16800", f"panel_period_us={period}")
        .replace("transition_end_us=560000", f"transition_end_us={end}")
    )
    speed.open_report([path], tmp_path / "open.json")
    result = json.loads((tmp_path / "open.json").read_text())
    assert result["captures"][0]["transition_pass"] is expected
    assert result["groups"][0]["transition_pass"] is expected


def test_open_unprofiled_zero_timing_is_explicitly_unavailable(tmp_path):
    text = open_text()
    for before, after in (
        ("raster_timing=1", "raster_timing=0"),
        ("raster_us=6000", "raster_us=0"),
        ("glass_timing=1", "glass_timing=0"),
        ("glass_us=1800", "glass_us=0"),
        ("material_counters=1", "material_counters=0"),
        ("blur_kernels=4", "blur_kernels=0"),
        ("accounting_timing=1", "accounting_timing=0"),
        ("preparation_us=9000", "preparation_us=0"),
        ("scene_prepare_us=6000", "scene_prepare_us=0"),
        ("whole_update_us=45000", "whole_update_us=0"),
    ):
        text = text.replace(before, after)
    path = tmp_path / "open-play-1.txt"
    path.write_text(text)
    speed.open_report([path], tmp_path / "open.json")
    row = json.loads((tmp_path / "open.json").read_text())["captures"][0]
    assert row["raster_us"] == row["raster_timing"] == row["glass_us"] == row["glass_timing"] == 0
    assert row["glass_mask_us"] == 1200  # Always-on actual SDF timing remains measured.


@pytest.mark.parametrize("open_runs", [2, 20])
def test_measure_opens_before_synthetic_fixtures_and_requires_real_receipts(
    tmp_path, monkeypatch, identity, snapshot, open_runs
):
    speed.write_json(tmp_path / "identity.json", identity)
    monkeypatch.setattr(speed.subprocess, "check_output", lambda *a, **kw: json.dumps(snapshot))
    monkeypatch.setattr(speed, "wait_settled", lambda probe: probe())
    commands = []

    def capture(command, destination, probe, **kwargs):
        assert probe()
        words = command[1:]
        commands.append(words)
        if words[0] == "bench_open":
            destination.write_text(open_text(words[1], cold=int(destination.stem.endswith("-1"))))
        else:
            destination.write_text("fake capture\n")

    monkeypatch.setattr(speed, "guarded_command", capture)
    speed.measure_case(tmp_path, Path("fake-hub"), opens=True, open_runs=open_runs)
    assert commands[3:5] == [["bench_open", "apps"], ["bench_open", "apps"]]
    assert commands[3 + 5 * open_runs] == ["bench", "16000"]
    result = json.loads((tmp_path / "measurement/open.json").read_text())
    assert len(result["captures"]) == 5 * open_runs
    assert len(result["groups"]) == 10


def surface_text(surface="agents", **changes):
    row = {
        "surface": surface,
        "workload": "page_swipe" if surface == "favourites" else "fling_settle",
        "seconds": "16.000",
        "frames": "300",
        "fps": "0" if surface == "favourites" else "32.5",
        "overall_fps": "18.75",
        "moving_us": "6500000",
        "moving_frames": "211",
        "idle_frames": "0",
        "gestures": "10",
        "draw_ms": "20.0",
        "wire_ms": "6.0",
        "whole_update_ms": "30.0",
        "accounting": "1",
        "cpu_available": "1",
        "cpu0_pct": "30.0",
        "cpu1_pct": "60.0",
        "moving_cpu_available": "1",
        "moving_cpu0_pct": "45.0",
        "moving_cpu1_pct": "95.0",
        "glass_timing": "1",
        "blur_task_ms": "5.0",
        "glass_prepare_ms": "1.0",
        "glass_compose_ms": "2.0",
        "dirty_pixels": "30000000",
        "dirty_pct": "46.0",
        "minimum_internal": "15000",
        "errors": "0",
        "failed_frames": "0",
    }
    row.update(changes)
    return (
        "@@ORBDEV SCROLL "
        + " ".join(f"{key}={value}" for key, value in row.items())
        + f"\n@@ORBDEV OK bench_scroll {surface} 16000\n"
    )


def surface_gate_text(surface="agents", *, gate_changes=None, **changes):
    observations = {
        "workload": "spring_page_swipe" if surface == "favourites" else "touch_fling_settle",
        "fps": "50.00",
        "frames": "400",
        "moving_frames": "325",
        "loops": "1000",
        "handler_max_us": "33000",
        "anim_ticks": "300",
        "anim_gap_max_us": "33000",
        "invalidated_pixels": "48000000",
        "refreshes": "300",
    }
    observations.update(changes)
    text = surface_text(surface, **observations)
    main = dict(re.findall(r"(\w+)=([^\s]+)", text.splitlines()[0]))
    target = 50 if surface in ("agents", "settings", "notifications") else 30
    gate = {
        "version": 1,
        "surface": surface,
        "target_fps": target,
        "fps_pass": int(int(main["moving_frames"]) * 1000000 >= int(main["moving_us"]) * target),
        "stall_limit_us": 33000,
        "handler_stalls": int(int(main["handler_max_us"]) > 33000),
        "animation_stalls": int(int(main["anim_gap_max_us"]) > 33000),
        "stall_pass": int(
            int(main["handler_max_us"]) <= 33000 and int(main["anim_gap_max_us"]) <= 33000
        ),
        "viewport_pixels": 120000,
        "invalidation_limit_permille": 1100,
        "damage_frames": 300,
        "damage_pixels": 30000000,
        "max_damage_pixels": 132000,
        "damage_ratio": "1.100000",
        "request_invalidation_pass": int(
            int(main["invalidated_pixels"]) * 1000 <= int(main["frames"]) * 120000 * 1100
        ),
        "damage_pass": 1,
        "invalidation_pass": int(
            int(main["invalidated_pixels"]) * 1000 <= int(main["frames"]) * 120000 * 1100
        ),
        "request_pixels_per_frame": f"{int(main['invalidated_pixels']) / int(main['frames']):.3f}",
        "source_builds": 0,
        "mip_builds": 0,
        "row_builds": 0,
        "row_evictions": 0,
    }
    gate.update(gate_changes or {})
    receipt = "@@ORBDEV SCROLL_GATE " + " ".join(f"{key}={value}" for key, value in gate.items())
    return text.replace("@@ORBDEV OK", receipt + "\n@@ORBDEV OK")


@pytest.mark.parametrize("surface", speed.SCROLL_SURFACES)
def test_surface_report_keeps_motion_idle_cpu_material_and_dirty_metrics(tmp_path, surface):
    path = tmp_path / f"scroll-{surface}-1.txt"
    path.write_text(surface_text(surface))
    speed.surface_report([path], tmp_path / "surfaces.json")
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    assert row["moving_cpu1_pct"] == 95
    assert row["cpu1_pct"] == 60
    assert row["dirty_pixels"] == 30000000
    assert row["blur_task_ms"] == 5
    assert row["idle_pass"] and row["internal_floor_pass"]
    assert row["scroll_fps_pass"] == (None if surface == "favourites" else True)
    assert row["fps"] == (0 if surface == "favourites" else 32.5)
    assert not row["attribution_gates_available"]
    assert row["stall_pass"] is None and row["invalidation_pass"] is None


@pytest.mark.parametrize(
    "changes",
    [
        {"fps": "nan"},
        {"fps": "inf"},
        {"frames": "0"},
        {"gestures": "0"},
        {"moving_frames": "301"},
        {"cpu_available": "0"},
        {"cpu1_pct": "101"},
        {"moving_cpu1_pct": "101"},
        {"moving_cpu_available": "0"},
        {"accounting": "0"},
        {"glass_timing": "0"},
        {"errors": "1"},
        {"failed_frames": "1"},
        {"dirty_pixels": "-1"},
        {"workload": "synthetic"},
    ],
)
def test_surface_report_rejects_misleading_or_faulted_receipts(tmp_path, changes):
    path = tmp_path / "scroll-agents-1.txt"
    path.write_text(surface_text(**changes))
    with pytest.raises(RuntimeError):
        speed.surface_report([path], tmp_path / "surfaces.json")
    assert not (tmp_path / "surfaces.json").exists()


def test_surface_report_keeps_explicitly_unavailable_counters_and_failed_gates(tmp_path):
    path = tmp_path / "scroll-settings-1.txt"
    path.write_text(
        surface_text(
            "settings",
            accounting="0",
            whole_update_ms="0",
            cpu_available="0",
            cpu0_pct="0",
            cpu1_pct="0",
            moving_cpu_available="0",
            moving_cpu0_pct="0",
            moving_cpu1_pct="0",
            glass_timing="0",
            blur_task_ms="0",
            glass_prepare_ms="0",
            glass_compose_ms="0",
            fps="21.5",
            idle_frames="3",
            minimum_internal="10000",
        )
    )
    speed.surface_report([path], tmp_path / "surfaces.json")
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    assert not row["accounting"] and not row["glass_timing"]
    assert not row["scroll_fps_pass"] and not row["idle_pass"] and not row["internal_floor_pass"]


def test_surface_paging_cannot_claim_continuous_animation_fps(tmp_path):
    path = tmp_path / "scroll-favourites-1.txt"
    path.write_text(surface_text("favourites", fps="60"))
    with pytest.raises(RuntimeError, match="inconsistent"):
        speed.surface_report([path], tmp_path / "surfaces.json")


@pytest.mark.parametrize("surface", speed.SCROLL_SURFACES)
def test_touch_and_spring_receipts_use_surface_specific_motion_gate(tmp_path, surface):
    path = tmp_path / f"scroll-{surface}-1.txt"
    path.write_text(
        surface_text(
            surface,
            workload="spring_page_swipe" if surface == "favourites" else "touch_fling_settle",
            fps="40",
        )
    )
    speed.surface_report([path], tmp_path / "surfaces.json")
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    fast_list = surface in ("agents", "settings", "notifications")
    assert row["target_fps"] == (50 if fast_list else 30)
    assert row["scroll_fps_pass"] is (not fast_list)


def test_attribution_gates_use_integer_fps_and_maximum_damage_not_sample_mean(tmp_path):
    path = tmp_path / "gates.txt"
    # Rounded FPS says 50; the integer sample remains just below the target.
    path.write_text(surface_gate_text(moving_us="6500001"))
    speed.surface_report([path], tmp_path / "surfaces.json", require_gates=True)
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    assert row["fps"] == 50 and not row["scroll_fps_pass"]
    assert row["stall_pass"] and row["invalidation_pass"] and not row["attribution_pass"]
    # Average damage is below one viewport, but one frame exceeds its limit.
    path.write_text(
        surface_gate_text(
            gate_changes={
                "max_damage_pixels": 132001,
                "damage_ratio": "1.100008",
                "damage_pass": 0,
                "invalidation_pass": 0,
            }
        )
    )
    speed.surface_report([path], tmp_path / "surfaces.json", require_gates=True)
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    assert (
        row["gates"]["damage_pixels"] / row["gates"]["damage_frames"]
        < row["gates"]["viewport_pixels"]
    )
    assert not row["invalidation_pass"] and not row["attribution_pass"]


@pytest.mark.parametrize("field", ["handler_max_us", "anim_gap_max_us"])
def test_attribution_gate_keeps_stalls_as_failed_acceptance(tmp_path, field):
    path = tmp_path / "gates.txt"
    path.write_text(surface_gate_text(**{field: "33001"}))
    speed.surface_report([path], tmp_path / "surfaces.json", require_gates=True)
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    assert row["scroll_fps_pass"] and row["invalidation_pass"]
    assert not row["stall_pass"] and not row["attribution_pass"]


@pytest.mark.parametrize(
    "requests,passes", [(52800000, True), (52800001, False), (91000000, False)]
)
def test_attribution_gate_checks_raw_request_mean_even_when_damage_union_fits(
    tmp_path, requests, passes
):
    path = tmp_path / "gates.txt"
    path.write_text(surface_gate_text(invalidated_pixels=str(requests)))
    speed.surface_report([path], tmp_path / "surfaces.json", require_gates=True)
    row = json.loads((tmp_path / "surfaces.json").read_text())["captures"][0]
    assert row["gates"]["damage_pass"]
    assert bool(row["gates"]["request_invalidation_pass"]) is passes
    assert row["invalidation_pass"] is passes


@pytest.mark.parametrize(
    "changes",
    [
        {"fps_pass": 0},
        {"stall_pass": 0},
        {"invalidation_pass": 0},
        {"request_invalidation_pass": 0},
        {"damage_pass": 0},
        {"target_fps": 30},
        {"stall_limit_us": 100000},
        {"viewport_pixels": 0},
        {"damage_ratio": "nan"},
        {"damage_ratio": "1.0"},
        {"damage_pixels": 100},
        {"damage_frames": 301},
        {"request_pixels_per_frame": "0"},
        {"surface": "rooms"},
        {"source_builds": "bad"},
    ],
)
def test_attribution_parser_rejects_false_assertions_and_inconsistent_counters(tmp_path, changes):
    path = tmp_path / "gates.txt"
    path.write_text(surface_gate_text(gate_changes=changes))
    with pytest.raises(RuntimeError, match="surface gate"):
        speed.surface_report([path], tmp_path / "surfaces.json", require_gates=True)


def test_attribution_parser_requires_current_receipts_for_new_measurements(tmp_path):
    path = tmp_path / "gates.txt"
    path.write_text(surface_text())
    with pytest.raises(RuntimeError, match="missing or duplicate surface gates"):
        speed.surface_report([path], tmp_path / "surfaces.json", require_gates=True)
    text = surface_gate_text()
    gate = next(line for line in text.splitlines() if "SCROLL_GATE " in line)
    path.write_text(text + gate + "\n")
    with pytest.raises(RuntimeError, match="duplicate"):
        speed.surface_report([path], tmp_path / "surfaces.json")


def test_surface_accounting_has_its_own_phase_and_command(tmp_path):
    path = tmp_path / "scroll-notifications-1.txt"
    path.write_text(accounting_text(("notifications",), "bench_scroll notifications 16000"))
    speed.accounting_report([path], tmp_path / "accounting.json", required=True)
    row = json.loads((tmp_path / "accounting.json").read_text())["captures"][0]
    assert row["command"] == "bench_scroll notifications 16000"
    assert row["account"][0]["phase"] == "notifications"


def test_measure_surfaces_drives_all_real_views_three_times_with_build_guard(
    tmp_path, monkeypatch, identity, snapshot
):
    speed.write_json(tmp_path / "identity.json", identity)
    monkeypatch.setattr(speed.subprocess, "check_output", lambda *a, **kw: json.dumps(snapshot))
    monkeypatch.setattr(speed, "wait_settled", lambda probe: probe())
    commands = []

    def capture(command, destination, probe, **kwargs):
        assert probe()
        commands.append(command[1:])
        if command[1] == "bench_scroll":
            destination.write_text(surface_gate_text(command[2]))

    monkeypatch.setattr(speed, "guarded_command", capture)
    speed.measure_case(tmp_path, Path("fake-hub"), surfaces=speed.SCROLL_SURFACES)
    assert [command for command in commands if command[0] == "bench_scroll"] == [
        ["bench_scroll", surface, "16000"] for surface in speed.SCROLL_SURFACES for _ in range(3)
    ]
    assert (tmp_path / "measurement/surfaces.json").exists()
    assert (tmp_path / "measurement/COMPLETE").exists()


@pytest.mark.parametrize("surfaces", [[], ["unknown"], ["agents", "agents"]])
def test_measure_rejects_invalid_surface_selection_before_any_external_action(tmp_path, surfaces):
    with pytest.raises(ValueError, match="surfaces"):
        speed.measure_case(tmp_path, Path("never-run"), surfaces=surfaces)
