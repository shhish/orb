"""Domain/action/delivery seams preserve durability across yields and stop."""

import asyncio
import sqlite3
import threading

import pytest
from test_sessions import card, session

from orb_hub.envelope import LinkError
from orb_hub.sessions import SessionHub


class CaptureLink:
    device_id = "orb-test"
    caps = frozenset({"cards"})

    def __init__(self):
        self.events = []

    async def notify(self, method, params):
        self.events.append((method, params))

    async def close(self, _code, _reason):
        pass


async def test_executor_cancel_after_durable_decision_never_replays(short_tmp):
    hub = await SessionHub.open(short_tmp)
    entered = asyncio.Event()
    calls = []

    async def provider(answer):
        calls.append(answer)
        # A distinct connection sees the answer before provider work starts.
        with sqlite3.connect(short_tmp / "sessions.db") as db:
            assert db.execute("SELECT answer FROM session_cards WHERE id='c'").fetchone()[0] == '{"option":"yes"}'
        entered.set()
        await asyncio.Event().wait()

    await hub.show_card(card(), answer_handler=provider)
    action = asyncio.create_task(hub.answer("orb-test", {"card": "c", "option": "yes"}))
    try:
        await asyncio.wait_for(entered.wait(), 1)
        assert any(task.get_name() == "orb-card-action" for task in hub.supervisor.tasks)
        action.cancel()
        with pytest.raises(asyncio.CancelledError):
            await action
        receipt = await hub.answer("orb-test", {"card": "c", "option": "no"})
        assert receipt == {"card": "c", "answer": {"option": "yes"}, "delivered": False}
        assert len(calls) == 1
        assert hub.status()["health"]["healthy"]
    finally:
        action.cancel()
        await asyncio.gather(action, return_exceptions=True)
        await hub.close()
    reopened = await SessionHub.open(short_tmp)
    try:
        assert await reopened.answer("orb-test", {"card": "c", "option": "no"}) == receipt
    finally:
        await reopened.close()


async def test_commit_yields_and_repeated_cancellation_cannot_outlive_stop(short_tmp, monkeypatch):
    hub = await SessionHub.open(short_tmp)
    entered, release = threading.Event(), threading.Event()
    original = hub.store.commit
    writer_finished = threading.Event()
    thread_ids = []

    def blocked(sql, params):
        thread_ids.append(threading.get_ident())
        entered.set()
        assert release.wait(3), "test did not release SQLite worker"
        try:
            original(sql, params)
        finally:
            writer_finished.set()

    monkeypatch.setattr(hub.store, "commit", blocked)
    request = asyncio.create_task(hub.show_card(card()))
    stop = None
    try:
        assert await asyncio.to_thread(entered.wait, 1)
        # Real commit is blocked, while arbitrary event-loop work can advance.
        for _ in range(20):
            await asyncio.sleep(0)
        assert thread_ids == [thread_ids[0]] and thread_ids[0] != threading.get_ident()
        assert not request.done()
        assert hub.pending_card_ids() == {"c"}
        assert hub.has_card("c")
        stop = asyncio.create_task(hub.close())
        await asyncio.sleep(0)
        request.cancel()
        await asyncio.sleep(0)
        request.cancel()
        await asyncio.sleep(0)
        assert not stop.done()
        release.set()
        await asyncio.wait_for(stop, 1)
        await asyncio.gather(request, return_exceptions=True)
        assert writer_finished.is_set()
        assert not hub.supervisor.tasks
        assert hub.store.closed
        with pytest.raises(sqlite3.ProgrammingError):
            hub.db.execute("SELECT 1")
    finally:
        release.set()
        await asyncio.gather(request, return_exceptions=True)
        if stop:
            await stop
        else:
            await hub.close()


