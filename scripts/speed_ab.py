#!/usr/bin/env python3
"""Lead-only speed experiments; never changes the hub feed or device settings.

Build uses a fresh SDKCONFIG per case and the normal firmware/build output.
Flash uses the existing signed, leased transport. Measurement requires the exact
ELF build online for 90 seconds, with no OTA since that flash, and monitors the
same conditions while each benchmark runs. Tests replace every external action.
"""

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "firmware/build"


def _load_dev_env() -> None:
    """Personal dev settings live outside the repo in ~/.config/orb/dev.env
    (KEY=VALUE lines). Variables already set in the environment win."""
    path = Path(os.environ.get("ORB_DEV_ENV", Path.home() / ".config/orb/dev.env"))
    if path.is_file():
        for line in path.read_text().splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip().isidentifier() and not line.lstrip().startswith("#"):
                os.environ.setdefault(key.strip(), value.strip())


_load_dev_env()

DEVICE = os.environ.get("ORB_DEV_DEVICE_ID", "orb-020000abcdef")
SETTLE_SECONDS = 90
POLL_SECONDS = 2
SCROLL_SURFACES = ("agents", "rooms", "settings", "notifications", "favourites")
BASE_OPTIONS = (
    "ORB_RENDER_FUSED_GLASS",
    "ORB_RENDER_ROWS_AT_SIZE",
    "ORB_SMALL_ALLOCATIONS_PSRAM",
    "ORB_NETWORK_BSS_PSRAM",
    "ORB_DISPLAY_TALL_BANDS",
    "ORB_DISPLAY_INTERNAL_DOUBLE_BUFFER",
    "ORB_DISPLAY_PSRAM_PIPELINE",
    "ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH",
    "ORB_DISPLAY_INTERNAL_BLUR_SCRATCH",
    "ORB_DISPLAY_INTERNAL_DRAW_MASKS",
    "ORB_DISPLAY_GLASS_SINGLE_PASS",
    "ORB_DISPLAY_GLASS_MOTION_QUALITY",
    "ORB_DISPLAY_RASTER_SCALE_CACHE",
    "ORB_DISPLAY_RGB565_SCALE_FAST",
    "ORB_DISPLAY_RENDER_ACCOUNTING",
    "ORB_DISPLAY_DRAW_PROFILE",
    "ORB_DISPLAY_SCROLL_TRACE",
    "ORB_UI_SCROLL_EDGE_SOFT",
    "ORB_DISPLAY_RETAINED_MATERIAL",
    "ORB_DISPLAY_RETAINED_SETTLED_ONLY",
    "ORB_DISPLAY_GLASS_SOLID_FAST",
    "ORB_DISPLAY_GLASS_SPARSE_MASKS",
    "ORB_DISPLAY_TE_PACING",
)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def image_identity(path):
    image = path.read_bytes()
    if len(image) < 288 or image[0] != 0xE9 or struct.unpack_from("<H", image, 12)[0] != 9:
        raise ValueError("expected an ESP32-S3 app image")
    if struct.unpack_from("<I", image, 32)[0] != 0xABCD5432:
        raise ValueError("missing app descriptor")
    version = image[48:80].split(b"\0", 1)[0].decode()
    elf_hash = image[176:208].hex()
    if not version or not any(image[176:208]):
        raise ValueError("image lacks version or ELF SHA-256")
    return {"fw": version, "build": elf_hash[:7], "elf_sha256": elf_hash}


def run_log(args, destination, *, timeout=1800):
    with destination.open("w") as output:
        subprocess.run(
            args, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT, check=True, timeout=timeout
        )


def config_difference(reference, candidate):
    def definitions(text):
        return dict(re.findall(r"^#define (CONFIG_\w+) (.*)$", text, re.MULTILINE))

    before, after = definitions(reference), definitions(candidate)
    return {
        key: [before.get(key), after.get(key)]
        for key in sorted(before.keys() | after.keys())
        if before.get(key) != after.get(key)
    }


