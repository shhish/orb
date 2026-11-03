"""Bridge tests use throwaway managers, loopback hooks and in-memory providers."""

import asyncio
import json
import stat
import sys
from dataclasses import replace
from datetime import UTC, datetime

import aiohttp
import pytest
from orb_agents import AgentManager, Session, StateStore
from orb_agents.testing import FakeAdapter

from orb_hub.agents import HOOK_TOKEN_PLACEHOLDER, AgentsBridge, _load_secret, hooks_snippet, session_id
from orb_hub.envelope import LinkError


class DeviceSessions:
    def __init__(self):
        self.changed = asyncio.Event()
        self.values = {}
        self._reservations = set()
        self.cards = {}
        self.handlers = {}
        self.messages = {}
        self.cancelled = []
        self.card_limit = 16

    def reserve_session_slot(self):
        if len(self.values) + len(self._reservations) >= 24:
            raise LinkError("busy")
        token = object()
        self._reservations.add(token)
        return token

    def release_session_slot(self, token):
        self._reservations.discard(token)

    async def upsert_session(self, value, *, reservation=None):
        reserved = len(self._reservations) - (reservation in self._reservations)
        if len(self.values) + reserved >= 24 and value["id"] not in self.values:
            raise LinkError("busy")
        self.release_session_slot(reservation)
        self.values[value["id"]] = value
        self.changed.set()

    async def remove_session(self, identity):
        self.values.pop(identity, None)
        self.changed.set()

    async def show_card(self, card, *, device_id=None, answer_handler=None):
        if len(self.cards) >= self.card_limit and card["id"] not in self.cards:
            raise LinkError("busy")
        self.cards[card["id"]] = card
        self.handlers[card["id"]] = answer_handler
        self.changed.set()

    async def cancel_card(self, identity):
        self.cancelled.append(identity)
        self.cards.pop(identity, None)
        self.changed.set()

    def register_message_handler(self, identity, handler):
        self.messages[identity] = handler

    def unregister_message_handler(self, identity):
        self.messages.pop(identity, None)


async def wait_for(hub, predicate):
    async with asyncio.timeout(2):
        while not predicate():
            hub.changed.clear()
            if not predicate():
                await hub.changed.wait()


def setup_manager():
    store = StateStore()
    fake = FakeAdapter(store, "codex")
    manager = AgentManager(store=store, adapters={"codex": fake}, poll_interval=0.01)
    return store, fake, manager


@pytest.mark.asyncio
async def test_owned_session_message_and_question_route(tmp_path):
    _, fake, manager = setup_manager()
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, poll_interval=0.01, manager=manager)
    await bridge.start()
    try:
        value = await bridge.start_session("codex", str(tmp_path), "Hello")
        assert value["can_message"] is True
        assert value["state"] == "busy"
        await hub.messages[value["id"]]("test-device", "Continue")
        assert fake.messages[-1] == (value["id"], "Continue")
        native = manager.store.get(value["id"])
        manager.store.update(replace(native, state="waiting_input", waiting_for="Which colour?", question_id="fake:q1"))
        await wait_for(hub, lambda: any(card["kind"] == "choice" for card in hub.cards.values()))
        card = next(card for card in hub.cards.values() if card["kind"] == "choice")
        assert card["reply"] is True
        await hub.handlers[card["id"]]({"card": card["id"], "text": "Blue", "device_id": "test-device"})
        assert fake.messages[-1] == (value["id"], "Blue")
        await wait_for(hub, lambda: card["id"] in hub.cancelled)
    finally:
        await bridge.stop()
    assert not hub.values
    assert fake.closed
    assert not bridge._tasks


