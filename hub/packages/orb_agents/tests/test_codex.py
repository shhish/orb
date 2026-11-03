from __future__ import annotations

import asyncio
import json
import os
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

from orb_agents.codex import MAX_LINE, CodexAdapter, RolloutObserver, _status
from orb_agents.models import (
    AgentError,
    Session,
    SessionUnavailable,
    UnsupportedOperation,
)
from orb_agents.store import StateStore

FAKE = Path(__file__).parent / "fixtures" / "codex_fake_server.py"


@pytest.fixture
async def adapter():
    store = StateStore()
    value = CodexAdapter(
        store, command=[sys.executable, str(FAKE)], observe=False, rpc_timeout=2
    )
    yield value
    await value.close()
    await store.close()


async def until(predicate, timeout=3):
    async with asyncio.timeout(timeout):
        while not predicate():  # noqa: ASYNC110 - poll fake subprocess state
            await asyncio.sleep(0.005)


async def test_start_complete_send_followup_and_clean_process(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "first", "test-model")
    await until(lambda: adapter.store.get(session.id).state == "idle")
    assert session.id == "codex:test-thread"
    assert adapter.store.get(session.id).origin == "hub-started"
    assert adapter.store.get(session.id).last_assistant_message == "first"
    await adapter.send(session.id, "second")
    await until(
        lambda: adapter.store.get(session.id).last_assistant_message == "second"
    )
    await adapter.close()
    assert adapter._proc.returncode == 0
    assert adapter.store.get(session.id).state == "done"


async def test_send_mid_turn_steers(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "hold")
    assert adapter.store.get(session.id).state == "working"
    result = await adapter.send(session.id, "use green")
    assert result.status == "delivered"
    await until(lambda: adapter.store.get(session.id).state == "idle")
    assert adapter.store.get(session.id).last_assistant_message == "steered: use green"


async def test_steer_rechecks_turn_after_waiting_for_write_lock(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "hold")
    assert adapter.can_message(session.id)
    await adapter._write_lock.acquire()
    task = asyncio.create_task(adapter.send(session.id, "synthetic followup"))
    await asyncio.sleep(0)
    adapter._active_turns[session.id] = "synthetic-replacement-turn"
    adapter._write_lock.release()
    with pytest.raises(SessionUnavailable, match="turn changed"):
        await task
    assert adapter.store.get(session.id).last_assistant_message == ""


@pytest.mark.parametrize("decision,wire", [("allow", "accept"), ("deny", "decline")])
@pytest.mark.parametrize("prompt", ["approval", "filechange"])
async def test_approval_roundtrip_and_safe_summary(
    adapter, tmp_path, decision, wire, prompt
):
    session = await adapter.start(str(tmp_path), prompt)
    await until(lambda: adapter.store.pending_approvals())
    request = adapter.store.pending_approvals()[0]
    assert adapter.store.get(session.id).state == "needs_approval"
    assert "file contents" not in request.summary
    assert "not-for-device" not in request.summary
    await adapter.store.approve(request.request_id, decision)
    await until(lambda: adapter.store.get(session.id).state == "idle")
    assert wire in adapter.store.get(session.id).last_assistant_message
    assert not adapter.store.pending_approvals()


@pytest.mark.parametrize(
    "decision,expected", [("allow", {"network": {"enabled": True}}), ("deny", {})]
)
async def test_permission_grants_are_turn_scoped(adapter, tmp_path, decision, expected):
    session = await adapter.start(str(tmp_path), "permissions")
    await until(lambda: adapter.store.pending_approvals())
    await adapter.store.approve(
        adapter.store.pending_approvals()[0].request_id, decision
    )
    await until(lambda: adapter.store.get(session.id).state == "idle")
    result = json.loads(adapter.store.get(session.id).last_assistant_message)
    assert result == {"permissions": expected, "scope": "turn"}


async def test_approval_timeout_denies(adapter, tmp_path):
    adapter.approval_timeout = 0.03
    session = await adapter.start(str(tmp_path), "approval")
    await until(
        lambda: "decline" in adapter.store.get(session.id).last_assistant_message
    )
    assert adapter.store.pending_approvals() == []


