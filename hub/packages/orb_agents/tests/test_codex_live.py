"""Real CLI smoke. No credentials are copied; optional key must be disposable.

Run with ORB_AGENTS_LIVE=1. With no dedicated provider key, this still attempts
an isolated real thread and reports the native authentication/setup blocker.
"""

from __future__ import annotations

import asyncio
import os
from pathlib import Path

import pytest

from orb_agents.codex import DEFAULT_CODEX, CodexAdapter
from orb_agents.models import AgentError, summary
from orb_agents.store import StateStore

pytestmark = [
    pytest.mark.live,
    pytest.mark.skipif(
        os.environ.get("ORB_AGENTS_LIVE") != "1",
        reason="set ORB_AGENTS_LIVE=1 for isolated real CLI smoke",
    ),
]


async def test_codex_isolated_status_approval_and_send(tmp_path):
    executable = os.environ.get("ORB_AGENTS_CODEX_BIN", DEFAULT_CODEX)
    if not await asyncio.to_thread(Path(executable).is_file):
        pytest.fail(
            "Codex CLI is unavailable; set ORB_AGENTS_CODEX_BIN to its absolute path"
        )
    home = tmp_path / "codex-home"
    project = tmp_path / "project"
    home.mkdir(mode=0o700)
    project.mkdir()
    config = 'cli_auth_credentials_store = "file"\n'
    # This opt-in variable must hold a dedicated test key, never a copied login.
    test_key = os.environ.get("ORB_AGENTS_CODEX_TEST_API_KEY")
    if test_key:
        config += '\nmodel_provider = "orb_smoke"\n[model_providers.orb_smoke]\nname = "Orb disposable smoke"\nbase_url = "https://api.openai.com/v1"\nenv_key = "ORB_AGENTS_CODEX_TEST_API_KEY"\nwire_api = "responses"\n'
    (home / "config.toml").write_text(config)
    env = {
        key: os.environ[key]
        for key in ("HOME", "PATH", "TMPDIR", "LANG")
        if key in os.environ
    }
    env["CODEX_HOME"] = str(home)
    if test_key:
        env["ORB_AGENTS_CODEX_TEST_API_KEY"] = test_key
    model = os.environ.get("ORB_AGENTS_CODEX_MODEL", "gpt-6-luna")
    store = StateStore()
    adapter = CodexAdapter(
        store,
        executable=executable,
        env=env,
        process_cwd=str(project),
        observe=False,
        approval_policy="on-request",
        sandbox="read-only",
        rpc_timeout=30,
    )
    stream = store.events()
    events = []
    changed = asyncio.Event()

    async def collect():
        async for event in stream:
            events.append(event)
            changed.set()
            if event.type == "approval.requested":
                await store.approve(
                    event.request_id, "deny", "Disposable test denial; do not retry"
                )

    collector = asyncio.create_task(collect())
    await asyncio.sleep(0)
    try:
        try:
            session = await adapter.start(
                str(project),
                "This is an isolated Orb integration smoke. Request explicit tool approval to execute `touch orb-smoke-marker` in this directory using exec_command with sandbox_permissions=require_escalated and justification 'Orb disposable permission smoke'. Do not use other tools. If permission is denied, do not retry; reply exactly DENIED.",
                model,
            )
        except AgentError as exc:
            pytest.fail(
                f"Isolated Codex startup/turn blocked ({exc.code}): {exc}. No credentials were copied. Set a dedicated ORB_AGENTS_CODEX_TEST_API_KEY to exercise the full model path."
            )
        async with asyncio.timeout(90):
            while not any(
                event.type == "turn.completed" and event.session_id == session.id
                for event in events
            ):  # noqa: ASYNC110 - external CLI probe
                current = store.get(session.id)
                if current and current.state == "error":
                    account = await adapter._rpc(
                        "account/read", {"refreshToken": False}
                    )
                    if account.get("account") is None and not test_key:
                        queue = await asyncio.create_subprocess_exec(
                            executable,
                            "queue",
                            "--thread",
                            session.id.removeprefix("codex:"),
                            "--message",
                            "Orb disposable queue probe: reply OK without tools.",
                            cwd=str(project),
                            env=env,
                            stdout=asyncio.subprocess.PIPE,
                            stderr=asyncio.subprocess.PIPE,
                        )
                        try:
                            async with asyncio.timeout(10):
                                stdout, stderr = await queue.communicate()
                            # Only this no-credential, throwaway invocation is inspected.
                            queue_result = f"exit={queue.returncode}; {summary((stderr or stdout).decode(errors='replace'), 250)}"
                        except TimeoutError:
                            queue.kill()
                            await queue.wait()
                            queue_result = (
                                "timed out after 10 seconds; child killed and reaped"
                            )
                        pytest.fail(
                            "Isolated Codex thread was created but has no credentials in fresh CODEX_HOME; native turn failed before approval. No owner credentials were copied. Rerun with a dedicated ORB_AGENTS_CODEX_TEST_API_KEY. "
                            f"Isolated codex queue probe: {queue_result}"
                        )
                    pytest.fail(
                        "Isolated Codex turn reported error before approval; check test account/model availability. Backend diagnostics are withheld to avoid secret exposure."
                    )
                await asyncio.sleep(0.05)
        assert any(
            event.type == "session.changed" and event.session.state == "working"
            for event in events
        ), "No working status observed"
        assert any(event.type == "approval.requested" for event in events), (
            "The model did not request approval; no approval proof"
        )
        assert any(
            event.type == "approval.resolved" and event.decision == "deny"
            for event in events
        )
        assert not (project / "orb-smoke-marker").exists(), (
            "Denied tool unexpectedly ran"
        )
        completions = sum(event.type == "turn.completed" for event in events)
        await adapter.send(
            session.id, "Reply exactly SECOND_OK without using any tools."
        )
        async with asyncio.timeout(60):
            while (
                sum(event.type == "turn.completed" for event in events) == completions
            ):
                changed.clear()
                await changed.wait()
        assert "SECOND_OK" in store.get(session.id).last_assistant_message
    finally:
        await adapter.close()
        await store.close()
        await collector
        await stream.aclose()
        assert adapter._proc is None or adapter._proc.returncode is not None