@pytest.mark.asyncio
async def test_observed_session_is_truthfully_read_only(tmp_path):
    store, _, manager = setup_manager()
    native_id = "codex:observed"
    store.update(Session(native_id, "codex", "desktop app", "Observed", str(tmp_path), state="waiting_input"))
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    try:
        assert hub.values[native_id]["can_message"] is False
        assert native_id not in hub.messages
        card = next(iter(hub.cards.values()))
        assert card["kind"] == "attention"
        assert card["reply"] is False
        with pytest.raises(LinkError, match="unsupported"):
            await hub.handlers[card["id"]]({"card": card["id"], "text": "No takeover", "device_id": "test"})
        await hub.handlers[card["id"]]({"card": card["id"], "option": "dismiss", "device_id": "test"})
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_approval_card_roundtrip_and_external_resolution(tmp_path):
    _, fake, manager = setup_manager()
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    try:
        value = await bridge.start_session("codex", str(tmp_path), "Prepare")
        permission = asyncio.create_task(fake.ask_permission(value["id"], tool="Write"))
        await wait_for(hub, lambda: bool(hub.cards))
        card = next(iter(hub.cards.values()))
        assert card["kind"] == "approval"
        assert card["scope"] == "operation"
        assert "never forwarded" not in json.dumps(card)
        assert card["options"] == [
            {"id": "allow", "label": "Allow once", "role": "default"},
            {"id": "deny", "label": "Deny", "role": "cancel"},
        ]
        await hub.handlers[card["id"]]({"card": card["id"], "option": "allow", "device_id": "test"})
        assert (await permission).decision == "allow"
        await wait_for(hub, lambda: card["id"] in hub.cancelled)
        with pytest.raises(LinkError, match="expired"):
            await hub.handlers[card["id"]]({"card": card["id"], "option": "deny", "device_id": "test"})
        permission = asyncio.create_task(fake.ask_permission(value["id"], tool="Bash"))
        await wait_for(hub, lambda: bool(hub.cards))
        pending = manager.pending_approvals()[0]
        card = next(iter(hub.cards.values()))
        manager.store.resolve(pending.request_id, reason="external")
        assert (await permission).decision == "deny"
        await wait_for(hub, lambda: card["id"] in hub.cancelled)
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_loopback_http_hook_approval_waits_for_device_answer(tmp_path):
    _, _, manager = setup_manager()
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    token = bridge.token_file.read_text().strip()
    assert stat.S_IMODE(bridge.token_file.stat().st_mode) == 0o600
    try:
        async with aiohttp.ClientSession() as client:
            pending = asyncio.create_task(
                client.post(
                    bridge.hooks.url,
                    headers={"X-Orb-Secret": token},
                    json={
                        "hook_event_name": "PermissionRequest",
                        "session_id": "hook-test",
                        "cwd": str(tmp_path),
                        "tool_name": "Bash",
                        "tool_input": {"command": "echo private-body"},
                    },
                )
            )
            await wait_for(hub, lambda: bool(hub.cards))
            card = next(iter(hub.cards.values()))
            assert "private-body" not in json.dumps(card)
            assert "claude:hook-test" in hub.values
            await hub.handlers[card["id"]]({"card": card["id"], "option": "deny", "device_id": "test"})
            async with await pending as response:
                assert response.status == 200
                assert (await response.json())["hookSpecificOutput"]["decision"]["behavior"] == "deny"
            async with client.post(bridge.hooks.url, json={}) as response:
                assert response.status == 401
    finally:
        await bridge.stop()


