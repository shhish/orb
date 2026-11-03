"""Fixture-only regressions for speech consent and connection lifetime boundaries."""

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import pytest
from test_sessions import session

from orb_hub.config import Config
from orb_hub.envelope import Envelope, LinkError
from orb_hub.link import Link
from orb_hub.server import Hub
from orb_hub.sessions import SessionHub
from orb_hub.speech_synth import SpeechError


@pytest.fixture
async def speech_link(short_tmp):
    sessions = SessionHub(short_tmp, temperature="C")
    await sessions.upsert_session(session("s", can_message=True))
    sessions.register_message_handler("s", AsyncMock(return_value={"status": "delivered"}))
    hub = SimpleNamespace(
        sessions=sessions,
        cfg=SimpleNamespace(request_timeout=1),
        voice=SimpleNamespace(begin=AsyncMock(return_value={}), confirmed=Mock(), consume=Mock()),
        speech=SimpleNamespace(available=True, selected_voice="Fixture Voice"),
        online={},
    )
    link = Link(hub, SimpleNamespace(send_str=AsyncMock(), send_bytes=AsyncMock()), "fixture", "127.0.0.1")
    link.state = "online"
    link.device_id = "orb-fixture"
    link.caps = {"cards", "voice"}
    hub.online[link.device_id] = link
    link.speech.configure({"formats": ["pcm_s16le"], "rates": [24000], "prebuffer_ms": 150}, voice=True)
    yield link
    await link.stop_speech()
    await link.speech.close()
    await link.lifecycle.stop()
    await sessions.close()


async def publish_answer(link, target, text):
    source = (link.services.sessions.session_snapshot(target) or {}).get("agent")
    link.trigger_speech(target, text, source)


@pytest.mark.parametrize("target", ["s", "time"])
async def test_capture_does_not_arm_speech_before_explicit_send(speech_link, target):
    link = speech_link
    await link._extension_request("voice.begin", {"stream": 1, "target": target})
    assert link._pending_voice_target is None
    link.speak_answer = AsyncMock()
    await publish_answer(link, "s", "Unrelated answer while reviewing.")
    assert link._speaking_task is None
    link.speak_answer.assert_not_awaited()


@pytest.mark.parametrize("failure", [LinkError("busy", "fixture rejected"), asyncio.CancelledError()])
async def test_failed_or_cancelled_send_cannot_arm_later_speech(speech_link, failure):
    link = speech_link
    link.hub.sessions.message = AsyncMock(side_effect=failure)
    with pytest.raises(type(failure)):
        await link._extension_request("message.send", {"stream": 1, "target": "s", "text": "Confirmed text"})
    assert link._pending_voice_target is None
    link.hub.voice.consume.assert_not_called()


async def test_explicit_accepted_send_arms_one_answer(speech_link):
    link = speech_link
    assert (await link._extension_request("message.send", {"stream": 1, "target": "s", "text": "Send"}))[
        "status"
    ] == "delivered"
    assert link._pending_voice_target == "s"
    link.hub.voice.consume.assert_called_once_with(link, 1)
    link.speak_answer = AsyncMock()
    await publish_answer(link, "s", "The answer.")
    await asyncio.gather(*link.lifecycle.tasks)
    await publish_answer(link, "s", "Another answer.")
    link.speak_answer.assert_awaited_once_with("The answer.")


async def test_disabled_source_cannot_speak_a_late_confirmed_answer(speech_link):
    link = speech_link
    await link._extension_request("message.send", {"stream": 1, "target": "s", "text": "Send"})
    await link.hub.sessions.set_prefs(link.device_id, {"sources": {"codex": False}})
    link.speak_answer = AsyncMock()
    await publish_answer(link, "s", "Late answer after source was disabled.")
    await asyncio.gather(*link.lifecycle.tasks)
    link.speak_answer.assert_not_awaited()
    assert link._pending_voice_target is None


async def test_removed_session_cannot_speak_a_queued_answer(speech_link):
    link = speech_link
    await link._extension_request("message.send", {"stream": 1, "target": "s", "text": "Send"})
    await link.hub.sessions.remove_session("s")
    await link.hub.sessions.set_prefs(link.device_id, {"sources": {"codex": False}})
    link.speak_answer = AsyncMock()
    await publish_answer(link, "s", "Queued answer from removed session.")
    await asyncio.gather(*link.lifecycle.tasks)
    link.speak_answer.assert_not_awaited()
    assert link._pending_voice_target is None