async def test_external_resolution_never_sends_a_late_answer(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "external")
    await until(lambda: adapter.store.get(session.id).state == "idle")
    assert adapter.store.get(session.id).last_assistant_message == "resolved elsewhere"
    assert adapter.store.pending_approvals() == []
    await asyncio.sleep(0.03)
    assert adapter.store.get(session.id).last_assistant_message == "resolved elsewhere"


async def test_send_answers_pending_question(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: adapter.store.get(session.id).state == "waiting_input")
    assert "What should I do?" in adapter.store.get(session.id).waiting_for
    await adapter.send(session.id, "Proceed with green")
    await until(lambda: adapter.store.get(session.id).state == "idle")
    assert json.loads(adapter.store.get(session.id).last_assistant_message) == {
        "answers": {"q1": {"answers": ["Proceed with green"]}}
    }


async def test_multiple_questions_need_explicit_answers(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "multiquestion")
    await until(lambda: adapter.store.get(session.id).state == "waiting_input")
    with pytest.raises(AgentError, match="Multiple questions"):
        await adapter.send(session.id, "yes")
    with pytest.raises(AgentError, match="exactly one"):
        await adapter.send(session.id, '{"q1":"yes"}')
    await adapter.send(session.id, '{"q1":"yes", "q2":"temp"}')
    await until(lambda: adapter.store.get(session.id).state == "idle")


async def test_complete_before_rpc_response_is_not_reanimated(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "complete-before-response")
    assert adapter.store.get(session.id).state == "idle"
    assert session.id not in adapter._active_turns


async def test_turn_failure_stays_error(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "fail")
    await until(lambda: adapter.store.get(session.id).state == "error")


async def test_process_exit_resolves_pending_requests(adapter, tmp_path):
    with pytest.raises(SessionUnavailable):
        await adapter.start(str(tmp_path), "crash")
    assert adapter.store.get("codex:test-thread").state == "error"


async def test_close_cancels_approval_and_reaps_child(adapter, tmp_path):
    await adapter.start(str(tmp_path), "approval")
    await until(lambda: adapter.store.pending_approvals())
    await adapter.close()
    assert adapter.store.pending_approvals() == []
    assert adapter._proc.returncode is not None


async def test_stderr_is_drained_without_logging(adapter, tmp_path, capsys):
    session = await adapter.start(str(tmp_path), "stderr")
    await until(lambda: adapter.store.get(session.id).state == "idle")
    assert capsys.readouterr().err == ""


@pytest.mark.parametrize(
    "mode,code",
    [
        ("hang", "timeout"),
        ("malformed", "session_unavailable"),
        ("error", "backend_error"),
    ],
)
async def test_initialization_failures_close_process(tmp_path, mode, code):
    store = StateStore()
    value = CodexAdapter(
        store,
        command=[sys.executable, str(FAKE)],
        env={**os.environ, "ORB_FAKE_INIT": mode},
        observe=False,
        rpc_timeout=0.2,
    )
    with pytest.raises(AgentError) as caught:
        await value.start(str(tmp_path), "hello")
    assert caught.value.code == code
    assert "never-print" not in str(caught.value)
    assert value._proc.returncode is not None
    await value.close()


async def test_observed_session_cannot_be_sent_to(adapter):
    adapter.store.update(
        Session(
            id="codex:external",
            kind="codex",
            origin="desktop app",
            title="external",
            cwd="/tmp",
        )
    )
    with pytest.raises(UnsupportedOperation, match="read-only"):
        await adapter.send("codex:external", "hello")
    assert adapter._proc is None


@pytest.mark.parametrize("text", ["", " ", "x" * 8193])
async def test_invalid_messages_not_delivered(adapter, tmp_path, text):
    session = await adapter.start(str(tmp_path), "hold")
    with pytest.raises(AgentError):
        await adapter.send(session.id, text)


@pytest.mark.parametrize(
    "native,state",
    [
        ({"type": "active"}, "working"),
        ({"type": "active", "activeFlags": ["waitingOnApproval"]}, "needs_approval"),
        ({"type": "active", "activeFlags": ["waitingOnUserInput"]}, "waiting_input"),
        ({"type": "idle"}, "idle"),
        ({"type": "systemError"}, "error"),
        (None, "idle"),
    ],
)
def test_status_mapping(native, state):
    assert _status(native)[0] == state


