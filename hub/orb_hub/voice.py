"""Bounded, link-private PCM capture and deferred transcription (protocol v1.1)."""

import asyncio
import importlib.util
import inspect
import logging
import math
import os
import re
import sys
from array import array
from collections.abc import Awaitable, Callable, Hashable
from dataclasses import dataclass, field
from pathlib import Path

from orb_hub.envelope import LinkError
from orb_hub.lifecycle import LifecycleSupervisor

SAMPLE_RATE = 16_000
MAX_SECONDS = 60
MAX_PCM_BYTES = SAMPLE_RATE * 2 * MAX_SECONDS
MAX_FRAME_BYTES = 1024
MAX_STREAMS = 4
MAX_TEXT_BYTES = 280
MODEL_REPO = "mlx-community/whisper-large-v3-turbo"
VAD_FRAME_SAMPLES = SAMPLE_RATE // 40  # 25 ms, independent of transport frame boundaries.
VAD_MIN_RMS = 0.005  # Normalized PCM16, approximately -46 dBFS.
VAD_NOISE_RATIO = 2.5
VAD_MIN_SAMPLES = SAMPLE_RATE // 10  # Ignore clicks/bursts shorter than 100 ms.
VAD_PADDING_SAMPLES = SAMPLE_RATE // 5  # Keep quiet consonants around detected activity.
NO_SPEECH_THRESHOLD = 0.6
LOGPROB_THRESHOLD = -1.0
COMPRESSION_RATIO_THRESHOLD = 2.4

logger = logging.getLogger(__name__)

Emit = Callable[[str, dict], Awaitable[None]]
Transcriber = Callable[[bytes], str | Awaitable[str]]


