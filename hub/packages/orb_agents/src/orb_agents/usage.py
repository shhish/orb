"""Passive account quota observations, separate from session context/token counts.

No credentials or provider transports are opened here. Callers feed rollout
records or replies from an app-server they already own.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import math
import os
import re
import secrets
import shutil
import stat
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

STALE_AFTER = 15 * 60
MAX_STATUSLINE_BYTES = 256 * 1024
CLAUDE_USAGE_FILENAME = "claude-usage.json"

CLAUDE_STATUSLINE_SOURCE = "statusline"
CLAUDE_USAGE_SOURCE = "usage"
CLAUDE_USAGE_INTERVAL = 5 * 60
CLAUDE_USAGE_TIMEOUT = 30
CLAUDE_USAGE_MAX_BACKOFF = 60 * 60
MAX_CLAUDE_USAGE_BYTES = 128 * 1024
MAX_CLAUDE_USAGE_WINDOWS = 8


def _number(value: object, maximum: float = 100_000_000_000) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    # Compare bounded integer values before converting them to a C double.
    if not 0 <= value <= maximum or not math.isfinite(value):
        return None
    return float(value)


def observation_time(value: object) -> float | None:
    """Rollouts use ISO timestamps; rate-limit reset values use Unix seconds."""
    if isinstance(value, str):
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
            if parsed.tzinfo is None:
                return None
            return _number(parsed.timestamp())
        except (ValueError, OverflowError, OSError):
            return None
    return _number(value)


def window_label(minutes: float) -> str:
    if minutes == 10080:
        return "week"
    if minutes % 60 == 0:
        hours = minutes / 60
        return f"{hours:g} {'hour' if hours == 1 else 'hours'}"
    return f"{minutes:g} {'minute' if minutes == 1 else 'minutes'}"


@dataclass(frozen=True, slots=True)
class UsageWindow:
    minutes: float
    used_percent: float
    resets_at: float
    label: str | None = None


@dataclass(frozen=True, slots=True)
class UsageObservation:
    provider: str
    observed_at: float
    windows: tuple[UsageWindow, ...]
    limit_id: str | None = None
    limit_name: str | None = None
    credits: dict[str, Any] | None = None
    source: str | None = None


def _metadata(value: object) -> str | None:
    if isinstance(value, str) and re.fullmatch(r"[\w .-]{1,64}", value):
        return value
    return None


def parse_codex_rate_limits(
    limits: object,
    observed_at: object,
    *,
    camel_case: bool = False,
    source: str | None = None,
) -> UsageObservation | None:
    """Normalize nullable quota windows without confusing minutes or percentages.

    Credits and bucket names stay provider metadata; they never become meters.
    Malformed present windows invalidate the observation instead of appearing as
    zero usage. A valid nullable pair is a known observation with no windows.
    """
    observed = _number(observed_at)
    if not isinstance(limits, dict) or observed is None:
        return None
    if not {"primary", "secondary"}.intersection(limits):
        return None
    windows = []
    used_key = "usedPercent" if camel_case else "used_percent"
    duration_key = "windowDurationMins" if camel_case else "window_minutes"
    reset_key = "resetsAt" if camel_case else "resets_at"
    for key in ("primary", "secondary"):
        value = limits.get(key)
        if value is None:
            continue
        if not isinstance(value, dict):
            return None
        used = _number(value.get(used_key), 100)
        duration = _number(value.get(duration_key), 525600)
        reset = _number(value.get(reset_key))
        if used is None or duration is None or duration == 0 or reset is None:
            return None
        windows.append(UsageWindow(duration, used, reset))
    credits = limits.get("credits")
    safe_credits = None
    if isinstance(credits, dict):
        safe_credits = {}
        for original, target in (
            ("hasCredits" if camel_case else "has_credits", "has_credits"),
            ("unlimited", "unlimited"),
        ):
            if isinstance(credits.get(original), bool):
                safe_credits[target] = credits[original]
        balance = credits.get("balance")
        if isinstance(balance, str) and re.fullmatch(
            r"\d{1,20}(?:\.\d{1,8})?", balance
        ):
            safe_credits["balance"] = balance
        elif (
            isinstance(balance, (int, float))
            and not isinstance(balance, bool)
            and math.isfinite(balance)
            and balance >= 0
        ):
            safe_credits["balance"] = balance
    return UsageObservation(
        "codex",
        observed,
        tuple(windows),
        _metadata(limits.get("limitId" if camel_case else "limit_id")),
        _metadata(limits.get("limitName" if camel_case else "limit_name")),
        safe_credits,
        source,
    )


def parse_claude_statusline(
    value: object, observed_at: object
) -> UsageObservation | None:
    observed = _number(observed_at)
    if not isinstance(value, dict) or observed is None:
        return None
    limits = value.get("rate_limits")
    if not isinstance(limits, dict):
        return None
    windows = []
    for key, duration in (("five_hour", 300), ("seven_day", 10080)):
        window = limits.get(key)
        if window is None:
            continue
        if not isinstance(window, dict):
            return None
        used = _number(window.get("used_percentage"), 100)
        reset = _number(window.get("resets_at"))
        if used is None or reset is None:
            return None
        windows.append(UsageWindow(duration, used, reset))
    return UsageObservation(
        "claude", observed, tuple(windows), source=CLAUDE_STATUSLINE_SOURCE
    )


_MONTHS = {
    "jan": 1,
    "feb": 2,
    "mar": 3,
    "apr": 4,
    "may": 5,
    "jun": 6,
    "jul": 7,
    "aug": 8,
    "sep": 9,
    "oct": 10,
    "nov": 11,
    "dec": 12,
}
_MONTH_PATTERN = "|".join(_MONTHS)
_CLAUDE_PERCENT = re.compile(r"(\d{1,3}(?:\.\d+)?)\s*%")
_CLAUDE_DATE = re.compile(
    rf"\b({_MONTH_PATTERN})\s+(\d{{1,2}})(?:st|nd|rd|th)?\b", re.I
)
_CLAUDE_CLOCK = re.compile(r"\b(\d{1,2})(?::(\d{2}))?\s*(am|pm)\b", re.I)
_CLAUDE_24H_CLOCK = re.compile(r"\b(\d{1,2}):(\d{2})\b")
_CLAUDE_WEEKDAY = re.compile(
    r"\b(mon(?:day)?|tue(?:sday)?|wed(?:nesday)?|thu(?:rsday)?|fri(?:day)?|sat(?:urday)?|sun(?:day)?)\b",
    re.I,
)
_CLAUDE_ZONE = re.compile(r"\(([^()]{1,64})\)")
_CLAUDE_WEEK = re.compile(r"\bweek(?:ly)?\b|seven[- ]day|7[- ]day", re.I)
_CLAUDE_SESSION = re.compile(r"\bsession\b|(?:five|5)[ -]?(?:hour|hr|h)\b", re.I)
_CLAUDE_ALL_MODELS = re.compile(r"all[ -]models?", re.I)
_CLAUDE_MODEL = re.compile(
    r"\b(?:week(?:ly)?|seven[- ]day|7[- ]day)\s*\(\s*([^)]{1,64})\s*\)", re.I
)


def _model_label(value: str) -> str | None:
    label = (
        re.sub(r"[^\w .-]+", "", value)
        .strip(" .-")
        .encode("utf-8")[:32]
        .decode("utf-8", "ignore")
    )
    return label or None


def parse_claude_reset(value: object, observed_at: object) -> float | None:
    """Turn 'Oct 28 at 4:39am (America/New_York)' into Unix seconds.

    The text has no year, so the next occurrence after the observation wins.
    An invalid or unknown zone is not guessed; the caller treats it as unknown.
    """
    observed = _number(observed_at)
    if not isinstance(value, str) or observed is None:
        return None
    try:
        date = _CLAUDE_DATE.search(value)
        clock = _CLAUDE_CLOCK.search(value)
        if clock is None:
            clock = _CLAUDE_24H_CLOCK.search(value)
        if clock is None:
            return None
        hour = int(clock.group(1))
        minute = int(clock.group(2) or 0)
        suffix = clock.group(3).casefold() if clock.re == _CLAUDE_CLOCK else ""
        if not 0 <= minute <= 59 or not (
            1 <= hour <= 12 if suffix else 0 <= hour <= 23
        ):
            return None
        if suffix == "pm" and hour != 12:
            hour += 12
        elif suffix == "am" and hour == 12:
            hour = 0
        zone = None
        for name in reversed(_CLAUDE_ZONE.findall(value)):
            try:
                zone = ZoneInfo(name.strip())
            except (ValueError, ZoneInfoNotFoundError):
                continue
            break
        if zone is None:
            return None
        base = datetime.fromtimestamp(observed, UTC).astimezone(zone)
        if date:
            dates = [
                (year, _MONTHS[date.group(1).casefold()], int(date.group(2)))
                for year in range(base.year, base.year + 9)
            ]
        else:
            weekday = _CLAUDE_WEEKDAY.search(value)
            if weekday:
                day_index = ("mon", "tue", "wed", "thu", "fri", "sat", "sun").index(
                    weekday[1][:3].casefold()
                )
                offset = (day_index - base.weekday()) % 7
                days = [
                    base + timedelta(days=offset),
                    base + timedelta(days=offset + 7),
                ]
            elif re.search(r"\btomorrow\b", value, re.I):
                days = [base + timedelta(days=1)]
            elif re.search(r"\btoday\b", value, re.I):
                days = [base]
            else:
                days = [base, base + timedelta(days=1)]
            dates = [(day.year, day.month, day.day) for day in days]
        for year, month, day in dates:
            try:
                candidate = datetime(year, month, day, hour, minute, tzinfo=zone)
            except ValueError:
                continue
            for timestamp in sorted(
                {candidate.replace(fold=fold).timestamp() for fold in (0, 1)}
            ):
                # A skipped wall time during spring DST is not a real reset.
                local = datetime.fromtimestamp(timestamp, zone)
                if (local.year, local.month, local.day, local.hour, local.minute) != (
                    year,
                    month,
                    day,
                    hour,
                    minute,
                ):
                    continue
                if timestamp > observed:
                    return timestamp
        return None
    except (ValueError, OverflowError, OSError, ZoneInfoNotFoundError):
        return None


def parse_claude_usage_output(
    value: object, observed_at: object
) -> UsageObservation | None:
    """Parse `claude -p /usage` text without trusting line order or wording.

    A malformed line is skipped, never fatal: one reworded provider line must
    not discard the usable windows on the others. No text is retained.
    """
    observed = _number(observed_at)
    if not isinstance(value, str) or observed is None:
        return None
    # Claude can return cached bars after an endpoint failure. Their fetch age
    # is not machine-readable, so this poll must not renew quota freshness.
    if re.search(
        r"(?:showing\s+)?last[- ]known usage|usage (?:endpoint|request).*rate limited",
        value,
        re.I,
    ):
        return None
    windows = []
    seen = set()
    try:
        sections = []
        section = ""
        clean = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", value)
        for raw_line in clean.splitlines():
            line = raw_line.strip()
            if not line:
                continue
            if _CLAUDE_WEEK.search(line) or _CLAUDE_SESSION.search(line):
                if section:
                    sections.append(section)
                section = line
            elif section and (
                "reset" in line.casefold() or _CLAUDE_PERCENT.search(line)
            ):
                section += " " + line
        if section:
            sections.append(section)
        for line in sections:
            if _CLAUDE_WEEK.search(line):
                model = None
                if not _CLAUDE_ALL_MODELS.search(line):
                    match = _CLAUDE_MODEL.search(line)
                    if match is not None:
                        model = _model_label(match.group(1))
                        if model is None:
                            continue
                duration, label = 10080, model
            elif _CLAUDE_SESSION.search(line):
                duration, label = 300, None
            else:
                continue
            percent = _CLAUDE_PERCENT.search(line)
            if percent is None:
                continue
            try:
                used = _number(float(percent.group(1)), 100)
            except ValueError:
                continue
            reset = parse_claude_reset(line, observed)
            if used is not None and re.match(
                r"\s*(?:remaining|left)\b", line[percent.end() :], re.I
            ):
                used = 100 - used
            if used is None or reset is None:
                continue
            key = (duration, label or "")
            if key in seen:
                continue
            seen.add(key)
            windows.append(UsageWindow(duration, used, reset, label))
            if len(windows) >= MAX_CLAUDE_USAGE_WINDOWS:
                break
    except (TypeError, ValueError, OverflowError, OSError, ZoneInfoNotFoundError):
        return None
    if not windows:
        return None
    return UsageObservation(
        "claude", observed, tuple(windows), source=CLAUDE_USAGE_SOURCE
    )


# A stable alias for callers that name the command rather than its output.
parse_claude_usage = parse_claude_usage_output


def claude_usage_path(data_dir: Path) -> Path:
    return Path(data_dir) / "agents" / CLAUDE_USAGE_FILENAME


def _private_directory(path: Path, *, create: bool) -> int:
    if create:
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    metadata = os.fstat(descriptor)
    if metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
        os.close(descriptor)
        raise ValueError("Usage directory must be private")
    return descriptor


def write_claude_statusline(
    data_dir: Path, payload: object, *, now: float | None = None
) -> None:
    """Atomically save only sanitized quotas, never statusline/session input.

    A missing rate_limits field is not an observation. In particular, a newly
    opened session must not erase a reading received from another session.
    """
    observed = time.time() if now is None else now
    observation = parse_claude_statusline(payload, observed)
    if _number(observed) is None:
        raise ValueError("Invalid observation time")
    if observation is None:
        return
    limits = None
    if observation is not None:
        limits = {}
        for window in observation.windows:
            key = "five_hour" if window.minutes == 300 else "seven_day"
            limits[key] = {
                "used_percentage": window.used_percent,
                "resets_at": window.resets_at,
            }
    sanitized = {"observed_at": observed, "rate_limits": limits}
    data_dir = Path(data_dir)
    root_descriptor = _private_directory(data_dir, create=True)
    try:
        try:
            os.mkdir("agents", mode=0o700, dir_fd=root_descriptor)
        except FileExistsError:
            pass
        descriptor = os.open(
            "agents",
            os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
            dir_fd=root_descriptor,
        )
    finally:
        os.close(root_descriptor)
    temporary = f".claude-usage-{secrets.token_hex(8)}.tmp"
    try:
        metadata = os.fstat(descriptor)
        if metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
            raise ValueError("Usage directory must be private")
        file_descriptor = os.open(
            temporary,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
            0o600,
            dir_fd=descriptor,
        )
        with os.fdopen(file_descriptor, "w", encoding="utf-8") as stream:
            json.dump(sanitized, stream, separators=(",", ":"), allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(
            temporary,
            CLAUDE_USAGE_FILENAME,
            src_dir_fd=descriptor,
            dst_dir_fd=descriptor,
        )
    finally:
        try:
            os.unlink(temporary, dir_fd=descriptor)
        except FileNotFoundError:
            pass
        os.close(descriptor)


def read_claude_usage(path: Path) -> UsageObservation | None:
    """Reject symlinks, shared files and malformed state without exposing input."""
    descriptor = None
    try:
        descriptor = _private_directory(Path(path).parent, create=False)
        file_descriptor = os.open(
            Path(path).name,
            os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
            dir_fd=descriptor,
        )
        with os.fdopen(file_descriptor, "r", encoding="utf-8") as stream:
            metadata = os.fstat(stream.fileno())
            if (
                not stat.S_ISREG(metadata.st_mode)
                or metadata.st_uid != os.getuid()
                or stat.S_IMODE(metadata.st_mode) != 0o600
                or metadata.st_size > MAX_STATUSLINE_BYTES
            ):
                return None
            value = json.loads(stream.read(MAX_STATUSLINE_BYTES + 1))
        if not isinstance(value, dict):
            return None
        return parse_claude_statusline(value, value.get("observed_at"))
    except (OSError, ValueError, UnicodeError, RecursionError):
        return None
    finally:
        if descriptor is not None:
            os.close(descriptor)


def resolve_claude_binary() -> Path | None:
    """Resolve the provider CLI without a shell; PATH wins over the user install."""
    found = shutil.which("claude")
    candidates = [Path(found)] if found else []
    candidates.append(Path.home() / ".local" / "bin" / "claude")
    for candidate in candidates:
        try:
            if candidate.is_file() and os.access(candidate, os.X_OK):
                return candidate
        except OSError:
            continue
    return None


async def _stop_process(process: asyncio.subprocess.Process) -> None:
    if process.returncode is None:
        with contextlib.suppress(ProcessLookupError):
            process.kill()
    with contextlib.suppress(ProcessLookupError):
        await process.wait()


async def poll_claude_usage(
    *,
    binary: Path | str | None = None,
    timeout_seconds: float = CLAUDE_USAGE_TIMEOUT,
    now: float | None = None,
) -> UsageObservation | None:
    """Run `claude -p /usage` off the event loop, returning only parsed numbers.

    A missing binary, timeout, non-zero exit, oversized output, or unrecognized
    text all collapse to None. The child's stdout/stderr are never logged.
    """
    observed = time.time() if now is None else now
    if (
        not math.isfinite(timeout_seconds)
        or timeout_seconds <= 0
        or _number(observed) is None
    ):
        return None
    executable = Path(binary) if binary is not None else resolve_claude_binary()
    if executable is None:
        return None
    process = None
    try:
        process = await asyncio.create_subprocess_exec(
            str(executable),
            "-p",
            "/usage",
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
            limit=MAX_CLAUDE_USAGE_BYTES + 1,
        )
        chunks = []
        size = 0
        async with asyncio.timeout(timeout_seconds):
            while True:
                chunk = await process.stdout.read(65536)
                if not chunk:
                    break
                size += len(chunk)
                if size > MAX_CLAUDE_USAGE_BYTES:
                    await _stop_process(process)
                    return None
                chunks.append(chunk)
            await process.wait()
        if process.returncode != 0:
            return None
        text = b"".join(chunks).decode("utf-8", errors="replace")
        return parse_claude_usage_output(text, observed)
    except (TimeoutError, OSError, ValueError):
        if process is not None:
            await _stop_process(process)
        return None
    except asyncio.CancelledError:
        if process is not None:
            await _stop_process(process)
        raise


class ClaudeUsagePoller:
    """Best-effort usage polling with failure backoff and no provider calls.

    The first poll happens immediately; successful polls wait at least the
    five-minute interval, and consecutive failures double the wait up to an
    hourly cap. Tests may shorten the intervals, but production never does.
    """

    def __init__(
        self,
        collector: UsageCollector,
        *,
        binary: Path | str | None = None,
        interval: float = CLAUDE_USAGE_INTERVAL,
        timeout: float = CLAUDE_USAGE_TIMEOUT,
        max_backoff: float = CLAUDE_USAGE_MAX_BACKOFF,
        now: Callable[[], float] | None = None,
    ):
        if (
            not math.isfinite(interval)
            or interval <= 0
            or not math.isfinite(timeout)
            or timeout <= 0
            or not math.isfinite(max_backoff)
            or max_backoff < interval
        ):
            raise ValueError("Invalid Claude usage polling interval")
        self.collector = collector
        self.binary = binary
        self.interval = interval
        self.timeout = timeout
        self.max_backoff = max_backoff
        self.now = now or time.time

    def failure_delay(self, failures: int) -> float:
        if failures <= 0:
            return self.interval
        return min(self.interval * 2 ** min(failures - 1, 16), self.max_backoff)

    async def poll_once(self) -> bool:
        observation = await poll_claude_usage(
            binary=self.binary, timeout_seconds=self.timeout, now=self.now()
        )
        if observation is None:
            self.collector.fail_claude_usage()
            return False
        self.collector.observe_claude_usage(observation)
        return True

    async def run(self) -> None:
        failures = 0
        while True:
            if await self.poll_once():
                failures = 0
                delay = self.interval
            else:
                failures += 1
                delay = self.failure_delay(failures)
            await asyncio.sleep(delay)


def _has_positive_credit_balance(credits: dict[str, Any] | None) -> bool:
    if not isinstance(credits, dict):
        return False
    balance = credits.get("balance")
    if balance is not None:
        try:
            return float(balance) > 0
        except (ValueError, TypeError, OverflowError):
            return False
    return False


class UsageCollector:
    def __init__(self, claude_file: Path | None = None):
        self.claude_file = claude_file
        self.observations: dict[str, UsageObservation] = {}
        self._claude_sources: dict[str, UsageObservation] = {}
        # Statusline reads run in a worker thread while /usage polling runs on
        # the event loop; the two channels update the same selected reading.
        self._claude_lock = threading.Lock()

    def observe(self, observation: UsageObservation | None) -> bool:
        """Accept an already normalized immutable provider observation."""
        if observation is None or observation.observed_at > time.time() + 300:
            return False
        previous = self.observations.get(observation.provider)
        if previous and previous.observed_at > observation.observed_at:
            return False
        self.observations[observation.provider] = observation
        return previous != observation

    def _sync_claude(self, before: UsageObservation | None) -> bool:
        candidates = tuple(self._claude_sources.values())
        current = (
            max(
                candidates,
                key=lambda value: (
                    value.observed_at,
                    value.source == CLAUDE_USAGE_SOURCE,
                ),
            )
            if candidates
            else None
        )
        if current is None:
            self.observations.pop("claude", None)
        else:
            self.observations["claude"] = current
        return before != current

    def _set_claude(self, source: str, observation: UsageObservation | None) -> bool:
        # Reject clock-skewed input before newest-wins arbitration. Otherwise
        # one future timestamp erases a good reading and blocks valid updates
        # until that timestamp is reached.
        if observation is not None and observation.observed_at > time.time() + 300:
            return False
        with self._claude_lock:
            before = self.observations.get("claude")
            if observation is None:
                self._claude_sources.pop(source, None)
            else:
                previous = self._claude_sources.get(source)
                if previous and previous.observed_at > observation.observed_at:
                    return False
                self._claude_sources[source] = observation
            return self._sync_claude(before)

    def observe_claude_usage(self, observation: UsageObservation | None) -> bool:
        if (
            observation is None
            or observation.provider != "claude"
            or observation.source != CLAUDE_USAGE_SOURCE
        ):
            return False
        return self._set_claude(CLAUDE_USAGE_SOURCE, observation)

    def fail_claude_usage(self) -> bool:
        """Drop only the /usage reading; a fresher statusline may still be known."""
        return self._set_claude(CLAUDE_USAGE_SOURCE, None)

    def observe_codex(self, record: object) -> bool:
        if not isinstance(record, dict) or record.get("type") != "event_msg":
            return False
        payload = record.get("payload")
        if not isinstance(payload, dict) or payload.get("type") != "token_count":
            return False
        limits = payload.get("rate_limits")
        if isinstance(limits, dict) and limits.get("limit_id") not in (None, "codex"):
            return False
        return self.observe(
            parse_codex_rate_limits(
                limits,
                observation_time(record.get("timestamp")),
                source="rollout",
            )
        )

    def observe_app_server(
        self, payload: object, observed_at: float | None = None
    ) -> bool:
        if not isinstance(payload, dict):
            return False
        # Prefer the explicit Codex bucket; do not merge unrelated metered limits.
        buckets = payload.get("rateLimitsByLimitId")
        limits = buckets.get("codex") if isinstance(buckets, dict) else None
        if limits is None:
            limits = payload.get("rateLimits")
        if isinstance(limits, dict) and limits.get("limitId") not in (None, "codex"):
            return False
        return self.observe(
            parse_codex_rate_limits(
                limits,
                time.time() if observed_at is None else observed_at,
                camel_case=True,
                source="app-server",
            )
        )

    def refresh_claude(self) -> bool:
        observation = read_claude_usage(self.claude_file) if self.claude_file else None
        return self._set_claude(CLAUDE_STATUSLINE_SOURCE, observation)

    def snapshot(self, now: float | None = None) -> dict:
        now = time.time() if now is None else now
        windows = []
        unknown = []
        for provider in ("claude", "codex"):
            observation = self.observations.get(provider)
            if observation is None or observation.observed_at > now + 300:
                unknown.append(provider)
                continue
            for window in observation.windows:
                if window.resets_at <= now:
                    continue
                on_credits = (
                    provider == "codex"
                    and window.used_percent >= 100
                    and _has_positive_credit_balance(observation.credits)
                )
                entry = {
                    "provider": provider,
                    "label": window.label or window_label(window.minutes),
                    "used_percent": window.used_percent,
                    "resets_at": window.resets_at,
                    "observed_at": observation.observed_at,
                    "stale": now - observation.observed_at > STALE_AFTER,
                    "source": observation.source or "unknown",
                }
                if on_credits:
                    entry["on_credits"] = True
                windows.append(entry)
        return {"windows": windows, "unknown": unknown}
