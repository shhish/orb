import asyncio
import json
from dataclasses import replace
from datetime import timedelta

import pytest

from orb_agents import AgentError, Session, StateStore
from orb_agents.models import summarize_tool, summary, utcnow


def session():
    return Session("claude:test", "claude", "cli", "Test", "/tmp")


async def test_approval_roundtrip_is_one_shot_and_redacted():
    store = StateStore()
    store.update(session())
    stream = store.events()
    reading = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    pending = asyncio.create_task(
        store.request_approval(
            "claude:test",
            "Write",
            {"file_path": "/tmp/x", "content": "FULL PRIVATE CONTENT"},
        )
    )
    event = await reading
    assert event.session.state == "needs_approval"
    request = await anext(stream)
    assert request.type == "approval.requested"
    assert "FULL PRIVATE CONTENT" not in json.dumps(request.to_dict())
    assert "/tmp/x" in request.approval.summary
    await store.approve(request.request_id, "deny", "Try a different file")
    decision = await pending
    assert decision.decision == "deny"
    assert decision.message == "Try a different file"
    assert (await anext(stream)).type == "approval.resolved"
    with pytest.raises(AgentError, match="no longer pending") as error:
        await store.approve(request.request_id, "allow")
    assert error.value.code == "stale_approval"
    await stream.aclose()
    await store.close()


async def test_timeout_cancellation_shutdown_resolve_without_approval():
    store = StateStore()
    store.update(session())
    result = await store.request_approval("claude:test", "Bash", {}, timeout=0.001)
    assert result.decision == "deny"
    assert not store.pending_approvals()
    task = asyncio.create_task(store.request_approval("claude:test", "Read", {}))
    await asyncio.sleep(0)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert not store.pending_approvals()
    task = asyncio.create_task(store.request_approval("claude:test", "Read", {}))
    await asyncio.sleep(0)
    await store.close()
    assert (await task).decision == "deny"
    assert not store.pending_approvals()


async def test_multiple_approvals_cannot_clobber_pending_state():
    store = StateStore()
    store.update(session())
    a = asyncio.create_task(
        store.request_approval("claude:test", "Read", {}, request_id="a")
    )
    b = asyncio.create_task(
        store.request_approval("claude:test", "Write", {}, request_id="b")
    )
    await asyncio.sleep(0)
    store.update(replace(session(), state="idle"))
    assert store.get("claude:test").state == "needs_approval"
    await store.approve("a", "allow")
    assert (await a).decision == "allow"
    assert store.get("claude:test").state == "needs_approval"
    store.turn_complete("claude:test", "Finished")
    assert (await b).decision == "deny"
    assert store.get("claude:test").state == "idle"
    assert store.get("claude:test").last_assistant_message == "Finished"


async def test_fanout_slow_reader_and_close():
    store = StateStore(queue_size=2)
    fast, slow = store.events(), store.events()
    first_fast = asyncio.create_task(anext(fast))
    first_slow = asyncio.create_task(anext(slow))
    await asyncio.sleep(0)
    store.update(session())
    assert (await first_fast).type == (await first_slow).type == "session.changed"
    for i in range(4):
        store.update(replace(session(), title=str(i)))
        assert (await anext(fast)).session.title == str(i)
    assert (await anext(slow)).type == "stream.gap"
    assert (await anext(slow)).session.title == "3"
    wait = asyncio.create_task(anext(fast))
    await asyncio.sleep(0)
    await store.close()
    with pytest.raises(StopAsyncIteration):
        await wait
    await slow.aclose()


async def test_invalid_decision_and_unknown_session_do_not_create_requests():
    store = StateStore()
    with pytest.raises(AgentError, match="unknown session"):
        await store.request_approval("absent", "Read", {})
    store.update(session())
    task = asyncio.create_task(
        store.request_approval("claude:test", "Read", {}, request_id="r")
    )
    await asyncio.sleep(0)
    with pytest.raises(ValueError):
        await store.approve("r", "yes")
    assert len(store.pending_approvals()) == 1
    with pytest.raises(AgentError, match="already pending"):
        await store.request_approval("claude:test", "Read", {}, request_id="r")
    store.fail_session("claude:test", "Transport closed")
    assert (await task).decision == "deny"
    assert store.get("claude:test").state == "error"


