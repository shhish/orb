"""Spoken-reply wire vectors; no TTS provider, microphone, network or live hub."""

import asyncio
import struct

import pytest

from orb_hub.envelope import LinkError
from orb_hub.speech import SpeechSender


async def chunks(*values):
    for value in values:
        yield value


class Sink:
    def __init__(self):
        self.events = []
        self.frames = []
        self.sender = SpeechSender(self.notify, self.audio)
        self.sender.configure({"formats": ["pcm_s16le"], "rates": [16000, 24000], "prebuffer_ms": 150}, voice=True)

    async def notify(self, name, params):
        self.events.append((name, params))

    async def audio(self, stream, frame):
        self.frames.append((stream, frame))


async def test_wire_and_pacing():
    sink = Sink()
    stream = sink.sender.start(chunks(b"\x00\x01" * 320, b"\x02\x03" * 320), rate=16000, text="Caption")
    async with asyncio.timeout(1):
        await sink.sender._task
    assert stream == 1
    assert sink.events == [
        ("voice.speak", {"stream": 1, "rate": 16000, "format": "pcm_s16le", "text": "Caption"}),
        ("voice.speak.end", {"stream": 1, "chunks": 2}),
    ]
    assert sink.frames == [
        (1, struct.pack(">BHI", 2, 1, 0) + b"\x00\x01" * 320),
        (1, struct.pack(">BHI", 2, 1, 1) + b"\x02\x03" * 320),
    ]
    await sink.sender.close()


async def test_local_stop_cancels_provider_and_old_streams_are_ignored():
    sink = Sink()
    ready = asyncio.Event()
    cancelled = asyncio.Event()

    async def producer():
        try:
            yield b"\x00\x00" * 320
            ready.set()
            await asyncio.Event().wait()
        finally:
            cancelled.set()

    sink.sender.start(producer(), rate=24000, text="Caption")
    async with asyncio.timeout(1):
        await ready.wait()
    sink.sender.stopped({"stream": 0, "reason": "talk"})
    sink.sender.stopped({"stream": 1, "reason": []})
    assert sink.sender.stream == 1
    sink.sender.stopped({"stream": 1, "reason": "talk"})
    async with asyncio.timeout(1):
        await cancelled.wait()
    assert len(sink.frames) == 1 and sink.sender.last_reason == "talk"
    await sink.sender.close()


async def test_formats_bounds_and_empty_stream_fail_closed():
    sink = Sink()
    with pytest.raises(LinkError):
        sink.sender.start(chunks(b"\0\0"), rate=32000, text="Caption")
    with pytest.raises(LinkError):
        sink.sender.start(chunks(b"\0\0"), rate=16000, text="x" * 1025)
    for values in [(), ("not PCM",), (b"x",), (b"x" * 1026,)]:
        stream = sink.sender.start(chunks(*values), rate=16000, text="Caption")
        await sink.sender._task
        assert sink.events[-1] == ("voice.speak.stop", {"stream": stream})
    sink.sender.configure({"formats": ["adpcm"], "rates": [16000], "prebuffer_ms": 150}, voice=True)
    assert not sink.sender.rates
    await sink.sender.close()


async def test_confirmed_voice_name_is_optional_and_bounded():
    sink = Sink()
    sink.sender.start(chunks(b"\0\0" * 160), rate=16000, text="Caption", voice="Ava")
    await sink.sender._task
    assert sink.events[0][1]["voice"] == "Ava"
    for value in ("", "x" * 65, "Ava\n", 12, "é" * 33):
        with pytest.raises(LinkError, match="voice name"):
            sink.sender.start(chunks(b"\0\0" * 160), rate=16000, text="Caption", voice=value)
    sink.sender.start(chunks(b"\0\0" * 160), rate=16000, text="Caption")
    await sink.sender._task
    assert "voice" not in sink.events[-2][1]
    await sink.sender.close()