def record(kind, payload, timestamp=None):
    return (
        json.dumps(
            {
                "timestamp": (timestamp or datetime.now(UTC)).isoformat(),
                "type": kind,
                "payload": payload,
            }
        )
        + "\n"
    )


def meta(sid="external", source="vscode", originator="Codex Desktop", timestamp=None):
    return record(
        "session_meta",
        {
            "id": sid,
            "source": source,
            "originator": originator,
            "cwd": "/tmp/project",
            "base_instructions": "secret content must not leak",
        },
        timestamp,
    )


@pytest.mark.parametrize("title", ["Thread title", "project", "Codex"])
def test_rollout_named_title_wins_over_first_prompt(tmp_path, title):
    store = StateStore()
    (tmp_path / "rollout-title.jsonl").write_text(
        record("session_meta", {"id": "named", "cwd": "/tmp/project", "title": title})
        + record("event_msg", {"type": "user_message", "message": "First prompt"})
    )
    RolloutObserver(store, tmp_path).scan()
    session = store.get("codex:named")
    assert session.title == title
    assert session.cwd == "/tmp/project"


@pytest.mark.parametrize("title", [None, "", "  ", {"text": "not a title"}, 123])
def test_rollout_without_title_never_uses_folder_or_provider(tmp_path, title):
    store = StateStore()
    (tmp_path / "rollout-title.jsonl").write_text(
        record("session_meta", {"id": "unknown", "cwd": "/tmp/project", "title": title})
    )
    RolloutObserver(store, tmp_path).scan()
    assert store.get("codex:unknown").title == ""
    assert store.get("codex:unknown").cwd == "/tmp/project"


@pytest.mark.parametrize("kind", ["event_msg", "response_item"])
def test_rollout_first_prompt_title_is_redacted_bounded_and_stable(tmp_path, kind):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    prompt = (
        "Fix detection password=throwaway-secret ```private code``` " + "panel " * 50
    )
    if kind == "event_msg":
        first = {"type": "user_message", "message": prompt}
    else:
        first = {
            "type": "message",
            "role": "user",
            "content": [
                {"type": "input_text", "text": prompt},
                {"type": "input_image", "text": "image body must not appear"},
            ],
        }
    path = tmp_path / "rollout-title.jsonl"
    path.write_text(meta() + record(kind, first))
    observer.scan()
    title = store.get("codex:external").title
    assert title.startswith("Fix detection password=[redacted] [code omitted]")
    assert len(title) <= 120
    assert title.endswith("…")
    assert "throwaway-secret" not in title
    assert "private code" not in title
    assert "image body" not in title
    with path.open("a") as output:
        output.write(
            record("event_msg", {"type": "user_message", "message": "Next task"})
        )
        output.write(
            record("session_meta", {"id": "external", "cwd": "/tmp/new-project"})
        )
    observer.scan()
    assert store.get("codex:external").title == title
    assert store.get("codex:external").cwd == "/tmp/new-project"
    assert store.get("codex:external").state == "working"


@pytest.mark.parametrize("message", [None, {}, 123, "   "])
def test_rollout_does_not_stringify_non_prompt_content(tmp_path, message):
    store = StateStore()
    (tmp_path / "rollout-title.jsonl").write_text(
        meta()
        + record("event_msg", {"type": "user_message", "message": message})
        + record(
            "response_item",
            {"type": "function_call_output", "output": "Tool body"},
        )
        + record(
            "response_item",
            {"type": "message", "role": "user", "content": [{"type": "image"}]},
        )
    )
    RolloutObserver(store, tmp_path).scan()
    assert store.get("codex:external").title == ""


def test_rollout_late_thread_title_replaces_prompt_without_resetting_state(tmp_path):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    path = tmp_path / "rollout-title.jsonl"
    path.write_text(
        meta()
        + record("event_msg", {"type": "user_message", "message": "First prompt"})
    )
    observer.scan()
    assert store.get("codex:external").title == "First prompt"
    with path.open("a") as output:
        output.write(
            record("session_meta", {"id": "external", "title": "Real thread name"})
        )
        output.write(record("session_meta", {"id": "external", "title": "   "}))
        output.write(
            record("event_msg", {"type": "user_message", "message": "Second prompt"})
        )
    observer.scan()
    session = store.get("codex:external")
    assert len(store.sessions()) == 1
    assert session.title == "Real thread name"
    assert session.cwd == "/tmp/project"
    assert session.state == "working"


