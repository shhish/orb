"""Production MCP boundary with throwaway tokens, SQLite and loopback HTTP."""

import asyncio
import base64
import json
import os
from types import SimpleNamespace

import aiohttp
import pytest
import pytest_asyncio
from fake_device import paired_device
from orb_mcp.auth import read_token
from orb_mcp.events import WebhookResponse
from orb_mcp.service import ServiceError

from orb_hub.dot import DotRuntime, last_reply, wire_id
from orb_hub.envelope import LinkError
from orb_hub.sessions import SessionHub

DEVICE = "orb-000000000001"
SECOND = "orb-000000000002"


@pytest_asyncio.fixture
async def runtime(tmp_path):
    sessions = SessionHub(tmp_path / "sessions")
    await sessions.start()
    hub = SimpleNamespace(
        cfg=SimpleNamespace(data_dir=tmp_path / "data"),
        store=SimpleNamespace(
            all=lambda: [
                SimpleNamespace(device_id=DEVICE, name="Orb"),
                SimpleNamespace(device_id=SECOND, name="Second"),
            ]
        ),
        online={DEVICE: SimpleNamespace(state="online", caps={"cards"})},
        voice=SimpleNamespace(confirmed=lambda link, stream, target, text: 1.25, available=False),
    )
    runtime = DotRuntime(hub, sessions)
    await runtime.start(port=0)
    try:
        yield runtime, sessions, hub
    finally:
        await runtime.stop()
        await sessions.close()


def ask(card_id="question", **changes):
    return {"card_id": card_id, "text": "Continue?", "options": [{"id": "yes", "label": "Continue"}], **changes}


async def subscribe(runtime, *, name="voice.transcribed", **arguments):
    async def receiver(url, body, headers):
        return WebhookResponse(200, json.dumps({"challenge": json.loads(body).get("challenge")}).encode())

    runtime.service.events.transport = receiver
    return await runtime.service.events.dispatch(
        "events/subscribe",
        {
            "name": name,
            "arguments": arguments,
            "delivery": {
                "mode": "webhook",
                "url": "https://callback.example/mcp",
                "secret": "whsec_" + base64.b64encode(os.urandom(32)).decode(),
            },
        },
        "dot",
    )


async def test_question_roundtrip_persists_original_option_and_outbox(runtime):
    dot, sessions, _ = runtime
    await subscribe(dot, name="card.answered")
    result = await dot.service.call_tool("orb_ask", ask(), "dot")
    assert result["status"] == "showing"
    key = wire_id("question")
    assert sessions.cards[key]["session"] == "dot"
    answer = await sessions.answer(DEVICE, {"card": key, "option": "option-0"})
    assert answer == {"card": key, "answer": {"option": "option-0"}, "delivered": True}
    durable = await dot.service.call_tool("orb_get_card", {"card_id": "question"}, "dot")
    assert durable["status"] == "answered"
    assert durable["answer"]["option_id"] == "yes"
    assert dot._conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1
    assert await sessions.answer(DEVICE, {"card": key, "option": "dismiss"}) == answer
    assert dot._conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


async def test_explicit_dismissal_and_cancel(runtime):
    dot, sessions, _ = runtime
    await dot.service.call_tool("orb_show", {"card_id": "notice", "text": "Finished"}, "dot")
    key = wire_id("notice")
    assert sessions.cards[key]["options"] == [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}]
    await sessions.answer(DEVICE, {"card": key, "option": "dismiss"})
    assert (await dot.service.call_tool("orb_get_card", {"card_id": "notice"}, "dot"))["status"] == "dismissed"
    await dot.service.call_tool("orb_ask", ask("cancelled"), "dot")
    await dot.service.call_tool("orb_cancel", {"card_id": "cancelled"}, "dot")
    assert wire_id("cancelled") not in sessions.cards


