#!/usr/bin/env python3
"""Explain observed bench_scroll cadence from a bounded debug trace, without board access."""

import argparse
import base64
import json
import re
import struct
import zlib
from bisect import bisect_right
from collections import Counter
from pathlib import Path

PREFIX = "@@ORBDEV SCROLLTRACE "
HIST_PREFIX = "@@ORBDEV SCROLLTRACE_HIST "
CHUNK_PREFIX = "@@ORBDEV SCROLLTRACE_CHUNK "
END_PREFIX = "@@ORBDEV SCROLLTRACE_END "
PACKED_RECORD = struct.Struct("<IiiB")
MAX_PACKED_EVENTS = 2048
CHUNK_BYTES = 192
EVENT_NAMES = (
    "INVALID",
    "TOUCH_SAMPLE",
    "TOUCH_RELEASE",
    "INPUT_READ",
    "INPUT_RELEASE",
    "INPUT_INJECT",
    "INJECT_RELEASE",
    "SCROLL",
    "INVALIDATE",
    "REFR_START",
    "REFR_END",
    "RENDER_START",
    "COMPLETE",
    "LOOP_BEGIN",
    "LOOP_END",
    "WAIT_BEGIN",
    "WAIT_END",
)
BUCKET_US = 1000
BUCKETS = 128
INPUT_EVENTS = {
    "TOUCH_SAMPLE",
    "TOUCH_RELEASE",
    "INPUT_READ",
    "INPUT_RELEASE",
    "INPUT_INJECT",
    "INJECT_RELEASE",
}
PAIRS = {
    "LOOP_BEGIN": ("LOOP_END", "handler"),
    "REFR_START": ("REFR_END", "refresh"),
    "WAIT_BEGIN": ("WAIT_END", "wait"),
}


def _fields(line, line_number):
    fields = {}
    for token in line.split():
        if "=" not in token:
            raise ValueError(f"line {line_number}: expected key=value, got {token!r}")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(f"line {line_number}: invalid or repeated field {key!r}")
        fields[key] = value
    return fields


def _integer(fields, key, line_number, *, nonnegative=False):
    try:
        value = int(fields[key])
    except (KeyError, ValueError) as error:
        raise ValueError(f"line {line_number}: missing or invalid integer {key!r}") from error
    if nonnegative and value < 0:
        raise ValueError(f"line {line_number}: {key!r} must be nonnegative")
    return value


def _crc32(fields, line_number):
    value = fields.get("crc32", "")
    if not re.fullmatch(r"[0-9a-fA-F]{8}", value):
        raise ValueError(f"line {line_number}: expected eight hexadecimal crc32 digits")
    return int(value, 16)


def _packed_events(metadata, chunks, trailer):
    if metadata.get("encoding") != "le13" or metadata.get("record_bytes") != "13":
        raise ValueError("unsupported packed trace encoding or record_bytes")
    for key in (
        "bytes",
        "chunks",
        "started_us",
        "ended_us",
        "captured_active",
        "completions",
        "loops",
        "handler_us",
        "timer_wait_us",
        "fairness_wait_us",
    ):
        metadata[key] = _integer(metadata, key, 0, nonnegative=True)
    if metadata["captured_active"] or (
        metadata["active"] and metadata["ended_us"] < metadata["started_us"]
    ):
        raise ValueError("packed trace is still active or has an invalid end timestamp")
    size = metadata["bytes"]
    count = metadata["count"]
    expected_chunks = (size + CHUNK_BYTES - 1) // CHUNK_BYTES
    if count > MAX_PACKED_EVENTS or size != count * PACKED_RECORD.size:
        raise ValueError("packed trace count/bytes exceed the bounded event format")
    if metadata["chunks"] != expected_chunks or set(chunks) != set(range(expected_chunks)):
        raise ValueError("packed trace has missing or unexpected chunk sequence numbers")
    if trailer is None:
        raise ValueError("missing SCROLLTRACE_END receipt")
    crc = _crc32(metadata, 0)
    if (trailer["bytes"], trailer["chunks"], trailer["crc32"]) != (size, expected_chunks, crc):
        raise ValueError("SCROLLTRACE_END differs from packed trace header")
    for seq, block in chunks.items():
        if len(block) != min(CHUNK_BYTES, size - seq * CHUNK_BYTES):
            raise ValueError("packed trace chunk has incorrect length")
    raw = b"".join(chunks[seq] for seq in range(expected_chunks))
    if zlib.crc32(raw) != crc:
        raise ValueError("packed trace full CRC mismatch")
    metadata["emit_us"] = trailer["emit_us"]
    events = []
    for offset, a, b, kind in PACKED_RECORD.iter_unpack(raw):
        if not 0 < kind < len(EVENT_NAMES):
            raise ValueError("packed trace has unknown event kind")
        at_us = metadata["started_us"] + offset
        if at_us > metadata["ended_us"]:
            raise ValueError("packed event timestamp follows capture end")
        events.append({"event": EVENT_NAMES[kind], "at_us": at_us, "a": a, "b": b})
    return events


