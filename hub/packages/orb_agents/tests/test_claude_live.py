"""Opt-in Claude smoke; never reads/copies credentials or sends to live sessions.

Run with ORB_AGENTS_LIVE=1. Authentication must already be available through an
explicitly supplied ORB_AGENTS_CLAUDE_TEST_API_KEY; an isolated config intentionally
does not copy the owner's OAuth files or consult the owner's settings.
"""

from __future__ import annotations

import asyncio
import json
import os
import secrets
import shutil

import pytest

from orb_agents.claude import ClaudeAdapter
from orb_agents.hooks import HookServer
from orb_agents.installer import generate_hooks
from orb_agents.store import StateStore

pytestmark = [
    pytest.mark.live,
    pytest.mark.skipif(
        os.environ.get("ORB_AGENTS_LIVE") != "1",
        reason="Set ORB_AGENTS_LIVE=1 for isolated provider smoke tests",
    ),
]


async def test_claude_live_status_approval_and_send(tmp_path):
    executable = shutil.which("claude")
    if not executable:
        pytest.skip("claude CLI is not installed")
    config = tmp_path / "config"
    config.mkdir()
    workspace = tmp_path / "workspace"
    workspace.mkdir()
    settings = tmp_path / "settings.json"
    store = StateStore()
    events = []
    changed = asyncio.Event()
    secret = secrets.token_urlsafe(32)
    # No credential files or settings are copied. A deliberately supplied API
    # key in the test environment may authenticate normally without persistence.
    env = {
        key: os.environ[key]
        for key in ("HOME", "PATH", "TMPDIR", "LANG")
        if key in os.environ
    }
    if test_key := os.environ.get("ORB_AGENTS_CLAUDE_TEST_API_KEY"):
        env["ANTHROPIC_API_KEY"] = test_key
    env["CLAUDE_CONFIG_DIR"] = str(config)
    env["CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"] = "1"
    env.pop("CLAUDECODE", None)
    async with HookServer(store, secret, approval_timeout=25) as hooks:
        fragment = generate_hooks(hooks.url, secret, timeout=30)
        # Force a harmless write through permission handling even on versions
        # that classify ordinary workspace edits as automatically safe.
        fragment["permissions"] = {"ask": ["Write"]}
        settings.write_text(json.dumps(fragment))
        settings.chmod(0o600)
        adapter = ClaudeAdapter(
            store,
            executable=executable,
            env=env,
            config_dir=config,
            settings=settings,
            permission_transport="hooks",
            max_budget_usd=0.10,
            startup_timeout=20,
        )

        async def collect():
            async for event in store.events():
                events.append(event)
                changed.set()

        consumer = asyncio.create_task(collect())
        await asyncio.sleep(0)
        try:
            async with asyncio.timeout(90):
                session = await adapter.start(
                    str(workspace),
                    "Use the Write tool to create orb_smoke.txt containing exactly ORB_SMOKE. Do not use Bash or any other tool. After it is allowed, reply exactly FIRST_DONE.",
                    model=os.environ.get("ORB_AGENTS_CLAUDE_MODEL", "haiku"),
                )
                while not store.pending_approvals():
                    current = store.get(session.id)
                    if current.state == "error":
                        pytest.fail(
                            f"Isolated Claude smoke could not reach approval: {current.waiting_for or current.last_assistant_message}"
                        )
                    if any(event.type == "turn.completed" for event in events):
                        pytest.fail(
                            "Claude completed without the required Write permission request"
                        )
                    await asyncio.sleep(0.05)
                request = store.pending_approvals()[0]
                assert request.tool == "Write"
                assert store.get(session.id).state == "needs_approval"
                assert "ORB_SMOKE" not in request.summary
                await store.approve(request.request_id, "allow")
                while not any(event.type == "turn.completed" for event in events):
                    changed.clear()
                    await changed.wait()
                assert (workspace / "orb_smoke.txt").read_text().strip() == "ORB_SMOKE"
                first_completed = sum(
                    event.type == "turn.completed" for event in events
                )
                await adapter.send(
                    session.id, "Without using tools, reply exactly SECOND_DONE."
                )
                while (
                    sum(event.type == "turn.completed" for event in events)
                    <= first_completed
                ):
                    changed.clear()
                    await changed.wait()
                assert "SECOND_DONE" in store.get(session.id).last_assistant_message
                assert any(
                    event.type == "approval.resolved" and event.decision == "allow"
                    for event in events
                )
                assert any(
                    event.type == "session.changed" and event.session.state == "working"
                    for event in events
                )
        finally:
            consumer.cancel()
            await asyncio.gather(consumer, return_exceptions=True)
            await adapter.close()
            await store.close()