async def test_display_utf8_limits_preserve_original_values_and_identity(runtime):
    dot, sessions, _ = runtime
    original_id = "界" * 64
    original_option = "選" * 32
    await dot.service.call_tool(
        "orb_ask",
        ask(original_id, text="文" * 280, title="題" * 40, options=[{"id": original_option, "label": "選" * 24}]),
        "dot",
    )
    key = wire_id(original_id)
    card = sessions.cards[key]
    assert len(key.encode()) <= 64
    assert len(card["body"].encode()) <= 280
    assert len(card["title"].encode()) <= 64
    assert len(card["options"][0]["label"].encode()) <= 20
    assert wire_id(original_id) != wire_id(original_id[:-1] + "語")
    await sessions.answer(DEVICE, {"card": key, "option": "option-0"})
    durable = await dot.service.call_tool("orb_get_card", {"card_id": original_id}, "dot")
    assert durable["text"] == "文" * 280
    assert durable["answer"]["option_id"] == original_option
    assert durable["answer"]["option_label"] == "選" * 24


async def test_targeting_and_v1_device_is_not_online(runtime):
    dot, sessions, hub = runtime
    hub.online[DEVICE].caps = set()
    assert not (await dot.backend.list_devices())[0]["online"]
    result = await dot.service.call_tool("orb_ask", ask(device=SECOND), "dot")
    assert result["status"] == "queued"
    with pytest.raises(LinkError) as error:
        await sessions.answer(DEVICE, {"card": wire_id("question"), "option": "option-0"})
    assert error.value.code == "unauthorized"
    hub.online[SECOND] = SimpleNamespace(state="online", caps={"cards"})
    await dot.sync_device(SECOND)
    assert (await dot.service.call_tool("orb_get_card", {"card_id": "question"}, "dot"))["status"] == "showing"


async def test_per_principal_isolation_remains_enforced(runtime):
    dot, sessions, _ = runtime
    await dot.service.call_tool("orb_ask", ask(), "claude")
    for tool in ("orb_get_card", "orb_cancel", "orb_wait"):
        with pytest.raises(ServiceError, match="not found"):
            await dot.service.call_tool(tool, {"card_id": "question"}, "codex")
    with pytest.raises(ServiceError, match="different content"):
        await dot.service.call_tool("orb_ask", ask(), "dot")
    assert sessions.cards[wire_id("question")]["title"] == "Claude"


async def test_dot_requires_matching_subscription_and_confirmation_routes_text(runtime):
    dot, sessions, _ = runtime
    with pytest.raises(LinkError) as error:
        await sessions.message(DEVICE, {"target": "dot", "text": "Hello"})
    assert error.value.code == "not_listening"
    await subscribe(dot, device=SECOND)
    with pytest.raises(LinkError) as error:
        await sessions.message(DEVICE, {"target": "dot", "text": "Hello"})
    assert error.value.code == "not_listening"
    await subscribe(dot, device=DEVICE)
    await dot.refresh_session()
    assert sessions.sessions["dot"]["can_message"] is True
    receipt = await sessions.message(DEVICE, {"target": "dot", "text": "Hello", "stream": 4})
    assert receipt["accepted"] is True and receipt["status"] == "queued"
    assert receipt["reason"]
    row = dot._conn.execute("SELECT * FROM transcripts").fetchone()
    assert row["principal"] == "dot"
    assert json.loads(row["content"])["text"] == "Hello"
    assert json.loads(row["content"])["duration_s"] == 1.25
    # Retrying one confirmed source recording does not enqueue a second event.
    await sessions.message(DEVICE, {"target": "dot", "text": "Hello", "stream": 4})
    assert dot._conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1


