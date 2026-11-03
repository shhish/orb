"""Bounded spoken-reply wire sender. The caller supplies Mac-rendered PCM.

This module deliberately does not choose a voice, start a process, or touch an
owner session. Link.speech.start() is the integration point for reply providers.
"""

import asyncio
import struct
from collections.abc import AsyncIterable, Awaitable, Callable

from .envelope import LinkError
from .lifecycle import LifecycleSupervisor

_STOP_REASONS = {"complete", "stop", "double_tap", "talk", "silent", "interrupted", "underrun", "overflow", "bad_audio"}


class SpeechSender:
    def __init__(
        self,
        notify: Callable[[str, dict], Awaitable[None]],
        audio: Callable[[int, bytes], Awaitable[None]],
    ):
        self.supervisor = LifecycleSupervisor("speech sender")
        self._notify = notify
        self._audio = audio
        self.rates: frozenset[int] = frozenset()
        self.stream = 0
        self._next_stream = 1
        self._task: asyncio.Task | None = None
        self.last_reason = ""

    def configure(self, descriptor: object, *, voice: bool) -> None:
        self.rates = frozenset()
        if not voice or not isinstance(descriptor, dict):
            return
        rates = descriptor.get("rates")
        if (
            descriptor.get("formats") == ["pcm_s16le"]
            and isinstance(rates, list)
            and rates
            and all(type(rate) is int and rate in (16000, 24000) for rate in rates)
            and descriptor.get("prebuffer_ms") == 150
        ):
            self.rates = frozenset(rates)

    def start(self, chunks: AsyncIterable[bytes], *, rate: int, text: str, voice: str | None = None) -> int:
        if type(rate) is not int or rate not in self.rates:
            raise LinkError("unsupported", "Spoken replies unavailable")
        if not isinstance(text, str) or not 1 <= len(text.encode("utf-8")) <= 1024:
            raise LinkError("bad_request", "Invalid speech caption")
        if voice is not None and (
            not isinstance(voice, str)
            or not 1 <= len(voice.encode("utf-8")) <= 64
            or any(ord(char) < 32 or ord(char) == 127 for char in voice)
        ):
            raise LinkError("bad_request", "Invalid speech voice name")
        if self._next_stream > 65535:
            raise LinkError("busy", "Reconnect before more speech")
        if self._task:
            # Task cancellation cannot emit another old chunk after the newer
            # begin. Link checks stream again at the actual binary write.
            self._task.cancel()
        stream = self._next_stream
        self._next_stream += 1
        self.stream = stream
        self.last_reason = ""
        self._task = self.supervisor.spawn(self._run(stream, chunks, rate, text, voice), name="orb-speech-send")
        self._task.add_done_callback(self._consume)
        return stream

    @staticmethod
    def _consume(task: asyncio.Task) -> None:
        if not task.cancelled():
            task.exception()

    async def _run(self, stream: int, chunks: AsyncIterable[bytes], rate: int, text: str, voice: str | None) -> None:
        sequence = 0
        loop = asyncio.get_running_loop()
        try:
            descriptor = {"stream": stream, "rate": rate, "format": "pcm_s16le", "text": text}
            if voice is not None:
                descriptor["voice"] = voice
            await self._notify("voice.speak", descriptor)
            deadline = loop.time()
            async for chunk in chunks:
                if not isinstance(chunk, bytes):
                    raise TypeError("PCM chunk must be bytes")
                if not chunk or len(chunk) > 1024 or len(chunk) % 2:
                    raise ValueError("PCM chunk must contain 1..512 complete samples")
                if stream != self.stream:
                    return
                # One packet's lookahead only. No whole-answer queue and no
                # catch-up burst after a slow TTS producer/network write.
                await asyncio.sleep(max(0.0, deadline - loop.time()))
                await self._audio(stream, struct.pack(">BHI", 2, stream, sequence) + chunk)
                deadline = loop.time() + len(chunk) / (2 * rate)
                sequence += 1
                if sequence > 0xFFFFFFFF:
                    raise ValueError("Speech stream is too long")
            if not sequence:
                raise ValueError("Empty speech")
            if stream == self.stream:
                await self._notify("voice.speak.end", {"stream": stream, "chunks": sequence})
        except asyncio.CancelledError:
            raise
        except Exception:  # noqa: BLE001 - all producer failures must stop device playback without exposing text.
            if stream == self.stream:
                self.last_reason = "bad_audio"
                try:
                    await self._notify("voice.speak.stop", {"stream": stream})
                except (ConnectionError, TimeoutError):
                    pass

    def stopped(self, params: dict) -> bool:
        if (
            set(params) != {"stream", "reason"}
            or type(params["stream"]) is not int
            or params["stream"] != self.stream
            or not isinstance(params["reason"], str)
            or params["reason"] not in _STOP_REASONS
        ):
            return False
        self.last_reason = params["reason"]
        self.stream = 0
        if self._task:
            self._task.cancel()
        return True

    async def close(self) -> None:
        self.stream = 0
        if self._task:
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
            self._task = None
        await self.supervisor.stop()
        self.rates = frozenset()
