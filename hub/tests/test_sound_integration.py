"""Integration tests for speech and nowplaying capabilities, CLI flags, Link RPCs, and speech triggers."""

import asyncio
import io
import wave
from pathlib import Path
from unittest.mock import AsyncMock, MagicMock

import pytest

from orb_hub.cli import build_parser
from orb_hub.config import Config
from orb_hub.envelope import Envelope
from orb_hub.link import Link
from orb_hub.nowplaying import NowPlayingService
from orb_hub.server import Hub
from orb_hub.sessions import SessionHub
from orb_hub.speech_synth import SpeechService


def test_cli_flags_and_config():
    parser = build_parser()
    args = parser.parse_args(["serve", "--speech", "--nowplaying"])
    assert args.speech is True
    assert args.nowplaying is True

    cfg = Config.load(speech_enabled=args.speech, nowplaying_enabled=args.nowplaying)
    assert cfg.speech_enabled is True
    assert cfg.nowplaying_enabled is True

    # Off by default
    args_default = parser.parse_args(["serve"])
    assert args_default.speech is False
    assert args_default.nowplaying is False
    cfg_default = Config.load()
    assert cfg_default.speech_enabled is False
    assert cfg_default.nowplaying_enabled is False


@pytest.mark.asyncio
async def test_link_negotiation_capabilities():
    class FakeHub:
        def __init__(self, speech_on=True, nowplaying_on=True):
            self.cfg = Config.load(hub_name="Test Hub", tz="UTC")
            self.speech = SpeechService() if speech_on else None
            self.nowplaying = NowPlayingService() if nowplaying_on else None
            self.home = None
            self.voice = MagicMock(available=False)
            self.store = MagicMock()
            self.sessions = MagicMock()
            self.devlogs = MagicMock()
            self.link_online = MagicMock()
            self.offer_for = MagicMock(return_value={"available": False})
            self.assets_offer_for = MagicMock(return_value={"available": False})

            # Mock store.get to return a valid device
            dev = MagicMock()
            dev.name = "Test Orb"
            dev.channel = "dev"
            dev.token_matches.return_value = True
            self.store.get.return_value = dev

    # 1. Both enabled
    hub = FakeHub(speech_on=True, nowplaying_on=True)
    hub.speech.available = True
    hub.nowplaying.available = True
    hub.nowplaying.subscribe = AsyncMock()

    sent = []

    async def fake_send(obj, **kwargs):
        sent.append(obj)

    ws = MagicMock()
    link = Link(hub, ws, peer="127.0.0.1:12345", local_ip="127.0.0.1")
    link._send = fake_send

    hello_env = Envelope(
        t="req",
        id=1,
        m="hello",
        p={
            "device_id": "orb-001122334455",
            "token": "0" * 64,
            "fw": "0.2.0",
            "build": "abcdef1",
            "caps": ["speech", "nowplaying"],
        },
    )

    await link._hello(hello_env)

    assert "speech" in link.caps
    assert "nowplaying" in link.caps
    assert hub.nowplaying.subscribe.called
    assert sent[0]["ok"] is True
    assert "speech" in sent[0]["r"]["caps"]
    assert "nowplaying" in sent[0]["r"]["caps"]