async def test_unchanged_observation_updates_freshness_without_event():
    store = StateStore()
    current = store.update(session())
    new = replace(current, updated_at=utcnow() + timedelta(seconds=1))
    store.update(new)
    assert store.get(current.id).updated_at == new.updated_at
    assert store._sequence == 1


@pytest.mark.parametrize(
    "payload",
    [
        {"content": "SECRET_CONTENT", "file_path": "/tmp/example"},
        {"old_string": "SECRET_CONTENT", "new_string": "SECRET_CONTENT"},
        {"command": "python -c 'SECRET_CONTENT'"},
        {"command": "cat <<EOF\nSECRET_CONTENT\nEOF"},
        {"arguments": {"nested": {"content": "SECRET_CONTENT"}}},
        {"command": ["sh", "-c", "SECRET_CONTENT"]},
    ],
)
def test_tool_summary_never_includes_bodies(payload):
    value = summarize_tool("SomeTool", payload)
    assert "SECRET_CONTENT" not in value
    assert len(value) <= 300


def test_display_summary_limits_and_json_serialization():
    assert len(summary("x" * 500)) == 300
    assert "code body" not in summary("Response ```\ncode body\n``` finished")
    assert "abcd" not in summary("token=abcd password=abcd Bearer abcd")
    store = StateStore()
    value = store.update(replace(session(), last_assistant_message="x" * 1000))
    assert len(value.last_assistant_message) == 300
    assert "+00:00" in json.dumps(value.to_dict())


async def test_late_provider_completion_cannot_lose_stream_shutdown():
    store = StateStore(queue_size=2)
    store.update(session())
    stream = store.events()
    wait = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    await store.close()
    store.turn_complete("claude:test", "late")
    store.turn_complete("claude:test", "later")
    with pytest.raises(StopAsyncIteration):
        await asyncio.wait_for(wait, 1)


async def test_invalid_message_does_not_consume_approval():
    store = StateStore()
    store.update(session())
    task = asyncio.create_task(
        store.request_approval("claude:test", "Write", {}, request_id="r")
    )
    await asyncio.sleep(0)
    for message in [[], {}, 42, "x" * 8193]:
        with pytest.raises(ValueError):
            await store.approve("r", "deny", message)
        assert len(store.pending_approvals()) == 1
    await store.close()
    await task


def test_permissions_summary_exposes_only_scope_counts():
    text = summarize_tool(
        "permissions",
        {
            "permissions": {
                "fileSystem": {
                    "write": ["/private/file"],
                    "read": ["/private/other"],
                    "entries": [{"path": "somewhere"}],
                },
                "network": {"enabled": True},
            }
        },
    )
    assert "network access requested" in text
    assert "write: 1 path" in text
    assert "/private/file" not in text


async def test_approval_expected_instance_rejects_reused_request_id():
    store = StateStore()
    store.update(session())
    old = asyncio.create_task(
        store.request_approval("claude:test", "Read", {}, request_id="reused")
    )
    await asyncio.sleep(0)
    expected = store.pending_approvals()[0]
    store.resolve("reused")
    await old
    replacement = asyncio.create_task(
        store.request_approval(
            "claude:test", "Write", {}, request_id="reused", scope="turn"
        )
    )
    await asyncio.sleep(0)
    try:
        with pytest.raises(AgentError) as error:
            await store.approve("reused", "allow", expected_request=expected)
        assert error.value.code == "stale_approval"
        assert not replacement.done()
        current = store.pending_approvals()[0]
        assert current.scope == "turn"
        await store.approve("reused", "deny", expected_request=current)
        assert (await replacement).decision == "deny"
    finally:
        await store.close()
        await replacement
