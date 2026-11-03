from __future__ import annotations

import asyncio
import json
import os
import sys
import threading
import uuid
from datetime import UTC, datetime
from pathlib import Path

import pytest

from orb_agents.claude import (
    ClaudeAdapter,
    _desktop_pids,
    _transcript_metadata,
    parse_agent_record,
)
from orb_agents.hooks import HookServer
from orb_agents.models import (
    AgentError,
    Session,
    SessionUnavailable,
    UnsupportedOperation,
)
from orb_agents.store import StateStore

FAKE = Path(__file__).parent / "fixtures" / "claude_fake.py"


async def until(predicate, timeout_seconds=3):
    async with asyncio.timeout(timeout_seconds):
        while not predicate():  # noqa: ASYNC110 - poll the public snapshot in a subprocess integration test
            await asyncio.sleep(0.005)


@pytest.fixture
def store():
    return StateStore()


@pytest.fixture
async def adapter(store, tmp_path):
    instance = ClaudeAdapter(
        store,
        executable=[sys.executable, str(FAKE)],
        config_dir=tmp_path / "config",
        env={"PATH": os.environ["PATH"]},
    )
    yield instance
    await instance.close()
    await store.close()


@pytest.mark.parametrize(
    ("record", "state", "waiting"),
    [
        ({"status": "busy"}, "working", None),
        (
            {"status": "waiting", "waitingFor": "permission_prompt"},
            "needs_approval",
            "permission_prompt",
        ),
        ({"status": "waiting", "waitingFor": "Question"}, "waiting_input", "Question"),
        ({"status": "waiting"}, "waiting_input", None),
        ({"status": "idle"}, "idle", None),
        ({"state": "done"}, "done", None),
        ({"state": "failed"}, "error", None),
    ],
)
def test_discovery_status_mapping(record, state, waiting):
    session = parse_agent_record(
        {"sessionId": "id", "cwd": "/tmp/test", "name": "Session", **record}
    )
    assert session.id == "claude:id"
    assert session.state == state
    assert session.waiting_for == waiting


def test_discovery_bad_id_and_timestamp():
    assert parse_agent_record({"sessionId": 22}) is None
    session = parse_agent_record({"sessionId": "id", "updatedAt": 1_000_000_000_000})
    assert session.updated_at == datetime.fromtimestamp(1_000_000_000, UTC)
    assert (
        parse_agent_record(
            {"sessionId": "id", "updatedAt": "invalid"}
        ).updated_at.tzinfo
        is UTC
    )


def test_discovery_without_updated_at_uses_native_start_time_for_new_session():
    # The installed `agents --json` serializer has startedAt, not updatedAt.
    started = datetime(2026, 10, 27, 12, tzinfo=UTC)
    record = {
        "sessionId": "id",
        "status": "idle",
        "startedAt": started.timestamp() * 1000,
    }
    session = parse_agent_record(record)
    assert session.updated_at == started
    previous = Session("claude:id", "claude", "cli", "Task", "/tmp", updated_at=started)
    assert (
        parse_agent_record(record, previous=previous).updated_at == previous.updated_at
    )


@pytest.mark.parametrize(
    ("record", "expected"),
    [
        (
            {"name": "ESP32 device detection", "title": "Older alias"},
            "ESP32 device detection",
        ),
        ({"title": "Older alias"}, "Older alias"),
        ({"name": "  ", "title": "Usable alias"}, "Usable alias"),
        ({"name": {"private": "body"}, "title": 17}, "First prompt"),
        ({}, "First prompt"),
    ],
)
def test_discovery_title_precedence_and_partial_metadata(record, expected):
    previous = Session("claude:id", "claude", "cli", "First prompt", "/tmp/project")
    session = parse_agent_record({"sessionId": "id", **record}, previous=previous)
    assert session.title == expected
    assert session.cwd == previous.cwd


def test_discovery_unknown_title_is_not_a_project_or_provider_name():
    session = parse_agent_record({"sessionId": "id", "cwd": "/tmp/project"})
    assert session.title == ""
    assert session.cwd == "/tmp/project"


