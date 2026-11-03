import asyncio
import json
import secrets
from dataclasses import replace

import pytest

from orb_agents.hooks import HookServer
from orb_agents.models import AgentError, Session
from orb_agents.store import StateStore


def payload(event="SessionStart", **fields):
    return {
        "hook_event_name": event,
        "session_id": "test-session",
        "cwd": "/tmp/throwaway",
        **fields,
    }


async def until(predicate):
    async with asyncio.timeout(2):
        while not predicate():  # noqa: ASYNC110 - bounded polling of observable state
            await asyncio.sleep(0.001)


async def post(
    server, secret, body=None, *, headers=None, method="POST", path="/hooks/claude"
):
    reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
    data = json.dumps(body if body is not None else payload()).encode()
    fields = {
        "Host": f"127.0.0.1:{server.port}",
        "Content-Type": "application/json",
        "Content-Length": str(len(data)),
        "X-Orb-Secret": secret,
        **(headers or {}),
    }
    message = (
        f"{method} {path} HTTP/1.1\r\n"
        + "".join(f"{k}: {v}\r\n" for k, v in fields.items())
        + "\r\n"
    )
    writer.write(message.encode() + data)
    await writer.drain()
    return reader, writer


async def response(reader, writer):
    try:
        async with asyncio.timeout(2):
            raw = await reader.read()
        head, body = raw.split(b"\r\n\r\n", 1)
        return int(head.split(b" ", 2)[1]), json.loads(body)
    finally:
        writer.close()
        await writer.wait_closed()


@pytest.mark.asyncio
async def test_lifecycle_and_summary_without_reading_transcript(tmp_path):
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    forbidden = tmp_path / "not-a-transcript"
    forbidden.write_text("private file content")
    await server.handle(payload(transcript_path=str(forbidden)))
    assert store.get("claude:test-session").state == "idle"
    await server.handle(
        payload("UserPromptSubmit", prompt="Review rendering token=throwaway-secret")
    )
    assert store.get("claude:test-session").state == "working"
    assert store.get("claude:test-session").title == "Review rendering token=[redacted]"
    await server.handle(
        payload(
            "Notification",
            notification_type="idle_prompt",
            message="private notification",
        )
    )
    assert store.get("claude:test-session").state == "waiting_input"
    await server.handle(
        payload("Stop", last_assistant_message="Completed " + "a" * 500)
    )
    session = store.get("claude:test-session")
    assert session.state == "idle"
    assert len(session.last_assistant_message) == 300
    assert "private" not in repr(store.sessions())
    assert "throwaway-secret" not in repr(store.sessions())
    await server.handle(payload("SessionEnd"))
    assert store.get(session.id).state == "done"


@pytest.mark.asyncio
async def test_hook_prompt_title_is_redacted_and_present_on_first_event():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    stream = store.events()
    first = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    await server.handle(
        payload(
            "UserPromptSubmit",
            prompt="Detect ESP32\n```secret body``` password=throwaway " + "x" * 500,
        )
    )
    event = await first
    title = event.session.title
    assert title.startswith("Detect ESP32 [code omitted] password=[redacted]")
    assert len(title) == 120
    assert "throwaway" not in title
    assert "secret body" not in title
    await server.handle(payload("UserPromptSubmit", prompt="Second prompt"))
    await server.handle(payload("SessionStart"))
    await server.handle(
        payload(
            "Notification", notification_type="idle_prompt", message="Notification body"
        )
    )
    assert store.get("claude:test-session").title == title
    await stream.aclose()
    await store.close()


@pytest.mark.asyncio
@pytest.mark.parametrize("event", ["SessionStart", "Notification", "UserPromptSubmit"])
async def test_hooks_do_not_invent_unknown_titles(event):
    store = StateStore()
    await HookServer(store, secrets.token_urlsafe(32)).handle(payload(event))
    session = store.get("claude:test-session")
    assert session.title == ""
    assert session.cwd == "/tmp/throwaway"


