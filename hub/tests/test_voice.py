"""Voice tests use deterministic fake speech; no microphone, model, network or owner state."""

import asyncio
import logging
import math
import random
import struct
import sys
import threading
from types import SimpleNamespace

import pytest
from conftest import voice_pcm

from orb_hub.envelope import LinkError
from orb_hub.voice import (
    MAX_PCM_BYTES,
    MAX_STREAMS,
    SAMPLE_RATE,
    CachedWhisper,
    VoiceService,
    _prepare_pcm,
    cached_model,
)


def audio(stream=1, seq=0, payload=b"\x00\x01" * 320, kind=1):
    return struct.pack(">BHI", kind, stream, seq) + payload


class Sink:
    def __init__(self):
        self.messages = []
        self.changed = asyncio.Event()

    async def __call__(self, method, params):
        self.messages.append((method, params))
        self.changed.set()

    async def receive(self):
        async with asyncio.timeout(2):
            await self.changed.wait()
        return self.messages[-1]


@pytest.fixture
async def voice():
    service = VoiceService(lambda pcm: "Hello Orb")
    yield service
    await service.close()


async def capture(service, owner, stream=1, pcm=None):
    pcm = voice_pcm() if pcm is None else pcm
    for seq, start in enumerate(range(0, len(pcm), 640)):
        await service.frame(owner, audio(stream, seq, pcm[start : start + 640]))


async def finish(service, owner, sink, stream=1, pcm=None):
    await service.begin(owner, {"stream": stream, "target": "dot"}, sink)
    await capture(service, owner, stream, pcm)
    await service.end(owner, {"stream": stream})
    return await sink.receive()


async def test_roundtrip_is_private_and_requires_matching_confirmation(voice):
    owner, other, sink = object(), object(), Sink()
    assert await finish(voice, owner, sink) == ("voice.result", {"stream": 1, "text": "Hello Orb"})
    assert voice.confirmed(owner, 1, "dot", "Hello Orb") == 0.3
    for args in ((other, 1, "dot", "Hello Orb"), (owner, 1, "session", "Hello Orb"), (owner, 1, "dot", "Edited")):
        with pytest.raises(LinkError):
            voice.confirmed(*args)
    assert voice.active_count == 1
    voice.consume(owner, 1)
    assert voice.active_count == 0
    with pytest.raises(LinkError, match="not_found"):
        voice.confirmed(owner, 1, "dot", "Hello Orb")


async def test_accepted_send_survives_cancel_without_consuming_new_recording(voice, tmp_path):
    from orb_hub.link import Link
    from orb_hub.sessions import SessionHub

    sessions = SessionHub(tmp_path / "sessions")
    hub = SimpleNamespace(voice=voice, sessions=sessions)
    link = Link(hub, object(), "test-only", "127.0.0.1")
    link.device_id, link.caps = "orb-test", {"cards", "voice"}
    entered, release, sink = asyncio.Event(), asyncio.Event(), Sink()

    async def send(_device, _text, **_kwargs):
        entered.set()
        await release.wait()
        return {"accepted": True, "status": "queued", "reason": "Queued by test transport"}

    sessions.register_message_handler("dot", send)
    try:
        await finish(voice, link, sink)
        pending = asyncio.create_task(
            link._extension_request("message.send", {"stream": 1, "target": "dot", "text": "Hello Orb"})
        )
        async with asyncio.timeout(2):
            await entered.wait()
        await link._extension_request("voice.cancel", {"stream": 1})
        await voice.begin(link, {"stream": 2, "target": "dot"}, sink)
        release.set()
        assert (await pending)["status"] == "queued"
        assert voice._records[link].stream == 2
        assert voice.active_count == 1
        # Consumption is idempotent, while confirmation still rejects replay.
        voice.consume(link, 1)
        with pytest.raises(LinkError, match="not_found"):
            voice.confirmed(link, 1, "dot", "Hello Orb")
    finally:
        release.set()
        await sessions.close()