def test_desktop_origin_uses_process_ancestry_without_arguments():
    assert _desktop_pids(
        "1 0 /sbin/launchd\n2 1 /Applications/Claude.app/Contents/MacOS/Claude\n3 2 claude\n4 1 /usr/bin/zsh\n5 4 claude\n6 7 cycle\n7 6 cycle\nbad\n"
    ) == {2, 3}


def test_transcript_only_last_assistant_text_and_bounded(tmp_path):
    native_id = str(uuid.uuid4())
    project = tmp_path / "projects" / "test"
    project.mkdir(parents=True)
    transcript = project / f"{native_id}.jsonl"
    with transcript.open("w") as target:
        target.write("x" * 150_000 + "\n")
        target.write(
            json.dumps(
                {
                    "type": "assistant",
                    "message": {
                        "content": [
                            {"type": "text", "text": "Last text " + "x" * 600},
                            {"type": "tool_use", "input": {"content": "SECRET"}},
                        ]
                    },
                }
            )
            + "\n"
        )
        target.write(
            json.dumps({"type": "user", "message": {"content": "USER SECRET"}}) + "\n"
        )
        target.write('{"unfinished')
    title, text, timestamp = _transcript_metadata(tmp_path, native_id)
    assert title == ""
    assert len(text) == 300
    assert text.startswith("Last text")
    assert "SECRET" not in text
    assert timestamp.tzinfo is UTC
    assert _transcript_metadata(tmp_path, "../../*") == ("", "", None)


def test_transcript_title_uses_first_user_text_from_bounded_head(tmp_path):
    native_id = str(uuid.uuid4())
    project = tmp_path / "projects" / "test"
    project.mkdir(parents=True)
    records = [
        {"type": "user", "isMeta": True, "message": {"content": "System context"}},
        {
            "type": "user",
            "message": {"content": [{"type": "tool_result", "content": "Tool output"}]},
        },
        {
            "type": "user",
            "message": {
                "content": [
                    {
                        "type": "text",
                        "text": "Detect ESP32\n token=throwaway-secret " + "x" * 500,
                    }
                ]
            },
        },
        {"type": "user", "message": {"content": "Later user prompt"}},
        {"type": "assistant", "message": {"content": "Finished"}},
    ]
    transcript = project / f"{native_id}.jsonl"
    transcript.write_text("".join(json.dumps(record) + "\n" for record in records))
    title, text, _ = _transcript_metadata(tmp_path, native_id)
    assert title.startswith("Detect ESP32 token=[redacted]")
    assert len(title) == 120
    assert "throwaway-secret" not in title
    assert "Later" not in title
    assert text == "Finished"


async def test_discovery_merges_hook_title_arriving_during_transcript_read(
    store, tmp_path, monkeypatch
):
    entered, release = threading.Event(), threading.Event()

    def metadata(*args):
        entered.set()
        assert release.wait(2)
        return "", "", None

    async def capture(*args, **kwargs):
        return (
            json.dumps([{"sessionId": "racing", "status": "busy"}]).encode()
            if args[0] == "agents"
            else b""
        )

    adapter = ClaudeAdapter(store, config_dir=tmp_path)
    monkeypatch.setattr(adapter, "_capture", capture)
    monkeypatch.setattr("orb_agents.claude._transcript_metadata", metadata)
    refresh = asyncio.create_task(adapter.refresh())
    try:
        assert await asyncio.to_thread(entered.wait, 2)
        await HookServer(store, "throwaway-hook-key-01234567890123456789").handle(
            {
                "session_id": "racing",
                "hook_event_name": "UserPromptSubmit",
                "cwd": "/tmp/project",
                "prompt": "Detect the board",
            }
        )
        release.set()
        await refresh
        session = store.get("claude:racing")
        assert session.title == "Detect the board"
        assert session.cwd == "/tmp/project"
        assert len(store.sessions()) == 1
    finally:
        release.set()
        await refresh
        await adapter.close()