def accounting_report(paths, destination, *, required=False):
    """Archive bounded deferred counters without turning task sums into latency."""
    captures = []
    expected_meta = {
        "clock": "wall_us",
        "interval_bucket_us": "1000",
        "buckets": "128",
        "last_unbounded": "1",
        "trace_capacity": "8",
        "overflow": "0",
        "active": "1",
    }
    for path in paths:
        if not path.exists():
            if required:
                raise RuntimeError(f"missing accounting capture: {path}")
            continue
        text = re.sub(r"(?m)^ORBDEV ", "@@ORBDEV ", path.read_text())
        if re.search(
            r"Guru Meditation|panic(?:'ed)?|assert failed|DMA.*underflow|Boot loop", text, re.I
        ):
            raise RuntimeError(f"faulted accounting capture: {path}")
        rows = {
            name: []
            for name in (
                "ACCOUNT_META",
                "ACCOUNT",
                "CADENCE",
                "PACING",
                "MATERIAL",
                "BAND",
                "BENCH",
            )
        }
        for line in text.splitlines():
            match = re.search(
                r"@@ORBDEV (ACCOUNT_META|ACCOUNT|CADENCE|PACING|MATERIAL|BAND|BENCH) ", line
            )
            if match:
                data = dict(re.findall(r"(\w+)=([^\s]+)", line))
                if match[1] == "BENCH":
                    data["phase"] = line[match.end() :].split()[0]
                rows[match[1]].append(data)
        if not rows["ACCOUNT_META"]:
            if required:
                raise RuntimeError(f"missing accounting metadata: {path}")
            continue
        if len(rows["ACCOUNT_META"]) != 1 or any(
            rows["ACCOUNT_META"][0].get(key) != value for key, value in expected_meta.items()
        ):
            raise RuntimeError(f"unsupported accounting metadata or counter overflow: {path}")
        scroll = re.fullmatch(
            r"scroll-(agents|rooms|settings|notifications|favourites)-\d+", path.stem
        )
        command = (
            f"bench_scroll {scroll[1]} 16000"
            if scroll
            else "bench_quiet 25000"
            if "quiet" in path.stem
            else "bench_transition 16000"
            if "transition" in path.stem
            else "bench 16000"
        )
        if f"@@ORBDEV OK {command}" not in text:
            raise RuntimeError(f"incomplete accounting capture: {path}")
        expected_phases = (
            {scroll[1]}
            if scroll
            else {"quiet"}
            if "quiet" in path.stem
            else ({"transition"} if "transition" in path.stem else {"scroll", "glass"})
        )
        phases = [row.get("phase") for row in rows["ACCOUNT"]]
        if set(phases) != expected_phases or len(phases) != len(expected_phases):
            raise RuntimeError(f"missing or duplicate accounting phases: {path}")
        converted = {}
        for kind, records in rows.items():
            converted[kind.lower()] = [
                {
                    key: int(value) if re.fullmatch(r"-?\d+", value) else value
                    for key, value in row.items()
                }
                for row in records
            ]
        for row in converted["account"]:
            for key in (
                "scheduled",
                "completed",
                "dropped",
                "preparation_us",
                "scene_prepare_us",
                "whole_update_us",
                "source_builds",
                "mip_builds",
                "largest_internal",
                "largest_dma",
                "largest_psram",
                "scratch_peak",
            ):
                if not isinstance(row.get(key), int) or row[key] < 0:
                    raise RuntimeError(f"invalid accounting field {key}: {path}")
            if "mip_bytes" in row or "mip_budget" in row:
                if (
                    any(
                        not isinstance(row.get(key), int) or not 0 <= row[key] <= 0xFFFFFFFF
                        for key in ("mip_bytes", "mip_budget")
                    )
                    or row["mip_bytes"] > row["mip_budget"]
                ):
                    raise RuntimeError(f"invalid accounting mip memory: {path}")
            if row["completed"] + row["dropped"] > row["scheduled"]:
                raise RuntimeError(f"invalid presentation counts: {path}")
        for row in converted["account_meta"]:
            if "cold_mip_builds" in row and (
                not isinstance(row["cold_mip_builds"], int)
                or not 0 <= row["cold_mip_builds"] <= 0xFFFFFFFF
            ):
                raise RuntimeError(f"invalid accounting cold mip builds: {path}")
        for row in converted["cadence"]:
            if row.get("phase") not in expected_phases or not 0 <= row.get("bucket", -1) < 128:
                raise RuntimeError(f"invalid cadence bucket: {path}")
        for row in converted["pacing"]:
            if row.get("phase") not in expected_phases:
                raise RuntimeError(f"invalid pacing record phase: {path}")
            for key in (
                "scheduled",
                "on_slot",
                "late",
                "skipped",
                "boundaries",
                "unpaced",
                "admitted_30",
                "period_us",
                "divisor",
                "max_error_us",
            ):
                if not isinstance(row.get(key), int) or row[key] < 0:
                    raise RuntimeError(f"invalid pacing field {key}: {path}")
            if row["divisor"] not in (0, 1, 2):
                raise RuntimeError(f"invalid pacing divisor: {path}")
        for row in converted["material"] + converted["band"]:
            if row.get("phase") not in expected_phases:
                raise RuntimeError(f"invalid accounting record phase: {path}")
        for row in converted["material"]:
            for key in ("solid_fills", "moving_bypasses"):
                if key in row and (not isinstance(row[key], int) or row[key] < 0):
                    raise RuntimeError(f"invalid material field {key}: {path}")
        captures.append({"capture": path.name, "command": command, **converted})
    write_json(destination, {"enabled": bool(captures), "captures": captures})