async def test_disconnected_dot_does_not_outrank_work_and_checks_each_device(runtime):
    dot, sessions, _ = runtime
    assert sessions.sessions["dot"]["state"] == "idle"
    assert sessions.sessions["dot"]["reason"] == "Dot not connected"
    with pytest.raises(LinkError) as error:
        sessions.check_target("dot", DEVICE)
    assert error.value.code == "not_listening"
    await subscribe(dot, device=DEVICE)
    await dot.refresh_session()
    assert sessions.check_target("dot", DEVICE) == "dot"
    first = sessions._selection(DEVICE)[0]
    second = sessions._selection(SECOND)[0]
    assert next(value for value in first if value["id"] == "dot")["can_message"] is True
    other_dot = next(value for value in second if value["id"] == "dot")
    assert other_dot["can_message"] is False and other_dot["reason"] == "Dot not connected"
    with pytest.raises(LinkError) as error:
        sessions.check_target("dot", SECOND)
    assert error.value.code == "not_listening"


async def test_last_dot_reply_is_sanitized_private_and_survives_restart(runtime):
    dot, sessions, _ = runtime
    await dot.service.call_tool(
        "orb_show", {"card_id": "first-private", "device": DEVICE, "text": "First. **Ready to test.**"}, "dot"
    )
    await dot.service.call_tool(
        "orb_show", {"card_id": "second-private", "device": SECOND, "text": "Second device only."}, "dot"
    )
    assert sessions._device_streams[DEVICE]["dot"]["assistant"]["text"] == "Ready to test."
    assert sessions._device_streams[SECOND]["dot"]["assistant"]["text"] == "Second device only."
    assert "Ready to test" not in json.dumps(sessions.sessions["dot"])
    assert "dot" not in sessions.streams
    await dot.stop()
    assert sessions._device_streams[DEVICE]["dot"]["activity"] == "Dot not connected"
    await dot.start(port=0)
    assert sessions._device_streams[DEVICE]["dot"]["assistant"]["text"] == "Ready to test."
    assert sessions._device_streams[DEVICE]["dot"]["activity"] == "Dot not connected"


async def test_subscription_expiry_prevents_send_and_updates_connectivity(runtime):
    dot, sessions, _ = runtime
    await subscribe(dot, device=DEVICE)
    await dot.refresh_session()
    assert sessions.sessions["dot"]["can_message"] is True
    with dot._conn:
        dot._conn.execute("UPDATE mcp_subscriptions SET expires_at=0")
    await dot.refresh_session()
    assert sessions.sessions["dot"]["can_message"] is False
    assert sessions._device_streams[DEVICE]["dot"]["activity"] == "Dot not connected"
    with pytest.raises(LinkError) as error:
        await sessions.message(DEVICE, {"target": "dot", "text": "No listener"})
    assert error.value.code == "not_listening"
    assert dot._conn.execute("SELECT COUNT(*) FROM transcripts").fetchone()[0] == 0


def test_dot_reply_prose_sanitizer_does_not_need_agents_package():
    assert last_reply("Earlier. **Last sentence.**\n```python\nprivate_code()\n```") == "Last sentence."
    assert last_reply(
        "First. Read `/home/example/private/work/report.md` with [details](https://example.test/private)."
    ) == ("Read report.md with details.")
    assert last_reply("A final sentence without punctuation") == "A final sentence without punctuation"
    assert last_reply("First. token=throwaway-secret") == "token=[redacted]"
    result = last_reply("界" * 100)
    assert len(result.encode()) <= 160 and result.endswith("…")


async def test_unpairing_drops_private_dot_streams(runtime):
    dot, sessions, hub = runtime
    assert set(sessions._device_streams) == {DEVICE, SECOND}
    hub.store.all = lambda: [SimpleNamespace(device_id=DEVICE, name="Orb")]
    await dot.refresh_session()
    assert set(sessions._device_streams) == {DEVICE}