@pytest.mark.parametrize("hook_before_listing", [False, True])
async def test_discovery_uses_prompt_until_real_name_is_available(
    store, tmp_path, monkeypatch, hook_before_listing
):
    native_id = str(uuid.uuid4())
    project = tmp_path / "projects" / "test"
    project.mkdir(parents=True)
    (project / f"{native_id}.jsonl").write_text(
        json.dumps({"type": "user", "message": {"content": "Detect the board"}}) + "\n"
    )
    record = {"sessionId": native_id, "cwd": "/tmp/project", "status": "busy"}

    async def capture(*args, **kwargs):
        return json.dumps([record]).encode() if args[0] == "agents" else b""

    adapter = ClaudeAdapter(store, config_dir=tmp_path)
    monkeypatch.setattr(adapter, "_capture", capture)
    try:
        if hook_before_listing:
            await HookServer(store, "throwaway-hook-key-01234567890123456789").handle(
                {
                    "session_id": native_id,
                    "hook_event_name": "UserPromptSubmit",
                    "cwd": "/tmp/project",
                    "prompt": "A later turn seen before discovery",
                }
            )
        await adapter.refresh()
        assert store.get(f"claude:{native_id}").title == "Detect the board"
        record["name"] = "ESP32 device detection"
        await adapter.refresh()
        assert store.get(f"claude:{native_id}").title == "ESP32 device detection"
        record.pop("name")
        await adapter.refresh()
        assert store.get(f"claude:{native_id}").title == "ESP32 device detection"
    finally:
        await adapter.close()


async def test_discovery_tracks_departed_sessions(store, tmp_path):
    records = [
        {
            "sessionId": "observed",
            "cwd": str(tmp_path),
            "kind": "interactive",
            "name": "Observed",
            "status": "busy",
        }
    ]
    adapter = ClaudeAdapter(
        store,
        executable=[sys.executable, str(FAKE)],
        config_dir=tmp_path,
        env={"PATH": os.environ["PATH"], "ORB_FAKE_AGENTS": json.dumps(records)},
    )
    try:
        await adapter.refresh()
        assert store.get("claude:observed").state == "working"
        adapter.env["ORB_FAKE_AGENTS"] = "[]"
        await adapter.refresh()
        assert store.get("claude:observed").state == "done"
    finally:
        await adapter.close()


async def test_discovery_names_owned_session_without_resetting_state(
    adapter, store, tmp_path, monkeypatch
):
    session = await adapter.start(str(tmp_path), "First prompt")
    await until(lambda: store.get(session.id).state == "idle")

    async def capture(*args, **kwargs):
        return (
            json.dumps(
                [
                    {
                        "sessionId": session.id.removeprefix("claude:"),
                        "name": "Real owned name",
                        "status": "busy",
                    }
                ]
            ).encode()
            if args[0] == "agents"
            else b""
        )

    monkeypatch.setattr(adapter, "_capture", capture)
    await adapter.refresh()
    current = store.get(session.id)
    assert current.title == "Real owned name"
    assert current.title_source == "name"
    assert current.origin == "hub-started"
    assert current.state == "idle"


async def test_discovery_malformed_does_not_erase_previous(store, tmp_path):
    adapter = ClaudeAdapter(
        store,
        executable=[sys.executable, str(FAKE)],
        config_dir=tmp_path,
        env={"ORB_FAKE_AGENTS": "not json"},
    )
    with pytest.raises(AgentError, match="discovery"):
        await adapter.refresh()
    await adapter.close()


async def test_discovery_collects_split_json(store, tmp_path):
    adapter = ClaudeAdapter(
        store,
        executable=[sys.executable, str(FAKE)],
        config_dir=tmp_path,
        env={
            "ORB_FAKE_SPLIT": "1",
            "ORB_FAKE_AGENTS": json.dumps([{"sessionId": "split", "status": "busy"}]),
        },
    )
    try:
        await adapter.refresh()
        assert store.get("claude:split").state == "working"
    finally:
        await adapter.close()