def open_report(paths, destination):
    """Keep causal DMA latency separate from motion's model-space endpoint."""
    captures = []
    numeric = (
        "cold",
        "sequence",
        "causal",
        "first_changed_band_us",
        "first_changed_frame_us",
        "transition_end_us",
        "motion_end_us",
        "motion_duration_us",
        "panel_period_us",
        "source_prepare_us",
        "object_us",
        "max_slice_us",
        "build_slices",
        "font_loads",
        "font_failures",
        "font_us",
        "raster_builds",
        "raster_us",
        "scale_raster_us",
        "raster_timing",
        "material_base_builds",
        "glass_mask_builds",
        "glass_mask_failures",
        "glass_mask_us",
        "glass_us",
        "glass_timing",
        "material_counters",
        "blur_kernels",
        "accounting_timing",
        "preparation_us",
        "scene_prepare_us",
        "whole_update_us",
        "draw_us",
        "wire_us",
        "bytes",
        "frames",
        "errors",
        "failed_frames",
        "minimum_internal",
        "cache_bytes",
        "cache_entries",
    )
    for path in paths:
        text = re.sub(r"(?m)^ORBDEV ", "@@ORBDEV ", path.read_text())
        lines = re.findall(r"@@ORBDEV OPEN ([^\r\n]+)", text)
        if len(lines) != 1 or re.search(
            r"@@ORBDEV ERROR|Guru Meditation|panic(?:'ed)?|assert failed|DMA.*underflow|Boot loop",
            text,
            re.I,
        ):
            raise RuntimeError(f"missing or faulted open receipt: {path}")
        row = dict(re.findall(r"(\w+)=([^\s]+)", lines[0]))
        surface = row.get("surface")
        if surface not in ("apps", "play", "dot", "agents", "back") or (
            f"@@ORBDEV OK bench_open {surface}" not in text
        ):
            raise RuntimeError(f"incomplete open capture: {path}")
        for key in numeric:
            if not re.fullmatch(r"\d+", row.get(key, "")):
                raise RuntimeError(f"invalid open field {key}: {path}")
            row[key] = int(row[key])
        if (
            row.get("origin") != "injected_navigation"
            or row["causal"] != 1
            or row["cold"] not in (0, 1)
            or not row["sequence"]
            or not row["frames"]
            or not 0
            < row["first_changed_band_us"]
            <= row["first_changed_frame_us"]
            <= row["transition_end_us"]
            or row["errors"]
            or row["failed_frames"]
            or row["motion_duration_us"] not in (150000, 550000)
            or row["motion_end_us"] > row["transition_end_us"]
        ):
            raise RuntimeError(f"non-causal, failed or unordered open capture: {path}")
        for availability, fields in (
            ("raster_timing", ("raster_us", "scale_raster_us")),
            ("glass_timing", ("glass_us",)),
            ("material_counters", ("material_base_builds", "blur_kernels")),
            ("accounting_timing", ("preparation_us", "scene_prepare_us", "whole_update_us")),
        ):
            if row[availability] not in (0, 1) or (
                not row[availability] and any(row[field] for field in fields)
            ):
                raise RuntimeError(f"unavailable timing or counters claimed as measured: {path}")
        row["transition_target_us"] = (
            row["motion_duration_us"] + row["panel_period_us"] if row["panel_period_us"] else None
        )
        row["transition_pass"] = (
            row["transition_end_us"] <= row["transition_target_us"]
            if row["transition_target_us"] is not None
            else None
        )
        captures.append({"capture": path.name, **row})
    groups = []
    for surface in ("apps", "play", "dot", "agents", "back"):
        for cold in (1, 0):
            samples = [row for row in captures if row["surface"] == surface and row["cold"] == cold]
            if not samples:
                continue
            group = {"surface": surface, "cold": bool(cold), "samples": len(samples)}
            for field in ("first_changed_band_us", "first_changed_frame_us", "transition_end_us"):
                values = sorted(row[field] for row in samples)
                group[f"{field}_p95"] = values[math.ceil(len(values) * 0.95) - 1]
            group["first_frame_target_us"] = 35000 if cold else 20000
            group["first_frame_pass"] = (
                group["first_changed_frame_us_p95"] <= group["first_frame_target_us"]
            )
            group["internal_floor_pass"] = all(
                row["minimum_internal"] >= 15 * 1024 for row in samples
            )
            group["transition_pass"] = (
                all(row["transition_pass"] for row in samples)
                if all(row["transition_pass"] is not None for row in samples)
                else None
            )
            groups.append(group)
    write_json(
        destination, {"origin": "injected_navigation", "captures": captures, "groups": groups}
    )


