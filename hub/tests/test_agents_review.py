"""Review regressions exercise real adapters through the durable card boundary."""

import asyncio
from dataclasses import replace
from datetime import UTC, datetime, timedelta
from types import SimpleNamespace
from unittest.mock import AsyncMock

import pytest
from orb_agents import AgentManager, Session, StateStore
from orb_agents.codex import CodexAdapter

from orb_hub import models
from orb_hub.agents import AgentsBridge
from orb_hub.envelope import LinkError
from orb_hub.sessions import SessionHub


@pytest.fixture
async def bridge_provider(tmp_path, monkeypatch):
    store = StateStore()
    adapter = CodexAdapter(store, observe=False)
    sid = "codex:review"
    adapter._owned.add(sid)
    adapter._locks[sid] = asyncio.Lock()
    store.update(Session(sid, "codex", "hub-started", "Review", str(tmp_path)))
    monkeypatch.setattr(adapter, "_ensure_server", AsyncMock())
    monkeypatch.setattr(adapter, "_write", AsyncMock())
    manager = AgentManager(store=store, adapters={"codex": adapter})
    hub = SessionHub(tmp_path / "hub")
    bridge = AgentsBridge(hub, tmp_path, manager=manager)
    tasks = []

    async def approval(method="item/permissions/requestApproval", *, reconcile=True):
        async def publication():
            async for event in store.events():
                if event.type == "approval.requested":
                    return

        published = asyncio.create_task(publication())
        tasks.append(published)
        # Subscribe before the synthetic request publishes its approval.
        await asyncio.sleep(0)
        task = asyncio.create_task(
            adapter._server_request(
                {
                    "id": "permission-1",
                    "method": method,
                    "params": {"threadId": "review", "permissions": {"network": {"enabled": True}}},
                }
            )
        )
        tasks.append(task)
        async with asyncio.timeout(2):
            await published
        if reconcile:
            await bridge.reconcile()
        return task, next(iter(hub.cards.values()), None)

    yield SimpleNamespace(
        store=store, adapter=adapter, manager=manager, hub=hub, bridge=bridge, sid=sid, approval=approval
    )
    for task in tasks:
        task.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    await manager.close()
    await hub.close()


async def question(provider, request_id):
    await provider.adapter._server_request(
        {
            "id": request_id,
            "method": "item/tool/requestUserInput",
            "params": {"threadId": "review", "questions": [{"id": "answer", "question": "Continue?"}]},
        }
    )


async def test_codex_permission_card_declares_actual_turn_scope(bridge_provider):
    p = bridge_provider
    task, card = await p.approval()
    assert card.get("scope") == "turn"
    allow = next(option for option in card["options"] if option["id"] == "allow")
    assert allow == {"id": "allow", "label": "Allow for turn", "role": "default"}
    assert "subsequent operations" in card["body"] and "current turn" in card["body"]
    assert "once" not in card["body"]
    result = await p.hub.answer("device", {"card": card["id"], "option": "allow"})
    assert result["delivered"] is True
    await task
    assert p.adapter._write.call_args.args[0]["result"] == {
        "permissions": {"network": {"enabled": True}},
        "scope": "turn",
    }


async def test_old_operation_card_cannot_grant_replacement_turn_scope(bridge_provider):
    p = bridge_provider
    task, old = await p.approval("item/commandExecution/requestApproval")
    request = p.manager.pending_approvals()[0]
    p.store.resolve(request.request_id, reason="external")
    await task
    p.adapter._write.reset_mock()
    replacement, _ = await p.approval(reconcile=False)
    # The same RPC ID now requests broader permission, before reconciliation.
    assert (await p.hub.answer("device", {"card": old["id"], "option": "allow"}))["delivered"] is False
    assert not p.adapter._write.called
    await p.bridge.reconcile()
    new = next(iter(p.hub.cards.values()))
    assert new["id"] != old["id"]
    assert new["scope"] == "turn"
    assert (await p.hub.answer("device", {"card": new["id"], "option": "allow"}))["delivered"] is True
    await replacement


@pytest.mark.parametrize("scope", ["session", "always", "", None])
@pytest.mark.parametrize("with_expiry", [False, True])
async def test_unrepresentable_permission_scope_has_no_grant(bridge_provider, scope, with_expiry):
    p = bridge_provider
    await p.bridge._approval(
        SimpleNamespace(
            request_id="unknown-scope",
            session_id=p.sid,
            tool="network",
            summary="Network",
            scope=scope,
            expires_at=datetime.now(UTC) + timedelta(seconds=120) if with_expiry else None,
        )
    )
    card = next(iter(p.hub.cards.values()))
    assert card["kind"] == "attention"
    assert card["reply"] is False
    assert card["options"] == [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}]
    with pytest.raises(LinkError, match="bad_request"):
        await p.hub.answer("device", {"card": card["id"], "option": "allow"})
    assert not p.adapter._write.called