async def test_pending_preference_commit_fences_delivery_and_merges_patches(short_tmp, monkeypatch):
    hub = await SessionHub.open(short_tmp)
    await hub.upsert_session(session())
    await hub.show_card(card(), source="claude")
    link = CaptureLink()
    await hub.subscribe(link)
    await hub.attach(link).queue.join()
    assert hub.delivery_allowed(link, "card.show", card())
    entered, release = threading.Event(), threading.Event()
    original = hub.store.commit

    def blocked(sql, params):
        entered.set()
        assert release.wait(3)
        original(sql, params)

    monkeypatch.setattr(hub.store, "commit", blocked)
    disable = asyncio.create_task(hub.set_prefs(link.device_id, {"sources": {"claude": False}}))
    units = None
    try:
        assert await asyncio.to_thread(entered.wait, 1)
        assert not hub.delivery_allowed(link, "card.show", card())
        units = asyncio.create_task(hub.set_prefs(link.device_id, {"units": {"temperature": "F"}}))
        release.set()
        await asyncio.gather(disable, units)
        prefs = hub.get_prefs(link.device_id)
        assert prefs["sources"]["claude"] is False
        assert prefs["units"] == {"temperature": "F", "source": "owner"}
        assert not hub.store.preference_updates
        with pytest.raises(LinkError):
            await hub.set_prefs(link.device_id, {"idle_minutes": -1})
        await asyncio.sleep(0)
        assert hub.status()["health"]["healthy"]
    finally:
        release.set()
        await asyncio.gather(disable, *([units] if units else []), return_exceptions=True)
        await hub.close()


async def test_dot_without_message_owner_keeps_disconnected_projection(short_tmp):
    hub = SessionHub(short_tmp)
    try:
        for invalid in (None, [], "invalid"):
            with pytest.raises(LinkError):
                await hub.upsert_session(invalid)
        await hub.upsert_session(session("dot", agent="dot", can_message=False, state="busy"))
        assert hub.store.selection("orb-test")[0][0]["can_message"] is False
        projected = hub.session_snapshot("dot")
        projected["summary"] = "caller mutation"
        assert hub.session_snapshot("dot")["summary"] != "caller mutation"
    finally:
        await hub.close()


async def test_identical_concurrent_preferences_publish_final_value_once(short_tmp):
    hub = await SessionHub.open(short_tmp)
    link = CaptureLink()
    try:
        await hub.subscribe(link)
        await hub.attach(link).queue.join()
        link.events.clear()
        await asyncio.gather(
            hub.set_prefs(link.device_id, {"sources": {"codex": False}}),
            hub.set_prefs(link.device_id, {"sources": {"codex": False}}),
        )
        await hub.attach(link).queue.join()
        prefs = [value for method, value in link.events if method == "prefs.update"]
        assert prefs == [hub.get_prefs(link.device_id)]
        assert prefs[0]["sources"]["codex"] is False
    finally:
        await hub.close()


async def test_session_close_finishes_database_cleanup_after_repeated_cancel(short_tmp, monkeypatch):
    hub = await SessionHub.open(short_tmp)
    entered, release = asyncio.Event(), asyncio.Event()
    original = hub.supervisor.stop

    async def blocked():
        entered.set()
        await release.wait()
        await original()

    monkeypatch.setattr(hub.supervisor, "stop", blocked)
    closing = asyncio.create_task(hub.close())
    try:
        await entered.wait()
        closing.cancel()
        await asyncio.sleep(0)
        closing.cancel()
        await asyncio.sleep(0)
        assert not closing.done()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await closing
        assert hub.store.closed
        with pytest.raises(sqlite3.ProgrammingError):
            hub.db.execute("SELECT 1")
    finally:
        release.set()
        await asyncio.gather(closing, return_exceptions=True)
        await hub.close()


async def test_cancel_during_answer_commit_still_reaches_provider(short_tmp, monkeypatch):
    # Shutdown can cancel an answer while its commit is in flight; the durable
    # answer must still reach the provider exactly once, never be dropped.
    hub = await SessionHub.open(short_tmp)
    entered, release = threading.Event(), threading.Event()
    original = hub.store.commit
    calls = []

    def blocked(sql, params):
        if "answered" in sql:
            entered.set()
            assert release.wait(3), "test did not release SQLite worker"
        original(sql, params)

    async def provider(answer):
        calls.append(answer)
        return True

    await hub.show_card(card(), answer_handler=provider)
    monkeypatch.setattr(hub.store, "commit", blocked)
    action = asyncio.create_task(hub.answer("orb-test", {"card": "c", "option": "yes"}))
    try:
        assert await asyncio.to_thread(entered.wait, 1)
        action.cancel()
        await asyncio.sleep(0)
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(action, 2)
        assert [answer["option"] for answer in calls] == ["yes"]
        receipt = await hub.answer("orb-test", {"card": "c", "option": "no"})
        assert receipt == {"card": "c", "answer": {"option": "yes"}, "delivered": True}
        assert len(calls) == 1
    finally:
        release.set()
        await asyncio.gather(action, return_exceptions=True)
        await hub.close()