async def test_discovery_output_is_bounded(store, tmp_path):
    adapter = ClaudeAdapter(
        store,
        executable=[sys.executable, str(FAKE)],
        config_dir=tmp_path,
        env={"ORB_FAKE_OVERSIZE": "1"},
    )
    try:
        with pytest.raises(AgentError, match="size limit"):
            await adapter.refresh()
    finally:
        await adapter.close()


async def test_owned_start_and_send_are_persistent(adapter, store, tmp_path):
    session = await adapter.start(str(tmp_path), "first", model="fake-cheapest")
    assert session.origin == "hub-started"
    await until(lambda: store.get(session.id).state == "idle")
    assert store.get(session.id).last_assistant_message == "Reply to first"
    await adapter.send(session.id, "second")
    await until(
        lambda: store.get(session.id).last_assistant_message == "Reply to second"
    )
    assert adapter.owns(session.id)
    await adapter.close()
    assert store.get(session.id).state == "done"
    assert all(
        child.process.returncode is not None for child in adapter._owned.values()
    )


async def test_send_does_not_overwrite_result_received_during_drain(
    adapter, store, tmp_path, monkeypatch
):
    session = await adapter.start(str(tmp_path), "first")
    await until(lambda: store.get(session.id).state == "idle")
    writer = adapter._owned[session.id].process.stdin
    original_drain = writer.drain

    async def delayed_drain():
        await original_drain()
        await until(
            lambda: store.get(session.id).last_assistant_message == "Reply to raced"
        )

    monkeypatch.setattr(writer, "drain", delayed_drain)
    await adapter.send(session.id, "raced")
    assert store.get(session.id).last_assistant_message == "Reply to raced"
    assert store.get(session.id).state == "idle"


async def test_user_send_rechecks_permission_state_at_write_boundary(
    adapter, store, tmp_path
):
    from dataclasses import replace

    session = await adapter.start(str(tmp_path), "first")
    await until(lambda: store.get(session.id).state == "idle")
    assert adapter.can_message(session.id)
    owned = adapter._owned[session.id]
    await owned.lock.acquire()
    task = asyncio.create_task(adapter.send(session.id, "synthetic followup"))
    await asyncio.sleep(0)
    current = store.get(session.id)
    store.update(replace(current, state="needs_approval"))
    owned.lock.release()
    with pytest.raises(SessionUnavailable, match="no longer accept"):
        await task
    assert store.get(session.id).state == "needs_approval"
    assert store.get(session.id).last_assistant_message == "Reply to first"
    assert not adapter.can_message(session.id)


async def test_close_during_spawn_reaps_child_without_sending(
    adapter, store, tmp_path, monkeypatch
):
    original_create = asyncio.create_subprocess_exec
    starting = asyncio.Event()
    release = asyncio.Event()
    children = []

    async def delayed_create(*args, **kwargs):
        starting.set()
        await release.wait()
        process = await original_create(*args, **kwargs)
        children.append(process)
        return process

    monkeypatch.setattr(asyncio, "create_subprocess_exec", delayed_create)
    launch = asyncio.create_task(adapter.start(str(tmp_path), "never send this"))
    await starting.wait()
    await adapter.close()
    release.set()
    with pytest.raises(SessionUnavailable, match="closed while starting"):
        await launch
    assert len(children) == 1
    assert children[0].returncode is not None
    assert not store.sessions()