@pytest.mark.parametrize("rotation", ["truncate", "replace_inode"])
@pytest.mark.parametrize("source", ["name", "prompt"])
async def test_rollout_rotation_preserves_title_for_same_session_id(
    tmp_path, rotation, source
):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    path = tmp_path / "rollout-title.jsonl"
    title = "Named thread" if source == "name" else "First prompt"
    metadata = {"id": "external", "cwd": "/tmp/project"}
    if source == "name":
        metadata["title"] = title
    path.write_text(
        record("session_meta", metadata)
        + record("event_msg", {"type": "user_message", "message": "First prompt"})
        + record(
            "response_item", {"type": "function_call_output", "output": "x" * 1000}
        )
    )
    observer.scan()
    stream = store.events()
    pending = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    if rotation == "truncate":
        path.write_text(meta())
    else:
        replacement = tmp_path / "replacement.jsonl"
        replacement.write_text(meta())
        replacement.replace(path)
    observer.scan()
    assert (await pending).session.title == title
    current = store.get("codex:external")
    assert current.title == title
    assert current.title_source == source
    assert current.state == "idle"
    with path.open("a") as output:
        output.write(
            record("event_msg", {"type": "user_message", "message": "Later prompt"})
        )
    observer.scan()
    assert store.get("codex:external").title == title
    assert len(store.sessions()) == 1
    await stream.aclose()
    await store.close()


async def test_owned_first_prompt_title_redacts_before_truncating(adapter, tmp_path):
    prompt = "Fix panel password=throwaway-secret ```private code``` " + "panel " * 50
    session = await adapter.start(str(tmp_path), prompt)
    assert session.title.startswith("Fix panel password=[redacted] [code omitted]")
    assert "throwaway-secret" not in session.title
    assert "private code" not in session.title
    assert len(session.title) <= 120


@pytest.mark.parametrize(
    "context",
    [
        "# AGENTS.md instructions for /tmp/project\n<INSTRUCTIONS>Read files</INSTRUCTIONS>",
        "<environment_context>cwd=/tmp/project</environment_context>",
        "<user_instructions>Read the project guide</user_instructions>",
    ],
)
def test_rollout_bootstrap_user_context_does_not_become_title(tmp_path, context):
    store = StateStore()
    (tmp_path / "rollout-title.jsonl").write_text(
        meta()
        + record(
            "response_item",
            {"type": "message", "role": "user", "content": context},
        )
        + record(
            "event_msg", {"type": "user_message", "message": "Inspect ESP32 detection"}
        )
    )
    RolloutObserver(store, tmp_path).scan()
    assert store.get("codex:external").title == "Inspect ESP32 detection"


async def test_owned_thread_title_takes_priority_over_prompt(
    adapter, tmp_path, monkeypatch
):
    rpc = adapter._rpc

    async def named_thread(method, params):
        result = await rpc(method, params)
        if method == "thread/start":
            result["thread"]["title"] = "Real thread name"
        return result

    monkeypatch.setattr(adapter, "_rpc", named_thread)
    session = await adapter.start(str(tmp_path), "First prompt")
    assert session.title == "Real thread name"


def test_rollout_incremental_partial_rotation_and_no_contents(tmp_path):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    path = tmp_path / "rollout-one.jsonl"
    path.write_text(
        meta()
        + record("event_msg", {"type": "task_started"})
        + record(
            "response_item",
            {"type": "function_call_output", "output": "secret file body"},
        )
    )
    observer.scan()
    session = store.get("codex:external")
    assert session.origin == "desktop app"
    assert session.state == "working"
    assert "secret" not in str(session)
    with path.open("a") as stream:
        line = record(
            "response_item",
            {
                "type": "message",
                "role": "assistant",
                "content": [{"type": "output_text", "text": "answer " * 100}],
            },
        )
        stream.write(line[:-3])
    observer.scan()
    assert not store.get(session.id).last_assistant_message
    with path.open("a") as stream:
        stream.write(line[-3:])
    observer.scan()
    assert len(store.get(session.id).last_assistant_message) <= 300
    assert store.get(session.id).last_assistant_message.startswith("answer")
    before = path.read_bytes()
    observer.scan()
    assert path.read_bytes() == before
    path.write_text(meta("replacement", "cli", "codex_cli"))
    observer.scan()
    assert store.get("codex:replacement").origin == "cli"


