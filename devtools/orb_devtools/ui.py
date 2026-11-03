"""YAML UI scenarios, pluggable capture backends and perceptual PNG comparisons.

Differences use CIE76 Delta E in D65 Lab after compositing sRGB on black.
Transparent pixels in both images and explicit rectangular masks are excluded.
Alpha changes are also compared (0..255 mapped to Delta E 0..100), so a missing
opaque black region cannot silently match a transparent screenshot.
"""

from __future__ import annotations

import argparse
import functools
import html
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

import yaml
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GOLDENS = ROOT / "tests/ui/golden"
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}\Z")


class ScenarioError(ValueError):
    """Invalid scenario or image; never treated as a passing comparison."""


class Backend(Protocol):
    """A simulator can implement this interface without changing scenario files."""

    def perform(self, action: str, values: list[int | float]) -> None: ...

    def shot(self, destination: Path) -> None: ...


class CommandBackend:
    """Invoke an argv prefix, followed by the existing orb-dev.sh CLI arguments."""

    def __init__(self, command: list[str], timeout: float = 90):
        self.command = command
        self.timeout = timeout

    def perform(self, action: str, values: list[int | float]) -> None:
        if action == "wait":
            time.sleep(values[0])
        else:
            subprocess.run(
                [*self.command, action, *map(str, values)],
                check=True,
                timeout=self.timeout,
                capture_output=True,
                text=True,
            )

    def shot(self, destination: Path) -> None:
        subprocess.run(
            [*self.command, "shot", str(destination)],
            check=True,
            timeout=self.timeout,
            capture_output=True,
            text=True,
        )


class FixtureBackend:
    """Offline replay of sorted PNGs, one per shot; touches/waits are recorded."""

    def __init__(self, directory: Path):
        self.images = iter(sorted(directory.glob("*.png")))
        self.actions: list[tuple[str, list[int | float]]] = []

    def perform(self, action: str, values: list[int | float]) -> None:
        self.actions.append((action, values))

    def shot(self, destination: Path) -> None:
        source = next(self.images, None)
        if source is None:
            raise ScenarioError("fixture backend has no more PNGs")
        shutil.copyfile(source, destination)


def _number(value: object, name: str, low: float, high: float) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or not low <= value <= high
    ):
        raise ScenarioError(f"{name} must be a number in [{low}, {high}]")
    return float(value)


def _integers(value: object, count: int, name: str) -> list[int]:
    if not isinstance(value, list) or len(value) != count or any(type(v) is not int for v in value):
        raise ScenarioError(f"{name} must contain {count} integers")
    return value


def _masks(value: object, size: tuple[int, int]) -> list[list[int]]:
    if not isinstance(value, list):
        raise ScenarioError("masks must be a list of [x, y, width, height]")
    for rectangle in value:
        x, y, w, h = _integers(rectangle, 4, "mask")
        if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > size[0] or y + h > size[1]:
            raise ScenarioError(f"mask {rectangle} is outside image size {size}")
    return value


def _golden_path(directory: Path, value: object) -> Path:
    if not isinstance(value, str) or not value.endswith(".png"):
        raise ScenarioError("golden must be a relative .png path")
    path = directory / value
    if Path(value).is_absolute() or not path.resolve().is_relative_to(directory.resolve()):
        raise ScenarioError("golden path escapes the golden directory")
    return path