async def test_real_dot_reply_completes_idle_send_and_idle_page_keeps_private_summary(runtime):
    dot, sessions, _ = runtime
    await subscribe(dot, device=DEVICE)
    receipt = await sessions.message(DEVICE, {"target": "dot", "text": "Run the check"})
    assert receipt["status"] == "queued"
    assert sessions.sessions["dot"]["state"] == "idle"
    await dot.service.call_tool(
        "orb_show", {"card_id": "actual-reply", "device": DEVICE, "text": "All checks passed."}, "dot"
    )
    assert sessions.sessions["dot"]["state"] == "done"
    assert sessions._device_streams[DEVICE]["dot"]["state"] == "done"
    await sessions.set_prefs(DEVICE, {"idle_minutes": 0})
    page = sessions.list_sessions(DEVICE, {"include": "idle"})
    assert page["sessions"][0]["summary"] == "All checks passed."
    assert page["sessions"][0]["can_message"] is True
    await sessions.set_prefs(SECOND, {"idle_minutes": 0})
    other = sessions.list_sessions(SECOND, {"include": "idle"})["sessions"][0]
    assert other["can_message"] is False and "All checks" not in other["summary"]
    await sessions.message(DEVICE, {"target": "dot", "text": "Another check"})
    assert sessions.sessions["dot"]["state"] == "idle"
    await dot.service.call_tool(
        "orb_show", {"card_id": "next-reply", "device": DEVICE, "text": "Next check passed."}, "dot"
    )
    assert sessions.sessions["dot"]["state"] == "done"


async def test_shared_limit_includes_other_provider_and_offline_mcp_queue(runtime):
    dot, sessions, hub = runtime
    hub.online.clear()
    for index in range(15):
        await dot.service.call_tool("orb_ask", ask(str(index)), "dot")
    agent_card = {
        "id": "agent-card",
        "kind": "attention",
        "title": "Agent",
        "body": "Ready",
        "options": [],
        "reply": False,
        "priority": "normal",
    }
    await sessions.show_card(agent_card)
    with pytest.raises(ServiceError, match="16"):
        await dot.service.call_tool("orb_ask", ask("overflow"), "dot")
    with pytest.raises(LinkError) as error:
        await sessions.show_card({**agent_card, "id": "overflow"})
    assert error.value.code == "busy"
    await dot.service.call_tool("orb_cancel", {"card_id": "0"}, "dot")
    await sessions.show_card({**agent_card, "id": "after-cancel"})
    assert len(sessions.cards) == 16


async def test_http_runtime_uses_private_tokens_and_forbids_origin(runtime):
    dot, _, hub = runtime
    tokens = hub.cfg.data_dir / "mcp" / "tokens"
    token_paths = list(tokens.glob("*.token"))
    assert len(token_paths) == 3
    assert all(path.stat().st_mode & 0o777 == 0o600 for path in token_paths)
    assert len({read_token(path) for path in token_paths}) == 3
    url = f"http://127.0.0.1:{dot.port}/mcp"
    message = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "orb_status", "arguments": {}}}
    async with aiohttp.ClientSession() as client:
        async with client.post(url, json=message) as response:
            assert response.status == 401
        headers = {
            "Authorization": "Bearer " + read_token(tokens / "dot.token"),
            "Accept": "application/json,text/event-stream",
        }
        async with client.post(url, json=message, headers=headers) as response:
            assert response.status == 200
            assert "result" in await response.json()
        async with client.post(url, json=message, headers={**headers, "Origin": "http://example.com"}) as response:
            assert response.status == 403


async def test_runtime_stop_preserves_pending_cards_for_restart(runtime):
    dot, sessions, _ = runtime
    await dot.service.call_tool("orb_ask", ask(), "dot")
    await dot.stop()
    assert wire_id("question") in sessions.cards
    assert sessions.sessions["dot"]["can_message"] is False
    await dot.start(port=0)
    await dot.service.deliver_pending()
    result = await sessions.answer(DEVICE, {"card": wire_id("question"), "option": "option-0"})
    assert result["delivered"] is True
    assert (await dot.service.call_tool("orb_get_card", {"card_id": "question"}, "dot"))["status"] == "answered"