async def test_initial_rollout_hydration_does_not_replay_completion(tmp_path):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    path = tmp_path / "rollout-one.jsonl"
    path.write_text(
        meta()
        + record("event_msg", {"type": "task_complete", "last_agent_message": "done"})
    )
    stream = store.events()
    pending = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    observer.scan()
    assert (await pending).type == "session.changed"
    pending = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    with path.open("a") as output:
        output.write(
            record(
                "event_msg", {"type": "task_complete", "last_agent_message": "second"}
            )
        )
    observer.scan()
    events = [await pending, await anext(stream)]
    assert [event.type for event in events] == ["session.changed", "turn.completed"]
    await stream.aclose()


def test_stale_working_rollout_reports_uncertainty(tmp_path):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    old = datetime.now(UTC) - timedelta(minutes=20)
    (tmp_path / "rollout-stale.jsonl").write_text(
        meta(timestamp=old) + record("event_msg", {"type": "task_started"}, old)
    )
    observer.scan()
    session = store.get("codex:external")
    assert session.state == "idle"
    assert "live state unknown" in session.waiting_for
    assert session.updated_at == old


def test_malformed_and_oversized_lines_are_skipped(tmp_path):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    path = tmp_path / "rollout-one.jsonl"
    path.write_text("not json\n" + "x" * (MAX_LINE + 20) + "\n" + meta())
    observer.scan()
    assert store.get("codex:external")


def test_owned_rollouts_do_not_overwrite_live_session(tmp_path):
    store = StateStore()
    live = store.update(
        Session(
            id="codex:external",
            kind="codex",
            origin="hub-started",
            title="owned",
            cwd="/tmp",
            state="needs_approval",
        )
    )
    (tmp_path / "rollout-one.jsonl").write_text(meta())
    RolloutObserver(store, tmp_path).scan({live.id})
    assert store.get(live.id) == live


def test_old_files_and_symlinks_are_ignored(tmp_path):
    store = StateStore()
    path = tmp_path / "rollout-old.jsonl"
    path.write_text(meta())
    os.utime(path, (1, 1))
    (tmp_path / "rollout-link.jsonl").symlink_to(path)
    RolloutObserver(store, tmp_path).scan()
    assert store.sessions() == []


@pytest.mark.parametrize("mode", ["expired", "missing"])
def test_previously_observed_files_become_explicitly_stale(tmp_path, mode):
    store = StateStore()
    observer = RolloutObserver(store, tmp_path)
    path = tmp_path / "rollout-one.jsonl"
    path.write_text(meta())
    observer.scan()
    if mode == "expired":
        os.utime(path, (1, 1))
    else:
        path.unlink()
    observer.scan()
    session = store.get("codex:external")
    assert session.state == "done"
    assert "live state unknown" in session.waiting_for


async def test_multiscan_hydration_never_replays_old_completions(tmp_path):
    store = StateStore()
    path = tmp_path / "rollout-one.jsonl"
    path.write_text(meta() + record("event_msg", {"type": "task_complete"}))
    observer = RolloutObserver(store, tmp_path, bytes_per_scan=1)
    observer.scan()
    stream = store.events()
    pending = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    observer.scan()
    await asyncio.sleep(0)
    assert not pending.done()
    pending.cancel()
    with pytest.raises(asyncio.CancelledError):
        await pending
    await stream.aclose()


@pytest.mark.parametrize("prompt", ["malformed-notification", "duplicate-request"])
async def test_malformed_backend_protocol_fails_owned_session(
    adapter, tmp_path, prompt
):
    session = await adapter.start(str(tmp_path), prompt)
    await until(lambda: adapter.store.get(session.id).state == "error")
    assert adapter.store.pending_approvals() == []
    with pytest.raises(SessionUnavailable):
        await adapter.send(session.id, "retry")


async def test_completed_turn_discards_stale_questions(adapter, tmp_path):
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: adapter.store.get(session.id).state == "waiting_input")
    adapter._notification(
        {
            "method": "turn/completed",
            "params": {
                "threadId": "test-thread",
                "turn": {"id": "test-turn", "status": "completed"},
            },
        }
    )
    assert session.id not in adapter._questions
    await adapter.send(session.id, "new turn")
    await until(
        lambda: adapter.store.get(session.id).last_assistant_message == "new turn"
    )