@pytest.mark.asyncio
@pytest.mark.parametrize("first_event", ["SessionStart", "Notification"])
async def test_http_hook_and_listing_merge_titles_without_replacing_device_session(tmp_path, monkeypatch, first_event):
    from orb_agents.claude import ClaudeAdapter

    class RecordingSessions(DeviceSessions):
        def __init__(self):
            super().__init__()
            self.published = []
            self.removed = []

        async def upsert_session(self, value, *, reservation=None):
            await super().upsert_session(value, reservation=reservation)
            self.published.append(value.copy())

        async def remove_session(self, identity):
            self.removed.append(identity)
            await super().remove_session(identity)

    records = []
    store = StateStore()
    adapter = ClaudeAdapter(store, config_dir=tmp_path / "claude")

    async def capture(*args, command=None):
        return json.dumps(records).encode() if args == ("agents", "--json") else b""

    monkeypatch.setattr(adapter, "_capture", capture)
    manager = AgentManager(store=store, adapters={"claude": adapter}, poll_interval=3600)
    hub = RecordingSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, poll_interval=3600, manager=manager, hooks_secret="x" * 32)
    await bridge.start()
    identity = "claude:title-merge"
    cwd = str(tmp_path / "firmware")
    try:
        async with aiohttp.ClientSession() as client:

            async def hook(event, **fields):
                async with client.post(
                    bridge.hooks.url,
                    headers={"X-Orb-Secret": "x" * 32},
                    json={"session_id": "title-merge", "cwd": cwd, "hook_event_name": event, **fields},
                ) as response:
                    assert response.status == 200
                await bridge.reconcile()

            await hook(first_event)
            assert hub.values[identity]["title"] == ""
            assert hub.values[identity]["project"] == "firmware"
            await hook(
                "UserPromptSubmit", prompt="Detect the ESP32 using token=throwaway-credential " + "details " * 40
            )
            fallback = hub.values[identity]["title"]
            assert fallback.startswith("Detect the ESP32 using token=[redacted]")
            assert len(fallback.encode()) <= 64
            assert "throwaway-credential" not in json.dumps(hub.published)
            await hook("UserPromptSubmit", prompt="A later request must not rename this session")
            assert hub.values[identity]["title"] == fallback

            records.append({"sessionId": "title-merge", "name": "ESP32 device detection", "cwd": cwd, "status": "busy"})
            await manager.refresh()
            await bridge.reconcile()
            named_start = len(hub.published)
            assert hub.values[identity]["title"] == "ESP32 device detection"
            await hook("SessionStart")
            await hook("Notification")
            await hook("UserPromptSubmit", prompt="Another request")
            records[0].pop("name")
            await manager.refresh()
            await bridge.reconcile()
            assert hub.values[identity]["title"] == "ESP32 device detection"
            assert hub.values[identity]["project"] == "firmware"
            assert set(hub.values) == {identity}
            assert not hub.removed
            assert all(value["title"] == "ESP32 device detection" for value in hub.published[named_start:])
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_rollout_thread_title_and_folder_remain_separate_on_device(tmp_path):
    from orb_agents.codex import CodexAdapter

    rollouts = tmp_path / "rollouts"
    rollouts.mkdir()
    timestamp = datetime.now(UTC).isoformat()
    (rollouts / "rollout-title.jsonl").write_text(
        "\n".join(
            json.dumps({"timestamp": timestamp, "type": kind, "payload": payload})
            for kind, payload in [
                ("session_meta", {"id": "named-thread", "cwd": "/tmp/firmware", "title": "ESP32 thread detection"}),
                ("event_msg", {"type": "user_message", "message": "This prompt does not replace the thread title"}),
            ]
        )
        + "\n"
    )
    store = StateStore()
    adapter = CodexAdapter(store, sessions_root=rollouts)
    manager = AgentManager(store=store, adapters={"codex": adapter}, poll_interval=3600)
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager, hooks_secret="x" * 32)
    await bridge.start()
    try:
        assert hub.values["codex:named-thread"]["title"] == "ESP32 thread detection"
        assert hub.values["codex:named-thread"]["project"] == "firmware"
        assert hub.values["codex:named-thread"]["can_message"] is False
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_busy_card_is_retried_and_timeout_cancels_it(tmp_path):
    _, fake, manager = setup_manager()
    hub = DeviceSessions()
    hub.card_limit = 0
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, poll_interval=0.01, manager=manager)
    await bridge.start()
    try:
        value = await bridge.start_session("codex", str(tmp_path), "Prepare")
        pending = asyncio.create_task(fake.ask_permission(value["id"], timeout=0.3))
        await wait_for(hub, lambda: bool(manager.pending_approvals()))
        await bridge.reconcile()
        assert not hub.cards
        assert not pending.done()
        hub.card_limit = 16
        await wait_for(hub, lambda: bool(hub.cards))
        card_id = next(iter(hub.cards))
        assert (await pending).decision == "deny"
        await wait_for(hub, lambda: card_id in hub.cancelled)
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_long_unicode_metadata_and_session_cap(tmp_path):
    store, _, manager = setup_manager()
    hub = DeviceSessions()
    for index in range(30):
        store.update(
            Session(
                f"codex:{index}-" + "ü" * 100,
                "codex",
                "desktop app",
                "題" * 100,
                str(tmp_path / ("項" * 40)),
                state="idle",
                waiting_for="理" * 100,
                last_assistant_message="摘" * 100,
                updated_at=datetime.fromtimestamp(1000 + index, UTC),
            )
        )
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    try:
        assert len(hub.values) == 24
        for value in hub.values.values():
            for key, bound in (("id", 48), ("title", 64), ("project", 32), ("reason", 64), ("summary", 160)):
                assert len(value[key].encode()) <= bound
        assert session_id("codex:" + "ü" * 100) == session_id("codex:" + "ü" * 100)
        assert session_id("codex:" + "ü" * 100) != session_id("codex:" + "ü" * 99)
    finally:
        await bridge.stop()


def test_snippet_never_prints_or_reads_hook_secret(tmp_path):
    secret = _load_secret(tmp_path / "agents")
    assert _load_secret(tmp_path / "agents") == secret
    snippet = hooks_snippet(8447)
    serialized = json.dumps(snippet)
    assert secret not in serialized
    assert HOOK_TOKEN_PLACEHOLDER in serialized
    assert set(snippet["hooks"]) >= {"PermissionRequest", "Notification", "Stop"}