@pytest.mark.parametrize("decision", ["allow", "deny"])
async def test_permission_request_and_resolution(adapter, store, tmp_path, decision):
    events = []

    async def collect():
        async for event in store.events():
            events.append(event)

    consumer = asyncio.create_task(collect())
    await asyncio.sleep(0)
    try:
        session = await adapter.start(str(tmp_path), "approve")
        await until(lambda: store.pending_approvals())
        request = store.pending_approvals()[0]
        assert request.session_id == session.id
        assert request.tool == "Write"
        assert "PRIVATE FILE CONTENT" not in request.summary
        assert store.get(session.id).state == "needs_approval"
        with pytest.raises(SessionUnavailable, match="permission"):
            await adapter.send(session.id, "new")
        await store.approve(request.request_id, decision, "Reviewed")
        await until(lambda: store.get(session.id).state == "idle")
        assert store.get(session.id).last_assistant_message == decision
        assert any(event.type == "approval.resolved" for event in events)
        await until(lambda: any(event.type == "turn.completed" for event in events))
        assert any(event.type == "turn.completed" for event in events)
        assert "PRIVATE FILE CONTENT" not in str([event.to_dict() for event in events])
    finally:
        consumer.cancel()
        await asyncio.gather(consumer, return_exceptions=True)


async def test_permission_timeout_denies(adapter, store, tmp_path):
    adapter.approval_timeout = 0.03
    session = await adapter.start(str(tmp_path), "approve")
    await until(lambda: store.get(session.id).last_assistant_message == "deny")
    assert store.pending_approvals() == []


async def test_external_cancel_clears_approval(adapter, store, tmp_path):
    session = await adapter.start(str(tmp_path), "cancel")
    await until(lambda: store.get(session.id).last_assistant_message == "Cancelled")
    assert store.pending_approvals() == []


async def test_send_answers_claude_question_without_permission_approval(
    adapter, store, tmp_path
):
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: store.get(session.id).state == "waiting_input")
    assert store.get(session.id).waiting_for == "q1: Which color?"
    assert store.pending_approvals() == []
    assert "PRIVATE CHOICE DETAILS" not in str(store.sessions())
    await adapter.send(session.id, "Blue")
    await until(
        lambda: store.get(session.id).last_assistant_message == "Answered: Blue"
    )
    assert adapter._owned[session.id].question is None


async def test_multiple_claude_questions_require_all_question_ids(
    adapter, store, tmp_path
):
    session = await adapter.start(str(tmp_path), "multi-question")
    await until(lambda: store.get(session.id).state == "waiting_input")
    for invalid in ("Blue", '{"q1":"Blue"}', '{"q1":"Blue","q2":2}'):
        with pytest.raises(AgentError) as exc:
            await adapter.send(session.id, invalid)
        assert exc.value.code == "bad_request"
    assert store.get(session.id).state == "waiting_input"
    await adapter.send(session.id, '{"q1":"Blue","q2":"Large"}')
    await until(
        lambda: store.get(session.id).last_assistant_message == "Answered: Blue, Large"
    )


async def test_question_timeout_denies(adapter, store, tmp_path):
    adapter.approval_timeout = 0.02
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: store.get(session.id).last_assistant_message == "deny")
    assert adapter._owned[session.id].question is None


@pytest.mark.parametrize("prompt", ["question-cancel", "question-result"])
async def test_question_resolved_externally_is_removed(
    adapter, store, tmp_path, prompt
):
    session = await adapter.start(str(tmp_path), prompt)
    await until(lambda: store.get(session.id).state == "idle")
    assert adapter._owned[session.id].question is None
    await adapter.send(session.id, "later")
    await until(
        lambda: store.get(session.id).last_assistant_message == "Reply to later"
    )


async def test_invalid_question_schema_is_denied(adapter, store, tmp_path):
    session = await adapter.start(str(tmp_path), "bad-question")
    await until(lambda: store.get(session.id).last_assistant_message == "deny")
    assert store.pending_approvals() == []


async def test_unknown_control_request_gets_explicit_error(adapter, store, tmp_path):
    session = await adapter.start(str(tmp_path), "unknown")
    await until(lambda: store.get(session.id).last_assistant_message == "unsupported")
    assert store.pending_approvals() == []


@pytest.mark.parametrize("prompt", ["exit", "corrupt", "malformed-control"])
async def test_process_failure_is_visible(adapter, store, tmp_path, prompt):
    session = await adapter.start(str(tmp_path), prompt)
    await until(lambda: store.get(session.id).state == "error")
    with pytest.raises(SessionUnavailable):
        await adapter.send(session.id, "next")