def surface_gates(text, row, path, *, required):
    """Check firmware assertions against integer observations, retaining failures."""
    lines = re.findall(r"@@ORBDEV SCROLL_GATE ([^\r\n]+)", text)
    if not lines and not required:
        return None
    if len(lines) != 1:
        raise RuntimeError(f"missing or duplicate surface gates: {path}")
    gate = dict(re.findall(r"(\w+)=([^\s]+)", lines[0]))
    fields = (
        "version",
        "target_fps",
        "fps_pass",
        "stall_limit_us",
        "handler_stalls",
        "animation_stalls",
        "stall_pass",
        "viewport_pixels",
        "invalidation_limit_permille",
        "damage_frames",
        "damage_pixels",
        "max_damage_pixels",
        "request_invalidation_pass",
        "damage_pass",
        "invalidation_pass",
        "source_builds",
        "mip_builds",
        "row_builds",
        "row_evictions",
    )
    for key in fields:
        if not re.fullmatch(r"\d+", gate.get(key, "")):
            raise RuntimeError(f"invalid surface gate {key}: {path}")
        gate[key] = int(gate[key])
    for key in (
        "loops",
        "handler_max_us",
        "anim_ticks",
        "anim_gap_max_us",
        "invalidated_pixels",
        "refreshes",
    ):
        if not re.fullmatch(r"\d+", str(row.get(key, ""))):
            raise RuntimeError(f"missing surface gate observation {key}: {path}")
        row[key] = int(row[key])
    for key in ("damage_ratio", "request_pixels_per_frame"):
        try:
            gate[key] = float(gate.get(key, ""))
        except ValueError as error:
            raise RuntimeError(f"invalid surface gate {key}: {path}") from error
        if not math.isfinite(gate[key]) or gate[key] < 0:
            raise RuntimeError(f"invalid surface gate {key}: {path}")
    if (
        gate.get("surface") != row["surface"]
        or gate["version"] != 1
        or gate["target_fps"] != row["target_fps"]
        or gate["stall_limit_us"] != 33000
        or gate["invalidation_limit_permille"] != 1100
        or not 0 < gate["viewport_pixels"] <= 466 * 466
        or gate["damage_frames"] > row["refreshes"]
        or gate["damage_pixels"] < gate["max_damage_pixels"]
        or gate["damage_pixels"] > gate["damage_frames"] * gate["max_damage_pixels"]
        or any(
            gate[key] not in (0, 1)
            for key in (
                "fps_pass",
                "stall_pass",
                "request_invalidation_pass",
                "damage_pass",
                "invalidation_pass",
            )
        )
    ):
        raise RuntimeError(f"inconsistent surface gates: {path}")
    expected = {
        "fps_pass": bool(row["moving_us"])
        and row["moving_frames"] * 1000000 >= row["moving_us"] * row["target_fps"],
        "stall_pass": bool(row["loops"])
        and row["handler_max_us"] <= 33000
        and row["anim_gap_max_us"] <= 33000
        and not gate["handler_stalls"]
        and not gate["animation_stalls"],
        "request_invalidation_pass": row["invalidated_pixels"] * 1000
        <= row["frames"] * gate["viewport_pixels"] * 1100,
        "damage_pass": bool(gate["damage_frames"])
        and gate["max_damage_pixels"] * 1000 <= gate["viewport_pixels"] * 1100,
    }
    expected["invalidation_pass"] = (
        expected["request_invalidation_pass"] and expected["damage_pass"]
    )
    if (
        any(gate[key] != value for key, value in expected.items())
        or not math.isclose(
            gate["damage_ratio"],
            gate["max_damage_pixels"] / gate["viewport_pixels"],
            abs_tol=0.00000051,
        )
        or not math.isclose(
            gate["request_pixels_per_frame"],
            row["invalidated_pixels"] / row["frames"],
            abs_tol=0.00051,
        )
    ):
        raise RuntimeError(f"surface gate assertion disagrees with observations: {path}")
    return gate