async def test_session_ends_before_send_keeps_result_for_explicit_cancel(voice, tmp_path):
    from test_sessions import session

    from orb_hub.link import Link
    from orb_hub.sessions import SessionHub

    sessions = SessionHub(tmp_path / "sessions")
    link = Link(SimpleNamespace(voice=voice, sessions=sessions), object(), "test-only", "127.0.0.1")
    link.device_id, link.caps = "orb-test", {"cards", "voice"}
    sink = Sink()
    await sessions.upsert_session(session("ended", can_message=True))
    await voice.begin(link, {"stream": 1, "target": "ended"}, sink)
    await capture(voice, link)
    await voice.end(link, {"stream": 1})
    await sink.receive()
    try:
        await sessions.remove_session("ended")
        with pytest.raises(LinkError, match="not_found"):
            await link._extension_request("message.send", {"stream": 1, "target": "ended", "text": "Hello Orb"})
        assert voice.confirmed(link, 1, "ended", "Hello Orb") == 0.3
        assert voice.active_count == 1
        await link._extension_request("voice.cancel", {"stream": 1})
        assert voice.active_count == 0
    finally:
        await sessions.close()


async def test_frames_preserve_pcm_order():
    received = []

    async def transcriber(pcm):
        received.append(pcm)
        return "Yes"

    service, sink, owner = VoiceService(transcriber), Sink(), object()
    try:
        await service.begin(owner, {"stream": 42, "target": "session"}, sink)
        await capture(service, owner, 42)
        assert await service.end(owner, {"stream": 42}) == {}
        await sink.receive()
        assert received == [voice_pcm()]
    finally:
        await service.close()


@pytest.mark.parametrize("stream", [0, 65536, -1, True, "1", None, 1.0])
async def test_reject_invalid_streams(voice, stream):
    with pytest.raises(LinkError, match="bad_request"):
        await voice.begin(object(), {"stream": stream, "target": "dot"}, Sink())
    assert voice.active_count == 0


@pytest.mark.parametrize("target", ["", "é" * 25, None, 2])
async def test_reject_invalid_targets(voice, target):
    with pytest.raises(LinkError, match="bad_request"):
        await voice.begin(object(), {"stream": 1, "target": target}, Sink())


@pytest.mark.parametrize("params", [{"stream": 1}, {"stream": 1, "target": "dot", "extra": 2}, []])
async def test_reject_unexpected_begin_fields(voice, params):
    with pytest.raises(LinkError, match="bad_request"):
        await voice.begin(object(), params, Sink())


@pytest.mark.parametrize(
    "frame",
    [
        b"",
        b"\x01" * 7,
        audio(kind=2),
        audio(stream=2),
        audio(seq=1),
        audio(payload=b"\x00"),
        audio(payload=b"\x00" * 1026),
    ],
)
async def test_invalid_audio_terminates_recording_with_error(voice, frame):
    sink, owner = Sink(), object()
    await voice.begin(owner, {"stream": 1, "target": "dot"}, sink)
    await voice.frame(owner, frame)
    assert sink.messages[-1][0] == "voice.error"
    assert sink.messages[-1][1]["code"] == "bad_request"
    assert voice.active_count == 0


async def test_repeated_sequence_is_rejected(voice):
    sink, owner = Sink(), object()
    await voice.begin(owner, {"stream": 1, "target": "dot"}, sink)
    await voice.frame(owner, audio())
    await voice.frame(owner, audio())
    assert sink.messages[-1][1]["code"] == "bad_request"
    assert voice.active_count == 0