@pytest.mark.asyncio
async def test_link_nowplaying_requests():
    class FakeHub:
        def __init__(self):
            self.cfg = Config.load(hub_name="Test Hub")
            self.nowplaying = MagicMock()
            self.nowplaying.request = AsyncMock()
            self.home = None
            self.sessions = MagicMock()

    hub = FakeHub()
    snapshot_data = {
        "title": "Test Title",
        "artist": "Test Artist",
        "album": "Test Album",
        "duration": 120.0,
        "position": 30.0,
        "playing": True,
        "state": "playing",
        "app": "Music",
        "volume": 70,
    }
    hub.nowplaying.request.return_value = snapshot_data

    sent = []

    async def fake_send(obj, **kwargs):
        sent.append(obj)

    link = Link(hub, MagicMock(), peer="127.0.0.1:12345", local_ip="127.0.0.1")
    link._send = fake_send
    link.device_id = "orb-001122334455"
    link.state = "online"

    # With capability negotiated
    link.caps.add("nowplaying")

    # 1. nowplaying.snapshot
    await link._on_request(Envelope(t="req", id=2, m="nowplaying.snapshot", p={}))
    assert sent[-1]["ok"] is True
    assert sent[-1]["r"]["title"] == "Test Title"
    hub.nowplaying.request.assert_called_with("orb-001122334455", "nowplaying.snapshot", {})

    # 2. nowplaying.control
    control_data = {"accepted": True, "action": "pause", "snapshot": snapshot_data}
    hub.nowplaying.request.return_value = control_data
    await link._on_request(Envelope(t="req", id=3, m="nowplaying.control", p={"action": "pause"}))
    assert sent[-1]["ok"] is True
    assert sent[-1]["r"]["action"] == "pause"

    # 3. Without capability -> error
    link.caps.clear()
    await link._on_request(Envelope(t="req", id=4, m="nowplaying.snapshot", p={}))
    assert sent[-1]["ok"] is False
    assert sent[-1]["e"]["code"] == "unsupported"


# -- Speech Trigger Rule Tests --


def _make_wav_bytes(pcm: bytes, rate: int = 16000) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm)
    return buf.getvalue()


class SpeechTestHub:
    """Mock hub for speech trigger tests."""

    def __init__(self, sessions: SessionHub, speech_enabled: bool = True):
        self.cfg = Config.load(hub_name="Test Hub")
        self.sessions = sessions
        self.sessions.on_speech_trigger = self.maybe_speak
        self.online: dict[str, Link] = {}
        self._tasks: set[asyncio.Task] = set()

        if speech_enabled:
            # 800 bytes PCM = 1 chunk of 800 bytes
            pcm_data = b"\x00\x01" * 400

            async def fake_runner(cmd):
                if cmd[0] == "say" and "?" in cmd:
                    return 0, b"Ava (Premium)  en_US", b""
                if cmd[0] == "say":
                    out_file = cmd[cmd.index("-o") + 1]
                    await asyncio.to_thread(Path(out_file).write_bytes, b"AIFF")
                    return 0, b"", b""
                if cmd[0] == "afconvert":
                    out_wav = cmd[-1]
                    rate = int(cmd[cmd.index("-d") + 1].split("@", 1)[1])
                    wav_data = _make_wav_bytes(pcm_data, rate=rate)
                    await asyncio.to_thread(Path(out_wav).write_bytes, wav_data)
                    return 0, b"", b""
                return 1, b"", b""

            self.speech = SpeechService(runner=fake_runner)
            self.speech._selected_voice = "Ava (Premium)"
            self.speech.available = True
        else:
            self.speech = None

        self.voice = MagicMock(available=True)
        self.home = None
        self.nowplaying = None

    # The real trigger, so these tests cover what the hub runs.
    maybe_speak = Hub.maybe_speak

    async def close(self):
        tasks = list(self._tasks)
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def _setup_link(hub: SpeechTestHub, device_id: str = "orb-device-1") -> tuple[Link, list, list]:
    sent_events = []
    sent_audio = []

    async def fake_send(obj, **kwargs):
        if obj.get("t") == "evt":
            sent_events.append((obj["m"], obj["p"]))

    ws = MagicMock()
    ws.send_bytes = AsyncMock(side_effect=lambda b: sent_audio.append(b))
    link = Link(hub, ws, peer="127.0.0.1:12345", local_ip="127.0.0.1")
    link._send = fake_send
    link.device_id = device_id
    link.state = "online"
    link.caps = {"cards", "voice", "speech"}
    link.speech.configure({"formats": ["pcm_s16le"], "rates": [16000, 24000], "prebuffer_ms": 150}, voice=True)
    hub.online[device_id] = link
    return link, sent_events, sent_audio


def _make_session(key="s", **changes):
    return {
        "id": key,
        "agent": "codex",
        "title": "Orb",
        "project": "hub",
        "state": "idle",
        "reason": "",
        "summary": "Ready",
        "updated": 1,
        "can_message": False,
        **changes,
    }