async def test_hub_mcp_http_to_tls_card_answer_and_offline_reconnect(make_hub):
    hub = await make_hub(mcp_enabled=True, mcp_port=0)
    await subscribe(hub.dot, name="card.answered")
    device = await paired_device(hub)
    token = read_token(hub.cfg.data_dir / "mcp" / "tokens" / "dot.token")
    headers = {"Authorization": "Bearer " + token, "Accept": "application/json,text/event-stream"}
    url = f"http://127.0.0.1:{hub.dot.port}/mcp"

    async def tool(client, name, arguments):
        message = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": arguments}}
        async with client.post(url, json=message, headers=headers) as response:
            assert response.status == 200
            result = (await response.json())["result"]
            assert result["isError"] is False
            return result["structuredContent"]

    async def event_named(name):
        async with asyncio.timeout(3):
            while True:
                method, params = await device.event()
                if method == name:
                    return params

    async def reconnect_cards():
        await device.disconnect()
        await device.connect()
        hello = await device.hello(caps=["cards"])
        assert hello["ok"] and hello["r"]["caps"] == ["cards"]
        snapshot = await event_named("sessions.snapshot")
        assert snapshot["reset"] is True
        assert any(session["id"] == "dot" for session in snapshot["sessions"])

    try:
        await reconnect_cards()
        async with aiohttp.ClientSession() as client:
            result = await tool(client, "orb_ask", ask("tls-first", device=device.device_id))
            assert result["status"] == "showing"
            card = await event_named("card.show")
            assert card["id"] == wire_id("tls-first")
            answered = await device.request("card.answer", {"card": card["id"], "option": "option-0"})
            assert answered["ok"] and answered["r"]["delivered"] is True
            persisted = await tool(client, "orb_get_card", {"card_id": "tls-first"})
            assert persisted["status"] == "answered"
            assert persisted["answer"]["option_id"] == "yes"
            assert hub.dot._conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 1

            # Work queued while disconnected must reach the same paired device
            # through Hub.link_online/sync_device and the fresh TLS snapshot.
            link = hub.online[device.device_id]
            await device.disconnect()
            await asyncio.wait_for(link._finished.wait(), 3)
            queued = await tool(client, "orb_ask", ask("tls-offline", device=device.device_id))
            assert queued["status"] == "queued" and queued["device_online"] is False
            await reconnect_cards()
            replayed = await event_named("card.show")
            assert replayed["id"] == wire_id("tls-offline")
            dismissed = await device.request("card.answer", {"card": replayed["id"], "option": "dismiss"})
            assert dismissed["ok"] and dismissed["r"]["delivered"] is True
            persisted = await tool(client, "orb_get_card", {"card_id": "tls-offline"})
            assert persisted["status"] == "dismissed"
            assert persisted["answer"]["device_id"] == device.device_id
            assert hub.dot._conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 2

            repeated = await device.request("card.answer", {"card": card["id"], "option": "dismiss"})
            assert repeated["ok"] and repeated["r"] == answered["r"]
            assert hub.dot._conn.execute("SELECT COUNT(*) FROM mcp_outbox").fetchone()[0] == 2
    finally:
        await device.aclose()