async def test_supported_approval_without_expiry_keeps_allow_once(bridge_provider):
    p = bridge_provider
    await p.bridge._approval(
        SimpleNamespace(request_id="legacy", session_id=p.sid, tool="Write", summary="File", scope="operation")
    )
    card = next(iter(p.hub.cards.values()))
    assert card["kind"] == "approval" and "expires" not in card
    assert card["options"][0] == {"id": "allow", "label": "Allow once", "role": "default"}


@pytest.mark.parametrize("original_id,replacement_id", [("q1", "q2"), ("q1", "q1"), (1, "1")])
async def test_old_question_card_cannot_answer_replacement(bridge_provider, original_id, replacement_id):
    p = bridge_provider
    await question(p, original_id)
    await p.bridge.reconcile()
    old = next(iter(p.hub.cards.values()))
    p.adapter._notification(
        {"method": "serverRequest/resolved", "params": {"threadId": "review", "requestId": original_id}}
    )
    await question(p, replacement_id)
    # Deliberately skip bridge reconciliation, reproducing its stale-card window.
    result = await p.hub.answer("device", {"card": old["id"], "text": "Yes"})
    assert result["delivered"] is False
    assert not p.adapter._write.called
    await p.bridge.reconcile()
    new = next(iter(p.hub.cards.values()))
    assert new["id"] != old["id"]
    assert (await p.hub.answer("device", {"card": new["id"], "text": "No"}))["delivered"] is True
    assert p.adapter._write.call_args.args[0] == {
        "id": replacement_id,
        "result": {"answers": {"answer": {"answers": ["No"]}}},
    }


async def test_question_card_identity_survives_metadata_refresh(bridge_provider):
    p = bridge_provider
    await question(p, "q1")
    await p.bridge.reconcile()
    original = next(iter(p.hub.cards))
    value = p.store.get(p.sid)
    p.store.update(replace(value, waiting_for="Refreshed question summary"))
    await p.bridge.reconcile()
    assert list(p.hub.cards) == [original]


async def test_unidentified_owned_question_is_read_only(bridge_provider):
    p = bridge_provider
    p.store.update(replace(p.store.get(p.sid), state="waiting_input", waiting_for="Unidentified question"))
    await p.bridge.reconcile()
    card = next(iter(p.hub.cards.values()))
    assert card["kind"] == "attention"
    assert card["reply"] is False


async def test_approval_write_failure_is_durable_and_never_replayed(bridge_provider, tmp_path):
    p = bridge_provider
    task, card = await p.approval()
    p.adapter._write.side_effect = BrokenPipeError("synthetic transport failure")
    result = await p.hub.answer("device", {"card": card["id"], "option": "allow"})
    await asyncio.gather(task, return_exceptions=True)
    assert result["delivered"] is False
    restored = SessionHub(tmp_path / "hub")
    try:
        assert await restored.answer("device", {"card": card["id"], "option": "deny"}) == result
    finally:
        await restored.close()
    assert p.adapter._write.call_count == 1


async def test_approval_answer_waits_for_transport_write(bridge_provider):
    p = bridge_provider
    entered = asyncio.Event()
    release = asyncio.Event()

    async def write(_message):
        entered.set()
        await release.wait()

    p.adapter._write.side_effect = write
    task, card = await p.approval()
    answer = asyncio.create_task(p.hub.answer("device", {"card": card["id"], "option": "allow"}))
    try:
        async with asyncio.timeout(2):
            await entered.wait()
        assert not answer.done()
        assert p.hub.db.execute("SELECT delivered FROM session_cards WHERE id=?", (card["id"],)).fetchone()[0] == 0
    finally:
        release.set()
        await asyncio.gather(answer, task, return_exceptions=True)
    assert answer.result()["delivered"] is True


@pytest.mark.parametrize("scope", ["session", "always", None])
def test_card_model_rejects_unrepresentable_scope(scope):
    with pytest.raises(LinkError, match="bad_request"):
        models.card(
            {
                "id": "scope",
                "kind": "approval",
                "title": "Permission",
                "body": "Allow?",
                "options": [],
                "reply": False,
                "priority": "high",
                "scope": scope,
            }
        )