def test_hook_secret_rejects_symlinks_and_public_files(tmp_path):
    directory = tmp_path / "agents"
    _load_secret(directory)
    token = directory / "hook.token"
    token.chmod(0o644)
    with pytest.raises(ValueError):
        _load_secret(directory)
    token.unlink()
    destination = tmp_path / "owner-state"
    destination.write_text("x" * 32)
    token.symlink_to(destination)
    with pytest.raises(OSError):
        _load_secret(directory)
    assert destination.read_text() == "x" * 32


@pytest.mark.asyncio
async def test_real_session_hub_routes_hook_and_cancels_card(tmp_path):
    from orb_hub.sessions import SessionHub

    class Link:
        device_id = "orb-000000000001"

        def __init__(self):
            self.events = asyncio.Queue()

        async def notify(self, method, params):
            self.events.put_nowait((method, params))

        async def close(self, *_args):
            pass

        async def event(self, name):
            async with asyncio.timeout(2):
                while True:
                    method, params = await self.events.get()
                    if method == name:
                        return params

    _, _, manager = setup_manager()
    sessions = SessionHub(tmp_path / "sessions")
    bridge = AgentsBridge(sessions, tmp_path, hooks_port=0, manager=manager)
    link = Link()
    await sessions.start()
    await sessions.subscribe(link)
    await bridge.start()
    try:
        assert (await link.event("sessions.snapshot"))["sessions"] == []
        async with aiohttp.ClientSession() as client:
            pending = asyncio.create_task(
                client.post(
                    bridge.hooks.url,
                    headers={"X-Orb-Secret": bridge.token_file.read_text().strip()},
                    json={
                        "hook_event_name": "PermissionRequest",
                        "session_id": "sessionhub-test",
                        "cwd": str(tmp_path),
                        "tool_name": "Write",
                        "tool_input": {"file_path": "test.txt", "content": "private contents"},
                    },
                )
            )
            card = await link.event("card.show")
            assert card["session"] == "claude:sessionhub-test"
            assert "private contents" not in json.dumps(card)
            answer = await sessions.answer(link.device_id, {"card": card["id"], "option": "allow"})
            assert answer == {"card": card["id"], "answer": {"option": "allow"}, "delivered": True}
            assert await sessions.answer(link.device_id, {"card": card["id"], "option": "deny"}) == answer
            assert await link.event("card.cancel") == {"card": card["id"]}
            assert card["id"] not in sessions.cards
            async with await pending as response:
                assert response.status == 200
                assert (await response.json())["hookSpecificOutput"]["decision"]["behavior"] == "allow"
        owned = await bridge.start_session("codex", str(tmp_path), "Start owned session")
        receipt = await sessions.message(link.device_id, {"target": owned["id"], "text": "Continue"})
        assert receipt["accepted"] is True and receipt["status"] == "delivered"
        assert receipt["reason"]
        assert manager.adapters["codex"].messages[-1] == (owned["id"], "Continue")
    finally:
        await bridge.stop()
        await sessions.close()