def parse_trace(text):
    """Read one legacy or CRC-protected packed receipt; damaged trace lines fail."""
    metadata = None
    full_histogram = None
    events = []
    chunks = {}
    trailer = None
    for number, line in enumerate(text.splitlines(), 1):
        if CHUNK_PREFIX in line:
            if metadata is None or metadata.get("version") != "2":
                raise ValueError("packed chunk without a version 2 trace header")
            fields = _fields(line.split(CHUNK_PREFIX, 1)[1], number)
            seq = _integer(fields, "seq", number, nonnegative=True)
            size = _integer(fields, "bytes", number, nonnegative=True)
            if seq >= (MAX_PACKED_EVENTS * PACKED_RECORD.size + CHUNK_BYTES - 1) // CHUNK_BYTES:
                raise ValueError("packed chunk sequence exceeds capture bound")
            if seq in chunks:
                raise ValueError("duplicate packed trace chunk sequence")
            if not 1 <= size <= CHUNK_BYTES or len(fields.get("data", "")) > CHUNK_BYTES * 4 // 3:
                raise ValueError("packed trace chunk exceeds size bound")
            try:
                block = base64.b64decode(fields.get("data", ""), validate=True)
            except ValueError as error:
                raise ValueError("invalid packed trace base64") from error
            if len(block) != size or zlib.crc32(block) != _crc32(fields, number):
                raise ValueError("packed trace chunk length/CRC mismatch")
            chunks[seq] = block
            continue
        if END_PREFIX in line:
            if metadata is None or metadata.get("version") != "2":
                raise ValueError("packed trailer without a version 2 trace header")
            if trailer is not None:
                raise ValueError("multiple SCROLLTRACE_END receipts")
            fields = _fields(line.split(END_PREFIX, 1)[1], number)
            trailer = {
                key: _integer(fields, key, number, nonnegative=True)
                for key in ("bytes", "chunks", "emit_us")
            }
            trailer["crc32"] = _crc32(fields, number)
            continue
        if HIST_PREFIX in line:
            if full_histogram is not None:
                raise ValueError("multiple SCROLLTRACE_HIST receipts")
            fields = _fields(line.split(HIST_PREFIX, 1)[1], number)
            bucket_us = _integer(fields, "bucket_us", number, nonnegative=True)
            try:
                counts = [int(value) for value in fields["counts"].split(",")]
            except (KeyError, ValueError) as error:
                raise ValueError(f"line {number}: missing or invalid histogram counts") from error
            if bucket_us != BUCKET_US or len(counts) != BUCKETS or any(n < 0 for n in counts):
                raise ValueError(f"line {number}: expected 128 nonnegative 1ms histogram counts")
            full_histogram = {
                "bucket_us": bucket_us,
                "overflow_from_us": (BUCKETS - 1) * bucket_us,
                "counts": counts,
            }
            continue
        if PREFIX not in line:
            continue
        fields = _fields(line.split(PREFIX, 1)[1], number)
        if "event" not in fields:
            if metadata is not None:
                raise ValueError("multiple SCROLLTRACE headers; analyze each capture separately")
            metadata = dict(fields)
            if metadata.get("version", "1") not in ("1", "2"):
                raise ValueError("unsupported SCROLLTRACE version")
            for key in ("active", "count", "dropped"):
                metadata[key] = _integer(fields, key, number, nonnegative=True)
            for key in (
                "captured_active",
                "started_us",
                "ended_us",
                "completions",
                "loops",
                "handler_us",
                "timer_wait_us",
                "fairness_wait_us",
            ):
                if key in fields:
                    metadata[key] = _integer(fields, key, number, nonnegative=True)
            if metadata["active"] not in (0, 1):
                raise ValueError(f"line {number}: active must be 0 or 1")
            if metadata.get("captured_active", 0) not in (0, 1):
                raise ValueError(f"line {number}: captured_active must be 0 or 1")
            continue
        events.append(
            {
                "event": fields["event"],
                "at_us": _integer(fields, "at_us", number, nonnegative=True),
                "a": _integer(fields, "a", number),
                "b": _integer(fields, "b", number),
            }
        )
    if metadata is None:
        raise ValueError("no SCROLLTRACE header; enable CONFIG_ORB_DISPLAY_SCROLL_TRACE")
    if metadata.get("version") == "2":
        if full_histogram is None:
            raise ValueError("missing SCROLLTRACE_HIST receipt for version 2 trace")
        if events:
            raise ValueError("packed trace mixes text events with binary chunks")
        events = _packed_events(metadata, chunks, trailer)
    if metadata["count"] != len(events):
        raise ValueError(
            f"incomplete receipt: header count={metadata['count']}, captured events={len(events)}"
        )
    if not metadata["active"] and (events or metadata["dropped"]):
        raise ValueError("inactive trace contains events or drops")
    if full_histogram is not None:
        if "completions" in metadata:
            expected = max(0, metadata["completions"] - 1)
            if sum(full_histogram["counts"]) != expected:
                raise ValueError("full-run histogram interval count differs from completions - 1")
        metadata["completion_histogram"] = full_histogram
    return metadata, events


