"""Integration of passive data collectors using invented local transcripts only."""

import asyncio
import json
import sys
from datetime import UTC, datetime
from pathlib import Path

from orb_agents import AgentManager, StateStore
from orb_agents.claude import ClaudeAdapter
from orb_agents.codex import CodexAdapter
from orb_agents.models import Session
from orb_agents.usage import write_claude_statusline

from orb_hub.agent_data import AgentData

CLAUDE_ID = "11111111-2222-4333-8444-555555555555"
AT = 1793102400.0


def manager(tmp_path):
    config = tmp_path / "synthetic-claude"
    root = tmp_path / "synthetic-codex"
    config.mkdir()
    root.mkdir()
    store = StateStore()
    value = AgentManager(
        store=store,
        adapters={
            "claude": ClaudeAdapter(store, config_dir=config, executable="synthetic-claude"),
            "codex": CodexAdapter(store, sessions_root=root),
        },
    )
    return value, config, root


def session(identity, provider="codex", state="working"):
    return Session(
        identity, provider, "cli", "Synthetic", "/synthetic", state=state, updated_at=datetime.fromtimestamp(AT, UTC)
    )


def line(record):
    return json.dumps(record) + "\n"


def reply(text="A synthetic reply.", identity="m1"):
    return {
        "type": "response_item",
        "timestamp": AT,
        "payload": {
            "type": "message",
            "id": identity,
            "role": "assistant",
            "content": [{"type": "output_text", "text": text}],
        },
    }