def surface_report(paths, destination, *, require_gates=False):
    """Archive real surface work; page commits never masquerade as animation FPS.

    Blur task times include worker preemption and overlap. Glass preparation and
    compose are separate profile stages; none may be summed into frame latency.
    """
    captures = []
    integer_fields = (
        "frames",
        "moving_us",
        "moving_frames",
        "idle_frames",
        "gestures",
        "accounting",
        "cpu_available",
        "moving_cpu_available",
        "glass_timing",
        "dirty_pixels",
        "minimum_internal",
        "errors",
        "failed_frames",
    )
    float_fields = (
        "seconds",
        "fps",
        "overall_fps",
        "draw_ms",
        "wire_ms",
        "whole_update_ms",
        "cpu0_pct",
        "cpu1_pct",
        "moving_cpu0_pct",
        "moving_cpu1_pct",
        "blur_task_ms",
        "glass_prepare_ms",
        "glass_compose_ms",
        "dirty_pct",
    )
    for path in paths:
        text = re.sub(r"(?m)^ORBDEV ", "@@ORBDEV ", path.read_text())
        lines = re.findall(r"@@ORBDEV SCROLL ([^\r\n]+)", text)
        if len(lines) != 1 or re.search(
            r"@@ORBDEV ERROR|Guru Meditation|panic(?:'ed)?|assert failed|DMA.*underflow|Boot loop",
            text,
            re.I,
        ):
            raise RuntimeError(f"missing or faulted surface receipt: {path}")
        row = dict(re.findall(r"(\w+)=([^\s]+)", lines[0]))
        surface = row.get("surface")
        if (
            surface not in SCROLL_SURFACES
            or f"@@ORBDEV OK bench_scroll {surface} 16000" not in text
        ):
            raise RuntimeError(f"incomplete surface capture: {path}")
        for key in integer_fields:
            if not re.fullmatch(r"\d+", row.get(key, "")):
                raise RuntimeError(f"invalid surface field {key}: {path}")
            row[key] = int(row[key])
        for key in float_fields:
            try:
                row[key] = float(row.get(key, ""))
            except ValueError as error:
                raise RuntimeError(f"invalid surface field {key}: {path}") from error
            if not math.isfinite(row[key]) or row[key] < 0:
                raise RuntimeError(f"invalid surface field {key}: {path}")
        for available, fields in (
            ("accounting", ("whole_update_ms",)),
            ("cpu_available", ("cpu0_pct", "cpu1_pct")),
            ("moving_cpu_available", ("moving_cpu0_pct", "moving_cpu1_pct")),
            ("glass_timing", ("blur_task_ms", "glass_prepare_ms", "glass_compose_ms")),
        ):
            if row[available] not in (0, 1) or (
                not row[available] and any(row[key] for key in fields)
            ):
                raise RuntimeError(f"unavailable surface metrics claimed as measured: {path}")
        if (
            row["seconds"] <= 0
            or not row["frames"]
            or not row["gestures"]
            or row["moving_frames"] > row["frames"]
            or row["errors"]
            or row["failed_frames"]
            or any(
                row[key] > 100
                for key in ("cpu0_pct", "cpu1_pct", "moving_cpu0_pct", "moving_cpu1_pct")
            )
            or row.get("workload")
            not in (
                ("page_swipe", "spring_page_swipe")
                if surface == "favourites"
                else ("fling_settle", "touch_fling_settle")
            )
            or (row.get("workload") == "page_swipe" and row["fps"] != 0)
        ):
            raise RuntimeError(f"failed or inconsistent surface receipt: {path}")
        continuous = row["workload"] != "page_swipe"
        modern = row["workload"] in ("touch_fling_settle", "spring_page_swipe")
        row["target_fps"] = (
            50 if modern and surface in ("agents", "settings", "notifications") else 30
        )
        row["scroll_fps_pass"] = row["fps"] >= row["target_fps"] if continuous else None
        gate = surface_gates(text, row, path, required=require_gates)
        row["attribution_gates_available"] = gate is not None
        row["gates"] = gate
        row["stall_pass"] = bool(gate["stall_pass"]) if gate else None
        row["invalidation_pass"] = bool(gate["invalidation_pass"]) if gate else None
        row["attribution_pass"] = (
            all(gate[key] for key in ("fps_pass", "stall_pass", "invalidation_pass"))
            if gate
            else None
        )
        if gate:
            row["scroll_fps_pass"] = bool(gate["fps_pass"])
        row["idle_pass"] = row["idle_frames"] == 0
        row["internal_floor_pass"] = row["minimum_internal"] >= 12 * 1024
        captures.append({"capture": path.name, **row})
    write_json(destination, {"origin": "private_production_surface", "captures": captures})