async def test_provider_error_body_is_not_exposed(adapter, store, tmp_path):
    session = await adapter.start(str(tmp_path), "api-error")
    await until(lambda: store.get(session.id).state == "error")
    assert "authentication is unavailable" in store.get(session.id).waiting_for
    assert "PRIVATE PROVIDER BODY" not in str(store.sessions())


async def test_initialization_timeout_cleans_child(store, tmp_path):
    adapter = ClaudeAdapter(
        store,
        executable=[sys.executable, str(FAKE)],
        config_dir=tmp_path,
        env={"ORB_FAKE_HANG": "1"},
        startup_timeout=0.05,
    )
    try:
        with pytest.raises(TimeoutError):
            await adapter.start(str(tmp_path), "hello")
        assert all(
            child.process.returncode is not None for child in adapter._owned.values()
        )
    finally:
        await adapter.close()


@pytest.mark.parametrize("experimental", [False, True])
async def test_existing_session_send_never_guesses_inbox(store, experimental):
    adapter = ClaudeAdapter(store, experimental_inbox=experimental)
    with pytest.raises(UnsupportedOperation):
        await adapter.send("claude:owner-session", "message")
    await adapter.close()


@pytest.mark.parametrize("text", ["", " ", "x" * 8193])
async def test_invalid_send_text(adapter, text):
    with pytest.raises(ValueError):
        await adapter.send("claude:missing", text)


def test_hooks_mode_requires_explicit_settings(store):
    with pytest.raises(ValueError, match="explicit settings"):
        ClaudeAdapter(store, permission_transport="hooks")
    with pytest.raises(ValueError, match="stdio or hooks"):
        ClaudeAdapter(store, permission_transport="invalid")


@pytest.mark.parametrize("prompt", ["approve", "question"])
async def test_decision_delivery_waits_for_claude_write_failure(
    adapter, store, tmp_path, monkeypatch, prompt
):
    session = await adapter.start(str(tmp_path), prompt)
    await until(
        lambda: store.get(session.id).state in {"needs_approval", "waiting_input"}
    )
    entered, release = asyncio.Event(), asyncio.Event()

    async def fail_write(owned, message):
        entered.set()
        await release.wait()
        raise BrokenPipeError("throwaway provider disconnected")

    monkeypatch.setattr(adapter, "_write", fail_write)
    if prompt == "approve":
        operation = store.approve(store.pending_approvals()[0].request_id, "allow")
    else:
        operation = adapter.answer_question(
            session.id, store.get(session.id).question_id, "Blue"
        )
    reply = asyncio.create_task(operation)
    try:
        await asyncio.wait_for(entered.wait(), 1)
        assert not reply.done()
        release.set()
        with pytest.raises(AgentError) as error:
            await reply
        assert error.value.code == "delivery_failed"
    finally:
        release.set()
        await asyncio.gather(reply, return_exceptions=True)


@pytest.mark.parametrize("reuse_request_id", [False, True])
async def test_claude_old_question_id_cannot_answer_replacement(
    adapter, store, tmp_path, reuse_request_id
):
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: store.get(session.id).state == "waiting_input")
    question_id = store.get(session.id).question_id
    assert question_id
    owned = adapter._owned[session.id]
    old = owned.question
    await adapter._handle(
        owned, {"type": "control_cancel_request", "request_id": old.request_id}
    )
    await until(
        lambda: owned.question is None and old.request_id not in owned.approvals
    )
    await adapter._handle(
        owned,
        {
            "type": "control_request",
            "request_id": old.request_id if reuse_request_id else "new-question",
            "request": {
                "subtype": "can_use_tool",
                "tool_name": "AskUserQuestion",
                "input": {"questions": [{"question": "Which animal?"}]},
            },
        },
    )
    await until(lambda: owned.question is not None)
    with pytest.raises(AgentError) as error:
        await adapter.answer_question(session.id, question_id, "Blue")
    assert error.value.code == "stale_question"
    assert not owned.question.answer.done()