async def test_codex_filename_identity_repairs_late_attach_without_session_meta(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    # First attach starts in the last bounded tail. Its metadata is intentionally
    # outside that tail; the stable filename must still bind the admitted session.
    path.write_text(
        line({"type": "session_meta", "payload": {"id": "thread-one"}}) + " " * (2 * 1024 * 1024) + "\n" + line(reply())
    )
    data = AgentData(value, tmp_path / "data")
    observed = session("codex:thread-one")
    await data.refresh([observed])
    result = data.snapshot(observed, "wire-one", now=AT + 10)
    assert result["session"] == "wire-one"
    assert result["assistant"]["text"] == "A synthetic reply."
    assert result["stale"] is False
    assert result["state"] == "idle"


async def test_session_discovered_after_usage_tail_read_hydrates_existing_progress(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    path.write_text(line({"type": "session_meta", "payload": {"id": "thread-one"}}) + line(reply()))
    data = AgentData(value, tmp_path / "data")
    await data.refresh([])
    assert data._reducers == {}
    observed = session("codex:thread-one")
    await data.refresh([observed])
    result = data.snapshot(observed, "wire-one", now=AT + 10)
    assert result["assistant"]["text"] == "A synthetic reply."
    assert result["stale"] is False


async def test_filename_identity_discovered_after_a_usage_only_read_rehydrates(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    path.write_text(line(reply()))
    data = AgentData(value, tmp_path / "data")
    await data.refresh([])
    observed = session("codex:thread-one")
    await data.refresh([observed])
    # Provider discovery can lag behind the progress loop's first new session.
    data._next_scan = 0
    await data.refresh([observed])
    assert data.snapshot(observed, "wire-one", now=AT + 10)["assistant"]["text"] == "A synthetic reply."


async def test_session_return_after_reducer_pruning_rehydrates_unchanged_tail(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    path.write_text(line(reply()))
    data = AgentData(value, tmp_path / "data")
    observed = session("codex:thread-one")
    await data.refresh([observed])
    data._next_scan = 0
    await data.refresh([])
    assert data._reducers == {}
    await data.refresh([observed])
    assert data.snapshot(observed, "wire-one", now=AT + 10)["assistant"]["text"] == "A synthetic reply."


async def test_claude_incremental_partial_lines_and_tool_failure(tmp_path):
    value, config, _ = manager(tmp_path)
    projects = config / "projects" / "synthetic-project"
    projects.mkdir(parents=True)
    path = projects / f"{CLAUDE_ID}.jsonl"
    tool = {
        "type": "assistant",
        "timestamp": AT,
        "message": {
            "id": "m1",
            "content": [
                {"type": "tool_use", "id": "t1", "name": "Edit", "input": {"path": "/synthetic/orb_nav.c"}},
            ],
        },
    }
    result = {
        "type": "user",
        "timestamp": AT + 1,
        "message": {
            "content": [
                {"type": "tool_result", "tool_use_id": "t1", "is_error": True, "content": "synthetic failure"},
            ]
        },
    }
    encoded = line(result)
    path.write_text(line(tool) + encoded[:-3])
    data = AgentData(value, tmp_path / "data")
    observed = session(f"claude:{CLAUDE_ID}", "claude", "needs_approval")
    await data.refresh([observed])
    first = data.snapshot(observed, "wire-one", now=AT + 10)
    assert first["steps"][0]["state"] == "running"
    assert first["state"] == "waiting"
    with path.open("a") as stream:
        stream.write(encoded[-3:])
    await data.refresh([observed])
    final = data.snapshot(observed, "wire-one", now=AT + 10)
    assert final["steps"][0]["state"] == "failed"
    assert final["seq"] > first["seq"]
    assert final["state"] == "waiting"


async def test_replayed_truncation_inode_replacement_and_new_records(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    original = reply()
    path.write_text(line(original))
    data = AgentData(value, tmp_path / "data")
    observed = session("codex:thread-one")
    await data.refresh([observed])
    first = data.snapshot(observed, "wire-one", now=AT + 10)
    # Rewriting the same records does not invent progress or renew freshness.
    path.write_text(line(original))
    await data.refresh([observed])
    assert data.snapshot(observed, "wire-one", now=AT + 10) == first
    replacement = root / "replacement"
    replacement.write_text(line(original) + line(reply("New synthetic sentence.", "m2")))
    replacement.replace(path)
    await data.refresh([observed])
    second = data.snapshot(observed, "wire-one", now=AT + 10)
    assert second["assistant"]["text"] == "New synthetic sentence."
    assert second["seq"] == first["seq"] + 1
    await data.refresh([observed])
    assert data.snapshot(observed, "wire-one", now=AT + 10) == second


async def test_usage_reads_unfocused_codex_and_private_claude_statusline(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-unadmitted.jsonl"
    path.write_text(
        line(
            {
                "type": "event_msg",
                "timestamp": AT,
                "payload": {
                    "type": "token_count",
                    "rate_limits": {
                        "primary": {"used_percent": 22, "window_minutes": 300, "resets_at": AT + 10000},
                        "secondary": None,
                    },
                },
            }
        )
    )
    directory = tmp_path / "data"
    write_claude_statusline(
        directory,
        {
            "rate_limits": {
                "seven_day": {
                    "used_percentage": 33,
                    "resets_at": AT + 10000,
                }
            }
        },
        now=AT,
    )
    data = AgentData(value, directory)
    await data.refresh([])
    snapshot = data.usage.snapshot(now=AT + 10)
    assert snapshot["unknown"] == []
    assert [(w["provider"], w["label"], w["used_percent"]) for w in snapshot["windows"]] == [
        ("claude", "week", 33),
        ("codex", "5 hours", 22),
    ]
    assert data._reducers == {}


async def test_owned_codex_account_usage_notification_reaches_hub_without_rollouts(tmp_path):
    fake = Path(__file__).parents[1] / "packages/orb_agents/tests/fixtures/codex_fake_server.py"
    store = StateStore()
    adapter = CodexAdapter(store, command=[sys.executable, str(fake)], observe=False, rpc_timeout=2)
    async with AgentManager(store=store, adapters={"codex": adapter}) as value:

        async def completed():
            async for event in store.events():
                if event.type == "turn.completed":
                    return

        completion = asyncio.create_task(completed())
        await asyncio.sleep(0)
        try:
            await value.start("codex", str(tmp_path), "usage-week")
            # Fake completion follows the account notification on the same pipe.
            async with asyncio.timeout(2):
                await completion
        finally:
            completion.cancel()
            await asyncio.gather(completion, return_exceptions=True)
        data = AgentData(value, tmp_path / "data")
        await data.refresh(store.sessions())
        snapshot = data.usage.snapshot()
        assert snapshot["unknown"] == ["claude"]
        assert [(w["provider"], w["label"], w["used_percent"], w["source"]) for w in snapshot["windows"]] == [
            ("codex", "week", 37, "app-server")
        ]


async def test_removed_transcript_does_not_abort_other_provider_data(tmp_path):
    value, _, root = manager(tmp_path)
    first = root / "rollout-synthetic-thread-one.jsonl"
    second = root / "rollout-synthetic-thread-two.jsonl"
    first.write_text(line(reply("First reply.")))
    second.write_text(line(reply("Second reply.")))
    data = AgentData(value, tmp_path / "data")
    observed = [session("codex:thread-one"), session("codex:thread-two")]
    await data.refresh(observed)
    first.unlink()
    with second.open("a") as stream:
        stream.write(line(reply("Second new reply.", "m2")))
    # This refresh intentionally occurs before the next path discovery scan.
    await data.refresh(observed)
    assert data.snapshot(observed[1], "wire-two", now=AT + 10)["assistant"]["text"] == "Second new reply."


async def test_discovery_prunes_removed_session_reducers(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    path.write_text(line(reply()))
    data = AgentData(value, tmp_path / "data")
    await data.refresh([session("codex:thread-one")])
    assert "codex:thread-one" in data._reducers
    data._next_scan = 0
    await data.refresh([])
    assert data._reducers == {}


async def test_reused_rollout_identity_stays_authoritative_across_rescan(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    path.write_text(
        line({"type": "session_meta", "payload": {"id": "thread-one"}}) + line(reply("Original thread reply."))
    )
    data = AgentData(value, tmp_path / "data")
    observed = [session("codex:thread-one"), session("codex:thread-two")]
    await data.refresh(observed)
    replacement = root / "replacement"
    replacement.write_text(
        line({"type": "session_meta", "payload": {"id": "thread-two"}}) + line(reply("Replacement thread reply.", "m2"))
    )
    replacement.replace(path)
    await data.refresh(observed)
    assert data.snapshot(observed[1], "wire-two", now=AT + 10)["assistant"]["text"] == "Replacement thread reply."
    data._next_scan = 0
    with path.open("a") as stream:
        stream.write(line(reply("Replacement appended reply.", "m3")))
    await data.refresh(observed)
    assert data.snapshot(observed[1], "wire-two", now=AT + 10)["assistant"]["text"] == "Replacement appended reply."
    assert data.snapshot(observed[0], "wire-one", now=AT + 10)["assistant"]["text"] == "Original thread reply."


async def test_new_tool_progress_overrides_lagging_idle_metadata(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    path.write_text(
        line(
            {
                "type": "response_item",
                "timestamp": AT + 1,
                "payload": {
                    "type": "function_call",
                    "name": "exec_command",
                    "call_id": "t1",
                    "arguments": {"cmd": "idf.py -j4 build"},
                },
            }
        )
    )
    data = AgentData(value, tmp_path / "data")
    observed = session("codex:thread-one", state="idle")
    await data.refresh([observed])
    snapshot = data.snapshot(observed, "wire-one", now=AT + 10)
    assert snapshot["steps"][0]["state"] == "running"
    assert snapshot["state"] == "busy"


async def test_final_progress_overrides_lagging_working_metadata(tmp_path):
    value, _, root = manager(tmp_path)
    path = root / "rollout-synthetic-thread-one.jsonl"
    record = reply()
    record["timestamp"] = AT + 1
    path.write_text(line(record))
    data = AgentData(value, tmp_path / "data")
    observed = session("codex:thread-one", state="working")
    await data.refresh([observed])
    snapshot = data.snapshot(observed, "wire-one", now=AT + 10)
    assert snapshot["assistant"]["final"]
    assert snapshot["state"] == "idle"