async def test_failed_approval_delivery_marks_session_error(
    adapter, tmp_path, monkeypatch
):
    session = await adapter.start(str(tmp_path), "approval")
    await until(lambda: adapter.store.pending_approvals())

    async def fail(message):
        raise SessionUnavailable("transport failure")

    monkeypatch.setattr(adapter, "_write", fail)
    with pytest.raises(AgentError) as error:
        await adapter.store.approve(
            adapter.store.pending_approvals()[0].request_id, "allow"
        )
    assert error.value.code == "delivery_failed"
    await until(lambda: adapter.store.get(session.id).state == "error")
    assert "delivery is uncertain" in adapter.store.get(session.id).waiting_for


async def test_close_during_subprocess_creation_reaps_late_child(tmp_path, monkeypatch):
    store = StateStore()
    adapter = CodexAdapter(store, command=[sys.executable, str(FAKE)], observe=False)
    original = asyncio.create_subprocess_exec
    entered = asyncio.Event()
    proceed = asyncio.Event()

    async def delayed(*args, **kwargs):
        entered.set()
        await proceed.wait()
        return await original(*args, **kwargs)

    monkeypatch.setattr(asyncio, "create_subprocess_exec", delayed)
    starting = asyncio.create_task(adapter.start(str(tmp_path), "hello"))
    await entered.wait()
    await adapter.close()
    proceed.set()
    with pytest.raises(SessionUnavailable, match="closed during startup"):
        await starting
    assert adapter._proc.returncode is not None
    assert adapter._reader is None


async def test_protocol_failure_kills_uncooperative_child_without_explicit_close():
    store = StateStore()
    adapter = CodexAdapter(
        store,
        command=[sys.executable, str(FAKE)],
        env={**os.environ, "ORB_FAKE_INIT": "malformed-ignore-term"},
        observe=False,
    )
    # Bypass _ensure_server cleanup to exercise the reader's own reaper.
    adapter._proc = await asyncio.create_subprocess_exec(
        *adapter.command,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        env=adapter.env,
    )
    reader = asyncio.create_task(adapter._read_loop())
    await adapter._write({"id": "test", "method": "initialize", "params": {}})
    async with asyncio.timeout(4):
        await reader
    assert adapter._proc.returncode is not None
    await adapter.close()


@pytest.mark.parametrize(
    "last_event,state", [("task_started", "working"), ("error", "error")]
)
def test_rollout_completion_does_not_override_later_record(tmp_path, last_event, state):
    store = StateStore()
    path = tmp_path / "rollout-one.jsonl"
    path.write_text(meta())
    observer = RolloutObserver(store, tmp_path)
    observer.scan()
    with path.open("a") as output:
        output.write(record("event_msg", {"type": "task_complete"}))
        output.write(record("event_msg", {"type": last_event}))
    observer.scan()
    assert store.get("codex:external").state == state


async def test_question_reply_does_not_reanimate_completed_turn(
    adapter, tmp_path, monkeypatch
):
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: adapter.store.get(session.id).state == "waiting_input")

    async def complete_during_write(message, **kwargs):
        adapter._notification(
            {
                "method": "turn/completed",
                "params": {
                    "threadId": "test-thread",
                    "turn": {"id": "test-turn", "status": "completed"},
                },
            }
        )

    monkeypatch.setattr(adapter, "_write", complete_during_write)
    await adapter.send(session.id, "yes")
    assert adapter.store.get(session.id).state == "idle"


async def test_permission_request_exposes_actual_grant_scope(adapter, tmp_path):
    await adapter.start(str(tmp_path), "permissions")
    await until(lambda: adapter.store.pending_approvals())
    assert adapter.store.pending_approvals()[0].scope == "turn"