def _spans(events):
    """Only paired observed endpoints form spans; partial endpoints remain explicit."""
    pending = {}
    spans = []
    unpaired = Counter()
    ends = {end: start for start, (end, _) in PAIRS.items()}
    for event in events:
        kind = event["event"]
        if kind in PAIRS:
            if kind in pending:
                unpaired[kind] += 1
            pending[kind] = event
        elif kind in ends:
            start_kind = ends[kind]
            start = pending.pop(start_kind, None)
            if start is None:
                unpaired[kind] += 1
                continue
            category = PAIRS[start_kind][1]
            if category == "wait":
                category = {1: "timer_wait", 2: "fairness_wait"}.get(start["b"], "other_wait")
            spans.append((start["at_us"], event["at_us"], category))
    unpaired.update(pending.keys())
    return spans, dict(unpaired)


def _duration_index(intervals):
    """Union same-category spans once; frame queries then take logarithmic time."""
    merged = []
    for left, right in sorted(intervals):
        if left >= right:
            continue
        if merged and left <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], right)
        else:
            merged.append([left, right])
    cumulative = [0]
    for left, right in merged:
        cumulative.append(cumulative[-1] + right - left)
    return [left for left, _ in merged], [right for _, right in merged], cumulative


def _covered_until(index, at):
    starts, ends, cumulative = index
    current = bisect_right(starts, at) - 1
    if current < 0:
        return 0
    return cumulative[current] + min(at, ends[current]) - starts[current]


def _covered_between(index, start, end):
    return _covered_until(index, end) - _covered_until(index, start)


