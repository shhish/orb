"""Measured battery drain from logged state reports: discharge segments and display-time shares."""

import datetime
from dataclasses import dataclass
from itertools import pairwise

# A normal report gap is at most 5 minutes (the storage coalescing window).
# Anything longer is missing data, so it ends a segment instead of inflating
# its duration.
MAX_GAP_MS = 15 * 60 * 1000
SHORT_SEGMENT_SECONDS = 20 * 60
SMALL_DROP_PCT = 2
DISPLAY_STATES = ("active", "dimmed", "always_on", "sleep")


@dataclass(frozen=True)
class BatterySample:
    at: int
    device_id: str
    pct: int | None
    mv: int | None
    charging: bool | None
    usb: bool | None
    display_state: str | None


@dataclass(frozen=True)
class DischargeSegment:
    device_id: str
    start_at: int
    end_at: int
    duration_s: float
    start_pct: int
    end_pct: int
    start_mv: int | None
    end_mv: int | None
    pct_drop: int
    pct_per_hour: float
    hours_per_full_charge: float | None
    estimated_ma: float | None
    caveats: tuple[str, ...]

    @property
    def reliable(self) -> bool:
        return not self.caveats


def iso_time(at_ms: int) -> str:
    """A local, offset-bearing timestamp for the CLI and its JSON."""
    return datetime.datetime.fromtimestamp(at_ms / 1000).astimezone().isoformat(timespec="seconds")


def _segment(rows: list[BatterySample], capacity_mah: float | None) -> DischargeSegment | None:
    if len(rows) < 2:
        return None
    start, end = rows[0], rows[-1]
    duration_s = (end.at - start.at) / 1000
    if duration_s <= 0 or start.pct is None or end.pct is None:
        return None
    drop = start.pct - end.pct
    hours = duration_s / 3600
    rate = drop / hours
    caveats = []
    if duration_s < SHORT_SEGMENT_SECONDS:
        caveats.append("under 20 minutes")
    if abs(drop) < SMALL_DROP_PCT:
        caveats.append("moved under 2 percent")
    if drop < 0:
        caveats.append("percentage rose")
    estimated_ma = None
    if capacity_mah and drop > 0:
        estimated_ma = capacity_mah * drop / 100 / hours
    return DischargeSegment(
        device_id=start.device_id,
        start_at=start.at,
        end_at=end.at,
        duration_s=duration_s,
        start_pct=start.pct,
        end_pct=end.pct,
        start_mv=start.mv,
        end_mv=end.mv,
        pct_drop=drop,
        pct_per_hour=rate,
        hours_per_full_charge=100 / rate if rate > 0 else None,
        estimated_ma=estimated_ma,
        caveats=tuple(caveats),
    )


def discharge_segments(samples: list[BatterySample], capacity_mah: float | None = None) -> list[DischargeSegment]:
    """Contiguous runs off USB and not charging. A charging/USB sample or a
    reporting gap ends a run; one-sample runs carry no duration, so they are
    not reported."""
    rows = sorted(samples, key=lambda sample: sample.at)
    segments = []
    current: list[BatterySample] = []

    def flush() -> None:
        segment = _segment(current, capacity_mah)
        if segment is not None:
            segments.append(segment)
        current.clear()

    for sample in rows:
        discharging = sample.usb is False and sample.charging is False and sample.pct is not None
        if not discharging:
            flush()
            continue
        if current and sample.at - current[-1].at > MAX_GAP_MS:
            flush()
        current.append(sample)
    flush()
    return segments


def display_state_shares(samples: list[BatterySample]) -> dict[str, float]:
    """Time-weighted share of each panel state. Gaps over MAX_GAP_MS are
    ignored, and an absent or unrecognised state counts as unknown."""
    rows = sorted(samples, key=lambda sample: sample.at)
    durations: dict[str, int] = {}
    for previous, current in pairwise(rows):
        elapsed = current.at - previous.at
        if elapsed <= 0 or elapsed > MAX_GAP_MS:
            continue
        state = previous.display_state if previous.display_state in DISPLAY_STATES else "unknown"
        durations[state] = durations.get(state, 0) + elapsed
    total = sum(durations.values())
    if not total:
        return {}
    return {state: durations[state] / total for state in (*DISPLAY_STATES, "unknown") if state in durations}


def average_ma(segments: list[DischargeSegment], capacity_mah: float | None) -> float | None:
    """Capacity-weighted draw across the segments where the percentage fell."""
    if not capacity_mah:
        return None
    useful = [segment for segment in segments if segment.pct_drop > 0 and segment.duration_s > 0]
    hours = sum(segment.duration_s for segment in useful) / 3600
    if hours <= 0:
        return None
    drop = sum(segment.pct_drop for segment in useful)
    return capacity_mah * drop / 100 / hours
