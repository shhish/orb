"""Requested speech follows device-private Dot streams without crossing recipients."""

import asyncio
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from test_agents2_stream import stream
from test_sessions import session

from orb_hub.config import Config
from orb_hub.link import Link
from orb_hub.server import Hub
from orb_hub.sessions import SessionHub


@pytest.mark.parametrize("device_id", ["orb-owner", "orb-offline", None])
async def test_final_dot_stream_speaks_only_to_its_requested_device(short_tmp, device_id):
    hub = Hub(Config(data_dir=short_tmp / "data", log_dir=short_tmp / "logs"))
    sessions = SessionHub(hub.cfg.data_dir, temperature="C")
    hub.sessions = sessions
    hub.speech = SimpleNamespace(available=True)
    hub.online = {}
    for identity in ("orb-owner", "orb-other"):
        link = Link(hub, None, "synthetic", "127.0.0.1")
        link.state = "online"
        link.device_id = identity
        link._pending_voice_target = "dot"
        link.speak_answer = AsyncMock()
        hub.online[identity] = link
    try:
        await sessions.upsert_session(session("dot", agent="dot"))
        sessions.on_speech_trigger = hub.maybe_speak
        final = stream("dot", assistant={"id": "reply", "text": "Private Dot answer.", "final": True})
        for _ in range(2):
            sessions.upsert_stream(final, device_id=device_id)
            await asyncio.gather(*tuple(sessions._tasks))
            await asyncio.gather(*hub.supervisor.tasks)
            for link in hub.online.values():
                await asyncio.gather(*link.lifecycle.tasks)
        for identity, link in hub.online.items():
            if device_id is None or identity == device_id:
                link.speak_answer.assert_awaited_once_with("Private Dot answer.")
                assert link._pending_voice_target is None
            else:
                link.speak_answer.assert_not_awaited()
                assert link._pending_voice_target == "dot"
    finally:
        for link in hub.online.values():
            await link.lifecycle.stop()
        await sessions.close()
