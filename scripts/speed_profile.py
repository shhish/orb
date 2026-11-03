#!/usr/bin/env python3
"""Implementation for speed_profile.sh; all external actions are lead-only.

Uses bounded draw-task counters instead of the serial-heavy LVGL ftrace buffer.
Archived builds and phase receipts make interruption/resumption explicit. The
historical renderer is built from clean orb/render sources with its own defaults.
"""

import argparse
import fcntl
import hashlib
import json
import os
import re
import signal
import subprocess
import time
from collections import defaultdict
from pathlib import Path

import speed_ab as ab  # also loads ~/.config/orb/dev.env

ROOT = Path(__file__).resolve().parents[1]
FAULT = re.compile(r"Guru Meditation|panic(?:'ed)?|assert failed|DMA.*underflow|Boot loop", re.I)
NATIVE = re.compile(
    r"I \((?P<uptime>\d+)\) main: (?P<phase>scroll|glass): "
    r"(?P<fps>[\d.]+) fps, frame (?P<frame_ms>[\d.]+) ms "
    r"\(draw (?P<draw_ms>[\d.]+), wait (?P<wait_ms>[\d.]+); "
    r"hold (?P<hold_ms>[\d.]+), wire (?P<wire_ms>[\d.]+)\)"
)
HOTSPOT_STAGES = (
    "ROWS_PREPARE",
    "ROWS_DRAW",
    "GLASS_PREPARE",
    "GLASS_DOWNSAMPLE",
    "GLASS_FILTER",
    "GLASS_COMPOSE",
    "DRAW_EVENTS",
)


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git(tree, *args):
    return subprocess.check_output(
        ["git", "-c", "maintenance.auto=false", *args], cwd=tree, text=True, timeout=20
    ).strip()


def source_identity(tree):
    names = git(tree, "ls-files", "--cached", "--others", "--exclude-standard", "--", "firmware")
    return {
        "commit": git(tree, "rev-parse", "HEAD"),
        "files": {name: sha(tree / name) for name in names.splitlines() if (tree / name).is_file()},
    }


