"""Spoken replies: synthesise speech via macOS 'say', convert via 'afconvert', and produce raw PCM16 chunks.

This module provides speech synthesis for Orb:
- Sanitises reply text: strips code blocks, markdown markers, HTML, and limits to 400 characters.
- Chooses a natural voice: defaults to 'Ava (Premium)' if installed, else the best installed
  English voice picked deterministically from 'say -v ?'.
- Synthesises to a temporary audio file using 'say', converts to mono PCM16 at 16000 Hz or 24000 Hz
  using 'afconvert', and chunks into even-sized frames of at most 1024 bytes (e.g. 800 bytes at 16 kHz,
  960 bytes at 24 kHz).
- Feeds raw PCM16 chunks as an async iterable to Link.speech.start(source, rate=rate, text=caption).
- Caches synthesized PCM audio by (text_hash, voice, rate) for 10 minutes in a bounded cache (max 64 entries).
- Runs all subprocesses asynchronously via asyncio without blocking the event loop.
"""

import asyncio
import hashlib
import logging
import re
import shutil
import tempfile
import time
import wave
from collections.abc import AsyncIterator, Callable
from dataclasses import dataclass
from pathlib import Path

log = logging.getLogger(__name__)

DEFAULT_SAMPLE_RATE = 16000
SUPPORTED_SAMPLE_RATES = (16000, 24000)
MAX_TEXT_LENGTH = 400
MAX_CAPTION_BYTES = 1024
COMMAND_TIMEOUT_SECONDS = 60.0
CACHE_TTL_SECONDS = 600.0  # 10 minutes
MAX_CACHE_ENTRIES = 64

PREFERRED_VOICES = (
    "Ava (Premium)",
    "Ava (Enhanced)",
    "Ava",
    "Samantha (Premium)",
    "Samantha (Enhanced)",
    "Samantha",
    "Allison (Premium)",
    "Allison (Enhanced)",
    "Allison",
    "Siri",
    "Daniel (Enhanced)",
    "Daniel",
    "Oliver (Enhanced)",
    "Oliver",
    "Alex",
    "Victoria",
    "Fred",
)


class SpeechError(Exception):
    """Raised when speech synthesis or audio conversion fails."""


@dataclass(frozen=True)
class VoiceInfo:
    """Installed macOS voice description."""

    name: str
    lang: str
    sample: str = ""


