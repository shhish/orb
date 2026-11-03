"""Unit tests for speech synthesis, voice selection, audio chunking, and PCM production."""

import asyncio
import io
import math
import struct
import time
import wave
from pathlib import Path

import pytest

from orb_hub.speech_synth import (
    SpeechCache,
    SpeechError,
    SpeechService,
    VoiceInfo,
    chunk_pcm,
    parse_voices,
    sanitize_speech_text,
    select_natural_voice,
    strip_markdown_and_code,
)


def generate_pcm_tone(freq: float = 440.0, rate: int = 16000, duration: float = 0.1) -> bytes:
    """Generates mono 16-bit signed little-endian PCM sine tone."""
    samples = []
    total_samples = int(rate * duration)
    for i in range(total_samples):
        val = int(16000 * math.sin(2 * math.pi * freq * (i / rate)))
        samples.append(max(-32768, min(32767, val)))
    return struct.pack(f"<{len(samples)}h", *samples)


def make_wav_bytes(pcm: bytes, rate: int = 16000) -> bytes:
    """Wraps raw PCM bytes in a valid WAV container."""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm)
    return buf.getvalue()


# -- Text sanitisation & limits


def test_strip_markdown_fenced_code_blocks():
    text = "Here is code:\n```python\nimport sys\nprint('hello')\n```\nAll done."
    cleaned = strip_markdown_and_code(text)
    assert "import sys" not in cleaned
    assert "print('hello')" not in cleaned
    assert "Here is code: All done." in cleaned


def test_strip_markdown_inline_code_and_formatting():
    text = "# Title\nUse the `say` command with **bold** and *italic* and ~~deleted~~ words."
    cleaned = strip_markdown_and_code(text)
    assert "#" not in cleaned
    assert "`" not in cleaned
    assert "*" not in cleaned
    assert "~" not in cleaned
    assert "Use the say command with bold and italic and deleted words." == cleaned


def test_strip_markdown_links_images_quotes_html():
    text = (
        "> Important note\n"
        "Look at ![Orb photo](https://example.com/orb.png) and "
        "[documentation](https://docs.orb.internal).\n"
        "<div>HTML tag</div>"
    )
    cleaned = strip_markdown_and_code(text)
    assert "Important note" in cleaned
    assert "Look at and documentation. HTML tag" in cleaned
    assert "https://" not in cleaned
    assert "<" not in cleaned


def test_text_length_limit():
    short_text = "A" * 100
    assert sanitize_speech_text(short_text) == short_text

    long_text = "Sentence one. " * 40  # ~560 chars
    sanitized = sanitize_speech_text(long_text, limit=400)
    assert len(sanitized) <= 400
    assert sanitized.endswith(".")

    words_text = "word " * 100  # 500 chars
    sanitized_words = sanitize_speech_text(words_text, limit=400)
    assert len(sanitized_words) <= 400
    assert not sanitized_words.endswith(" ")


# -- Voice parsing & deterministic natural voice selection


def test_parse_voices():
    sample_output = """
Ava (Premium)       en_US    # Hello! My name is Ava.
Samantha            en_US    # Hello, my name is Samantha.
Alex                en_US    # Most people recognize me.
Thomas              fr_FR    # Bonjour, je m'appelle Thomas.
Anna                de_DE    # Hallo, ich heisse Anna.
"""
    voices = parse_voices(sample_output)
    assert len(voices) == 5
    assert voices[0].name == "Ava (Premium)"
    assert voices[0].lang == "en_US"
    assert voices[0].sample == "Hello! My name is Ava."
    assert voices[3].name == "Thomas"
    assert voices[3].lang == "fr_FR"


def test_select_natural_voice_prefers_ava_premium():
    voices = [
        VoiceInfo("Alex", "en_US"),
        VoiceInfo("Samantha", "en_US"),
        VoiceInfo("Ava (Premium)", "en_US"),
    ]
    assert select_natural_voice(voices) == "Ava (Premium)"