@pytest.mark.asyncio
async def test_existing_origin_and_title_survive_hooks():
    store = StateStore()
    session = Session(
        "claude:test-session", "claude", "desktop app", "Work", "/tmp/project"
    )
    store.update(session)
    await HookServer(store, secrets.token_urlsafe(32)).handle(
        payload("UserPromptSubmit")
    )
    current = store.get(session.id)
    assert current.origin == "desktop app"
    assert current.title == "Work"
    assert current.cwd == "/tmp/throwaway"


@pytest.mark.asyncio
async def test_owned_stop_updates_summary_but_waits_for_stream_completion():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    session = Session(
        "claude:test-session",
        "claude",
        "hub-started",
        "Owned",
        "/tmp/project",
        state="working",
    )
    store.update(session)
    stream = store.events()
    first = asyncio.create_task(anext(stream))
    await asyncio.sleep(0)
    await server.handle(payload("Stop", last_assistant_message="Finished tool work"))
    event = await first
    assert event.type == "session.changed"
    assert store.get(session.id).state == "working"
    assert store.get(session.id).last_assistant_message == "Finished tool work"
    await stream.aclose()


@pytest.mark.asyncio
async def test_owned_stop_does_not_clear_pending_stream_approval():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    session = Session(
        "claude:test-session",
        "claude",
        "hub-started",
        "Owned",
        "/tmp/project",
        state="working",
    )
    store.update(session)
    pending = asyncio.create_task(store.request_approval(session.id, "Write", {}))
    await until(lambda: store.pending_approvals())
    await server.handle(
        payload("Stop", last_assistant_message="Stop hook raced with stream")
    )
    assert len(store.pending_approvals()) == 1
    assert store.get(session.id).state == "needs_approval"
    store.turn_complete(session.id)
    await pending
    assert not store.pending_approvals()


@pytest.mark.asyncio
@pytest.mark.parametrize("decision", ["allow", "deny"])
async def test_http_approval_end_to_end_and_privacy(decision):
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    async with HookServer(store, secret) as server:
        assert server.url == f"http://127.0.0.1:{server.port}/hooks/claude"
        reader, writer = await post(
            server,
            secret,
            payload(
                "PermissionRequest",
                tool_name="Write",
                tool_input={
                    "file_path": "/tmp/hello",
                    "content": "NEVER EXPOSE THESE FILE CONTENTS",
                },
            ),
        )
        await until(lambda: store.pending_approvals())
        request = store.pending_approvals()[0]
        assert "NEVER EXPOSE" not in repr(request)
        assert "/tmp/hello" in request.summary
        assert store.get(request.session_id).state == "needs_approval"
        await store.approve(request.request_id, decision, "reviewed")
        code, data = await response(reader, writer)
        assert code == 200
        answer = data["hookSpecificOutput"]["decision"]
        assert answer["behavior"] == decision
        assert ("message" in answer) == (decision == "deny")
        assert not store.pending_approvals()
        with pytest.raises(AgentError):
            await store.approve(request.request_id, decision)
    with pytest.raises(RuntimeError):
        _ = server.url
    with pytest.raises(OSError):
        await asyncio.open_connection("127.0.0.1", server.port)


@pytest.mark.asyncio
async def test_disconnect_cancels_pending_permission():
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    async with HookServer(store, secret) as server:
        _, writer = await post(
            server,
            secret,
            payload(
                "PermissionRequest",
                tool_name="Bash",
                tool_input={"command": "echo hello"},
            ),
        )
        await until(lambda: store.pending_approvals())
        writer.close()
        await writer.wait_closed()
        await until(lambda: not store.pending_approvals())


@pytest.mark.asyncio
async def test_shutdown_cancels_pending_permission():
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    async with HookServer(store, secret) as server:
        reader, writer = await post(
            server,
            secret,
            payload(
                "PermissionRequest",
                tool_name="Bash",
                tool_input={"command": "echo hello"},
            ),
        )
        await until(lambda: store.pending_approvals())
    assert not store.pending_approvals()
    assert await reader.read() == b""
    writer.close()
    await writer.wait_closed()


