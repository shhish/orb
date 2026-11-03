"""Bridges run against deliberately narrow public service ports."""

import asyncio
import threading
from types import SimpleNamespace

import pytest
from orb_agents.models import TranscriptSource

from orb_hub.agent_data import AgentData
from orb_hub.dot import HubDeviceBackend
from orb_hub.link import Link
from orb_hub.link_ports import LinkServices, LinkSettings
from orb_hub.weather_delivery import WeatherBridge


async def test_agent_data_uses_provider_selected_inputs_without_adapter_layout(tmp_path):
    path = tmp_path / "provider-selected.jsonl"
    path.write_text('{"type":"session_meta","payload":{"id":"synthetic"}}\n')
    selected = []

    def sources(sessions):
        selected.append(sessions)
        return [TranscriptSource(path, "codex")]

    data = AgentData(SimpleNamespace(transcript_sources=sources, usage_observations=lambda: ()), tmp_path / "data")
    await data.refresh([])
    assert selected == [[]]
    assert data.usage.snapshot()["unknown"] == ["claude", "codex"]


async def test_dot_recording_identity_is_backend_owned_and_reconnect_scoped():
    class SlottedLink:
        __slots__ = ()

    calls = []

    async def transcript(value):
        calls.append(value)
        return {"accepted": True}

    link = SlottedLink()
    hub = SimpleNamespace(
        online={"orb-synthetic": link},
        voice=SimpleNamespace(confirmed=lambda *_: 1.0),
    )
    backend = HubDeviceBackend(hub, None)
    backend.set_handlers(None, transcript)
    for _ in range(2):
        assert (await backend.send_message("orb-synthetic", "Hello", stream=1))["status"] == "queued"
    assert calls[0].recording_id == calls[1].recording_id
    hub.online["orb-synthetic"] = SlottedLink()
    await backend.send_message("orb-synthetic", "Hello", stream=1)
    assert calls[2].recording_id != calls[0].recording_id
    backend.clear_recordings()


async def test_weather_publishes_through_public_delivery_port_only():
    class Delivery:
        def __init__(self):
            self.provider = None
            self.subscribed = []
            self.published = asyncio.Event()

        def set_weather_provider(self, provider):
            self.provider = provider

        def subscribe_weather(self, link):
            self.subscribed.append(link)

        def publish_weather(self):
            self.published.set()

        def weather_units(self):
            return {"C"}

        def get_prefs(self, device):
            return {"units": {"temperature": "C"}}

    class Provider:
        async def start(self):
            pass

        async def stop(self):
            pass

        async def tick(self, units):
            assert units == {"C"}

        def snapshot(self, unit):
            return {"unit": unit}

    delivery = Delivery()
    bridge = WeatherBridge(delivery, provider=Provider())
    await bridge.start()
    try:
        bridge.subscribe("synthetic-link")
        async with asyncio.timeout(1):
            await delivery.published.wait()
        assert delivery.subscribed == ["synthetic-link"]
        assert delivery.provider("synthetic-device") == {"unit": "C"}
    finally:
        await bridge.stop()
    assert delivery.provider is None
    assert bridge.lifecycle.health()["active"] == 0


async def test_link_uses_explicit_ports_without_retaining_hub():
    ports = LinkServices(cfg=LinkSettings())
    link = Link(ports, None, "synthetic", "127.0.0.1")
    assert link.services is ports
    assert link.hub is ports
    assert link.allocate_devtools_stream(1) == 1
    assert link.allocate_devtools_stream(1) is None
    await link.lifecycle.stop()


async def test_idle_link_timeout_is_normal_shutdown_and_leaves_no_receiver():
    from unittest.mock import AsyncMock

    async def receive(**options):
        raise TimeoutError

    socket = SimpleNamespace(receive=receive, close=AsyncMock())
    link = Link(LinkServices(cfg=LinkSettings(idle_timeout=0.01)), socket, "synthetic", "127.0.0.1")
    try:
        assert await link._next_frame() is None
        socket.close.assert_awaited_once()
        assert link.lifecycle.health()["healthy"]
    finally:
        await link.lifecycle.stop()
    assert not link.lifecycle.tasks


async def test_agent_data_repeated_cancellation_joins_native_reader(tmp_path):
    loop = asyncio.get_running_loop()
    entered = asyncio.Event()
    release, running = threading.Event(), threading.Event()
    completed = []
    data = AgentData(SimpleNamespace(transcript_sources=lambda _: []), tmp_path)

    def blocked_read(sessions):
        running.set()
        loop.call_soon_threadsafe(entered.set)
        try:
            if not release.wait(2):
                raise AssertionError("Fixture reader was not released")
            completed.append(sessions)
        finally:
            running.clear()

    data._read = blocked_read
    reader = asyncio.create_task(data.refresh(["synthetic"]))
    try:
        async with asyncio.timeout(1):
            await entered.wait()
        for _ in range(2):
            reader.cancel()
            await asyncio.sleep(0)
            assert not reader.done()
            assert running.is_set()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await reader
        assert not running.is_set()
        assert completed == [["synthetic"]]
    finally:
        release.set()
        await asyncio.gather(reader, return_exceptions=True)
