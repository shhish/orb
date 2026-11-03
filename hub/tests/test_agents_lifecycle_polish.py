"""Needs-you admission and permission expiry with isolated provider state."""

import asyncio
from datetime import UTC, datetime

import pytest
from orb_agents import AgentManager, Session, StateStore
from orb_agents.testing import FakeAdapter
from test_agents_bridge import DeviceSessions

from orb_hub.agents import AgentsBridge
from orb_hub.envelope import LinkError


async def test_new_permission_is_admitted_ahead_of_owned_work_at_session_cap(tmp_path):
    store, hub = StateStore(), DeviceSessions()
    fake = FakeAdapter(store)
    manager = AgentManager(store=store, adapters={"claude": fake})
    bridge = AgentsBridge(hub, tmp_path, manager=manager)
    for index in range(24):
        value = Session(f"claude:owned-{index}", "claude", "hub-started", "Working", "/tmp", state="working")
        fake._owned.add(value.id)
        store.update(value)
    await bridge.reconcile()
    store.update(Session("claude:new", "claude", "cli", "Needs you", "/tmp"))
    pending = asyncio.create_task(store.request_approval("claude:new", "Write", {}, timeout=30))
    try:
        await asyncio.sleep(0)
        await bridge.reconcile()
        assert len(hub.values) == 24
        assert "claude:new" in hub.values
        card = next(iter(hub.cards.values()))
        assert card["kind"] == "approval"
        request = manager.pending_approvals()[0]
        assert card["expires"] == request.expires_at.timestamp()
        assert datetime.now(UTC).timestamp() < card["expires"] <= datetime.now(UTC).timestamp() + 30
        # Answering on the Mac resolves only this pending hook, cancels the
        # presentation, and a delayed physical answer cannot revive it.
        store.resolve(request.request_id, reason="external")
        await pending
        await bridge.reconcile()
        assert card["id"] in hub.cancelled
        with pytest.raises(LinkError, match="expired"):
            await hub.handlers[card["id"]]({"option": "allow"})
    finally:
        pending.cancel()
        await asyncio.gather(pending, return_exceptions=True)
        await manager.close()


@pytest.mark.parametrize("deadline", [0, -1, float("nan"), float("inf")])
async def test_invalid_approval_deadline_does_not_publish(deadline):
    store = StateStore()
    store.update(Session("claude:test", "claude", "cli", "Test", "/tmp"))
    with pytest.raises(ValueError, match="positive"):
        await store.request_approval("claude:test", "Write", {}, timeout=deadline)
    assert store.pending_approvals() == []