@pytest.mark.asyncio
async def test_permission_notification_offers_native_attention_without_fake_approval(tmp_path):
    _, _, manager = setup_manager()
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    try:
        await bridge.hooks.handle(
            {
                "hook_event_name": "Notification",
                "session_id": "native-permission",
                "cwd": str(tmp_path),
                "notification_type": "permission_prompt",
            }
        )
        await wait_for(hub, lambda: bool(hub.cards))
        card = next(iter(hub.cards.values()))
        assert card["kind"] == "attention"
        assert card["reply"] is False
        assert card["options"] == [{"id": "dismiss", "label": "Dismiss", "role": "cancel"}]
        assert not manager.pending_approvals()
        assert hub.values["claude:native-permission"]["state"] == "waiting"
        await bridge.hooks.handle(
            {
                "hook_event_name": "Stop",
                "session_id": "native-permission",
                "cwd": str(tmp_path),
            }
        )
        await wait_for(hub, lambda: card["id"] in hub.cancelled)
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_session_start_reserves_slot_across_provider_io_and_serializes_callers(tmp_path):
    from orb_hub.sessions import SessionHub

    class PausedAdapter(FakeAdapter):
        def __init__(self, store):
            super().__init__(store, "codex")
            self.entered = asyncio.Event()
            self.release = asyncio.Event()
            self.calls = 0

        async def start(self, cwd, prompt, model=None):
            self.calls += 1
            value = await super().start(cwd, prompt, model)
            self.entered.set()
            await self.release.wait()
            return value

    store = StateStore()
    fake = PausedAdapter(store)
    manager = AgentManager(store=store, adapters={"codex": fake}, poll_interval=0.01)
    for index in range(23):
        store.update(Session(f"codex:observed-{index}", "codex", "desktop app", "Observed", str(tmp_path)))
    sessions = SessionHub(tmp_path / "sessions")
    bridge = AgentsBridge(sessions, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    first = asyncio.create_task(bridge.start_session("codex", str(tmp_path), "First"))
    second = None
    try:
        await asyncio.wait_for(fake.entered.wait(), 2)
        assert len(sessions.sessions) == 23
        assert len(sessions._session_reservations) == 1
        second = asyncio.create_task(bridge.start_session("codex", str(tmp_path), "Second"))
        await asyncio.sleep(0)
        assert fake.calls == 1
        store.update(Session("codex:new-observed", "codex", "desktop app", "New arrival", str(tmp_path)))
        await bridge.reconcile()
        assert "codex:new-observed" not in sessions.sessions
        assert len(sessions.sessions) == 23
        with pytest.raises(LinkError, match="busy"):
            await sessions.upsert_session(
                {
                    "id": "dot",
                    "agent": "dot",
                    "title": "Dot",
                    "project": "",
                    "state": "idle",
                    "reason": "",
                    "summary": "",
                    "updated": 0,
                    "can_message": False,
                }
            )
        fake.release.set()
        started = await first
        assert started["id"] in sessions.sessions
        assert started["id"] in sessions._message_handlers
        with pytest.raises(LinkError, match="busy"):
            await second
        assert fake.calls == 1
        assert len(sessions.sessions) == 24
        assert not sessions._session_reservations
        assert (await bridge.status())["pending_starts"] == 0
    finally:
        fake.release.set()
        await asyncio.gather(first, *((second,) if second else ()), return_exceptions=True)
        await bridge.stop()
        await sessions.close()


@pytest.mark.asyncio
async def test_failed_or_cancelled_start_releases_reservation(tmp_path):
    _, _, manager = setup_manager()
    hub = DeviceSessions()
    bridge = AgentsBridge(hub, tmp_path, hooks_port=0, manager=manager)
    await bridge.start()
    try:
        with pytest.raises(LinkError, match="unsupported"):
            await bridge.start_session("unsupported", str(tmp_path), "Prompt")
        assert not hub._reservations
        assert (await bridge.status())["pending_starts"] == 0
        with pytest.raises(LinkError, match="bad_request"):
            await bridge.start_session("codex", str(tmp_path / "missing"), "Prompt")
        assert not hub._reservations
        entered = asyncio.Event()
        release = asyncio.Event()

        async def cancelled_start(*_args):
            entered.set()
            await release.wait()

        manager.start = cancelled_start
        task = asyncio.create_task(bridge.start_session("codex", str(tmp_path), "Prompt"))
        await asyncio.wait_for(entered.wait(), 2)
        assert len(hub._reservations) == 1
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert not hub._reservations
        assert (await bridge.status())["pending_starts"] == 0
    finally:
        await bridge.stop()


@pytest.mark.asyncio
async def test_claude_usage_poller_starts_with_a_fake_binary(tmp_path, monkeypatch):
    _, _, manager = setup_manager()
    source = tmp_path / "synthetic-usage.txt"
    source.write_text("Current session: 2% used · resets Oct 28 at 4:39am (America/New_York)\n")
    binary = tmp_path / "claude"
    binary.write_text(
        f"#!{sys.executable}\n"
        "import pathlib, sys\n"
        "if sys.argv[1:] != ['-p', '/usage']:\n"
        "    sys.exit(9)\n"
        f"sys.stdout.write(pathlib.Path({str(source)!r}).read_text())\n"
    )
    binary.chmod(0o755)
    monkeypatch.setenv("PATH", str(tmp_path))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    hub = DeviceSessions()
    bridge = AgentsBridge(
        hub,
        tmp_path,
        hooks_port=0,
        manager=manager,
        claude_usage=True,
        claude_binary=binary,
    )
    await bridge.start()
    try:
        for _ in range(200):
            if "claude" in bridge._data.usage.observations:
                break
            await asyncio.sleep(0.01)
        else:
            raise AssertionError("synthetic Claude usage poll did not run at startup")
        observation = bridge._data.usage.observations["claude"]
        assert observation.source == "usage"
        assert [window.used_percent for window in observation.windows] == [2]
    finally:
        await bridge.stop()