def validate_scenario(data: object) -> dict:
    """Validate all steps before performing any device actions."""
    if (
        not isinstance(data, dict)
        or type(data.get("version")) is not int
        or data.get("version") != 1
    ):
        raise ScenarioError("scenario must be a mapping with version: 1")
    if set(data) - {"version", "name", "description", "example", "size", "steps"}:
        raise ScenarioError("unknown scenario keys")
    name = data.get("name")
    if not isinstance(name, str) or not NAME.fullmatch(name):
        raise ScenarioError("name must use 1-80 letters, digits, dots, underscores or hyphens")
    size = tuple(_integers(data.get("size", [466, 466]), 2, "size"))
    if any(not 1 <= v <= 4096 for v in size):
        raise ScenarioError("image dimensions must be in [1, 4096]")
    if "description" in data and not isinstance(data["description"], str):
        raise ScenarioError("description must be text")
    if "example" in data and type(data["example"]) is not bool:
        raise ScenarioError("example must be true or false")
    steps = data.get("steps")
    if not isinstance(steps, list) or not 1 <= len(steps) <= 1000:
        raise ScenarioError("steps must contain 1-1000 actions")
    shots: set[str] = set()
    last_shot = None
    expectations = 0
    normalized = []
    for index, step in enumerate(steps, 1):
        if not isinstance(step, dict) or len(step) != 1:
            raise ScenarioError(f"step {index} must contain one action")
        action, value = next(iter(step.items()))
        if action in ("tap", "hold", "swipe"):
            values = _integers(value, {"tap": 2, "hold": 3, "swipe": 5}[action], action)
            coords = values[:4] if action == "swipe" else values[:2]
            if any(not 0 <= v < size[i % 2] for i, v in enumerate(coords)):
                raise ScenarioError(f"step {index}: coordinates are outside the panel")
            if action != "tap" and not 1 <= values[-1] <= 60000:
                raise ScenarioError(f"step {index}: gesture duration must be 1-60000 ms")
        elif action == "wait":
            value = _number(value, "wait seconds", 0, 3600)
        elif action == "shot":
            if not isinstance(value, str) or not NAME.fullmatch(value) or value in shots:
                raise ScenarioError(f"step {index}: shot needs a unique simple name")
            shots.add(value)
            last_shot = value
        elif action == "expect":
            value = {"golden": value} if isinstance(value, str) else value
            if not isinstance(value, dict) or set(value) - {
                "shot",
                "golden",
                "tolerance",
                "max_changed",
                "masks",
            }:
                raise ScenarioError(f"step {index}: invalid expect options")
            value = dict(value)
            value.setdefault("shot", last_shot)
            if not isinstance(value["shot"], str) or value["shot"] not in shots:
                raise ScenarioError(f"step {index}: expect requires a preceding shot")
            _golden_path(DEFAULT_GOLDENS, value.get("golden"))
            value["tolerance"] = _number(value.get("tolerance", 2), "Delta E tolerance", 0, 200)
            value["max_changed"] = _number(value.get("max_changed", 0), "max_changed", 0, 1)
            value["masks"] = _masks(value.get("masks", []), size)
            expectations += 1
        else:
            raise ScenarioError(f"step {index}: unknown action {action!r}")
        normalized.append((action, value))
    if not expectations:
        raise ScenarioError("scenario needs at least one expect step")
    return {**data, "size": size, "steps": normalized}


def load_scenario(path: Path) -> dict:
    if path.stat().st_size > 1024 * 1024:
        raise ScenarioError("scenario exceeds 1 MiB")
    try:
        return validate_scenario(yaml.safe_load(path.read_text()))
    except yaml.YAMLError as error:
        raise ScenarioError(f"invalid YAML: {error}") from error


@functools.lru_cache(maxsize=65536)
def _lab(pixel: tuple[int, int, int, int]) -> tuple[float, float, float]:
    alpha = pixel[3] / 255
    rgb = [channel * alpha / 255 for channel in pixel[:3]]
    r, g, b = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in rgb]
    x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883
    x, y, z = [
        v ** (1 / 3) if v > (6 / 29) ** 3 else v / (3 * (6 / 29) ** 2) + 4 / 29 for v in (x, y, z)
    ]
    return 116 * y - 16, 500 * (x - y), 200 * (y - z)


@dataclass
class Comparison:
    passed: bool
    changed: int
    compared: int
    fraction: float
    mean_delta_e: float
    max_delta_e: float
    diff: Image.Image
    error: str | None = None