def strip_markdown_and_code(text: str) -> str:
    """Strips markdown code blocks, backticks, links, headers, formatting and HTML tags."""
    if not text:
        return ""
    # A reply can stop mid-fence. Never turn unfinished or alternate Markdown
    # code into speech while retaining the prose before/after a closed block.
    text = re.sub(r"```.*?(?:```|$)|~~~.*?(?:~~~|$)", " ", text, flags=re.DOTALL)
    text = re.sub(r"(?m)^(?: {4}|\t).*$", " ", text)
    # Strip inline code backticks, retaining content
    text = re.sub(r"`([^`]+)`", r"\1", text)
    # Strip markdown images
    text = re.sub(r"!\[([^\]]*)\]\([^)]+\)", "", text)
    # Strip markdown links, preserving anchor text
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    # Strip headers completely
    text = re.sub(r"^\s*#{1,6}\s+.*$", "", text, flags=re.MULTILINE)
    # Strip bold / italic / strikethrough delimiters
    text = re.sub(r"(\*\*|__|\*|_|~~)(.*?)\1", r"\2", text)
    # Strip blockquotes
    text = re.sub(r"^\s*>\s+", "", text, flags=re.MULTILINE)
    # Strip list item bullets
    text = re.sub(r"^\s*[-*+]\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"^\s*\d+\.\s+", "", text, flags=re.MULTILINE)
    # Strip HTML tags
    text = re.sub(r"<[^>]+>", "", text)
    # Normalize whitespace
    return re.sub(r"\s+", " ", text).strip()


def sanitize_speech_text(text: str, limit: int = MAX_TEXT_LENGTH) -> str:
    """Keep speech and its caption identical within both text and wire limits."""
    cleaned = strip_markdown_and_code(text)
    if len(cleaned) <= limit and len(cleaned.encode("utf-8")) <= MAX_CAPTION_BYTES:
        return cleaned

    truncated = cleaned[:limit].encode("utf-8")[:MAX_CAPTION_BYTES].decode("utf-8", "ignore")
    limit = len(truncated)
    # Look for sentence boundary (.!?) in the second half
    for sep in (". ", "! ", "? "):
        idx = truncated.rfind(sep)
        if idx > limit // 2:
            return truncated[: idx + 1].strip()

    # Look for word boundary in second half
    idx = truncated.rfind(" ")
    if idx > limit // 2:
        return truncated[:idx].strip()

    return truncated.strip()


clean_speech_text = sanitize_speech_text


def parse_voices(raw_output: str) -> list[VoiceInfo]:
    """Parses output lines from 'say -v ?' into VoiceInfo entries."""
    voices: list[VoiceInfo] = []
    for line in raw_output.strip().splitlines():
        line = line.strip()
        if not line:
            continue
        # Format: Name    lang_LANG   # Sample text
        sample = ""
        if "#" in line:
            parts = line.split("#", 1)
            line = parts[0].strip()
            sample = parts[1].strip()
        tokens = line.split()
        if len(tokens) >= 2:
            lang = tokens[-1]
            name = " ".join(tokens[:-1])
            voices.append(VoiceInfo(name=name, lang=lang, sample=sample))
    return voices


def select_natural_voice(voices: list[VoiceInfo]) -> str:
    """Selects the best installed natural voice deterministically.

    Defaults to 'Ava (Premium)' if installed, otherwise checks preferred
    high-quality voices, falls back to the first English voice sorted alphabetically,
    or the first available voice.
    """
    installed_names = {v.name: v for v in voices}
    if "Ava (Premium)" in installed_names:
        return "Ava (Premium)"

    for preferred in PREFERRED_VOICES:
        if preferred in installed_names:
            return preferred

    # Filter for English voices (e.g. en_US, en_GB, en)
    english_voices = [v.name for v in voices if v.lang.lower().startswith("en")]
    if english_voices:
        return min(english_voices, key=lambda s: s.lower())

    # Fallback to any installed voice sorted
    if voices:
        return min((v.name for v in voices), key=lambda s: s.lower())

    return "Ava (Premium)"


def chunk_pcm(pcm: bytes, chunk_size: int = 800) -> list[bytes]:
    """Splits raw mono PCM16LE audio into even-sized chunks of at most 1024 bytes."""
    if chunk_size <= 0 or chunk_size > 1024 or chunk_size % 2 != 0:
        raise ValueError(f"chunk_size must be positive, even, and <= 1024 bytes; got {chunk_size}")
    if not pcm:
        return []
    chunks: list[bytes] = []
    total_bytes = len(pcm)
    for start in range(0, total_bytes, chunk_size):
        chunk = pcm[start : start + chunk_size]
        if len(chunk) % 2 != 0:
            chunk += b"\x00"
        chunks.append(chunk)
    return chunks


class SpeechCache:
    """Brief in-memory LRU/TTL cache for synthesized raw PCM bytes."""

    def __init__(self, ttl: float = CACHE_TTL_SECONDS, max_entries: int = MAX_CACHE_ENTRIES):
        self.ttl = ttl
        self.max_entries = max_entries
        self._entries: dict[str, tuple[float, bytes]] = {}

    def get(self, key: str) -> bytes | None:
        now = time.monotonic()
        if key in self._entries:
            timestamp, pcm = self._entries[key]
            if now - timestamp <= self.ttl:
                return pcm
            del self._entries[key]
        return None

    def put(self, key: str, pcm: bytes) -> None:
        self.prune()
        if len(self._entries) >= self.max_entries:
            oldest_key = min(self._entries.keys(), key=lambda k: self._entries[k][0])
            del self._entries[oldest_key]
        self._entries[key] = (time.monotonic(), pcm)

    def prune(self) -> None:
        now = time.monotonic()
        expired = [k for k, (t, _) in self._entries.items() if now - t > self.ttl]
        for k in expired:
            del self._entries[k]

    def clear(self) -> None:
        self._entries.clear()


async def _run_command(cmd: list[str]) -> tuple[int, bytes, bytes]:
    """Runs a subprocess asynchronously and returns (code, stdout, stderr)."""
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        async with asyncio.timeout(COMMAND_TIMEOUT_SECONDS):
            stdout, stderr = await proc.communicate()
    except (asyncio.CancelledError, TimeoutError) as exc:
        # Cancelling communicate does not stop say/afconvert. Reap the child
        # before its temporary files disappear or a replacement reply starts.
        if proc.returncode is None:
            try:
                proc.kill()
            except ProcessLookupError:
                pass
        await proc.wait()
        if isinstance(exc, TimeoutError):
            raise SpeechError("speech subprocess timed out") from None
        raise
    return proc.returncode or 0, stdout, stderr


class SpeechService:
    """Speech synthesis service wrapping 'say' and 'afconvert' with caching and PCM chunking."""

    def __init__(
        self,
        runner: Callable[[list[str]], asyncio.Future] | None = None,
        available: bool | None = None,
    ):
        self._runner = runner or _run_command
        self._available = available
        self.cache = SpeechCache()
        self._selected_voice: str | None = None

    @property
    def available(self) -> bool:
        """Checks if the required macOS binaries 'say' and 'afconvert' are present."""
        if self._available is not None:
            return self._available
        return shutil.which("say") is not None and shutil.which("afconvert") is not None

    @available.setter
    def available(self, value: bool) -> None:
        self._available = value

    @property
    def selected_voice(self) -> str:
        """The actual default voice used by render_pcm, suitable for captions."""
        return self._selected_voice or "Ava (Premium)"

    async def start(self) -> None:
        """Initialises the speech service and resolves the natural voice."""
        try:
            self._selected_voice = await self.resolve_voice()
        except (SpeechError, OSError) as exc:
            log.warning("could not resolve system voice: %s", exc)
            self._selected_voice = "Ava (Premium)"

    async def stop(self) -> None:
        """Cleans up service state and clears cache."""
        self.cache.clear()

    async def list_installed_voices(self) -> list[VoiceInfo]:
        """Lists installed macOS speech voices via 'say -v ?'."""
        code, stdout, stderr = await self._runner(["say", "-v", "?"])
        if code != 0:
            raise SpeechError(f"'say -v ?' failed with code {code}: {stderr.decode('utf-8', 'replace')}")
        return parse_voices(stdout.decode("utf-8", "replace"))

    async def resolve_voice(self) -> str:
        """Selects the best natural English voice installed on this Mac."""
        try:
            voices = await self.list_installed_voices()
            return select_natural_voice(voices)
        except (SpeechError, OSError) as exc:
            log.info("falling back to default voice Ava (Premium): %s", exc)
            return "Ava (Premium)"

    async def synthesize_audio_pcm(self, text: str, voice: str | None = None, rate: int = DEFAULT_SAMPLE_RATE) -> bytes:
        """Synthesises spoken text to raw mono PCM16 at the given rate using 'say' and 'afconvert'."""
        if rate not in SUPPORTED_SAMPLE_RATES:
            raise ValueError(f"unsupported sample rate: {rate}; expected {SUPPORTED_SAMPLE_RATES}")

        voice = voice or self._selected_voice or "Ava (Premium)"

        with tempfile.TemporaryDirectory(prefix="orbspeech-") as tmpdir:
            aiff_path = Path(tmpdir) / "output.aiff"
            wav_path = Path(tmpdir) / "output.wav"

            # 1. Synthesise to AIFF with say
            # Provider text is data even when it starts with a say option such
            # as --input-file. It must not select a local file or audio device.
            say_cmd = ["say", "-v", voice, "-o", str(aiff_path), "--", text]
            code, _, stderr = await self._runner(say_cmd)
            if code != 0 or not aiff_path.is_file():
                msg = stderr.decode("utf-8", "replace").strip() or f"exit code {code}"
                raise SpeechError(f"speech synthesis failed with voice {voice!r}: {msg}")

            # 2. Convert to mono PCM16 at the target sample rate using afconvert
            afconvert_cmd = [
                "afconvert",
                "-f",
                "WAVE",
                "-d",
                f"LEI16@{rate}",
                "-c",
                "1",
                str(aiff_path),
                str(wav_path),
            ]
            code, _, stderr = await self._runner(afconvert_cmd)
            if code != 0 or not wav_path.is_file():
                msg = stderr.decode("utf-8", "replace").strip() or f"exit code {code}"
                raise SpeechError(f"audio conversion with afconvert failed: {msg}")

            # 3. Read raw PCM samples from the WAV container without blocking
            def _read_wav(path: str) -> bytes:
                with wave.open(path, "rb") as wf:
                    if wf.getnchannels() != 1:
                        raise SpeechError(f"expected mono audio, got {wf.getnchannels()} channels")
                    if wf.getsampwidth() != 2:
                        raise SpeechError(f"expected 16-bit PCM, got {wf.getsampwidth() * 8}-bit")
                    if wf.getframerate() != rate:
                        raise SpeechError("converted audio has an unexpected sample rate")
                    return wf.readframes(wf.getnframes())

            return await asyncio.to_thread(_read_wav, str(wav_path))

    async def render_pcm(
        self,
        raw_text: str,
        rate: int = DEFAULT_SAMPLE_RATE,
        voice: str | None = None,
        chunk_size: int | None = None,
    ) -> tuple[str, AsyncIterator[bytes]]:
        """Synthesises sanitized text into an async stream of raw PCM16 chunks for Link.speech.start().

        Returns (sanitized_caption_text, async_chunk_stream).
        """
        text = sanitize_speech_text(raw_text)
        if not text:
            raise SpeechError("speech text is empty after sanitization")
        if rate not in SUPPORTED_SAMPLE_RATES:
            raise ValueError(f"unsupported sample rate: {rate}; expected {SUPPORTED_SAMPLE_RATES}")

        selected_voice = voice or self._selected_voice or "Ava (Premium)"
        cache_key = hashlib.sha256(f"{text}:{selected_voice}:{rate}".encode()).hexdigest()

        pcm = self.cache.get(cache_key)
        if pcm is None:
            pcm = await self.synthesize_audio_pcm(text, voice=selected_voice, rate=rate)
            self.cache.put(cache_key, pcm)

        # 25 ms at 16 kHz is 800 bytes; 20 ms at 24 kHz is 960 bytes (<=1024 bytes)
        target_chunk_size = chunk_size or (960 if rate == 24000 else 800)
        chunks = chunk_pcm(pcm, chunk_size=target_chunk_size)

        async def _stream() -> AsyncIterator[bytes]:
            for c in chunks:
                yield c

        return text, _stream()

    async def synthesize_pcm(
        self,
        raw_text: str,
        rate: int = DEFAULT_SAMPLE_RATE,
        voice: str | None = None,
        chunk_size: int | None = None,
    ) -> tuple[str, list[bytes]]:
        """Synthesises sanitized text into a list of raw PCM16 chunks."""
        text, stream = await self.render_pcm(raw_text, rate=rate, voice=voice, chunk_size=chunk_size)
        chunks: list[bytes] = []
        async for c in stream:
            chunks.append(c)
        return text, chunks