def run(command, log, *, probe=None, timeout=1800, cwd=ROOT):
    """Stop the entire local subprocess group before a recovery flash can start."""
    if probe and not probe():
        raise RuntimeError("device offline before capture")
    with log.open("w") as output:
        process = subprocess.Popen(
            [str(arg) for arg in command],
            cwd=cwd,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + timeout
            while process.poll() is None:
                if probe and not probe():
                    raise RuntimeError("device offline during capture")
                if time.monotonic() > deadline:
                    raise TimeoutError(f"command timed out; see {log}")
                time.sleep(2)
            if process.returncode:
                raise RuntimeError(f"command exited {process.returncode}; see {log}")
            if probe and not probe():
                raise RuntimeError("device offline after capture")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def verify_artifacts(case):
    receipt = json.loads((case / "built.json").read_text())
    for name, digest in receipt["artifacts"].items():
        if not (case / name).is_file() or sha(case / name) != digest:
            raise RuntimeError(f"archived build changed: {case / name}; use a new --output")
    return receipt


def build(case, tree, version, *, profile, enabled=()):
    if any(name not in ab.BASE_OPTIONS for name in enabled):
        raise ValueError("unsupported Kconfig experiment")
    sources = source_identity(tree)
    if (case / "built.json").exists():
        receipt = verify_artifacts(case)
        if receipt["source"] != sources:
            raise RuntimeError(f"source changed since {case.name} build; use a new --output")
        if receipt.get("enabled", []) != sorted(enabled):
            raise RuntimeError("experiment changed since build; use a new --output")
        return receipt
    case.mkdir(parents=True, exist_ok=True)
    # A failed configure must not leave a generated SDKCONFIG overriding defaults.
    (case / "sdkconfig").unlink(missing_ok=True)
    defaults = str(tree / "firmware/sdkconfig.defaults")
    if profile:
        # DRAW_PROFILE is now an ordinary A/B option too. This runner always
        # enables it for profile cases; emit/verify one effective definition.
        profile_options = set(enabled) | {"ORB_DISPLAY_DRAW_PROFILE"}
        overlay = case / "profile.defaults"
        overlay.write_text(
            "".join(
                f"CONFIG_{key}={'y' if key in profile_options else 'n'}\n"
                for key in ab.BASE_OPTIONS
            )
            + "CONFIG_LV_USE_PROFILER=n\n"
            + "CONFIG_LV_USE_PROFILER_BUILTIN=n\n"
            + "CONFIG_APP_PROJECT_VER_FROM_CONFIG=y\n"
            + f'CONFIG_APP_PROJECT_VER="{version}"\n'
        )
        defaults += f";{overlay}"
    command = [
        tree / "scripts/idf.sh",
        "-B",
        case / "build",
        "-D",
        f"SDKCONFIG={case / 'sdkconfig'}",
        "-D",
        f"SDKCONFIG_DEFAULTS={defaults}",
        "reconfigure",
        "build",
    ]
    save(case / "build-command.json", [str(arg) for arg in command])
    (case / "source.patch").write_text(git(tree, "diff", "HEAD", "--", "firmware") + "\n")
    print(f"Building {case.name}; log: {case / 'build.log'}", flush=True)
    run(command, case / "build.log", cwd=tree)
    if source_identity(tree) != sources:
        raise RuntimeError("sources changed during build; use a new --output")
    header = (case / "build/config/sdkconfig.h").read_text()
    if profile:
        if "#define CONFIG_ORB_DISPLAY_DRAW_PROFILE 1\n" not in header:
            raise RuntimeError("profiling Kconfig was not enabled")
        for name in ab.BASE_OPTIONS:
            if (f"#define CONFIG_{name} 1\n" in header) != (name in profile_options):
                raise RuntimeError(f"effective CONFIG_{name} differs from requested experiment")
        if "#define CONFIG_LV_USE_PROFILER_BUILTIN 1\n" in header:
            raise RuntimeError("serial-heavy built-in profiler unexpectedly enabled")
    identity = ab.image_identity(case / "build/orb.bin")
    if profile and identity["fw"] != version:
        raise RuntimeError("image version differs from --bench-version")
    if sha(case / "build/orb.elf") != identity["elf_sha256"]:
        raise RuntimeError("ELF does not match app descriptor")
    artifacts = [
        "sdkconfig",
        "build/orb.bin",
        "build/orb.elf",
        "build/orb.map",
        "build/ota_data_initial.bin",
        "build/config/sdkconfig.h",
        "build/CMakeCache.txt",
        "build/compile_commands.json",
        "build/project_description.json",
    ]
    receipt = {
        "source": sources,
        "enabled": sorted(enabled),
        "identity": identity,
        "artifacts": {n: sha(case / n) for n in artifacts},
    }
    save(case / "built.json", receipt)
    return receipt


def flash(case, destination):
    receipt = verify_artifacts(case)
    destination.mkdir(parents=True, exist_ok=True)
    identity = receipt["identity"] | {"flashed_at_ms": int(time.time() * 1000)}
    print(f"Flashing {case.name} (app + OTA selector; NVS preserved)", flush=True)
    run(
        [ROOT / "scripts/remote.sh", "flash-app", case / "build"],
        destination / "flash.log",
        timeout=180,
    )
    save(destination / "flashed.json", identity)
    run(
        [ROOT / "scripts/orb-transport.sh", "--logs", "20", ""],
        destination / "boot.log",
        timeout=40,
    )
    if FAULT.search((destination / "boot.log").read_text()):
        raise RuntimeError(f"boot fault; see {destination / 'boot.log'}")
    return identity


def probe_for(hub, identity, destination):
    def probe():
        raw = subprocess.check_output([str(hub), "devices", "--json"], text=True, timeout=10)
        snapshot = json.loads(raw)
        with (destination / "devices.jsonl").open("a") as output:
            output.write(json.dumps({"observed_ms": int(time.time() * 1000), **snapshot}) + "\n")
        return ab.device_status(snapshot, identity)

    return probe


def fresh_attempt(case, name):
    index = 1
    while (case / f"{name}-{index:03d}").exists():
        index += 1
    path = case / f"{name}-{index:03d}"
    path.mkdir()
    return path


def profile_capture(case, hub, *, fixtures=False):
    attempt = fresh_attempt(case, "capture")
    try:
        identity = flash(case, attempt)
        probe = probe_for(hub, identity, attempt)
        print("Waiting for exact profiling build online for 90 continuous seconds", flush=True)
        ab.wait_settled(probe)
        save(attempt / "settled.json", identity | {"at_ms": int(time.time() * 1000)})
        commands = [
            ("heap-before.txt", ["heap"]),
            ("top-before.txt", ["top"]),
            ("power-before.txt", ["power"]),
            *((f"bench-{i}.txt", ["bench", "16000"]) for i in range(1, 4)),
        ]
        if fixtures:
            commands += [
                ("fixture-transition.txt", ["bench_transition", "16000"]),
                ("fixture-quiet.txt", ["bench_quiet", "25000"]),
            ]
        commands += [("heap-after.txt", ["heap"]), ("top-after.txt", ["top"])]
        for name, command in commands:
            print(f"Capturing {name}", flush=True)
            run([ROOT / "scripts/orb-dev.sh", *command], attempt / name, probe=probe, timeout=45)
            if command[0].startswith("bench_"):
                save(
                    attempt / name.replace(".txt", ".json"),
                    {
                        "command": " ".join(command),
                        "capture": name,
                        "completed_at_ms": int(time.time() * 1000),
                    },
                )
        ranked_report(attempt)
        enabled = verify_artifacts(case).get("enabled", [])
        ab.accounting_report(
            sorted(attempt.glob("bench-*.txt")) + sorted(attempt.glob("fixture-*.txt")),
            attempt / "accounting.json",
            required="ORB_DISPLAY_RENDER_ACCOUNTING" in enabled,
        )
        save(case / "captured.json", {"attempt": attempt.name, "fixtures": fixtures})
    except BaseException as error:
        (attempt / "INVALID").write_text(str(error) + "\n")
        raise


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def ranked_report(attempt):
    totals = defaultdict(lambda: {"draw_us": 0, "frames": 0, "rows": {}, "raster_builds": 0})
    bench = []
    scale_cache = defaultdict(lambda: defaultdict(int))
    hotspots = defaultdict(dict)
    row_atlas = defaultdict(lambda: defaultdict(int))
    fused_glass = defaultdict(lambda: defaultdict(int))
    raster_mips = defaultdict(lambda: {"builds": 0, "bytes": 0, "budget": 0})
    optional_shapes = None
    for path in sorted(attempt.glob("bench-*.txt")):
        # orb-dev's human CLI removes leading @@; archived/raw serial keeps it.
        text = re.sub(r"(?m)^ORBDEV ", "@@ORBDEV ", path.read_text())
        if "@@ORBDEV OK bench 16000" not in text or FAULT.search(text):
            raise RuntimeError(f"incomplete/faulted benchmark: {path}")
        metadata = [
            fields(line) for line in text.splitlines() if "@@ORBDEV DRAWPROFILE_META " in line
        ]
        if len(metadata) != 1 or metadata[0].get("overflow") != "0":
            raise RuntimeError(f"missing profile metadata or counter overflow: {path}")
        if metadata[0].get("clock") != "wall_us":
            raise RuntimeError("unsupported profiling clock")
        stage_count = int(metadata[0].get("stages", "0"))
        # Six-stage archived receipts predate synchronous custom-painter
        # attribution. Preserve them while accepting current DRAW_EVENTS.
        if stage_count not in (0, len(HOTSPOT_STAGES) - 1, len(HOTSPOT_STAGES)):
            raise RuntimeError("unsupported hotspot profiling stages")
        declared_stages = HOTSPOT_STAGES[:stage_count]
        seen, tasks, rasters, scales = set(), set(), set(), set()
        stage_records = set()
        atlas_phases = set()
        fused_phases = set()
        mip_phases = set()
        for line in text.splitlines():
            data = fields(line)
            if "@@ORBDEV BENCH " in line:
                bench.append(line)
            if "@@ORBDEV DRAWPROFILE " in line:
                phase = data["phase"]
                if phase in seen or phase not in ("scroll", "glass"):
                    raise RuntimeError("duplicate or unknown profile phase")
                seen.add(phase)
                if int(data["draw_us"]) <= 0 or int(data["frames"]) <= 0:
                    raise RuntimeError("empty DRAW denominator")
                totals[phase]["draw_us"] += int(data["draw_us"])
                totals[phase]["frames"] += int(data["frames"])
            elif "@@ORBDEV DRAWTASK " in line:
                phase = data["phase"]
                if phase not in seen:
                    raise RuntimeError("task before phase header")
                key = (data["type"], data["class"])
                row = totals[phase]["rows"].setdefault(key, {"calls": 0, "elapsed_us": 0})
                calls, elapsed = int(data["calls"]), int(data["elapsed_us"])
                if calls <= 0 or elapsed < 0:
                    raise RuntimeError("invalid task counters")
                row["calls"] += calls
                row["elapsed_us"] += elapsed
                tasks.add(phase)
            elif "@@ORBDEV RASTER " in line:
                if data["phase"] not in seen or data["phase"] in rasters:
                    raise RuntimeError("invalid raster phase")
                rasters.add(data["phase"])
                totals[data["phase"]]["raster_builds"] += int(data["builds"])
                mip_fields = {"mip_builds", "mip_bytes", "mip_budget"}
                if mip_fields & data.keys():
                    if not mip_fields <= data.keys():
                        raise RuntimeError("incomplete raster mip accounting")
                    mip_phases.add(data["phase"])
                    mip = {name: int(data[name]) for name in mip_fields}
                    if any(not 0 <= value <= 0xFFFFFFFF for value in mip.values()) or (
                        mip["mip_bytes"] > mip["mip_budget"]
                    ):
                        raise RuntimeError("invalid raster mip accounting")
                    target = raster_mips[data["phase"]]
                    target["builds"] += mip["mip_builds"]
                    target["bytes"] = max(target["bytes"], mip["mip_bytes"])
                    target["budget"] = max(target["budget"], mip["mip_budget"])
            elif "@@ORBDEV HOTSPOT " in line:
                phase, stage = data["phase"], data["stage"]
                key = (phase, stage)
                if (
                    not stage_count
                    or phase not in seen
                    or stage not in declared_stages
                    or key in stage_records
                ):
                    raise RuntimeError("duplicate, unknown or undeclared hotspot stage")
                stage_records.add(key)
                counters = {name: int(data[name]) for name in ("calls", "elapsed_us", "pixels")}
                if any(not 0 <= value <= 0xFFFFFFFF for value in counters.values()) or (
                    not counters["calls"] and (counters["elapsed_us"] or counters["pixels"])
                ):
                    raise RuntimeError("invalid hotspot counters")
                target = hotspots[phase].setdefault(stage, dict.fromkeys(counters, 0))
                for name, value in counters.items():
                    target[name] += value
            elif "@@ORBDEV ROWATLAS " in line:
                phase = data["phase"]
                if phase not in seen or phase in atlas_phases:
                    raise RuntimeError("duplicate or invalid row atlas phase")
                atlas_phases.add(phase)
                for name in ("builds", "fallbacks", "draws", "pixels"):
                    value = int(data[name])
                    if not 0 <= value <= 0xFFFFFFFF:
                        raise RuntimeError("invalid row atlas counter")
                    row_atlas[phase][name] += value
                gauges = {
                    name: int(data[name]) for name in ("rows", "glyphs", "bytes", "peak", "budget")
                }
                if any(not 0 <= value <= 0xFFFFFFFF for value in gauges.values()) or not (
                    gauges["bytes"] <= gauges["peak"] <= gauges["budget"]
                ):
                    raise RuntimeError("invalid row atlas memory accounting")
                for name, value in gauges.items():
                    row_atlas[phase][name] = max(row_atlas[phase][name], value)
            elif "@@ORBDEV FUSEDGLASS " in line:
                phase = data["phase"]
                if phase not in seen or phase in fused_phases:
                    raise RuntimeError("duplicate or invalid fused glass phase")
                fused_phases.add(phase)
                for name in ("prepares", "draws", "fallbacks"):
                    value = int(data[name])
                    if not 0 <= value <= 0xFFFFFFFF:
                        raise RuntimeError("invalid fused glass counter")
                    fused_glass[phase][name] += value
                gauges = {name: int(data[name]) for name in ("maps", "bytes", "peak", "budget")}
                if any(not 0 <= value <= 0xFFFFFFFF for value in gauges.values()) or not (
                    gauges["bytes"] <= gauges["peak"] <= gauges["budget"]
                ):
                    raise RuntimeError("invalid fused glass memory accounting")
                for name, value in gauges.items():
                    fused_glass[phase][name] = max(fused_glass[phase][name], value)
            elif "@@ORBDEV SCALECACHE " in line:
                phase = data["phase"]
                if phase not in ("scroll", "glass") or phase in scales:
                    raise RuntimeError("duplicate or invalid scale cache phase")
                scales.add(phase)
                for key in (
                    "hits",
                    "misses",
                    "builds",
                    "evictions",
                    "alloc_fail",
                    "blits",
                    "build_us",
                ):
                    value = int(data[key])
                    if value < 0:
                        raise RuntimeError("negative scale cache counter")
                    scale_cache[phase][key] += value
                gauges = {key: int(data[key]) for key in ("bytes", "peak", "budget")}
                if not 0 <= gauges["bytes"] <= gauges["peak"] <= gauges["budget"]:
                    raise RuntimeError("invalid scale cache memory accounting")
                for key, value in gauges.items():
                    scale_cache[phase][key] = max(scale_cache[phase][key], value)
        if seen != {"scroll", "glass"} or tasks != seen or rasters != seen:
            raise RuntimeError(f"missing phase/task counters: {path}")
        if scales and scales != seen:
            raise RuntimeError(f"incomplete scale cache phases: {path}")
        if stage_count and stage_records != {
            (phase, stage) for phase in seen for stage in declared_stages
        }:
            raise RuntimeError(f"incomplete hotspot stages: {path}")
        if atlas_phases and atlas_phases != seen:
            raise RuntimeError(f"incomplete row atlas phases: {path}")
        if fused_phases and fused_phases != seen:
            raise RuntimeError(f"incomplete fused glass phases: {path}")
        if mip_phases and mip_phases != seen:
            raise RuntimeError(f"incomplete raster mip phases: {path}")
        shape = (bool(atlas_phases), bool(fused_phases), bool(mip_phases), stage_count)
        if optional_shapes is not None and optional_shapes != shape:
            raise RuntimeError(
                "inconsistent row atlas/fused glass/raster mip/hotspot records across captures"
            )
        optional_shapes = shape
    if not bench or not totals:
        raise RuntimeError("no benchmark/profile records")
    lines = [
        "Draw-task wall time, ranked by elapsed us; NOT scheduler CPU time.",
        "%DRAW = sum of task elapsed / measured display DRAW wall time.",
        "Two workers overlap; totals can exceed 100%. Preemption is included.",
        "Raster preparation occurs before DRAW; raster builds are reported separately.",
        "Within each task row nested primitive time is inclusive; rows do not nest.",
        "Hotspot stages are separate attribution: preparation can precede DRAW.",
        "GLASS_PREPARE includes DOWNSAMPLE/FILTER; never sum hotspot stages or add them to tasks.",
        "DRAW_EVENTS includes synchronous painting, nested events and preceding task drains.",
        "",
    ]
    serializable = {}
    for phase in ("scroll", "glass"):
        values = totals[phase]
        rows = []
        lines += [
            f"{phase}: {values['frames']} frames; DRAW {values['draw_us'] / 1000:.3f} ms; "
            f"raster builds {values['raster_builds']}",
            "Draw task / widget class                  %DRAW       Calls      us/call    ms/frame",
        ]
        if values["raster_builds"]:
            lines.insert(
                len(lines) - 1,
                "WARNING: raster rebuilds occurred outside DRAW; task/DRAW ratios include that work.",
            )
        for (kind, widget), row in sorted(
            values["rows"].items(), key=lambda item: -item[1]["elapsed_us"]
        ):
            percent = 100 * row["elapsed_us"] / values["draw_us"]
            per_call = row["elapsed_us"] / row["calls"]
            per_frame = row["elapsed_us"] / values["frames"] / 1000
            rows.append(
                {
                    "type": kind,
                    "class": widget,
                    **row,
                    "percent_draw": percent,
                    "us_per_call": per_call,
                    "ms_per_frame": per_frame,
                }
            )
            lines.append(
                f"{kind + ' / ' + widget:<40} {percent:7.2f} {row['calls']:11d} {per_call:12.2f} {per_frame:11.3f}"
            )
        lines.append("")
        serializable[phase] = {**values, "rows": rows}
        if phase in raster_mips:
            mips = dict(raster_mips[phase])
            serializable[phase]["raster_mips"] = mips
            lines += [
                f"Raster material mips: {mips['builds']} builds; max payload "
                f"{mips['bytes']}/{mips['budget']} bytes (included in raster bytes).",
                "",
            ]
        if phase in hotspots:
            stages = []
            lines.append("Hotspot stage                     Calls      ms/frame     pixels/frame")
            for stage in HOTSPOT_STAGES:
                if stage not in hotspots[phase]:
                    continue
                row = hotspots[phase][stage]
                per_frame = row["elapsed_us"] / values["frames"] / 1000
                pixels_per_frame = row["pixels"] / values["frames"]
                stages.append(
                    {
                        "stage": stage,
                        **row,
                        "ms_per_frame": per_frame,
                        "pixels_per_frame": pixels_per_frame,
                    }
                )
                lines.append(
                    f"{stage:<30} {row['calls']:9d} {per_frame:13.3f} {pixels_per_frame:16.2f}"
                )
            serializable[phase]["hotspots"] = stages
            lines.append("")
        if phase in row_atlas:
            atlas = dict(row_atlas[phase])
            serializable[phase]["row_atlas"] = atlas
            lines += [
                f"Row atlas: {atlas['builds']} builds, {atlas['fallbacks']} fallbacks, "
                f"{atlas['draws']} band draws, {atlas['pixels']} pixels; "
                f"resident {atlas['rows']} rows/{atlas['glyphs']} glyphs; "
                f"max resident {atlas['bytes']}, peak {atlas['peak']}/{atlas['budget']} bytes.",
                "",
            ]
        if phase in fused_glass:
            material = dict(fused_glass[phase])
            serializable[phase]["fused_glass"] = material
            lines += [
                f"Fused glass: {material['prepares']} prepares, {material['draws']} band draws, "
                f"{material['fallbacks']} fallbacks; resident {material['maps']} maps; "
                f"max internal payload {material['bytes']}, "
                f"peak {material['peak']}/{material['budget']} bytes.",
                "Payload gauges exclude temporary source/mask/scanline scratch; check whole-scene heap minima.",
                "",
            ]
        if phase in scale_cache:
            variants = dict(scale_cache[phase])
            serializable[phase]["scale_cache"] = variants
            lines += [
                f"Scale variants: {variants['hits']} hits, {variants['misses']} misses, "
                f"{variants['builds']} builds, {variants['blits']} band blits; "
                f"pre-DRAW creation {variants['build_us'] / 1000:.3f} ms total; "
                f"peak {variants['peak']}/{variants['budget']} bytes.",
                "Creation is outside DRAW; compare whole-frame FPS and build_us/frame too.",
                "",
            ]
    lines += ["Raw benchmark summaries (profiling overhead included):", *bench]
    (attempt / "ranked.txt").write_text("\n".join(lines) + "\n")
    save(attempt / "ranked.json", serializable)


def renderer_report(text):
    if FAULT.search(text) or "ESP-ROM:" in text or "rst:0x" in text:
        raise RuntimeError("renderer reset/fault during capture")
    records, stable = [], []
    for line in text.splitlines():
        match = NATIVE.search(line)
        if not match:
            continue
        row = {
            key: (value if key == "phase" else float(value))
            for key, value in match.groupdict().items()
        }
        # Native counters reset each log; first sample and phase-crossing
        # windows cannot be attributed. Keep them in raw logs, not aggregates.
        if records and row["uptime"] <= records[-1]["uptime"]:
            raise RuntimeError("renderer reboot or duplicated log tail")
        if (
            records
            and row["phase"] == records[-1]["phase"]
            and 1500 <= row["uptime"] - records[-1]["uptime"] <= 3000
        ):
            stable.append(row)
        records.append(row)
    if len(records) < 10 or records[-1]["uptime"] - records[0]["uptime"] < 24000:
        raise RuntimeError("not enough fresh native renderer stats across the 30-second capture")
    lines = [
        "orb/render native metrics, unmodified renderer; 30-second live log capture.",
        "Native phase label is instantaneous; first/phase-transition windows excluded.",
        "Historical DRAW/frame accounting differs from integrated counters; not an equal-clock A/B.",
        "Phase   Samples   Mean FPS   Native draw ms   Native frame ms",
    ]
    for phase in ("scroll", "glass"):
        rows = [row for row in stable if row["phase"] == phase]
        if len(rows) < 2:
            raise RuntimeError(f"not enough stable {phase} windows")

        def mean(key):
            return sum(row[key] for row in rows) / len(rows)

        lines.append(
            f"{phase:<8} {len(rows):7d} {mean('fps'):10.2f} {mean('draw_ms'):16.2f} {mean('frame_ms'):17.2f}"
        )
    return {"all_samples": records, "stable_samples": stable}, "\n".join(lines) + "\n"


def renderer_capture(case):
    attempt = fresh_attempt(case, "capture")
    identity = flash(case, attempt)
    boot = (attempt / "boot.log").read_text()
    hashes = re.findall(r"ELF file SHA256:\s*([a-fA-F0-9]{8,64})", boot)
    if not hashes or not identity["elf_sha256"].startswith(hashes[-1].lower()):
        raise RuntimeError("renderer boot ELF identity missing/mismatched; see boot.log")
    # No Wi-Fi/link exists on orb/render. Do not wait for a hub identity it
    # cannot send. Twenty seconds boot capture above + 10s logs warm the demo.
    run(
        [ROOT / "scripts/orb-transport.sh", "--logs-new", "10", ""],
        attempt / "warmup.log",
        timeout=25,
    )
    if FAULT.search((attempt / "warmup.log").read_text()):
        raise RuntimeError("renderer fault during warmup")
    print("Capturing unmodified renderer's own stats for 30 seconds", flush=True)
    run(
        [ROOT / "scripts/orb-transport.sh", "--logs-new", "30", ""],
        attempt / "native-30s.log",
        timeout=50,
    )
    data, table = renderer_report((attempt / "native-30s.log").read_text())
    save(attempt / "native-stats.json", data)
    (attempt / "native-stats.txt").write_text(table)
    save(case / "captured.json", {"attempt": attempt.name})


def restore(case, hub):
    attempt = fresh_attempt(case, "restore")
    identity = flash(case, attempt)
    print("Verifying restored profiling image and pairing for 90 seconds", flush=True)
    ab.wait_settled(probe_for(hub, identity, attempt))
    save(attempt / "RESTORED.json", identity | {"at_ms": int(time.time() * 1000)})


def orchestrate(output, renderer, version, hub, *, enabled=(), profile_only=False, fixtures=False):
    profile, old = output / "profile", output / "render"
    state_path = output / "state.json"
    state = json.loads(state_path.read_text()) if state_path.exists() else {}
    request = {"bench_version": version, "renderer": str(renderer)}
    # Preserve the original no-option resume receipts, including lead's pending
    # historical capture. A/B candidates must use a fresh output directory.
    if enabled or profile_only:
        request.update(enabled=sorted(enabled), profile_only=profile_only)
    if fixtures:
        request["fixtures"] = True
    if state.get("request", request) != request:
        raise RuntimeError("resume options differ; use the original command or a new --output")
    state["request"] = request
    save(state_path, state)
    if state.get("restore_pending"):
        restore(profile, hub)
        state["restore_pending"] = False
        save(state_path, state)
    if state.get("complete"):
        print(f"Already complete: {output / 'RESULTS.txt'}")
        return
    if git(ROOT, "branch", "--show-current") not in ("orb/w2-speed", "orb/w2-render"):
        raise RuntimeError("run from orb/w2-speed or orb/w2-render")
    if not profile_only and (
        git(renderer, "branch", "--show-current") != "orb/render"
        or git(renderer, "status", "--porcelain")
    ):
        raise RuntimeError("../render must be a clean orb/render worktree; never auto-clean it")
    build(profile, ROOT, version, profile=True, enabled=enabled)
    if not (profile / "captured.json").exists():
        if fixtures:
            profile_capture(profile, hub, fixtures=True)
        else:
            profile_capture(profile, hub)
    if profile_only:
        profile_result = profile / json.loads((profile / "captured.json").read_text())["attempt"]
        (output / "RESULTS.txt").write_text(
            (profile_result / "ranked.txt").read_text()
            + "\nCandidate remains installed; exact build stayed online during capture.\n"
        )
        state["complete"] = True
        save(state_path, state)
        print(f"Complete: {output / 'RESULTS.txt'}", flush=True)
        return
    build(old, renderer, version, profile=False)
    if git(renderer, "status", "--porcelain"):
        raise RuntimeError(
            "renderer build changed tracked/untracked source; inspect it before continuing"
        )
    if not (old / "captured.json").exists():
        # Persist before the first old flash: even SIGKILL/power failure leaves
        # an unambiguous recovery action for the next invocation.
        state["restore_pending"] = True
        save(state_path, state)
        try:
            renderer_capture(old)
        finally:
            restore(profile, hub)
            state["restore_pending"] = False
            save(state_path, state)
    profile_result = profile / json.loads((profile / "captured.json").read_text())["attempt"]
    old_result = old / json.loads((old / "captured.json").read_text())["attempt"]
    (output / "RESULTS.txt").write_text(
        (profile_result / "ranked.txt").read_text()
        + "\n"
        + (old_result / "native-stats.txt").read_text()
        + "\nProfiling image restored; exact build online and no fresh OTA for 90 seconds.\n"
    )
    state["complete"] = True
    save(state_path, state)
    print(f"Complete: {output / 'RESULTS.txt'}", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bench-version",
        required=True,
        help="current live dev-feed version; never publish these reused-version images",
    )
    parser.add_argument("--output", type=Path, default=ROOT / "work/w2-speed-r3")
    parser.add_argument("--render-worktree", type=Path, default=ROOT.parent / "render")
    parser.add_argument("--hub", type=Path, default=Path.home() / ".venvs/orb-hub/bin/orb-hub")
    parser.add_argument(
        "--enable",
        action="append",
        default=[],
        choices=ab.BASE_OPTIONS,
        help="enable one experiment (without CONFIG_); repeat for combined candidates",
    )
    parser.add_argument(
        "--profile-only",
        action="store_true",
        help="profile this candidate without flashing the historical renderer",
    )
    parser.add_argument(
        "--fixtures", action="store_true", help="also capture transition and 25s quiet fixtures"
    )
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[A-Za-z0-9.+_-]{1,31}", args.bench_version):
        parser.error("bench-version must fit 31 ASCII bytes")
    key = os.environ.get("ORB_SIGNING_KEY")
    if not key or not Path(key).is_file():
        parser.error(
            "set ORB_SIGNING_KEY to the existing trusted development key file (no Keychain access)"
        )
    if (
        os.environ.get("ORB_DEV_LOCAL", "0") != "0"
        or os.environ.get("ESP_REMOTE_PORT", "/dev/cu.usbmodem1101") != "/dev/cu.usbmodem1101"
    ):
        parser.error("round 3 uses only the development board's standard remote transport")
    if (
        os.environ.get("ESP_REMOTE_HOST", "my-mac.local")
        != os.environ.get("ORB_DEV_REMOTE_HOST", "my-mac.local")
        or os.environ.get("ORB_LOGD_SOCKET", "~/.local/share/orb-logd/orb.sock")
        != "~/.local/share/orb-logd/orb.sock"
    ):
        parser.error("round 3 requires the development board's default host and logd socket")
    os.umask(0o077)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Also serialize different output directories in this worktree. Transport
    # leases still arbitrate each serial operation with the existing log daemon.
    (ROOT / "work").mkdir(exist_ok=True)
    with (ROOT / "work/.speed-profile.lock").open("w") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error("another speed_profile run owns this worktree")
        try:
            if args.enable or args.profile_only or args.fixtures:
                orchestrate(
                    output,
                    args.render_worktree.resolve(),
                    args.bench_version,
                    args.hub,
                    enabled=tuple(sorted(set(args.enable))),
                    profile_only=args.profile_only,
                    fixtures=args.fixtures,
                )
            else:
                orchestrate(output, args.render_worktree.resolve(), args.bench_version, args.hub)
        except (Exception, KeyboardInterrupt) as error:
            (output / "LAST_ERROR.txt").write_text(str(error) + "\n")
            print(
                f"Stopped: {error}\nRepeat the same command to resume. Check state.json restore_pending first.",
                flush=True,
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