async def test_unassigned_mcp_reservations_bound_agents_before_first_device(runtime):
    dot, sessions, hub = runtime
    devices = hub.store.all
    hub.store.all = list
    hub.online.clear()
    for index in range(15):
        assert (await dot.service.call_tool("orb_ask", ask(f"unassigned-{index}"), "dot"))["status"] == "queued"
    assert sessions.cards == {}
    assert sessions.pending_extra() == 15
    agent_card = {
        "id": "before-pairing",
        "kind": "attention",
        "title": "Agent",
        "body": "Ready",
        "options": [],
        "reply": False,
        "priority": "normal",
    }
    await sessions.show_card(agent_card)
    with pytest.raises(LinkError) as error:
        await sessions.show_card({**agent_card, "id": "agent-overflow"})
    assert error.value.code == "busy"
    with pytest.raises(ServiceError, match="16"):
        await dot.service.call_tool("orb_ask", ask("mcp-overflow"), "dot")

    # Materializing reservations must exclude the card being admitted, including
    # when the combined budget is already exactly full.
    hub.store.all = devices
    hub.online[DEVICE] = SimpleNamespace(state="online", caps={"cards"})
    await dot.service.deliver_pending()
    assert len(sessions.cards) == 16
    assert sessions.pending_extra() == 0
    await dot.service.call_tool("orb_cancel", {"card_id": "unassigned-0"}, "dot")
    await sessions.show_card({**agent_card, "id": "freed-slot"})
    assert len(sessions.cards) == 16


async def test_dot_completion_and_next_send_are_private_to_the_receiving_device(runtime):
    dot, sessions, _ = runtime
    await subscribe(dot, device=DEVICE)
    await subscribe(dot, device=SECOND)
    await sessions.message(DEVICE, {"target": "dot", "text": "First request"})
    await dot.service.call_tool(
        "orb_show", {"card_id": "first-device-reply", "device": DEVICE, "text": "First done."}, "dot"
    )
    first = next(value for value in sessions._selection(DEVICE)[0] if value["id"] == "dot")
    second = next(value for value in sessions._selection(SECOND)[0] if value["id"] == "dot")
    assert first["state"] == "done"
    assert second["state"] == "idle"
    assert sessions._device_streams[SECOND]["dot"]["state"] == "idle"
    second_updated = second["updated"]

    await sessions.message(SECOND, {"target": "dot", "text": "Second request"})
    await dot.service.call_tool(
        "orb_show", {"card_id": "second-device-reply", "device": SECOND, "text": "Second done."}, "dot"
    )
    await sessions.message(DEVICE, {"target": "dot", "text": "First next request"})
    assert sessions._device_streams[DEVICE]["dot"]["state"] == "idle"
    assert sessions._device_streams[SECOND]["dot"]["state"] == "done"
    assert sessions._device_streams[SECOND]["dot"]["at"] > second_updated

    # Polling and reconnect replay retain each private exchange's own state.
    await dot.refresh_session()
    assert next(value for value in sessions._selection(DEVICE)[0] if value["id"] == "dot")["state"] == "idle"
    assert next(value for value in sessions._selection(SECOND)[0] if value["id"] == "dot")["state"] == "done"


async def test_dot_reply_does_not_renew_another_devices_focus_grace(runtime):
    dot, sessions, _ = runtime
    now = [dot._empty_updated]
    sessions.store.clock = lambda: now[0]
    dot.service.clock = lambda: now[0]
    now[0] += 601
    await dot.service.call_tool(
        "orb_show", {"card_id": "renew-first-only", "device": DEVICE, "text": "First done."}, "dot"
    )
    assert "dot" in {value["id"] for value in sessions._selection(DEVICE)[0]}
    assert "dot" not in {value["id"] for value in sessions._selection(SECOND)[0]}
    assert sessions._selection(SECOND)[2]["idle"] == 1


async def test_dot_queued_state_survives_fresh_runtime_and_keeps_last_reply(runtime):
    dot, sessions, hub = runtime
    await subscribe(dot, device=DEVICE)
    await dot.service.call_tool(
        "orb_show", {"card_id": "before-restart", "device": DEVICE, "text": "Previous answer."}, "dot"
    )
    await sessions.message(DEVICE, {"target": "dot", "text": "Next request"})
    await dot.stop()
    restarted = DotRuntime(hub, sessions)
    try:
        await restarted.start(port=0)
        value = sessions._device_streams[DEVICE]["dot"]
        assert value["state"] == "idle"
        assert value["assistant"]["text"] == "Previous answer."
    finally:
        await restarted.stop()