def build_case(directory, version, enabled, compare_config=None, *, open_control=False):
    # Existing generated configs take precedence over defaults. Refuse reuse so
    # an old option can never silently survive a new A/B command.
    directory.mkdir(parents=True, exist_ok=False)
    if not re.fullmatch(r"[A-Za-z0-9.+_-]{1,31}", version):
        raise ValueError("bench version must fit the 31-byte app descriptor")
    options = dict.fromkeys(BASE_OPTIONS, "n")
    for name in enabled:
        if name not in BASE_OPTIONS:
            raise ValueError("--enable takes a supported ORB_ Kconfig experiment")
        options[name] = "y"
    # The product improvement defaults on. Its instrumented control is an
    # explicit A/B choice, independent of default-off renderer experiments.
    options["ORB_OPEN_LATENCY_FIXES"] = "n" if open_control else "y"
    # Reuse only the current feed version for this USB-only experiment. OTA's
    # version comparison then leaves the candidate alone; ELF SHA identifies it.
    defaults = directory / "experiment.defaults"
    defaults.write_text(
        "".join(f"CONFIG_{key}={value}\n" for key, value in options.items())
        + "CONFIG_APP_PROJECT_VER_FROM_CONFIG=y\n"
        + f'CONFIG_APP_PROJECT_VER="{version}"\n'
    )
    command = [
        str(ROOT / "scripts/idf.sh"),
        "-B",
        "build",
        "-D",
        f"SDKCONFIG={directory / 'sdkconfig'}",
        "-D",
        f"SDKCONFIG_DEFAULTS={ROOT / 'firmware/sdkconfig.defaults'};{defaults}",
        "reconfigure",
        "build",
    ]
    write_json(directory / "build-command.json", command)
    run_log(command, directory / "build.log")
    header = (BUILD / "config/sdkconfig.h").read_text()
    for name, value in options.items():
        present = re.search(rf"^#define CONFIG_{name} 1$", header, re.MULTILINE) is not None
        if present != (value == "y"):
            raise RuntimeError(f"effective CONFIG_{name} differs from requested {value}")
    if compare_config:
        differences = config_difference(compare_config.read_text(), header)
        write_json(directory / "config-difference.json", differences)
        allowed = {f"CONFIG_{name}" for name in BASE_OPTIONS} | {
            "CONFIG_APP_PROJECT_VER_FROM_CONFIG",
            "CONFIG_APP_PROJECT_VER",
            "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY",
            "CONFIG_ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY",
            "CONFIG_ORB_OPEN_LATENCY_FIXES",
        }
        # An enabled option may bring its own dependent settings (e.g. a cache size).
        enabled = [f"CONFIG_{name}_" for name, value in options.items() if value == "y"]
        allowed |= {key for key in differences if key.startswith(tuple(enabled))}
        if unexpected := differences.keys() - allowed:
            raise RuntimeError(
                f"unexpected effective config changes: {', '.join(sorted(unexpected))}"
            )
    identity = image_identity(BUILD / "orb.bin")
    if identity["fw"] != version:
        raise RuntimeError("effective app version differs from the bench version")
    for source, name in (
        (BUILD / "config/sdkconfig.h", "sdkconfig.h"),
        (BUILD / "orb.map", "orb.map"),
        (BUILD / "orb.bin", "orb.bin"),
        (BUILD / "CMakeCache.txt", "CMakeCache.txt"),
        (BUILD / "compile_commands.json", "compile_commands.json"),
        (BUILD / "project_description.json", "project_description.json"),
    ):
        shutil.copyfile(source, directory / name)
    run_log(
        ["git", "-c", "maintenance.auto=false", "diff", "HEAD", "--", "firmware"],
        directory / "firmware.patch",
    )
    source_files = subprocess.check_output(
        [
            "git",
            "-c",
            "maintenance.auto=false",
            "ls-files",
            "-z",
            "--cached",
            "--others",
            "--exclude-standard",
            "--",
            "firmware",
        ],
        cwd=ROOT,
    ).split(b"\0")
    sources = {
        name.decode(): hashlib.sha256((ROOT / name.decode()).read_bytes()).hexdigest()
        for name in source_files
        if name and (ROOT / name.decode()).is_file()
    }
    write_json(directory / "source-sha256.json", sources)
    identity["source_commit"] = subprocess.check_output(
        ["git", "-c", "maintenance.auto=false", "rev-parse", "HEAD"], cwd=ROOT, text=True
    ).strip()
    identity["options"] = options
    identity["bench_only_version"] = True
    write_json(directory / "identity.json", identity)