@pytest.mark.asyncio
async def test_permission_expires_with_deny():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32), approval_timeout=0.01)
    data = await server.handle(
        payload("PermissionRequest", tool_name="Write", tool_input={})
    )
    assert data["hookSpecificOutput"]["decision"]["behavior"] == "deny"
    assert not store.pending_approvals()


@pytest.mark.asyncio
async def test_question_defers_to_native_input_without_false_approval():
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    async with HookServer(store, secret) as server:
        reader, writer = await post(
            server,
            secret,
            payload(
                "PermissionRequest",
                tool_name="AskUserQuestion",
                tool_input={
                    "questions": [{"question": "PRIVATE QUESTION BODY", "options": []}]
                },
            ),
        )
        code, data = await response(reader, writer)
        assert code == 200
        assert data == {}
        assert not store.pending_approvals()
        session = store.get("claude:test-session")
        assert session.state == "waiting_input"
        assert session.waiting_for == "Answer this question in Claude"
        assert "PRIVATE" not in repr(session)


@pytest.mark.asyncio
async def test_plan_approval_retains_normal_permission_path():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    task = asyncio.create_task(
        server.handle(
            payload(
                "PermissionRequest",
                tool_name="ExitPlanMode",
                tool_input={"plan": "PRIVATE PLAN BODY"},
            )
        )
    )
    await until(lambda: store.pending_approvals())
    approval = store.pending_approvals()[0]
    assert approval.tool == "ExitPlanMode"
    assert "PRIVATE" not in approval.summary
    await store.approve(approval.request_id, "deny", "Please revise the plan")
    result = await task
    assert result["hookSpecificOutput"]["decision"] == {
        "behavior": "deny",
        "message": "Please revise the plan",
    }


@pytest.mark.asyncio
async def test_matching_tool_completion_resolves_only_matching_approval():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    one = asyncio.create_task(
        server.handle(
            payload(
                "PermissionRequest",
                tool_name="Bash",
                tool_input={"command": "echo one"},
            )
        )
    )
    two = asyncio.create_task(
        server.handle(
            payload(
                "PermissionRequest",
                tool_name="Bash",
                tool_input={"command": "echo two"},
            )
        )
    )
    await until(lambda: len(store.pending_approvals()) == 2)
    await server.handle(
        payload(
            "PostToolUse",
            tool_name="Bash",
            tool_input={"command": "echo one"},
            tool_response="PRIVATE",
        )
    )
    await until(lambda: one.done())
    assert not two.done()
    assert len(store.pending_approvals()) == 1
    await server.handle(payload("SessionEnd"))
    await asyncio.gather(one, two)
    assert not store.pending_approvals()
    assert store.get("claude:test-session").state == "done"


@pytest.mark.asyncio
async def test_ambiguous_identical_tools_do_not_resolve_wrong_approval():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    tasks = [
        asyncio.create_task(
            server.handle(
                payload(
                    "PermissionRequest",
                    tool_name="Bash",
                    tool_input={"command": "echo same"},
                )
            )
        )
        for _ in range(2)
    ]
    await until(lambda: len(store.pending_approvals()) == 2)
    await server.handle(
        payload("PostToolUse", tool_name="Bash", tool_input={"command": "echo same"})
    )
    assert len(store.pending_approvals()) == 2
    await server.handle(payload("Stop"))
    await asyncio.gather(*tasks)


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("headers", "expected"),
    [
        ({"X-Orb-Secret": "incorrect"}, 401),
        ({"Origin": "http://example.com"}, 403),
        ({"Origin": ""}, 403),
        ({"Host": "evil.example"}, 403),
        ({"Host": "localhost"}, 403),
        ({"Content-Type": "text/plain"}, 415),
        ({"Transfer-Encoding": "chunked"}, 400),
        ({"Content-Length": "-1"}, 400),
        ({"Content-Length": "999999999"}, 413),
        ({"X-Orb-Secret": "bad\r\nX-Orb-Secret: other"}, 400),
    ],
)
async def test_http_rejects_untrusted_requests(headers, expected):
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    async with HookServer(store, secret) as server:
        code, _ = await response(*await post(server, secret, headers=headers))
        assert code == expected
        assert not store.sessions()