def _prepare_pcm(pcm: bytes) -> bytes:
    """Gate and trim PCM by energy; this is a noise gate, not a speech classifier."""
    samples = array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    energies = []
    square_sum = 0
    peak = 0
    for start in range(0, len(samples), VAD_FRAME_SAMPLES):
        frame = samples[start : start + VAD_FRAME_SAMPLES]
        squares = sum(sample * sample for sample in frame)
        square_sum += squares
        peak = max(peak, max(abs(sample) for sample in frame))
        # Remove DC from activity energy so a biased microphone cannot count as speech.
        variance = max(0.0, squares / len(frame) - (sum(frame) / len(frame)) ** 2)
        energies.append(math.sqrt(variance) / 32768.0)
    if not energies:
        return b""
    # The quietest decile estimates the background. A relative threshold also
    # rejects steady fan/hiss energy even when it exceeds the absolute floor.
    noise_rms = sorted(energies)[(len(energies) - 1) // 10]
    threshold = max(VAD_MIN_RMS, noise_rms * VAD_NOISE_RATIO)
    active = [index for index, rms in enumerate(energies) if rms >= threshold]
    active_samples = sum(min(VAD_FRAME_SAMPLES, len(samples) - i * VAD_FRAME_SAMPLES) for i in active)
    logger.info(
        "Voice levels rms=%.6f peak=%.6f noise_rms=%.6f threshold_rms=%.6f active_fraction=%.6f active_ms=%.1f",
        math.sqrt(square_sum / len(samples)) / 32768.0,
        peak / 32768.0,
        noise_rms,
        threshold,
        active_samples / len(samples),
        active_samples * 1000 / SAMPLE_RATE,
    )
    if active_samples < VAD_MIN_SAMPLES:
        return b""
    first = max(0, active[0] * VAD_FRAME_SAMPLES - VAD_PADDING_SAMPLES)
    last = min(len(samples), (active[-1] + 1) * VAD_FRAME_SAMPLES + VAD_PADDING_SAMPLES)
    return pcm[first * 2 : last * 2]


def cached_model() -> Path | None:
    """Find complete local weights without asking Hugging Face to resolve/download."""
    override = os.environ.get("ORB_WHISPER_MODEL")
    if override:
        candidates = [Path(override).expanduser()]
    else:
        cache = Path(
            os.environ.get("HF_HUB_CACHE")
            or os.environ.get("HUGGINGFACE_HUB_CACHE")
            or str(Path(os.environ.get("HF_HOME", "~/.cache/huggingface")).expanduser() / "hub")
        ).expanduser()
        model = cache / "models--mlx-community--whisper-large-v3-turbo"
        candidates = []
        try:
            revision = (model / "refs" / "main").read_text().strip()
            if re.fullmatch(r"[0-9a-f]{40}", revision):
                candidates.append(model / "snapshots" / revision)
        except OSError:
            pass
        candidates.extend(sorted((model / "snapshots").glob("*"), reverse=True))
    for candidate in candidates:
        if (candidate / "config.json").is_file() and any(
            (candidate / name).is_file() for name in ("model.safetensors", "weights.safetensors", "weights.npz")
        ):
            return candidate.resolve()
    return None


class CachedWhisper:
    """Load only the optional local MLX runtime and cached large-v3-turbo model."""

    @property
    def ready(self) -> bool:
        return importlib.util.find_spec("mlx_whisper") is not None and cached_model() is not None

    def __call__(self, pcm: bytes) -> str:
        model = cached_model()
        if model is None:
            raise LinkError("unsupported", "Cached voice model is unavailable")
        try:
            import mlx_whisper
            import numpy as np
        except ImportError:
            raise LinkError("unsupported", "Install the hub voice extra to enable transcription") from None
        # A real local path prevents mlx-whisper's model loader taking its download path.
        audio = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
        language = os.environ.get("ORB_VOICE_LANGUAGE", "en").strip().lower() or "en"
        result = mlx_whisper.transcribe(
            audio,
            path_or_hf_repo=str(model),
            verbose=None,
            language=None if language == "auto" else language,
            condition_on_previous_text=False,
            temperature=(0.0, 0.2, 0.4, 0.6, 0.8, 1.0),
            no_speech_threshold=NO_SPEECH_THRESHOLD,
            logprob_threshold=LOGPROB_THRESHOLD,
            compression_ratio_threshold=COMPRESSION_RATIO_THRESHOLD,
        )
        # MLX's built-in silence skip can retain high-no_speech segments when
        # token confidence is high. Enforce each guard on the returned segments,
        # and never fall back to the unfiltered aggregate result["text"].
        return "".join(
            segment["text"]
            for segment in result["segments"]
            if all(math.isfinite(segment[key]) for key in ("no_speech_prob", "avg_logprob", "compression_ratio"))
            and segment["no_speech_prob"] <= NO_SPEECH_THRESHOLD
            and segment["avg_logprob"] >= LOGPROB_THRESHOLD
            and segment["compression_ratio"] <= COMPRESSION_RATIO_THRESHOLD
        ).strip()


@dataclass(eq=False)
class Recording:
    owner: Hashable
    stream: int
    target: str
    emit: Emit
    samples: bytearray = field(default_factory=bytearray)
    next_sequence: int = 0
    state: str = "capture"
    duration_s: float = 0
    text: str = ""
    cancelled: bool = False
    released: bool = False
    timer: asyncio.Task | None = None
    task: asyncio.Task | None = None


class VoiceService:
    def __init__(
        self,
        transcriber: Transcriber | None = None,
        *,
        capture_timeout: float = MAX_SECONDS,
        transcription_timeout: float = 120.0,
    ):
        self.supervisor = LifecycleSupervisor("voice")
        self.transcriber = transcriber if transcriber is not None else CachedWhisper()
        self.capture_timeout = min(capture_timeout, MAX_SECONDS)
        self.transcription_timeout = transcription_timeout
        self._records: dict[Hashable, Recording] = {}
        # A bitset makes stream-ID replay prevention constant-space per live link.
        self._used: dict[Hashable, bytearray] = {}
        self._jobs: set[asyncio.Task] = set()
        self._serial = asyncio.Lock()
        self._admitted = 0
        self._closed = False

    @property
    def ready(self) -> bool:
        return getattr(self.transcriber, "ready", True)

    @property
    def available(self) -> bool:
        """Negotiation readiness; checking it never imports MLX or loads weights."""
        return not self._closed and self.ready

    @property
    def active_count(self) -> int:
        return self._admitted

    @staticmethod
    def _stream(params: dict, keys: set[str]) -> int:
        if not isinstance(params, dict) or set(params) != keys:
            raise LinkError("bad_request", "Invalid voice fields")
        stream = params.get("stream")
        if isinstance(stream, bool) or not isinstance(stream, int) or not 1 <= stream <= 65535:
            raise LinkError("bad_request", "stream must be an integer from 1 to 65535")
        return stream

    def _record(self, owner: Hashable, stream: int) -> Recording:
        record = self._records.get(owner)
        if record is None or record.stream != stream or record.cancelled:
            raise LinkError("not_found", "Unknown voice stream")
        return record

    def _release(self, record: Recording) -> None:
        if not record.released:
            record.released = True
            self._admitted -= 1
        if self._records.get(record.owner) is record:
            self._records.pop(record.owner)
        record.samples.clear()
        if record.timer and record.timer is not asyncio.current_task():
            record.timer.cancel()
        record.timer = None

    async def begin(self, owner: Hashable, params: dict, emit: Emit) -> dict:
        stream = self._stream(params, {"stream", "target"})
        target = params["target"]
        if not isinstance(target, str) or not target or len(target.encode()) > 48:
            raise LinkError("bad_request", "target must contain 1 to 48 UTF-8 bytes")
        if self._closed:
            raise LinkError("unsupported", "Voice service is stopped")
        if not self.ready:
            raise LinkError("unsupported", "Local voice runtime or cached model is unavailable")
        used = self._used.get(owner)
        if used is not None and used[stream >> 3] & (1 << (stream & 7)):
            raise LinkError("conflict", "stream has already been used on this link")
        if owner in self._records or self._admitted >= MAX_STREAMS:
            raise LinkError("busy", "Voice recording capacity is full")
        used = self._used.setdefault(owner, bytearray(8192))
        used[stream >> 3] |= 1 << (stream & 7)
        record = Recording(owner, stream, target, emit)
        self._records[owner] = record
        self._admitted += 1
        record.timer = self.supervisor.spawn(self._capture_deadline(record), name="orb-voice-capture")
        return {}

    async def _capture_deadline(self, record: Recording) -> None:
        await asyncio.sleep(self.capture_timeout)
        if self._records.get(record.owner) is record and record.state == "capture":
            await self._error(record, "timeout", "Voice capture timed out")

    async def _error(self, record: Recording, code: str, msg: str) -> None:
        was_cancelled = record.cancelled
        record.cancelled = True
        # Native calls cannot be interrupted. Keep their admission until they return.
        if record.task is None:
            self._release(record)
        else:
            if self._records.get(record.owner) is record:
                self._records.pop(record.owner)
            record.samples.clear()
        if not self._closed and not was_cancelled:
            try:
                await record.emit("voice.error", {"stream": record.stream, "code": code, "msg": msg})
            except (ConnectionError, TimeoutError):
                pass

    async def frame(self, owner: Hashable, data: bytes) -> None:
        record = self._records.get(owner)
        if record is None:
            raise LinkError("not_found", "No active voice recording")
        if record.state != "capture":
            await self._error(record, "bad_request", "Audio arrived after voice.end")
            return
        if not isinstance(data, bytes) or len(data) < 9 or len(data) > 7 + MAX_FRAME_BYTES:
            await self._error(record, "bad_request", "Invalid audio frame size")
            return
        kind = data[0]
        stream = int.from_bytes(data[1:3], "big")
        sequence = int.from_bytes(data[3:7], "big")
        payload = data[7:]
        if kind != 1 or stream != record.stream or sequence != record.next_sequence or len(payload) % 2:
            await self._error(record, "bad_request", "Invalid audio header, sequence or PCM samples")
            return
        if len(record.samples) + len(payload) > MAX_PCM_BYTES:
            await self._error(record, "too_large", "Voice recording exceeds 60 seconds")
            return
        record.samples.extend(payload)
        record.next_sequence += 1

    async def end(self, owner: Hashable, params: dict) -> dict:
        stream = self._stream(params, {"stream"})
        record = self._record(owner, stream)
        if record.state != "capture":
            return {}
        if not record.samples:
            self._release(record)
            raise LinkError("bad_request", "Voice recording contains no PCM samples")
        if record.timer:
            record.timer.cancel()
            record.timer = None
        record.duration_s = len(record.samples) / (SAMPLE_RATE * 2)
        record.state = "transcribing"
        record.task = self.supervisor.spawn(self._transcribe(record), name="orb-voice-transcribe")
        self._jobs.add(record.task)
        record.task.add_done_callback(self._jobs.discard)
        return {}

    async def _invoke(self, pcm: bytes) -> str:
        # Energy analysis stays off the event loop and inside native job lifetime
        # accounting, just like transcription (including timeout/cancellation).
        pcm = await asyncio.to_thread(_prepare_pcm, pcm)
        if not pcm:
            return ""
        if inspect.iscoroutinefunction(self.transcriber) or inspect.iscoroutinefunction(self.transcriber.__call__):
            return await self.transcriber(pcm)
        result = await asyncio.to_thread(self.transcriber, pcm)
        if inspect.isawaitable(result):
            return await result
        return result

    async def _transcribe(self, record: Recording) -> None:
        try:
            async with self._serial:
                if record.cancelled:
                    return
                pcm = bytes(record.samples)
                record.samples.clear()
                native = self.supervisor.spawn(self._invoke(pcm), name="orb-voice-native")
                try:
                    text = await asyncio.wait_for(asyncio.shield(native), self.transcription_timeout)
                except TimeoutError:
                    await self._error(record, "timeout", "Voice transcription timed out")
                    # Do not free serialization/admission while a native thread still runs.
                    await asyncio.gather(native, return_exceptions=True)
                    return
                if record.cancelled or self._closed:
                    return
                if not isinstance(text, str):
                    raise TypeError("invalid transcriber result")
                text = text.strip()
                if not text:
                    await self._error(record, "no_speech", "No speech was recognized. Please try speaking again.")
                    return
                if len(text.encode()) > MAX_TEXT_BYTES:
                    await self._error(record, "too_large", "Transcript exceeds 280 UTF-8 bytes")
                    return
                record.text = text
                record.state = "result"
                await record.emit("voice.result", {"stream": record.stream, "text": text})
        except LinkError as exc:
            await self._error(record, exc.code, "Local voice transcription is unavailable")
        except Exception:  # noqa: BLE001 - suppress all provider errors without exposing private audio/text.
            # Provider exception strings may contain text, paths or credentials.
            await self._error(record, "internal", "Voice transcription failed")
        finally:
            if record.state != "result" or record.cancelled or self._closed:
                self._release(record)
            record.task = None

    async def cancel(self, owner: Hashable, params: dict) -> dict:
        stream = self._stream(params, {"stream"})
        record = self._records.get(owner)
        if record is None:
            used = self._used.get(owner)
            if used is not None and used[stream >> 3] & (1 << (stream & 7)):
                return {}
            raise LinkError("not_found", "Unknown voice stream")
        if record.stream != stream:
            raise LinkError("not_found", "Unknown voice stream")
        record.cancelled = True
        self._records.pop(owner, None)
        record.samples.clear()
        if record.task is None:
            self._release(record)
        return {}

    def result_pending(self, owner: Hashable, stream: int) -> bool:
        """Validate a queued result at the transport boundary, after any awaits."""
        record = self._records.get(owner)
        return (
            record is not None
            and record.stream == stream
            and record.state == "result"
            and not record.cancelled
            and not self._closed
        )

    def confirmed(self, owner: Hashable, stream: int, target: str, text: str) -> float:
        if isinstance(stream, bool) or not isinstance(stream, int):
            raise LinkError("bad_request", "Invalid confirmation stream")
        record = self._record(owner, stream)
        if record.state != "result" or record.target != target or record.text != text:
            raise LinkError("conflict", "Confirmation does not match the voice result")
        return record.duration_s

    def consume(self, owner: Hashable, stream: int) -> None:
        # Provider delivery awaits I/O. The owner may cancel that recording or
        # begin another while the write completes; its accepted receipt remains
        # true, and consuming the old result must not discard the new capture.
        record = self._records.get(owner)
        if record is None or record.stream != stream:
            used = self._used.get(owner)
            if (
                isinstance(stream, int)
                and not isinstance(stream, bool)
                and 1 <= stream <= 65535
                and used is not None
                and used[stream >> 3] & (1 << (stream & 7))
            ):
                return
        record = self._record(owner, stream)
        if record.state != "result":
            raise LinkError("conflict", "Voice result is not ready")
        self._release(record)

    async def disconnect(self, owner: Hashable) -> None:
        record = self._records.get(owner)
        if record:
            await self.cancel(owner, {"stream": record.stream})
        self._used.pop(owner, None)

    async def close(self) -> None:
        self._closed = True
        for owner in list(self._records):
            await self.disconnect(owner)
        self._used.clear()
        if self._jobs:
            # The MLX call owns its memory until it exits; shutdown joins it.
            await asyncio.gather(*self._jobs, return_exceptions=True)
        await self.supervisor.stop()