def flash_case(directory):
    identity = json.loads((directory / "identity.json").read_text())
    if image_identity(BUILD / "orb.bin")["elf_sha256"] != identity["elf_sha256"]:
        raise RuntimeError(
            "firmware/build no longer contains this case; rebuild in a new directory"
        )
    if (BUILD / "config/sdkconfig.h").read_bytes() != (directory / "sdkconfig.h").read_bytes():
        raise RuntimeError("firmware/build configuration differs from this case")
    identity.pop("flashed_at_ms", None)
    write_json(directory / "identity.json", identity)
    started_ms = int(time.time() * 1000)
    run_log([str(ROOT / "scripts/remote.sh"), "flash"], directory / "flash.log")
    # Sign-app can add a signature sector; preserve exactly what remote.sh sent.
    shutil.copyfile(BUILD / "orb.bin", directory / "orb.bin")
    identity["flashed_at_ms"] = started_ms
    identity["signed_image_sha256"] = hashlib.sha256((BUILD / "orb.bin").read_bytes()).hexdigest()
    write_json(directory / "identity.json", identity)
    run_log(
        [
            str(ROOT / "scripts/orb-transport.sh"),
            "--logs",
            "20",
            "display:|main:|net:|link:|ota:|panic|underflow",
        ],
        directory / "boot.log",
        timeout=40,
    )


def device_status(snapshot, identity):
    devices = [d for d in snapshot.get("devices", []) if d.get("device_id") == DEVICE]
    if len(devices) != 1:
        raise RuntimeError("development device absent or duplicated in hub snapshot")
    device = devices[0]
    report = device.get("ota", {}).get("report") or {}
    # Hub retains reports from previous boots. Only reports since this flash
    # describe this experiment; a new ready state also invalidates measurement.
    if report.get("at", 0) >= identity["flashed_at_ms"] and report.get("state"):
        raise RuntimeError("OTA active since flash; candidate is contaminated or being replaced")
    if not device.get("online"):
        return False
    if any(device.get(key) != identity[key] for key in ("fw", "build")):
        raise RuntimeError("online firmware/build differs from flashed candidate")
    return True


def wait_settled(probe, *, clock=time.monotonic, sleep=time.sleep, timeout=300):
    start = clock()
    online_since = None
    while clock() - start < timeout:
        online = probe()
        now = clock()
        if online:
            if online_since is None:
                online_since = now
            if now - online_since >= SETTLE_SECONDS:
                return
        else:
            online_since = None
        sleep(POLL_SECONDS)
    raise TimeoutError("device did not remain online for 90 seconds")


