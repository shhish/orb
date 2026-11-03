"""Delivery conformance against disposable fake executables only."""

from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

import pytest

from orb_agents import AgentError, AgentManager, DeliveryResult, Session, StateStore
from orb_agents.codex import CodexAdapter


@pytest.fixture
def queue_adapter(tmp_path):
    binary = tmp_path / "fake-codex"
    binary.write_text(
        f"#!{sys.executable}\n"
        "import json, os, sys, time\n"
        "from pathlib import Path\n"
        "assert sys.argv[1:3] == ['queue', '--thread']\n"
        "assert sys.argv[4] == '--message'\n"
        "Path(os.environ['ORB_FAKE_ARGS']).write_text(json.dumps(sys.argv[1:]))\n"
        "print('token=synthetic-do-not-forward')\n"
        "print('password=synthetic-do-not-forward', file=sys.stderr)\n"
        "time.sleep(float(os.environ.get('ORB_FAKE_SLEEP', '0')))\n"
        "sys.exit(int(os.environ.get('ORB_FAKE_EXIT', '0')))\n"
    )
    binary.chmod(0o700)
    store = StateStore()
    store.update(
        Session(
            "codex:synthetic-thread", "codex", "desktop app", "Synthetic", str(tmp_path)
        )
    )
    adapter = CodexAdapter(
        store,
        executable=str(binary),
        observe=False,
        enable_queue=True,
        rpc_timeout=2,
        env={"ORB_FAKE_ARGS": str(tmp_path / "arguments.json")},
    )
    return adapter


async def test_queue_is_opt_in_and_does_not_start_app_server(queue_adapter):
    queue_adapter.enable_queue = False
    assert not queue_adapter.can_message("codex:synthetic-thread")
    with pytest.raises(AgentError, match="read-only"):
        await queue_adapter.send("codex:synthetic-thread", "synthetic request")
    assert not queue_adapter._queue_processes
    assert queue_adapter._proc is None
    assert not await asyncio.to_thread(Path(queue_adapter.env["ORB_FAKE_ARGS"]).exists)
    await queue_adapter.close()


async def test_queue_preserves_argv_and_reports_queued(queue_adapter, capsys):
    message = "synthetic `echo shell` $(false) ; END=synthetic\nnext line"
    async with AgentManager(
        store=queue_adapter.store, adapters={"codex": queue_adapter}
    ) as manager:
        assert manager.can_message("codex:synthetic-thread")
        result = await manager.send("codex:synthetic-thread", message)
        assert result.status == "queued"
        assert "accepted" in result.reason
    args = await asyncio.to_thread(Path(queue_adapter.env["ORB_FAKE_ARGS"]).read_text)
    assert json.loads(args) == [
        "queue",
        "--thread",
        "synthetic-thread",
        "--message",
        message,
    ]
    assert queue_adapter._proc is None
    assert capsys.readouterr() == ("", "")


@pytest.mark.parametrize("code", [1, 2, 127])
async def test_queue_failure_is_safe_and_never_delivered(queue_adapter, code, capsys):
    queue_adapter.env["ORB_FAKE_EXIT"] = str(code)
    with pytest.raises(AgentError) as caught:
        await queue_adapter.send("codex:synthetic-thread", "synthetic request")
    assert caught.value.code == "delivery_failed"
    assert "synthetic-do-not-forward" not in str(caught.value)
    assert capsys.readouterr() == ("", "")
    assert not queue_adapter._queue_processes
    await queue_adapter.close()


async def test_queue_timeout_reaps_child_without_retry(queue_adapter):
    queue_adapter.env["ORB_FAKE_SLEEP"] = "60"
    queue_adapter.rpc_timeout = 0.2
    with pytest.raises(AgentError) as caught:
        await queue_adapter.send("codex:synthetic-thread", "synthetic request")
    assert caught.value.code == "timeout"
    assert "not retried" in str(caught.value)
    assert not queue_adapter._queue_processes
    await queue_adapter.close()


async def test_queue_cancel_reaps_child(queue_adapter):
    queue_adapter.env["ORB_FAKE_SLEEP"] = "60"
    queue_adapter.rpc_timeout = 10
    task = asyncio.create_task(
        queue_adapter.send("codex:synthetic-thread", "synthetic request")
    )
    async with asyncio.timeout(2):
        while not queue_adapter._queue_processes:  # noqa: ASYNC110 - wait for disposable child creation
            await asyncio.sleep(0.005)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert not queue_adapter._queue_processes
    await queue_adapter.close()


@pytest.mark.parametrize("state", ["done", "error", "needs_approval"])
async def test_queue_capability_rechecks_state(queue_adapter, state):
    from dataclasses import replace

    session = queue_adapter.store.get("codex:synthetic-thread")
    queue_adapter.store.update(replace(session, state=state))
    assert not queue_adapter.can_message(session.id)
    with pytest.raises(AgentError, match="unavailable"):
        await queue_adapter.send(session.id, "synthetic request")
    assert not await asyncio.to_thread(Path(queue_adapter.env["ORB_FAKE_ARGS"]).exists)
    await queue_adapter.close()


async def test_queue_rejects_unknown_or_malformed_identity(queue_adapter):
    for identity in ("codex:unknown", "claude:synthetic-thread", "codex:--unsafe"):
        assert not queue_adapter.can_message(identity)
        with pytest.raises(AgentError):
            await queue_adapter.send(identity, "synthetic request")
    await queue_adapter.close()


def test_manager_default_queue_flag_is_off():
    manager = AgentManager()
    assert manager.adapters["codex"].enable_queue is False
    opted_in = AgentManager(enable_codex_queue=True)
    assert opted_in.adapters["codex"].enable_queue is True


def test_delivery_result_has_only_explicit_transport_outcome():
    assert DeliveryResult("delivered", "transport acknowledged").status == "delivered"
