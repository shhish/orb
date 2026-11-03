"""Convert LVGL's ftrace scope records to Chrome Trace / Perfetto JSON.

Times are wall times, not scheduler CPU measurements. Tracks group a task by
its observed core at scope entry; a migration has no known transition time.
Incomplete or mismatched scopes are reported and never given invented ends.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from decimal import Decimal
from pathlib import Path

RECORD = re.compile(
    r"LVGL-(?P<tid>\d+)\s+\[(?P<cpu>\d+)\]\s+"
    r"(?:\S+\s+)?(?P<time>\d+\.\d+):\s+tracing_mark_write:\s+"
    r"(?P<phase>[BE])\|(?P<pid>\d+)\|(?P<name>\S[^\r\n]*?)\s*$"
)


@dataclass
class Scope:
    name: str
    tid: int
    cpu: int
    start_ns: int
    end_ns: int = 0
    end_cpu: int = 0
    children: list[tuple[int, int]] = field(default_factory=list)

    @property
    def inclusive_ns(self) -> int:
        return self.end_ns - self.start_ns

    @property
    def self_intervals(self) -> list[tuple[int, int]]:
        cursor = self.start_ns
        intervals = []
        for start, end in self.children:
            intervals.append((cursor, start))
            cursor = end
        intervals.append((cursor, self.end_ns))
        return intervals

    @property
    def self_ns(self) -> int:
        return sum(end - start for start, end in self.self_intervals)


def parse(text: str) -> tuple[list[Scope], list[str]]:
    """Parse mixed console/ftrace text, preserving per-task nesting."""
    stacks: dict[int, list[Scope]] = defaultdict(list)
    last_time: dict[int, int] = {}
    scopes, warnings = [], []

    def unfinished(reason: str) -> None:
        for tid, stack in stacks.items():
            if stack:
                warnings.append(f"{reason}: task {tid}: {len(stack)} unclosed scope(s)")
                stack.clear()

    for line_number, line in enumerate(text.splitlines(), 1):
        # orb-logd keeps JSONL on disk; its CLI instead prints timestamped text.
        if line.lstrip().startswith("{"):
            try:
                record = json.loads(line)
            except ValueError:
                record = None
            if isinstance(record, dict) and isinstance(record.get("text"), str):
                line = record["text"]
        if "=== PROFILE BEGIN ===" in line or "=== PROFILE END ===" in line:
            unfinished(f"line {line_number}: capture boundary")
            last_time.clear()
            continue
        match = RECORD.search(line)
        if not match:
            if "tracing_mark_write:" in line:
                warnings.append(f"line {line_number}: malformed scope record")
            continue
        tid, cpu = int(match["tid"]), int(match["cpu"])
        stamp = int(Decimal(match["time"]) * 1_000_000_000)
        stack = stacks[tid]
        if stamp < last_time.get(tid, stamp):
            warnings.append(f"line {line_number}: task {tid}: time moved backwards")
            stack.clear()
            last_time[tid] = stamp
            continue
        last_time[tid] = stamp
        name = match["name"]
        if match["phase"] == "B":
            stack.append(Scope(name, tid, cpu, stamp))
            continue
        if not stack:
            warnings.append(f"line {line_number}: task {tid}: unmatched end {name}")
            continue
        if stack[-1].name != name:
            warnings.append(
                f"line {line_number}: task {tid}: end {name} mismatches {stack[-1].name}; "
                "discarding open scopes"
            )
            stack.clear()
            continue
        scope = stack.pop()
        scope.end_ns, scope.end_cpu = stamp, cpu
        if scope.cpu != cpu:
            warnings.append(
                f"line {line_number}: task {tid}: {name} migrated CPU {scope.cpu}->{cpu}; "
                "track uses entry CPU (transition time unavailable)"
            )
        if stack:
            stack[-1].children.append((scope.start_ns, scope.end_ns))
        scopes.append(scope)
    unfinished("end of input")
    return sorted(scopes, key=lambda s: (s.start_ns, -s.end_ns)), warnings


def overlap(start: int, end: int, lower: int, upper: int) -> int:
    return max(0, min(end, upper) - max(start, lower))


def statistics(scopes: list[Scope], window: tuple[int, int] | None = None) -> list[dict]:
    totals: dict[str, dict] = {}
    for scope in scopes:
        inclusive = scope.inclusive_ns
        own = scope.self_ns
        if window:
            inclusive = overlap(scope.start_ns, scope.end_ns, *window)
            own = sum(overlap(start, end, *window) for start, end in scope.self_intervals)
            if not inclusive:
                continue
        row = totals.setdefault(
            scope.name, {"name": scope.name, "calls": 0, "self_us": 0.0, "inclusive_us": 0.0}
        )
        row["calls"] += 1
        row["self_us"] += own / 1000
        row["inclusive_us"] += inclusive / 1000
    return sorted(totals.values(), key=lambda row: (-row["self_us"], row["name"]))


def convert(text: str, frame_name: str = "lv_display_refr_timer") -> dict:
    scopes, warnings = parse(text)
    origin = min((scope.start_ns for scope in scopes), default=0)
    events = []
    for cpu in sorted({scope.cpu for scope in scopes}):
        events.append(
            {
                "ph": "M",
                "pid": cpu + 1,
                "tid": 0,
                "name": "process_name",
                "args": {"name": f"CPU {cpu} (scope entry)"},
            }
        )
    for cpu, tid in sorted({(scope.cpu, scope.tid) for scope in scopes}):
        events.append(
            {
                "ph": "M",
                "pid": cpu + 1,
                "tid": tid,
                "name": "thread_name",
                "args": {"name": f"LVGL task {tid}"},
            }
        )
    for scope in scopes:
        events.append(
            {
                "ph": "X",
                "cat": "lvgl",
                "name": scope.name,
                "pid": scope.cpu + 1,
                "tid": scope.tid,
                "ts": (scope.start_ns - origin) / 1000,
                "dur": scope.inclusive_ns / 1000,
                "args": {
                    "self_us": scope.self_ns / 1000,
                    "entry_cpu": scope.cpu,
                    "exit_cpu": scope.end_cpu,
                },
            }
        )
    frames = []
    for scope in scopes:
        if scope.name == frame_name:
            rows = statistics(scopes, (scope.start_ns, scope.end_ns))
            frames.append(
                {
                    "index": len(frames) + 1,
                    "ts_us": (scope.start_ns - origin) / 1000,
                    "wall_us": scope.inclusive_ns / 1000,
                    "scoped_self_us": sum(row["self_us"] for row in rows),
                    "functions": rows,
                }
            )
    return {
        "traceEvents": events,
        "displayTimeUnit": "ms",
        "otherData": {
            "source": "LVGL built-in profiler",
            "origin_ns": origin,
            "timing": "Scope wall time, not CPU time; grouped by core observed at entry.",
            "frame_name": frame_name,
            "functions": statistics(scopes),
            "frames": frames,
            "warnings": warnings,
        },
    }


def summary(trace: dict, top: int = 20) -> str:
    data = trace["otherData"]
    lines = [
        "LVGL scope wall time (not scheduler CPU time)",
        "Function                              Calls     Self ms Inclusive ms",
    ]
    for row in data["functions"][:top]:
        lines.append(
            f"{row['name']:<37} {row['calls']:5d} {row['self_us'] / 1000:11.3f} {row['inclusive_us'] / 1000:12.3f}"
        )
    lines.extend(
        [
            "",
            f"Frames: {data['frame_name']}",
            "Frame    Start ms    Wall ms   Scoped self ms  Top self function (self / inclusive ms)",
        ]
    )
    for frame in data["frames"]:
        first = frame["functions"][0] if frame["functions"] else None
        detail = (
            f"{first['name']} ({first['self_us'] / 1000:.3f} / {first['inclusive_us'] / 1000:.3f})"
            if first
            else "-"
        )
        lines.append(
            f"{frame['index']:5d} {frame['ts_us'] / 1000:11.3f} {frame['wall_us'] / 1000:10.3f} {frame['scoped_self_us'] / 1000:16.3f}  {detail}"
        )
    if not data["frames"]:
        lines.append(
            "No complete frame scopes; use --frame NAME if this LVGL build uses another name."
        )
    lines.extend(
        [
            "",
            "Scoped self time may exceed frame wall time because both cores run concurrently.",
            "All per-frame function totals are in trace JSON: otherData.frames[].functions.",
        ]
    )
    lines.extend(f"WARNING: {warning}" for warning in data["warnings"])
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="raw ftrace or orb-logd console log")
    parser.add_argument("output", type=Path, help="Chrome Trace JSON; open in ui.perfetto.dev")
    parser.add_argument("--frame", default="lv_display_refr_timer", help="scope marking one frame")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail on truncated/malformed scopes after writing report",
    )
    args = parser.parse_args(argv)
    if args.top < 1:
        parser.error("--top must be positive")
    try:
        trace = convert(args.input.read_text(errors="replace"), args.frame)
        if not trace["otherData"]["functions"]:
            parser.error("no complete LVGL scopes found")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(trace, indent=2) + "\n")
        print(summary(trace, args.top), end="")
        print(f"Trace: {args.output}")
    except OSError as exc:
        print(f"orb-profile: {exc}", file=sys.stderr)
        return 1
    return 1 if args.strict and trace["otherData"]["warnings"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