def compare_images(
    actual: Image.Image,
    golden: Image.Image,
    *,
    tolerance: float = 2,
    max_changed: float = 0,
    masks: list[list[int]] | None = None,
) -> Comparison:
    """Pass if at most max_changed of visible, unmasked pixels exceed Delta E tolerance."""
    _number(tolerance, "Delta E tolerance", 0, 200)
    _number(max_changed, "max_changed", 0, 1)
    if actual.size != golden.size:
        diff = Image.new(
            "RGBA", (max(actual.width, golden.width), max(actual.height, golden.height)), "red"
        )
        return Comparison(
            False, 0, 0, 1, 0, 0, diff, f"size mismatch: {actual.size} != {golden.size}"
        )
    masks = _masks([] if masks is None else masks, actual.size)
    selected = Image.new("1", actual.size, 1)
    draw = ImageDraw.Draw(selected)
    for x, y, w, h in masks:
        draw.rectangle((x, y, x + w - 1, y + h - 1), fill=0)
    actual, golden = actual.convert("RGBA"), golden.convert("RGBA")
    compared = changed = 0
    total = peak = 0.0
    output = []
    for a, g, visible in zip(actual.getdata(), golden.getdata(), selected.getdata(), strict=True):
        if not visible or (a[3] == 0 and g[3] == 0):
            output.append((0, 0, 0, 0))
            continue
        compared += 1
        delta = 0 if a == g else max(math.dist(_lab(a), _lab(g)), abs(a[3] - g[3]) * 100 / 255)
        total += delta
        peak = max(peak, delta)
        if delta > tolerance:
            changed += 1
            output.append((255, 32, 64, 255))
        else:
            output.append((a[0] // 3, a[1] // 3, a[2] // 3, a[3]))
    if not compared:
        raise ScenarioError("comparison has no visible unmasked pixels")
    diff = Image.new("RGBA", actual.size)
    diff.putdata(output)
    fraction = changed / compared
    return Comparison(
        fraction <= max_changed, changed, compared, fraction, total / compared, peak, diff
    )


def _load_png(path: Path, size: tuple[int, int] | None = None) -> Image.Image:
    with Image.open(path) as image:
        if image.format != "PNG":
            raise ScenarioError(f"{path}: screenshot must be PNG")
        if size and image.size != size:
            raise ScenarioError(f"{path}: expected {size}, got {image.size}")
        return image.convert("RGBA")


def _atomic_png(image: Image.Image, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=".orb-ui-", suffix=".png", dir=destination.parent)
    os.close(fd)
    try:
        image.save(name, format="PNG")
        os.replace(name, destination)
    finally:
        Path(name).unlink(missing_ok=True)


def run_scenario(
    data: dict, backend: Backend, goldens: Path, output: Path, *, update: bool = False
) -> dict:
    """Execute a validated scenario; preserve failures and evidence for the report."""
    output.mkdir(parents=True, exist_ok=True)
    results = []
    shots = {}
    failure = None
    for index, (action, value) in enumerate(data["steps"], 1):
        try:
            if action in ("tap", "hold", "swipe", "wait"):
                backend.perform(action, [value] if action == "wait" else value)
            elif action == "shot":
                path = output / f"{value}.png"
                backend.shot(path)
                _load_png(path, data["size"])
                shots[value] = path
            elif action == "expect":
                golden_path = _golden_path(goldens, value["golden"])
                actual = _load_png(shots[value["shot"]])
                result = {
                    "step": index,
                    "golden_name": value["golden"],
                    "actual": shots[value["shot"]].name,
                }
                if update:
                    # Validate masks/nonempty coverage even when establishing a baseline.
                    compare_images(actual, actual, masks=value["masks"])
                    if golden_path.is_file():
                        previous = f"expect-{index}-previous.png"
                        shutil.copyfile(golden_path, output / previous)
                        comparison = compare_images(
                            actual,
                            _load_png(golden_path),
                            tolerance=value["tolerance"],
                            max_changed=value["max_changed"],
                            masks=value["masks"],
                        )
                        diff_name = f"expect-{index}-diff.png"
                        comparison.diff.save(output / diff_name)
                        result.update(previous=previous, diff=diff_name)
                    _atomic_png(actual, golden_path)
                    result.update(passed=True, status="updated")
                elif not golden_path.is_file():
                    result.update(passed=False, status="missing golden; review and run --update")
                else:
                    golden = _load_png(golden_path)
                    comparison = compare_images(
                        actual,
                        golden,
                        tolerance=value["tolerance"],
                        max_changed=value["max_changed"],
                        masks=value["masks"],
                    )
                    diff_name = f"expect-{index}-diff.png"
                    comparison.diff.save(output / diff_name)
                    result.update(
                        passed=comparison.passed,
                        status=comparison.error or ("passed" if comparison.passed else "different"),
                        changed=comparison.changed,
                        compared=comparison.compared,
                        fraction=comparison.fraction,
                        mean_delta_e=comparison.mean_delta_e,
                        max_delta_e=comparison.max_delta_e,
                        tolerance=value["tolerance"],
                        max_changed=value["max_changed"],
                        diff=diff_name,
                    )
                if golden_path.is_file():
                    name = f"expect-{index}-golden.png"
                    shutil.copyfile(golden_path, output / name)
                    result["golden"] = name
                results.append(result)
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            failure = f"step {index} ({action}): {error}"
            break
    return {
        "name": data["name"],
        "example": data.get("example", False),
        "description": data.get("description", ""),
        "passed": failure is None and bool(results) and all(r["passed"] for r in results),
        "error": failure,
        "expectations": results,
    }


def write_report(results: list[dict], output: Path, *, update: bool) -> None:
    output.mkdir(parents=True, exist_ok=True)
    (output / "results.json").write_text(
        json.dumps({"update": update, "scenarios": results}, indent=2) + "\n"
    )
    parts = [
        "<!doctype html><html lang='en'><meta charset='utf-8'><title>Orb UI report</title>",
        "<style>body{font:16px system-ui;margin:2rem;background:#171719;color:#eee}section{border-top:1px solid #666;padding:1rem 0}figure{display:inline-block;margin:0 1rem 1rem 0}img{max-width:30vw;background:#333}figcaption{padding:.3rem}code{white-space:pre-wrap}.failed{color:#ff8390}.passed{color:#74db9c}</style>",
        f"<h1>Orb UI {'golden update' if update else 'regression'} report</h1>",
        "<p>Diff: changed pixels are pink; masked/fully transparent pixels are transparent. Thresholds use CIE76 Delta E on black.</p>",
    ]
    for result in results:
        esc = html.escape
        status = "passed" if result["passed"] else "failed"
        parts.append(f"<section><h2 class='{status}'>{esc(result['name'])}: {status}</h2>")
        if result["example"]:
            parts.append(
                "<p><strong>Example scenario; capture reproducibility is not established.</strong></p>"
            )
        parts.append(f"<p>{esc(result['description'])}</p>")
        if result["error"]:
            parts.append(f"<p class='failed'><code>{esc(result['error'])}</code></p>")
        for item in result["expectations"]:
            parts.append(f"<h3>{esc(item['golden_name'])}: {esc(item['status'])}</h3>")
            if "fraction" in item:
                parts.append(
                    f"<p>{item['changed']}/{item['compared']} pixels changed ({item['fraction']:.3%}, allowed {item['max_changed']:.3%}); mean ΔE {item['mean_delta_e']:.3f}, max {item['max_delta_e']:.3f}, threshold {item['tolerance']:.3f}.</p>"
                )
            for key in ("actual", "previous", "golden", "diff"):
                if key in item:
                    href = esc(f"{result['directory']}/{item[key]}", quote=True)
                    parts.append(
                        f"<figure><a href='{href}'><img src='{href}' alt='{key}'></a><figcaption>{key}</figcaption></figure>"
                    )
        parts.append("</section>")
    (output / "index.html").write_text("\n".join(parts) + "\n</html>\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenarios", nargs="+", type=Path)
    parser.add_argument("--goldens", type=Path, default=DEFAULT_GOLDENS)
    parser.add_argument(
        "--output", type=Path, default=ROOT / "artifacts/ui" / time.strftime("%Y%m%d-%H%M%S")
    )
    parser.add_argument(
        "--update",
        action="store_true",
        help="replace goldens from new captures (review PNGs before committing)",
    )
    parser.add_argument("--backend", choices=("device", "fixture"), default="device")
    parser.add_argument(
        "--command",
        type=Path,
        default=ROOT / "scripts/orb-dev.sh",
        help="device/simulator executable implementing the orb-dev.sh CLI",
    )
    parser.add_argument(
        "--fixtures",
        type=Path,
        help="directory of sorted PNGs for offline replay; waits are skipped",
    )
    args = parser.parse_args(argv)
    try:
        scenarios = [load_scenario(path) for path in args.scenarios]
        for scenario in scenarios:
            for action, value in scenario["steps"]:
                if action == "expect":
                    _golden_path(args.goldens, value["golden"])
        if args.backend == "fixture" and not args.fixtures:
            raise ScenarioError("--backend fixture requires --fixtures")
        backend = (
            FixtureBackend(args.fixtures)
            if args.backend == "fixture"
            else CommandBackend([str(args.command.resolve())])
        )
        results = []
        for index, scenario in enumerate(scenarios, 1):
            directory = f"{index:03d}-{scenario['name']}"
            result = run_scenario(
                scenario, backend, args.goldens, args.output / directory, update=args.update
            )
            result["directory"] = directory
            results.append(result)
        write_report(results, args.output, update=args.update)
    except (OSError, ValueError) as error:
        print(f"orb-ui-test: {error}", file=sys.stderr)
        return 2
    print(args.output / "index.html")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
