import asyncio

import pytest

from orb_agents import AgentError, AgentManager, Session, SessionUnavailable, StateStore
from orb_agents.testing import FakeAdapter


@pytest.mark.parametrize("kind", ["claude", "codex"])
async def test_hub_fakes_exercise_start_send_approve_complete(tmp_path, kind):
    store = StateStore()
    fake = FakeAdapter(store, kind)
    async with AgentManager(store=store, adapters={kind: fake}) as agents:
        stream = agents.events()
        first = asyncio.create_task(anext(stream))
        await asyncio.sleep(0)
        session = await agents.start(kind, str(tmp_path), "Start working")
        assert (await first).session.origin == "hub-started"
        permission = asyncio.create_task(fake.ask_permission(session.id))
        await anext(stream)  # state update
        request = await anext(stream)
        with pytest.raises(SessionUnavailable, match="approval"):
            await agents.send(session.id, "Continue")
        await agents.approve(request.request_id, "allow")
        assert (await permission).decision == "allow"
        fake.complete(session.id, "Created example.txt")
        assert (await agents.sessions())[
            0
        ].last_assistant_message == "Created example.txt"
        await agents.send(session.id, "Now review it")
        assert fake.messages[-1] == (session.id, "Now review it")
        await stream.aclose()
    assert fake.closed
    with pytest.raises(AgentError, match="closed"):
        await agents.send(session.id, "again")


async def test_missing_provider_discovery_failure_and_other_provider_survives(tmp_path):
    class Failing(FakeAdapter):
        async def refresh(self):
            raise OSError("token=DO_NOT_EXPOSE")

    store = StateStore()
    good = FakeAdapter(store, "codex")
    bad = Failing(store)
    async with AgentManager(
        store=store, adapters={"claude": bad, "codex": good}
    ) as agents:
        assert agents.health["claude"] == "Discovery unavailable (OSError)"
        assert agents.health["codex"] is None
        session = await agents.start("codex", str(tmp_path), "Hello")
        assert session in await agents.sessions()
        with pytest.raises(AgentError, match="not known"):
            await agents.send("unknown", "Hi")


async def test_validation_and_observed_session_cannot_be_silently_controlled(tmp_path):
    store = StateStore()
    fake = FakeAdapter(store)
    async with AgentManager(store=store, adapters={"claude": fake}) as agents:
        for text in ["", "  ", "a" * 8193]:
            with pytest.raises(ValueError):
                await agents.start("claude", str(tmp_path), text)
        with pytest.raises(ValueError, match="directory"):
            await agents.start("claude", str(tmp_path / "absent"), "hi")
        with pytest.raises(AgentError, match="not configured"):
            await agents.start("codex", str(tmp_path), "hi")
        store.update(
            Session("claude:external", "claude", "desktop app", "Other", str(tmp_path))
        )
        with pytest.raises(SessionUnavailable):
            await agents.send("claude:external", "hi")


async def test_polling_shutdown_cancels_inflight_refresh():
    class Slow(FakeAdapter):
        async def refresh(self):
            await asyncio.sleep(10)

    store = StateStore()
    slow = Slow(store)
    manager = AgentManager(store=store, adapters={"claude": slow}, refresh_timeout=0.01)
    async with manager:
        assert manager.health["claude"] == "Discovery unavailable (TimeoutError)"
    assert manager._poll_task.done()
    assert slow.closed