async def test_audio_limit_does_not_allocate_excess(voice):
    sink, owner = Sink(), object()
    await voice.begin(owner, {"stream": 1, "target": "dot"}, sink)
    for seq in range(MAX_PCM_BYTES // 1024):
        await voice.frame(owner, audio(seq=seq, payload=b"\x00" * 1024))
    remaining = MAX_PCM_BYTES % 1024
    seq = MAX_PCM_BYTES // 1024
    if remaining:
        await voice.frame(owner, audio(seq=seq, payload=b"\x00" * remaining))
        seq += 1
    assert len(voice._records[owner].samples) == MAX_PCM_BYTES
    await voice.frame(owner, audio(seq=seq, payload=b"\x00\x00"))
    assert sink.messages[-1][1]["code"] == "too_large"
    assert voice.active_count == 0


async def test_admission_global_and_per_link_and_result_retention(voice):
    owners = [object() for _ in range(MAX_STREAMS + 1)]
    for owner in owners[:MAX_STREAMS]:
        await voice.begin(owner, {"stream": 1, "target": "dot"}, Sink())
    with pytest.raises(LinkError, match="busy"):
        await voice.begin(owners[-1], {"stream": 1, "target": "dot"}, Sink())
    with pytest.raises(LinkError, match="busy"):
        await voice.begin(owners[0], {"stream": 2, "target": "dot"}, Sink())
    await voice.cancel(owners[0], {"stream": 1})
    await voice.begin(owners[-1], {"stream": 1, "target": "dot"}, Sink())
    assert voice.active_count == MAX_STREAMS


async def test_capture_timeout_releases_pcm():
    service, owner, sink = VoiceService(lambda pcm: "Hi", capture_timeout=0.01), object(), Sink()
    try:
        await service.begin(owner, {"stream": 1, "target": "dot"}, sink)
        await service.frame(owner, audio())
        assert (await sink.receive())[1]["code"] == "timeout"
        assert service.active_count == 0
        assert owner not in service._records
    finally:
        await service.close()


async def test_cancel_and_disconnect_prevent_id_reuse_until_new_link(voice):
    owner, sink = object(), Sink()
    await voice.begin(owner, {"stream": 65535, "target": "dot"}, sink)
    await voice.cancel(owner, {"stream": 65535})
    assert await voice.cancel(owner, {"stream": 65535}) == {}
    with pytest.raises(LinkError, match="conflict"):
        await voice.begin(owner, {"stream": 65535, "target": "dot"}, sink)
    assert len(voice._used[owner]) == 8192
    await voice.disconnect(owner)
    assert owner not in voice._used
    await voice.begin(owner, {"stream": 65535, "target": "dot"}, sink)
    assert not sink.messages


async def test_end_empty_releases_capacity(voice):
    owner = object()
    await voice.begin(owner, {"stream": 1, "target": "dot"}, Sink())
    with pytest.raises(LinkError, match="bad_request"):
        await voice.end(owner, {"stream": 1})
    assert voice.active_count == 0


async def test_no_active_stream_frame_or_end(voice):
    with pytest.raises(LinkError, match="not_found"):
        await voice.frame(object(), audio())
    with pytest.raises(LinkError, match="not_found"):
        await voice.end(object(), {"stream": 1})
    with pytest.raises(LinkError, match="bad_request"):
        await voice.end(object(), {"stream": 1, "extra": True})


async def test_end_retry_does_not_start_second_transcriber():
    calls = []

    async def transcriber(pcm):
        calls.append(pcm)
        return "Hi"

    service, sink, owner = VoiceService(transcriber), Sink(), object()
    try:
        await service.begin(owner, {"stream": 1, "target": "dot"}, sink)
        await capture(service, owner)
        await service.end(owner, {"stream": 1})
        await service.end(owner, {"stream": 1})
        await sink.receive()
        await service.end(owner, {"stream": 1})
        assert len(calls) == 1
    finally:
        await service.close()


@pytest.mark.parametrize(
    "result,code", [("é" * 141, "too_large"), ("", "no_speech"), ("   ", "no_speech"), (None, "internal")]
)
async def test_invalid_transcription_fails_without_truncating(result, code):
    service, sink, owner = VoiceService(lambda pcm: result), Sink(), object()
    try:
        method, params = await finish(service, owner, sink)
        assert method == "voice.error"
        assert params["code"] == code
        assert service.active_count == 0
    finally:
        await service.close()


async def test_transcriber_exception_never_exposes_private_details():
    def fail(_pcm):
        raise RuntimeError("secret /home/example/private TRANSCRIPT")

    service, sink, owner = VoiceService(fail), Sink(), object()
    try:
        await finish(service, owner, sink)
        assert "secret" not in str(sink.messages)
        assert "/Users" not in str(sink.messages)
        assert sink.messages[-1][1]["code"] == "internal"
    finally:
        await service.close()


async def test_cancelled_native_job_holds_serialization_and_admission():
    entered, release = threading.Event(), threading.Event()
    calls = []

    def transcriber(pcm):
        calls.append(pcm)
        entered.set()
        release.wait(2)
        return "Hi"

    service, sink, first, second = VoiceService(transcriber), Sink(), object(), object()
    try:
        await service.begin(first, {"stream": 1, "target": "dot"}, sink)
        await capture(service, first)
        await service.end(first, {"stream": 1})
        assert await asyncio.to_thread(entered.wait, 1)
        await service.cancel(first, {"stream": 1})
        assert service.active_count == 1
        await service.begin(second, {"stream": 1, "target": "dot"}, Sink())
        await capture(service, second)
        await service.end(second, {"stream": 1})
        await asyncio.sleep(0.01)
        assert len(calls) == 1
        assert service.active_count == 2
        release.set()
        await asyncio.gather(*service._jobs)
        assert len(calls) == 2
        assert not sink.messages
        assert service.active_count == 1
    finally:
        release.set()
        await service.close()


async def test_native_timeout_stays_admitted_until_thread_returns():
    entered, release = threading.Event(), threading.Event()

    def transcriber(_pcm):
        entered.set()
        release.wait(2)
        return "Hi"

    service, sink, owner = VoiceService(transcriber, transcription_timeout=0.01), Sink(), object()
    try:
        await service.begin(owner, {"stream": 1, "target": "dot"}, sink)
        await capture(service, owner)
        await service.end(owner, {"stream": 1})
        assert await asyncio.to_thread(entered.wait, 1)
        assert (await sink.receive())[1]["code"] == "timeout"
        assert service.active_count == 1
        release.set()
        await asyncio.gather(*service._jobs)
        assert service.active_count == 0
        assert len(sink.messages) == 1
    finally:
        release.set()
        await service.close()


async def test_cancelled_provider_failure_cannot_remove_new_capture():
    started, release = asyncio.Event(), asyncio.Event()

    async def transcriber(_pcm):
        started.set()
        await release.wait()
        raise RuntimeError("private")

    service, sink, owner = VoiceService(transcriber), Sink(), object()
    try:
        await service.begin(owner, {"stream": 1, "target": "dot"}, sink)
        await capture(service, owner)
        await service.end(owner, {"stream": 1})
        await started.wait()
        await service.cancel(owner, {"stream": 1})
        await service.begin(owner, {"stream": 2, "target": "dot"}, sink)
        release.set()
        await asyncio.gather(*service._jobs)
        assert service._records[owner].stream == 2
        assert service.active_count == 1
        assert not sink.messages
    finally:
        release.set()
        await service.close()


async def test_closed_service_rejects_new_begin(voice):
    await voice.close()
    with pytest.raises(LinkError, match="unsupported"):
        await voice.begin(object(), {"stream": 1, "target": "dot"}, Sink())


def test_cached_model_finds_only_complete_local_snapshots(tmp_path, monkeypatch):
    monkeypatch.delenv("ORB_WHISPER_MODEL", raising=False)
    monkeypatch.setenv("HF_HUB_CACHE", str(tmp_path))
    model = tmp_path / "models--mlx-community--whisper-large-v3-turbo"
    revision = "a" * 40
    snapshot = model / "snapshots" / revision
    snapshot.mkdir(parents=True)
    (model / "refs").mkdir()
    (model / "refs" / "main").write_text(revision)
    (snapshot / "config.json").write_text("{}")
    assert cached_model() is None
    (snapshot / "weights.safetensors").write_bytes(b"TEST-ONLY")
    assert cached_model() == snapshot


def test_cached_model_explicit_override_does_not_fall_back_or_download(tmp_path, monkeypatch):
    monkeypatch.setenv("ORB_WHISPER_MODEL", str(tmp_path))
    assert cached_model() is None
    (tmp_path / "config.json").write_text("{}")
    (tmp_path / "weights.npz").write_bytes(b"TEST-ONLY")
    assert cached_model() == tmp_path


async def test_missing_optional_model_rejects_capture_without_consuming_id(voice, monkeypatch):
    class Unavailable:
        ready = False

    voice.transcriber = Unavailable()
    with pytest.raises(LinkError, match="unsupported"):
        await voice.begin(object(), {"stream": 1, "target": "dot"}, Sink())
    assert not voice._used
    assert voice.active_count == 0


def test_default_service_does_not_import_mlx():
    assert isinstance(VoiceService().transcriber, CachedWhisper)


def pcm16(samples):
    return struct.pack(f"<{len(samples)}h", *samples)


def noise(amplitude, seconds=4):
    rng = random.Random(42)
    return pcm16([rng.randint(-amplitude, amplitude) for _ in range(SAMPLE_RATE * seconds)])


@pytest.mark.parametrize(
    "pcm",
    [
        bytes(SAMPLE_RATE * 2 * 4),
        pcm16([2000] * SAMPLE_RATE),  # DC bias is not speech.
        noise(60),  # Quiet microphone hiss.
        noise(3000),  # Steady background energy above the absolute floor.
        bytes(SAMPLE_RATE) + pcm16([20000, -20000] * 160) + bytes(SAMPLE_RATE),  # A 20 ms click.
        voice_pcm()[1600:2400],  # A loud 25 ms partial recording is too short.
    ],
    ids=["four-second-silence", "dc-offset", "quiet-noise", "loud-stationary-noise", "click", "short-tone"],
)
async def test_no_activity_never_calls_transcriber_and_releases_capacity(pcm):
    calls = []

    def transcribe(audio):
        calls.append(audio)
        return "¡Gracias!"

    service, sink, owner = VoiceService(transcribe), Sink(), object()
    try:
        assert await finish(service, owner, sink, pcm=pcm) == (
            "voice.error",
            {"stream": 1, "code": "no_speech", "msg": "No speech was recognized. Please try speaking again."},
        )
        assert not calls
        assert service.active_count == 0
        assert not service.result_pending(owner, 1)
        await service.begin(owner, {"stream": 2, "target": "dot"}, sink)
    finally:
        await service.close()


async def test_speech_energy_trims_only_outer_silence_and_keeps_original_duration(caplog):
    calls = []
    speech = voice_pcm()
    # An internal pause must survive trimming, unlike the one-second quiet edges.
    middle = speech + bytes(SAMPLE_RATE) + speech
    pcm = bytes(SAMPLE_RATE * 2) + middle + bytes(SAMPLE_RATE * 2)
    service, sink, owner = VoiceService(lambda audio: calls.append(audio) or "PRIVATE TEXT"), Sink(), object()
    try:
        with caplog.at_level(logging.INFO, logger="orb_hub.voice"):
            assert (await finish(service, owner, sink, pcm=pcm))[0] == "voice.result"
        # 50 ms of quiet in the synthetic speech plus 150 ms from the outer quiet
        # gives the gate's 200 ms padding on either side.
        assert calls == [bytes(4800) + middle + bytes(4800)]
        assert service.confirmed(owner, 1, "dot", "PRIVATE TEXT") == len(pcm) / (2 * SAMPLE_RATE)
        records = [record for record in caplog.records if record.name == "orb_hub.voice"]
        assert len(records) == 1
        assert all(isinstance(value, (int, float)) for value in records[0].args)
        assert "PRIVATE" not in caplog.text
        assert "rms=" in caplog.text and "peak=" in caplog.text and "active_fraction=" in caplog.text
    finally:
        await service.close()


def test_level_measurements_handle_full_signed_pcm16_range(caplog):
    with caplog.at_level(logging.INFO, logger="orb_hub.voice"):
        assert _prepare_pcm(pcm16([-32768, 32767] * 400)) == b""
    rms, peak, *_ = caplog.records[-1].args
    assert rms == pytest.approx(math.sqrt((32768**2 + 32767**2) / 2) / 32768)
    assert peak == 1.0


@pytest.fixture
def fake_whisper(tmp_path, monkeypatch):
    state = SimpleNamespace(calls=[], result={"text": "UNFILTERED PRIVATE TEXT", "segments": []})

    def transcribe(audio, **options):
        state.calls.append((audio, options))
        return state.result

    monkeypatch.setitem(sys.modules, "mlx_whisper", SimpleNamespace(transcribe=transcribe))
    monkeypatch.setattr("orb_hub.voice.cached_model", lambda: tmp_path)
    monkeypatch.setattr(CachedWhisper, "ready", property(lambda self: True))
    monkeypatch.delenv("ORB_VOICE_LANGUAGE", raising=False)
    return state


def segment(text=" Hello Orb", **scores):
    return {"text": text, "no_speech_prob": 0.01, "avg_logprob": -0.2, "compression_ratio": 1.2, **scores}


@pytest.mark.parametrize("language,expected", [(None, "en"), ("auto", None), ("es", "es"), (" EN ", "en")])
def test_cached_whisper_uses_installed_api_options(fake_whisper, monkeypatch, tmp_path, language, expected):
    if language is not None:
        monkeypatch.setenv("ORB_VOICE_LANGUAGE", language)
    fake_whisper.result["segments"] = [segment()]
    assert CachedWhisper()(pcm16([-32768, 0, 32767])) == "Hello Orb"
    audio, options = fake_whisper.calls[0]
    assert audio.dtype.name == "float32"
    assert audio.tolist() == [-1.0, 0.0, 32767 / 32768]
    assert options == {
        "path_or_hf_repo": str(tmp_path),
        "verbose": None,
        "language": expected,
        "condition_on_previous_text": False,
        "temperature": (0.0, 0.2, 0.4, 0.6, 0.8, 1.0),
        "no_speech_threshold": 0.6,
        "logprob_threshold": -1.0,
        "compression_ratio_threshold": 2.4,
    }


@pytest.mark.parametrize(
    "segments",
    [
        [],
        [segment(" ¡Gracias!", no_speech_prob=0.95)],  # Even if token confidence is high.
        [segment(avg_logprob=-1.1)],
        [segment(compression_ratio=2.5)],
        [segment(no_speech_prob=float("nan"))],
        [segment(avg_logprob=float("inf"))],
        [segment(compression_ratio=float("nan"))],
        [segment("  ")],
    ],
    ids=[
        "empty",
        "silence-probability",
        "low-confidence",
        "repetition",
        "nan-prob",
        "inf-logprob",
        "nan-ratio",
        "blank",
    ],
)
async def test_all_hallucinated_segments_emit_no_speech(fake_whisper, segments, caplog):
    fake_whisper.result["segments"] = segments
    service, sink, owner = VoiceService(), Sink(), object()
    try:
        with caplog.at_level(logging.INFO, logger="orb_hub.voice"):
            method, params = await finish(service, owner, sink)
        assert method == "voice.error" and params["code"] == "no_speech"
        assert service.active_count == 0
        assert len(fake_whisper.calls) == 1
        assert "Gracias" not in caplog.text and "PRIVATE" not in caplog.text
    finally:
        await service.close()


async def test_only_reliable_segments_reach_confirmation(fake_whisper):
    fake_whisper.result["segments"] = [
        segment(" ¡Gracias!", no_speech_prob=0.9),
        segment(" Hello"),
        segment(" repeated spam", compression_ratio=3.0),
        segment(" low confidence", avg_logprob=-2.0),
        segment(" Orb", no_speech_prob=0.6, avg_logprob=-1.0, compression_ratio=2.4),
    ]
    service, sink, owner = VoiceService(), Sink(), object()
    try:
        assert await finish(service, owner, sink) == ("voice.result", {"stream": 1, "text": "Hello Orb"})
        assert service.confirmed(owner, 1, "dot", "Hello Orb") == 0.3
    finally:
        await service.close()