def test_select_natural_voice_fallback_preferences():
    # Without Ava (Premium), picks Samantha (Premium) if present
    voices = [
        VoiceInfo("Alex", "en_US"),
        VoiceInfo("Samantha (Premium)", "en_US"),
        VoiceInfo("Daniel", "en_GB"),
    ]
    assert select_natural_voice(voices) == "Samantha (Premium)"


def test_select_natural_voice_fallback_english_alphabetical():
    # When no preferred voice is present, pick first English voice alphabetically
    voices = [
        VoiceInfo("Zoe", "en_US"),
        VoiceInfo("Beth", "en_GB"),
        VoiceInfo("Thomas", "fr_FR"),
    ]
    assert select_natural_voice(voices) == "Beth"


def test_select_natural_voice_empty_fallback():
    assert select_natural_voice([]) == "Ava (Premium)"


# -- PCM Chunking Interface


def test_chunk_pcm_16000():
    pcm = generate_pcm_tone(440.0, rate=16000, duration=0.1)  # 100 ms = 3200 bytes
    chunks = chunk_pcm(pcm, chunk_size=800)

    # 3200 bytes total -> 4 chunks of 800 bytes
    assert len(chunks) == 4
    for chunk in chunks:
        assert len(chunk) == 800
        assert len(chunk) % 2 == 0
        assert len(chunk) <= 1024


def test_chunk_pcm_24000():
    pcm = generate_pcm_tone(440.0, rate=24000, duration=0.06)  # 60 ms = 2880 bytes
    # 20 ms at 24 kHz = 480 samples = 960 bytes per chunk
    chunks = chunk_pcm(pcm, chunk_size=960)
    assert len(chunks) == 3
    for chunk in chunks:
        assert len(chunk) == 960
        assert len(chunk) % 2 == 0
        assert len(chunk) <= 1024


def test_chunk_pcm_odd_and_empty():
    assert chunk_pcm(b"") == []

    # Odd length byte sequence should be padded by a zero byte
    odd_pcm = b"\x01\x02\x03"
    chunks = chunk_pcm(odd_pcm, chunk_size=800)
    assert len(chunks) == 1
    assert len(chunks[0]) == 4
    assert chunks[0] == b"\x01\x02\x03\x00"


def test_chunk_pcm_invalid_size():
    pcm = b"\x00\x01" * 10
    with pytest.raises(ValueError, match="chunk_size must be positive"):
        chunk_pcm(pcm, chunk_size=0)
    with pytest.raises(ValueError, match="chunk_size must be positive"):
        chunk_pcm(pcm, chunk_size=1025)
    with pytest.raises(ValueError, match="chunk_size must be positive"):
        chunk_pcm(pcm, chunk_size=801)


# -- SpeechCache


def test_speech_cache_ttl_and_lru():
    cache = SpeechCache(ttl=0.1, max_entries=2)
    b1 = b"pcm1"
    b2 = b"pcm2"
    b3 = b"pcm3"

    cache.put("k1", b1)
    cache.put("k2", b2)
    assert cache.get("k1") == b1
    assert cache.get("k2") == b2

    # Exceed capacity: k1 was older, should be evicted when k3 is added
    cache.put("k3", b3)
    assert cache.get("k3") == b3
    assert len(cache._entries) <= 2

    # Test TTL expiration
    time.sleep(0.15)
    assert cache.get("k3") is None


# -- SpeechService Subprocess & Error Handling