async def test_removal_during_synthesis_suppresses_stale_audio(speech_link):
    link = speech_link
    entered, release = asyncio.Event(), asyncio.Event()

    async def chunks():
        yield b"\x00\x01" * 400

    async def render(text, *, rate):
        entered.set()
        await release.wait()
        return text, chunks()

    link.hub.speech.render_pcm = render
    await link._extension_request("message.send", {"stream": 1, "target": "s", "text": "Send"})
    await publish_answer(link, "s", "A stale answer.")
    await entered.wait()
    await link.hub.sessions.remove_session("s")
    release.set()
    await asyncio.gather(*link.lifecycle.tasks)
    if link.speech._task is not None:
        await link.speech._task
    link.ws.send_bytes.assert_not_awaited()
    link.ws.send_str.assert_not_awaited()


async def test_removed_sessions_release_per_link_stream_bookkeeping(short_tmp):
    from test_agents2_stream import CaptureLink, drain, stream

    hub = SessionHub(short_tmp, temperature="C", clock=lambda: 10000)
    link = CaptureLink()
    try:
        await hub.subscribe(link)
        for index in range(40):
            key = f"churn-{index}"
            await hub.upsert_session(session(key, state="busy", updated=10000))
            hub.upsert_stream(stream(key))
            hub._refresh(hub._subscribers[link])
            await drain(hub, link)
            await hub.remove_session(key)
            hub._refresh(hub._subscribers[link])
            await drain(hub, link)
        subscriber = hub._subscribers[link]
        assert not subscriber.stream_seq
        assert not subscriber.stream_sent
        assert not subscriber.stream_pending
    finally:
        await hub.close()


async def test_old_or_invalid_stopped_event_does_not_cancel_new_synthesis(speech_link):
    link = speech_link
    rendering = asyncio.create_task(asyncio.Event().wait())
    link._speaking_task = rendering
    link.speech.stream = 2
    try:
        for params in ({"stream": 1, "reason": "complete"}, {"stream": 2, "reason": "unknown"}):
            link._on_event(Envelope(t="evt", m="voice.speak.stopped", p=params))
            await asyncio.sleep(0)
            assert not rendering.cancelled()
            assert link._speaking_task is rendering
        link._on_event(Envelope(t="evt", m="voice.speak.stopped", p={"stream": 2, "reason": "talk"}))
        await asyncio.sleep(0)
        assert rendering.cancelled()
    finally:
        rendering.cancel()
        await asyncio.gather(rendering, return_exceptions=True)


async def test_new_answer_replaces_playback_without_cancelling_itself_and_names_voice(speech_link):
    link = speech_link

    async def chunks():
        yield b"\x00\x01" * 400

    link.hub.speech.render_pcm = AsyncMock(return_value=("New answer.", chunks()))
    link.speech.stream = 1  # Prior audio awaits its device completion receipt.
    link.speech._next_stream = 2
    task = asyncio.create_task(link.speak_answer("New answer."))
    link._speaking_task = task
    assert await task == 2
    await link.speech._task
    events = [call.args[0] for call in link.ws.send_str.await_args_list]
    assert any('"voice":"Fixture Voice"' in event for event in events)


async def test_synthesis_error_does_not_log_private_provider_output(speech_link, caplog):
    link = speech_link
    private = "Private transcript and /private/fixture/voice.wav"
    link.hub.speech.render_pcm = AsyncMock(side_effect=SpeechError(private))
    assert await link.speak_answer("Requested reply") is None
    assert "speech playback failed" in caplog.text
    assert private not in caplog.text


@pytest.mark.parametrize("running", ["0.5.2-bench", "0.5.3", "0.5.1"])
def test_assets_feed_does_not_replace_another_apps_pack(short_tmp, running):
    hub = Hub(Config(data_dir=short_tmp / "data", log_dir=short_tmp / "logs"))
    assets = {
        "kind": "assets",
        "version": "6",
        "channel": "dev",
        "build": "abcdef0",
        "size": 8192,
        "sha256": "a" * 64,
        "notes": "Fixture pack",
    }
    hub.releases.current = lambda channel, assets=False: assets_manifest if assets else {"version": "0.5.2-feed"}
    assets_manifest = assets
    device = SimpleNamespace(channel="dev", fw=running, build="abcdef0", hello={"assets": "5"}, ota={})
    assert hub.assets_offer_for(device, "127.0.0.1") == {"available": False}