@pytest.mark.asyncio
@pytest.mark.parametrize(
    ("method", "path", "expected"),
    [("GET", "/hooks/claude", 405), ("POST", "/approve", 404)],
)
async def test_http_exposes_only_hook_route(method, path, expected):
    secret = secrets.token_urlsafe(32)
    async with HookServer(StateStore(), secret) as server:
        code, _ = await response(*await post(server, secret, method=method, path=path))
        assert code == expected


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "body",
    [
        [],
        "secret",
        {"session_id": "a"},
        payload(session_id=""),
        payload(cwd=12),
        payload("PermissionRequest", tool_name="Write", tool_input="raw"),
    ],
)
async def test_invalid_payloads_are_rejected(body):
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    with pytest.raises(ValueError):
        await server.handle(body)
    assert not store.sessions()


@pytest.mark.asyncio
async def test_permission_notification_and_failure_state():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    await server.handle(payload("Notification", notification_type="permission_prompt"))
    assert store.get("claude:test-session").state == "needs_approval"
    await server.handle(payload("StopFailure", error="PRIVATE"))
    assert store.get("claude:test-session").state == "error"
    assert "PRIVATE" not in repr(store.sessions())
    session = store.get("claude:test-session")
    store.update(replace(session, state="idle"))
    await server.handle(
        payload("Notification", notification_type="unrecognized", message="PRIVATE")
    )
    assert store.get("claude:test-session").state == "idle"


@pytest.mark.asyncio
async def test_authentication_failure_is_actionable_without_raw_error_body():
    store = StateStore()
    server = HookServer(store, secrets.token_urlsafe(32))
    await server.handle(
        payload(
            "StopFailure",
            error="authentication_failed",
            error_details="PRIVATE CREDENTIAL RESPONSE",
        )
    )
    session = store.get("claude:test-session")
    assert session.state == "error"
    assert (
        session.waiting_for
        == "Claude authentication is unavailable in this configuration"
    )
    assert "PRIVATE" not in repr(store.sessions())


@pytest.mark.asyncio
@pytest.mark.parametrize("body", [b"not-json", b"[1,2,3]", b'{"x": NaN}', b'{"x":'])
async def test_bad_json_cannot_change_state(body):
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    async with HookServer(store, secret) as server:
        reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
        writer.write(
            f"POST /hooks/claude HTTP/1.1\r\nHost: 127.0.0.1:{server.port}\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\nX-Orb-Secret: {secret}\r\n\r\n".encode()
            + body
        )
        await writer.drain()
        code, _ = await response(reader, writer)
        assert code == 400
        assert not store.sessions()


@pytest.mark.asyncio
async def test_slow_header_deadline_closes_connection():
    async with HookServer(
        StateStore(), secrets.token_urlsafe(32), request_timeout=0.01
    ) as server:
        reader, writer = await asyncio.open_connection("127.0.0.1", server.port)
        writer.write(b"POST /hooks/claude HTTP/1.1\r\n")
        await writer.drain()
        code, _ = await response(reader, writer)
        assert code == 408


@pytest.mark.asyncio
async def test_concurrency_limit_does_not_leave_clients_after_close():
    async with HookServer(
        StateStore(), secrets.token_urlsafe(32), max_connections=1
    ) as server:
        first_reader, first_writer = await asyncio.open_connection(
            "127.0.0.1", server.port
        )
        await until(lambda: len(server._clients) == 1)
        second_reader, second_writer = await asyncio.open_connection(
            "127.0.0.1", server.port
        )
        async with asyncio.timeout(1):
            assert await second_reader.read() == b""
        second_writer.close()
        await second_writer.wait_closed()
    assert not server._clients
    assert await first_reader.read() == b""
    first_writer.close()
    await first_writer.wait_closed()