@pytest.mark.asyncio
async def test_speech_service_render_pcm_and_caching():
    pcm_data = generate_pcm_tone(440.0, rate=16000, duration=0.05)
    wav_data = make_wav_bytes(pcm_data, rate=16000)

    called_commands = []

    async def fake_runner(cmd):
        called_commands.append(cmd)
        if cmd[0] == "say" and "-v" in cmd and "?" in cmd:
            return 0, b"Ava (Premium)  en_US  # Natural voice", b""
        if cmd[0] == "say":
            out_file = cmd[cmd.index("-o") + 1]
            await asyncio.to_thread(Path(out_file).write_bytes, b"AIFF_DATA")
            return 0, b"", b""
        if cmd[0] == "afconvert":
            out_wav = cmd[-1]
            await asyncio.to_thread(Path(out_wav).write_bytes, wav_data)
            return 0, b"", b""
        return 1, b"", b"unknown command"

    service = SpeechService(runner=fake_runner)
    await service.start()
    assert service._selected_voice == "Ava (Premium)"

    # Synthesise text into PCM stream
    caption, stream = await service.render_pcm("Hello from Orb **companion**!", rate=16000)
    assert caption == "Hello from Orb companion!"
    chunks = [c async for c in stream]
    assert len(chunks) > 0
    for c in chunks:
        assert isinstance(c, bytes)
        assert len(c) <= 1024
        assert len(c) % 2 == 0
    assert b"".join(chunks) == pcm_data

    initial_call_count = len(called_commands)

    # Second synthesis of same text must hit cache without invoking runner
    cached_caption, cached_stream = await service.render_pcm("Hello from Orb **companion**!", rate=16000)
    assert cached_caption == caption
    cached_chunks = [c async for c in cached_stream]
    assert cached_chunks == chunks
    assert len(called_commands) == initial_call_count

    # Also test synthesize_pcm convenience method
    text2, chunks2 = await service.synthesize_pcm("Hello from Orb **companion**!", rate=16000)
    assert text2 == caption
    assert chunks2 == chunks

    await service.stop()


@pytest.mark.asyncio
async def test_speech_service_say_error():
    async def failing_say_runner(cmd):
        if cmd[0] == "say" and "?" in cmd:
            return 0, b"Ava (Premium)  en_US", b""
        if cmd[0] == "say":
            return 1, b"", b"voice not found"
        return 0, b"", b""

    service = SpeechService(runner=failing_say_runner)
    await service.start()
    with pytest.raises(SpeechError, match="speech synthesis failed"):
        await service.render_pcm("Test failure")
    await service.stop()


@pytest.mark.asyncio
async def test_speech_service_afconvert_error():
    async def failing_afconvert_runner(cmd):
        if cmd[0] == "say" and "?" in cmd:
            return 0, b"Ava (Premium)  en_US", b""
        if cmd[0] == "say":
            out_file = cmd[cmd.index("-o") + 1]
            await asyncio.to_thread(Path(out_file).write_bytes, b"AIFF")
            return 0, b"", b""
        if cmd[0] == "afconvert":
            return 1, b"", b"unsupported format"
        return 0, b"", b""

    service = SpeechService(runner=failing_afconvert_runner)
    await service.start()
    with pytest.raises(SpeechError, match="audio conversion with afconvert failed"):
        await service.render_pcm("Test afconvert error")
    await service.stop()


@pytest.mark.parametrize(
    "block",
    [
        "```python\nprint('private')",
        "~~~python\nprint('private')\n~~~",
        "~~~python\nprint('private')",
        "    print('private')",
        "\tprint('private')",
    ],
)
def test_spoken_reply_omits_every_code_block_form(block):
    assert sanitize_speech_text(f"Check complete.\n{block}") == "Check complete."


async def test_speech_text_cannot_become_a_say_option():
    calls = []

    async def runner(cmd):
        calls.append(cmd)
        if cmd[0] == "say":
            await asyncio.to_thread(Path(cmd[cmd.index("-o") + 1]).write_bytes, b"TEST-AIFF")
        else:
            await asyncio.to_thread(Path(cmd[-1]).write_bytes, make_wav_bytes(b"\0\0" * 160))
        return 0, b"", b""

    service = SpeechService(runner=runner)
    await service.render_pcm("--input-file=/private/example.txt")
    assert calls[0][-2:] == ["--", "--input-file=/private/example.txt"]


async def test_speech_rejects_conversion_with_wrong_sample_rate():
    async def runner(cmd):
        if cmd[0] == "say":
            await asyncio.to_thread(Path(cmd[cmd.index("-o") + 1]).write_bytes, b"TEST-AIFF")
        else:
            await asyncio.to_thread(Path(cmd[-1]).write_bytes, make_wav_bytes(b"\0\0" * 160, rate=16000))
        return 0, b"", b""

    service = SpeechService(runner=runner)
    with pytest.raises(SpeechError, match="sample rate"):
        await service.render_pcm("Check complete.", rate=24000)
    assert not service.cache._entries