def _make_card(key="c", **changes):
    return {
        "id": key,
        "kind": "choice",
        "title": "Choose",
        "body": "Next check?",
        "options": [
            {"id": "yes", "label": "Continue", "role": "primary"},
            {"id": "no", "label": "Cancel", "role": "cancel"},
        ],
        "priority": "normal",
        "reply": False,
        **changes,
    }


def _make_stream(session="s1", text="", final=False, **changes):
    return {
        "session": session,
        "seq": 0,
        "at": 10000,
        "state": "busy",
        "stale": False,
        "activity": "",
        "steps": [],
        "assistant": {"id": "m1", "text": text, "final": final},
        **changes,
    }


@pytest.mark.asyncio
async def test_trigger_requested_answer_spoken_once(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    hub = SpeechTestHub(sessions, speech_enabled=True)

    link1, events1, audio1 = _setup_link(hub, "orb-first")
    link2, events2, audio2 = _setup_link(hub, "orb-second")

    # Device 1 makes a voice request for "dot"
    link1._pending_voice_target = "dot"
    assert link2._pending_voice_target is None

    # Dot answer arrives via show_card
    card = _make_card("dot-card-1", title="Weather", body="It is sunny and 72 degrees.")
    await sessions.show_card(card, source="dot", device_id="orb-first")
    await asyncio.sleep(0.01)

    assert link1._speaking_task is not None
    await link1._speaking_task
    await link1.speech._task

    # Spoken on link 1: voice.speak, audio chunks, voice.speak.end
    speak_evts = [p for m, p in events1 if m == "voice.speak"]
    assert len(speak_evts) == 1
    assert speak_evts[0]["text"] == "It is sunny and 72 degrees."
    assert speak_evts[0]["rate"] in (16000, 24000)
    assert speak_evts[0]["format"] == "pcm_s16le"
    assert len(audio1) > 0
    assert any(m == "voice.speak.end" for m, _ in events1)

    # Link 2 did NOT make the voice request: no speech
    assert not any(m.startswith("voice.speak") for m, _ in events2)
    assert len(audio2) == 0

    # Spoken once only: subsequent card does NOT speak again
    events1.clear()
    card2 = _make_card("dot-card-2", title="Followup", body="Another unrequested card.")
    await sessions.show_card(card2, source="dot", device_id="orb-first")
    assert not any(m.startswith("voice.speak") for m, _ in events1)

    await hub.close()
    await sessions.close()


@pytest.mark.asyncio
async def test_trigger_unrequested_not_spoken(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    hub = SpeechTestHub(sessions, speech_enabled=True)

    link1, events1, audio1 = _setup_link(hub, "orb-first")
    assert link1._pending_voice_target is None

    # 1. Unrequested card from agent
    await sessions.show_card(_make_card("agent-card-1", body="Autonomous approval card"), source="codex")
    assert not any(m.startswith("voice.speak") for m, _ in events1)

    # 2. Unrequested stream progress from agent
    await sessions.upsert_session(
        _make_session("codex:1", agent="codex", title="Task", project="p", state="idle", updated=100)
    )
    sessions.upsert_stream(_make_stream(session="codex:1", text="Unsolicited reply.", final=True))
    await asyncio.sleep(0.01)
    assert not any(m.startswith("voice.speak") for m, _ in events1)
    assert len(audio1) == 0

    await hub.close()
    await sessions.close()


@pytest.mark.asyncio
async def test_trigger_stop_honoured(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    hub = SpeechTestHub(sessions, speech_enabled=True)

    link1, _events1, _ = _setup_link(hub, "orb-first")
    link1._pending_voice_target = "s1"

    # Start speech streaming
    stream_id = await link1.speak_answer("Streaming speech that will be stopped by user.")
    assert stream_id == 1
    assert link1.speech.stream == 1

    # Device interrupts speech (e.g. user double-taps or talks)
    link1._on_event(Envelope(t="evt", m="voice.speak.stopped", p={"stream": 1, "reason": "talk"}))

    assert link1.speech.stream == 0
    assert link1.speech.last_reason == "talk"
    assert link1._speaking_task is None or link1._speaking_task.done()

    await hub.close()
    await sessions.close()


@pytest.mark.asyncio
async def test_trigger_new_request_preempts(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    hub = SpeechTestHub(sessions, speech_enabled=True)

    link1, events1, _ = _setup_link(hub, "orb-first")
    link1._pending_voice_target = "dot"

    # Start speech for request 1
    stream_id = await link1.speak_answer("Initial response audio playing.")
    assert stream_id == 1
    assert link1.speech.stream == 1

    # Device initiates a new voice request (e.g. user speaks a new question)
    hub.sessions.check_target = MagicMock()
    hub.voice.begin = AsyncMock(return_value={})
    await link1._extension_request("voice.begin", {"stream": 2, "target": "dot"})

    # Previous stream was cancelled and voice.speak.stop sent
    assert any(m == "voice.speak.stop" and p.get("stream") == 1 for m, p in events1)
    assert link1._pending_voice_target is None  # Capture waits for explicit Send.

    await hub.close()
    await sessions.close()


@pytest.mark.asyncio
async def test_trigger_agent_stream_final(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    hub = SpeechTestHub(sessions, speech_enabled=True)

    link1, events1, _ = _setup_link(hub, "orb-first")
    await sessions.upsert_session(
        _make_session("agent:s1", agent="claude", title="Chat", project="p", state="busy", updated=100)
    )

    # Device made voice request to "agent:s1"
    link1._pending_voice_target = "agent:s1"

    # Intermediate stream with final=False -> must NOT speak
    sessions.upsert_stream(_make_stream(session="agent:s1", text="Draft sentence...", final=False))
    await asyncio.sleep(0.01)
    assert link1._speaking_task is None
    assert not any(m.startswith("voice.speak") for m, _ in events1)

    # Final stream with final=True -> speaks!
    sessions.upsert_stream(_make_stream(session="agent:s1", text="Task finished successfully.", final=True))
    await asyncio.sleep(0.01)
    assert link1._speaking_task is not None
    await link1._speaking_task
    await link1.speech._task
    assert any(m == "voice.speak" and p.get("text") == "Task finished successfully." for m, p in events1)

    await hub.close()
    await sessions.close()


@pytest.mark.asyncio
async def test_trigger_ignores_reply_from_before_the_request(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    hub = SpeechTestHub(sessions, speech_enabled=True)

    link1, events1, _ = _setup_link(hub, "orb-first")
    await sessions.upsert_session(
        _make_session("agent:s1", agent="claude", title="Chat", project="p", state="busy", updated=100)
    )
    sessions.upsert_stream(_make_stream(session="agent:s1", text="The previous answer.", final=True))
    await asyncio.sleep(0.01)

    # A voice request arms speech; a late re-send of the old final reply is not its answer.
    link1._arm_speech("agent:s1")
    sessions.upsert_stream(_make_stream(session="agent:s1", text="The previous answer.", final=True, seq=1))
    await asyncio.sleep(0.01)
    assert link1._speaking_task is None
    assert link1._pending_voice_target == "agent:s1"

    sessions.upsert_stream(_make_stream(session="agent:s1", text="The new answer.", final=True, seq=2))
    await asyncio.sleep(0.01)
    assert link1._speaking_task is not None
    await link1._speaking_task
    await link1.speech._task
    spoken = [p.get("text") for m, p in events1 if m == "voice.speak"]
    assert spoken == ["The new answer."]

    await hub.close()
    await sessions.close()


@pytest.mark.asyncio
async def test_speech_flag_off_never_speaks(short_tmp):
    sessions = SessionHub(short_tmp)
    await sessions.start()
    # speech_enabled=False
    hub = SpeechTestHub(sessions, speech_enabled=False)

    link1, events1, _ = _setup_link(hub, "orb-first")
    link1._pending_voice_target = "dot"

    await sessions.show_card(_make_card("dot-card", title="Reply", body="No speech flag."), source="dot")
    assert link1._speaking_task is None
    assert not any(m.startswith("voice.speak") for m, _ in events1)

    await hub.close()
    await sessions.close()