async def test_approval_waits_for_provider_write_failure(
    adapter, tmp_path, monkeypatch
):
    await adapter.start(str(tmp_path), "approval")
    await until(lambda: adapter.store.pending_approvals())
    request = adapter.store.pending_approvals()[0]
    entered, release = asyncio.Event(), asyncio.Event()

    async def fail_write(message):
        entered.set()
        await release.wait()
        raise BrokenPipeError("throwaway provider disconnected")

    monkeypatch.setattr(adapter, "_write", fail_write)
    approval = asyncio.create_task(adapter.store.approve(request.request_id, "allow"))
    try:
        await asyncio.wait_for(entered.wait(), 1)
        assert not approval.done()
        release.set()
        with pytest.raises(AgentError) as error:
            await approval
        assert error.value.code == "delivery_failed"
    finally:
        release.set()
        await asyncio.gather(approval, return_exceptions=True)


async def test_stale_question_rejected_after_waiting_for_session_lock(
    adapter, tmp_path, monkeypatch
):
    session = await adapter.start(str(tmp_path), "question")
    await until(lambda: adapter.store.get(session.id).state == "waiting_input")
    question_id = adapter.store.get(session.id).question_id
    assert question_id
    sent = []

    async def capture(message, **kwargs):
        sent.append(message)

    monkeypatch.setattr(adapter, "_write", capture)
    lock = adapter._locks[session.id]
    async with lock:
        answer = asyncio.create_task(
            adapter.answer_question(session.id, question_id, "old answer")
        )
        await asyncio.sleep(0)
        old_rpc_id = adapter._questions[session.id][0]
        adapter._notification(
            {
                "method": "serverRequest/resolved",
                "params": {"threadId": "test-thread", "requestId": old_rpc_id},
            }
        )
        await adapter._server_request(
            {
                "id": "new-question",
                "method": "item/tool/requestUserInput",
                "params": {
                    "threadId": "test-thread",
                    "questions": [{"id": "q1", "question": "New question"}],
                },
            }
        )
    with pytest.raises(AgentError) as error:
        await answer
    assert error.value.code == "stale_question"
    assert sent == []
    assert adapter._questions[session.id][0] == "new-question"


@pytest.mark.parametrize("replacement_id", [1, "1"])
async def test_question_identity_survives_reused_or_mixed_type_rpc_ids(
    adapter, tmp_path, monkeypatch, replacement_id
):
    session = await adapter.start(str(tmp_path), "hold")
    sent = []

    async def capture(message, **kwargs):
        sent.append(message)

    monkeypatch.setattr(adapter, "_write", capture)
    request = {
        "id": 1,
        "method": "item/tool/requestUserInput",
        "params": {
            "threadId": "test-thread",
            "questions": [{"id": "q1", "question": "Original"}],
        },
    }
    await adapter._server_request(request)
    old_question_id = adapter.store.get(session.id).question_id
    adapter._notification(
        {
            "method": "serverRequest/resolved",
            "params": {"threadId": "test-thread", "requestId": 1},
        }
    )
    await adapter._server_request({**request, "id": replacement_id})
    with pytest.raises(AgentError) as error:
        await adapter.answer_question(session.id, old_question_id, "Old answer")
    assert error.value.code == "stale_question"
    assert sent == []
    current_question_id = adapter.store.get(session.id).question_id
    assert current_question_id != old_question_id
    await adapter.answer_question(session.id, current_question_id, "Current answer")
    assert sent == [
        {
            "id": replacement_id,
            "result": {"answers": {"q1": {"answers": ["Current answer"]}}},
        }
    ]


async def test_question_rechecks_instance_after_transport_lock(
    adapter, tmp_path, monkeypatch
):
    session = await adapter.start(str(tmp_path), "hold")
    request = {
        "id": 1,
        "method": "item/tool/requestUserInput",
        "params": {
            "threadId": "test-thread",
            "questions": [{"id": "q1", "question": "Original"}],
        },
    }
    await adapter._server_request(request)
    old_question_id = adapter.store.get(session.id).question_id
    writes = []
    monkeypatch.setattr(
        adapter._proc.stdin, "write", lambda data: writes.append(json.loads(data))
    )
    async with adapter._write_lock:
        reply = asyncio.create_task(
            adapter.answer_question(session.id, old_question_id, "Old answer")
        )
        await asyncio.sleep(0)
        adapter._notification(
            {
                "method": "serverRequest/resolved",
                "params": {"threadId": "test-thread", "requestId": 1},
            }
        )
        await adapter._server_request(request)
    with pytest.raises(AgentError) as error:
        await reply
    assert error.value.code == "stale_question"
    assert writes == []
    assert adapter.store.get(session.id).state == "waiting_input"