def guarded_command(command, destination, probe, *, timeout=75):
    if not probe():
        raise RuntimeError("device went offline before measurement")
    with destination.open("w") as output:
        process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + timeout
        try:
            while process.poll() is None:
                if not probe():
                    raise RuntimeError("device went offline during measurement")
                if time.monotonic() >= deadline:
                    raise TimeoutError("measurement command timed out")
                time.sleep(POLL_SECONDS)
            if process.returncode:
                raise subprocess.CalledProcessError(process.returncode, command)
            if not probe():
                raise RuntimeError("device went offline after measurement")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def measure_case(directory, hub, *, fixtures=False, opens=False, open_runs=3, surfaces=None):
    if not 2 <= open_runs <= 20:
        raise ValueError("open runs must be 2..20 (first visit plus warm repeats)")
    if surfaces is not None and (
        not surfaces or set(surfaces) - set(SCROLL_SURFACES) or len(set(surfaces)) != len(surfaces)
    ):
        raise ValueError("surfaces must be unique supported production surfaces")
    identity = json.loads((directory / "identity.json").read_text())
    if "flashed_at_ms" not in identity:
        raise RuntimeError("case has no successful flash record")
    result = directory / "measurement"
    result.mkdir(exist_ok=False)

    def probe():
        raw = subprocess.check_output(
            [str(hub), "devices", "--json"], cwd=ROOT, text=True, timeout=45
        )
        snapshot = json.loads(raw)
        with (result / "devices.jsonl").open("a") as output:
            output.write(json.dumps({"observed_ms": int(time.time() * 1000), **snapshot}) + "\n")
        return device_status(snapshot, identity)

    try:
        wait_settled(probe)
        write_json(
            result / "settled.json",
            {"at_ms": int(time.time() * 1000), "online_seconds": SETTLE_SECONDS, **identity},
        )
        commands = [
            ("heap", "heap-before.txt"),
            ("top", "top-before.txt"),
            ("power", "power-before.txt"),
            ("bench 16000", "bench-1.txt"),
            ("bench 16000", "bench-2.txt"),
            ("bench 16000", "bench-3.txt"),
            ("heap", "heap-after.txt"),
            ("top", "top-after.txt"),
            ("power", "power-after.txt"),
        ]
        if opens:
            commands[3:3] = [
                (f"bench_open {surface}", f"open-{surface}-{run + 1}.txt")
                for surface in ("apps", "play", "dot", "agents", "back")
                for run in range(open_runs)
            ]
        if surfaces:
            commands[-3:-3] = [
                (f"bench_scroll {surface} 16000", f"scroll-{surface}-{run + 1}.txt")
                for surface in surfaces
                for run in range(3)
            ]
        if fixtures:
            commands[-3:-3] = [
                ("bench_transition 16000", "fixture-transition.txt"),
                ("bench_quiet 25000", "fixture-quiet.txt"),
            ]
        for command, name in commands:
            guarded_command(
                [str(ROOT / "scripts/orb-dev.sh"), *command.split()], result / name, probe
            )
            if command.startswith("bench_"):
                write_json(
                    result / name.replace(".txt", ".json"),
                    {
                        "command": command,
                        "capture": name,
                        "completed_at_ms": int(time.time() * 1000),
                    },
                )
        accounting_report(
            [
                result / name
                for command, name in commands
                if command.startswith("bench") and not command.startswith("bench_open")
            ],
            result / "accounting.json",
            required=identity.get("options", {}).get("ORB_DISPLAY_RENDER_ACCOUNTING") == "y",
        )
        if surfaces:
            surface_report(
                [result / name for command, name in commands if command.startswith("bench_scroll")],
                result / "surfaces.json",
                require_gates=True,
            )
        if opens:
            open_report(
                [result / name for command, name in commands if command.startswith("bench_open")],
                result / "open.json",
            )
        guarded_command(
            [str(ROOT / "scripts/orb-dev.sh"), "shot", str(result / "home.png")],
            result / "shot.txt",
            probe,
        )
        guarded_command(
            [
                str(ROOT / "scripts/orb-transport.sh"),
                "--logs",
                "3",
                "display:|scratch|panic|underflow",
            ],
            result / "display-after.log",
            probe,
            timeout=15,
        )
        (result / "COMPLETE").write_text("Exact build remained online; no fresh OTA report.\n")
    except Exception as error:
        (result / "INVALID").write_text(str(error) + "\n")
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    build = sub.add_parser("build")
    build.add_argument("directory", type=Path)
    build.add_argument(
        "--bench-version",
        required=True,
        help="current dev-feed version, USB experiments only; never publish these images",
    )
    build.add_argument(
        "--enable", action="append", default=[], choices=BASE_OPTIONS, metavar="ORB_OPTION"
    )
    build.add_argument(
        "--compare-config",
        type=Path,
        help="control sdkconfig.h; reject unrelated effective config differences",
    )
    build.add_argument(
        "--open-control",
        action="store_true",
        help="disable app-open fixes only; keep bench_open instrumentation for a same-source control",
    )
    flash = sub.add_parser("flash")
    flash.add_argument("directory", type=Path)
    measure = sub.add_parser("measure")
    measure.add_argument("directory", type=Path)
    measure.add_argument(
        "--fixtures",
        action="store_true",
        help="also capture synthetic transition and 25s quiet fixtures",
    )
    measure.add_argument(
        "--surfaces",
        nargs="*",
        choices=SCROLL_SURFACES,
        help="capture each real scroll surface three times; no names means all five",
    )
    measure.add_argument("--hub", type=Path, default=Path.home() / ".venvs/orb-hub/bin/orb-hub")
    measure.add_argument(
        "--opens",
        action="store_true",
        help="measure live Face→Apps and Apps entries before renderer fixtures; report actual cold/warm state",
    )
    measure.add_argument(
        "--open-runs", type=int, default=3, help="2..20 samples per surface with --opens"
    )
    args = parser.parse_args()
    directory = args.directory.resolve()
    if args.action == "build":
        build_case(
            directory,
            args.bench_version,
            args.enable,
            args.compare_config,
            open_control=args.open_control,
        )
    elif args.action == "flash":
        flash_case(directory)
    else:
        measure_case(
            directory,
            args.hub,
            fixtures=args.fixtures,
            opens=args.opens,
            open_runs=args.open_runs,
            surfaces=SCROLL_SURFACES if args.surfaces == [] else args.surfaces,
        )
    print(f"{args.action} complete: {directory}")


if __name__ == "__main__":
    main()