def analyze_trace(text):
    """Return timings observed in the trace; inclusive stages are never added as latency."""
    metadata, raw_events = parse_trace(text)
    out_of_order = sum(
        right["at_us"] < left["at_us"] for left, right in zip(raw_events, raw_events[1:])
    )
    # Touch sampling runs on the other core; concurrent append order can differ
    # from timestamp order. Keep the original order in the artifact for auditing.
    events = sorted(raw_events, key=lambda event: event["at_us"])
    spans, unpaired = _spans(events)
    categories = {category for _, _, category in spans}
    by_category = {
        category: _duration_index([(start, end) for start, end, kind in spans if kind == category])
        for category in sorted(categories)
    }
    waits_and_refresh = _duration_index(
        [
            (left, right)
            for left, right, kind in spans
            if kind == "refresh" or kind.endswith("_wait")
        ]
    )
    sample_times = {
        category: [event["at_us"] for event in events if event["event"] in kinds]
        for category, kinds in (
            ("input", INPUT_EVENTS),
            ("scroll", {"SCROLL"}),
            ("invalidation", {"INVALIDATE"}),
        )
    }
    counts = Counter(event["event"] for event in events)
    completed = [event["at_us"] for event in events if event["event"] == "COMPLETE"]
    histogram = [0] * BUCKETS
    intervals = []
    for start, end in zip(completed, completed[1:]):
        duration = end - start
        histogram[min(duration // BUCKET_US, BUCKETS - 1)] += 1
        observed = {
            category: _covered_between(index, start, end) for category, index in by_category.items()
        }
        unknown = duration - _covered_between(waits_and_refresh, start, end)
        waits = [
            f"{kind}={value}us"
            for kind, value in observed.items()
            if kind.endswith("_wait") and value
        ]
        latest = {}
        for category, times in sample_times.items():
            previous = bisect_right(times, end) - 1
            latest[f"latest_{category}_age_us"] = end - times[previous] if previous >= 0 else None
        intervals.append(
            {
                "start_us": start,
                "end_us": end,
                "interval_us": duration,
                "observed_inclusive_us": observed,
                "outside_observed_refresh_or_wait_us": unknown,
                "wait_description": f"Observed waits: {', '.join(waits) or 'none paired'}; "
                f"{unknown}us outside observed refresh/wait (unattributed).",
                "input_events": bisect_right(sample_times["input"], end)
                - bisect_right(sample_times["input"], start),
                "scroll_events": bisect_right(sample_times["scroll"], end)
                - bisect_right(sample_times["scroll"], start),
                **latest,
            }
        )
    return {
        "schema_version": 1,
        "metadata": metadata,
        "full_run_completion_intervals": {
            "available": bool(metadata["active"] and "completion_histogram" in metadata),
            "histogram": metadata.get("completion_histogram") if metadata["active"] else None,
            "scope": "All successful DMA completion intervals, including idle gaps and after raw overflow.",
        },
        "trace_complete": bool(
            metadata["active"]
            and not metadata["dropped"]
            and not metadata.get("captured_active", 0)
        ),
        "limitations": [
            "Timings are observed overlaps, not exclusive causes of a delayed frame.",
            "Handler and refresh spans overlap; do not add them.",
            "RENDER_START and COMPLETE have no frame identity, so they are not paired into latency spans.",
            "Outside refresh/wait includes input, preparation, scheduling, and uninstrumented work.",
            "Intervals cover consecutive traced successful DMA completions, including idle gaps.",
            "Dropped events truncate the detailed timeline; no missing frame timings are inferred.",
        ],
        "events_out_of_timestamp_order": out_of_order,
        "unpaired_endpoints": unpaired,
        "event_counts": dict(sorted(counts.items())),
        "completion_intervals": {
            "scope": "Only consecutive completions present in the detailed event buffer.",
            "count": len(intervals),
            "minimum_us": min((row["interval_us"] for row in intervals), default=None),
            "maximum_us": max((row["interval_us"] for row in intervals), default=None),
            "histogram": {
                "bucket_us": BUCKET_US,
                "overflow_from_us": (BUCKETS - 1) * BUCKET_US,
                "counts": histogram,
            },
            "frames": intervals,
        },
        "events": raw_events,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", type=Path, help="raw output of one bench_scroll command")
    parser.add_argument("--output", type=Path, required=True, help="write the JSON timing artifact")
    args = parser.parse_args(argv)
    try:
        result = analyze_trace(args.receipt.read_text())
    except (OSError, ValueError) as error:
        parser.error(str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    intervals = result["completion_intervals"]
    print(
        f"{args.output}: {intervals['count']} completion intervals; "
        f"trace_complete={int(result['trace_complete'])}; dropped={result['metadata']['dropped']}"
    )


if __name__ == "__main__":
    main()