@pytest.mark.parametrize(
    "kwargs",
    [
        {"port": -1},
        {"port": 65536},
        {"approval_timeout": float("inf")},
        {"request_timeout": float("nan")},
        {"max_connections": 0},
        {"max_body_bytes": 0},
    ],
)
def test_invalid_configuration(kwargs):
    with pytest.raises(ValueError):
        HookServer(StateStore(), secrets.token_urlsafe(32), **kwargs)


@pytest.mark.asyncio
@pytest.mark.parametrize("fail_write", [False, True])
async def test_approval_waits_for_http_transport_completion(monkeypatch, fail_write):
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    entered, release = asyncio.Event(), asyncio.Event()
    original_respond = HookServer._respond

    async def controlled_respond(writer, status, data=None):
        if status == 200:
            entered.set()
            await release.wait()
            if fail_write:
                raise BrokenPipeError("test transport closed")
        await original_respond(writer, status, data)

    monkeypatch.setattr(HookServer, "_respond", staticmethod(controlled_respond))
    async with HookServer(store, secret) as server:
        reader, writer = await post(
            server,
            secret,
            payload("PermissionRequest", tool_name="Write", tool_input={}),
        )
        await until(lambda: store.pending_approvals())
        approval = store.pending_approvals()[0]
        accepting = asyncio.create_task(store.approve(approval.request_id, "allow"))
        try:
            await asyncio.wait_for(entered.wait(), 2)
            assert not accepting.done(), (
                "a decision is not delivered before the HTTP write"
            )
            release.set()
            if fail_write:
                with pytest.raises(AgentError) as error:
                    await accepting
                assert error.value.code == "delivery_failed"
                assert await asyncio.wait_for(reader.read(), 2) == b""
            else:
                await accepting
                code, data = await response(reader, writer)
                assert code == 200
                assert data["hookSpecificOutput"]["decision"]["behavior"] == "allow"
        finally:
            release.set()
            writer.close()
            await writer.wait_closed()
            accepting.cancel()
            await asyncio.gather(accepting, return_exceptions=True)


@pytest.mark.asyncio
async def test_http_response_propagates_failed_drain():
    class FailedWriter:
        def write(self, _data):
            pass

        async def drain(self):
            raise BrokenPipeError("test transport closed")

    with pytest.raises(BrokenPipeError):
        await HookServer._respond(FailedWriter(), 200, {})


@pytest.mark.asyncio
async def test_shutdown_during_http_write_fails_approval_receipt(monkeypatch):
    store = StateStore()
    secret = secrets.token_urlsafe(32)
    entered = asyncio.Event()

    async def blocked_respond(_writer, _status, _data=None):
        entered.set()
        await asyncio.Future()

    monkeypatch.setattr(HookServer, "_respond", staticmethod(blocked_respond))
    async with HookServer(store, secret) as server:
        reader, writer = await post(
            server,
            secret,
            payload("PermissionRequest", tool_name="Write", tool_input={}),
        )
        await until(lambda: store.pending_approvals())
        approval = store.pending_approvals()[0]
        accepting = asyncio.create_task(store.approve(approval.request_id, "allow"))
        try:
            await asyncio.wait_for(entered.wait(), 2)
            await server.close()
            with pytest.raises(AgentError) as error:
                await asyncio.wait_for(accepting, 2)
            assert error.value.code == "delivery_failed"
            assert await asyncio.wait_for(reader.read(), 2) == b""
        finally:
            writer.close()
            await writer.wait_closed()
            accepting.cancel()
            await asyncio.gather(accepting, return_exceptions=True)
