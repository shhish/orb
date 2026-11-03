"""Worker failure and resource ownership, using only temporary state."""

import asyncio
import sqlite3
import threading

import pytest

from orb_hub.home import HomeBridge
from orb_hub.lifecycle import LifecycleSupervisor


async def test_worker_failure_is_retrieved_redacted_and_health_visible(caplog):
    supervisor = LifecycleSupervisor("fixture")

    async def fail():
        raise RuntimeError("private credential and owner text")

    task = supervisor.spawn(fail(), name="fixture-worker")
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    assert task.done()
    assert supervisor.health()["failures"] == {"fixture-worker": {"type": "RuntimeError", "count": 1}}
    assert "private credential" not in caplog.text
    assert "RuntimeError" in caplog.text
    await supervisor.stop()
    assert not supervisor.tasks


async def test_safe_recovery_is_bounded_and_commands_are_not_replayed():
    supervisor = LifecycleSupervisor("fixture")
    calls = []

    async def fail():
        calls.append(None)
        raise OSError("fixture")

    await supervisor.run(fail, name="read-only-loop", restarts=2, delay=0)
    assert len(calls) == 3
    assert supervisor.health()["failures"]["read-only-loop"]["count"] == 3
    with pytest.raises(OSError):
        await supervisor.spawn(fail(), name="one-shot-action")
    assert len(calls) == 4
    await supervisor.stop()


async def test_stop_awaits_worker_cleanup_and_rejects_new_work():
    supervisor = LifecycleSupervisor("fixture")
    entered, cleaned = asyncio.Event(), asyncio.Event()

    async def worker():
        entered.set()
        try:
            await asyncio.Event().wait()
        finally:
            await asyncio.sleep(0)
            cleaned.set()

    task = supervisor.spawn(worker(), name="fixture-worker")
    await entered.wait()
    await supervisor.stop()
    assert cleaned.is_set() and task.done() and not supervisor.tasks
    with pytest.raises(RuntimeError, match="stopped"):
        supervisor.spawn(worker(), name="late")


async def test_home_stop_closes_client_database_and_workers(tmp_path, monkeypatch):
    bridge = HomeBridge(None, tmp_path, url="http://127.0.0.1:1")
    entered = asyncio.Event()

    async def blocked():
        entered.set()
        await asyncio.Event().wait()

    monkeypatch.setattr(bridge, "_run", blocked)
    monkeypatch.setattr(bridge, "_coalesce", blocked)
    before = asyncio.all_tasks()
    await bridge.start()
    client = bridge._client
    await entered.wait()
    assert len(bridge.supervisor.tasks) == 2
    await bridge.stop()
    assert client.closed
    assert not bridge.supervisor.tasks
    assert not (asyncio.all_tasks() - before)
    with pytest.raises(sqlite3.ProgrammingError, match="closed"):
        bridge.db.execute("SELECT 1")


async def test_home_commit_runs_off_loop_and_cancellation_joins_it(tmp_path, monkeypatch):
    bridge = HomeBridge(None, tmp_path, url="http://127.0.0.1:1")
    bridge.accessories["fixture"] = {"id": "fixture", "name": "Fixture", "_default": False}
    entered, release, finished = threading.Event(), threading.Event(), threading.Event()
    save = bridge._save_favourite

    def delayed_save(*args):
        entered.set()
        assert release.wait(5)
        save(*args)
        finished.set()

    monkeypatch.setattr(bridge, "_save_favourite", delayed_save)
    task = asyncio.create_task(bridge.set_favourite("fixture-device", "fixture", True))
    try:
        async with asyncio.timeout(5):
            while not entered.is_set():
                await asyncio.sleep(0)
        # The loop advances while the commit thread is deliberately blocked.
        task.cancel()
        await asyncio.sleep(0)
        assert not task.done()
        task.cancel()
        await asyncio.sleep(0)
        assert not task.done()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await task
        assert finished.is_set()
        assert bridge.favourites("fixture-device")["favourites"][0]["id"] == "fixture"
    finally:
        release.set()
        await asyncio.gather(task, return_exceptions=True)
        await bridge.stop()


async def test_expected_foreground_error_is_retrieved_without_worker_failure():
    supervisor = LifecycleSupervisor("fixture")

    async def invalid_request():
        raise ValueError("invalid owner input")

    with pytest.raises(ValueError):
        await supervisor.spawn(invalid_request(), name="request", expected_errors=(ValueError,))
    await asyncio.sleep(0)
    assert supervisor.health()["healthy"]
    await supervisor.stop()


async def test_hub_stop_closes_listeners_database_and_all_workers(make_hub):
    before = asyncio.all_tasks()
    hub = await make_hub()
    listeners = tuple(hub._listeners)
    store = hub.store
    await hub.stop()
    await asyncio.sleep(0)
    assert not hub.supervisor.tasks
    assert not hub.pairing.supervisor.tasks
    assert not hub.voice.supervisor.tasks
    assert all(not listener.is_serving() for listener in listeners)
    assert not hub.cfg.control_socket.exists()
    assert not (asyncio.all_tasks() - before)
    with pytest.raises(sqlite3.ProgrammingError, match="closed"):
        store._db.execute("SELECT 1")


async def test_home_schema_open_runs_in_worker_and_closes(tmp_path, monkeypatch):
    thread_ids = []
    connect = sqlite3.connect
    loop_thread = threading.get_ident()

    def record_connect(*args, **kwargs):
        thread_ids.append(threading.get_ident())
        return connect(*args, **kwargs)

    monkeypatch.setattr(sqlite3, "connect", record_connect)
    bridge = await HomeBridge.open(None, tmp_path, url="http://127.0.0.1:1")
    assert thread_ids and all(identity != loop_thread for identity in thread_ids)
    assert bridge.db.execute("SELECT count(*) FROM favourites").fetchone()[0] == 0
    await bridge.stop()
    with pytest.raises(sqlite3.ProgrammingError, match="closed"):
        bridge.db.execute("SELECT 1")


async def test_cancelled_stop_still_awaits_worker_finalizers():
    supervisor = LifecycleSupervisor("fixture")
    entered, finalizing, finish = asyncio.Event(), asyncio.Event(), asyncio.Event()

    async def worker():
        entered.set()
        try:
            await asyncio.Event().wait()
        finally:
            finalizing.set()
            await finish.wait()

    task = supervisor.spawn(worker(), name="fixture-worker")
    await entered.wait()
    stopping = asyncio.create_task(supervisor.stop())
    await finalizing.wait()
    stopping.cancel()
    await asyncio.sleep(0)
    stopping.cancel()
    await asyncio.sleep(0)
    assert not stopping.done()
    finish.set()
    with pytest.raises(asyncio.CancelledError):
        await stopping
    assert task.done() and not supervisor.tasks


async def test_cancelled_home_stop_finishes_client_and_database_cleanup(tmp_path, monkeypatch):
    bridge = HomeBridge(None, tmp_path, url="http://127.0.0.1:1")
    closing, release = asyncio.Event(), asyncio.Event()

    async def blocked():
        await asyncio.Event().wait()

    monkeypatch.setattr(bridge, "_run", blocked)
    monkeypatch.setattr(bridge, "_coalesce", blocked)
    before = asyncio.all_tasks()
    await bridge.start()
    client = bridge._client
    original_close = client.close

    async def delayed_close():
        closing.set()
        await release.wait()
        await original_close()

    monkeypatch.setattr(client, "close", delayed_close)
    stopping = asyncio.create_task(bridge.stop())
    try:
        await closing.wait()
        for _ in range(2):
            stopping.cancel()
            await asyncio.sleep(0)
        assert not stopping.done()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await stopping
        assert client.closed and not bridge.supervisor.tasks
        with pytest.raises(sqlite3.ProgrammingError, match="closed"):
            bridge.db.execute("SELECT 1")
        assert not (asyncio.all_tasks() - before)
    finally:
        release.set()
        await asyncio.gather(stopping, return_exceptions=True)


async def test_cancelled_hub_stop_finishes_services_sockets_and_databases(make_hub, monkeypatch):
    before = asyncio.all_tasks()
    hub = await make_hub()
    closing, release = asyncio.Event(), asyncio.Event()
    weather_stop = hub.weather.stop
    listeners, store, sessions = tuple(hub._listeners), hub.store, hub.sessions

    async def delayed_stop():
        closing.set()
        await release.wait()
        await weather_stop()

    monkeypatch.setattr(hub.weather, "stop", delayed_stop)
    stopping = asyncio.create_task(hub.stop())
    try:
        await closing.wait()
        for _ in range(2):
            stopping.cancel()
            await asyncio.sleep(0)
        assert not stopping.done()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await stopping
        assert not hub.supervisor.tasks and not sessions.supervisor.tasks
        assert all(not listener.is_serving() for listener in listeners)
        assert not hub.cfg.control_socket.exists()
        with pytest.raises(sqlite3.ProgrammingError, match="closed"):
            store._db.execute("SELECT 1")
        with pytest.raises(sqlite3.ProgrammingError, match="closed"):
            sessions.db.execute("SELECT 1")
        assert not (asyncio.all_tasks() - before)
    finally:
        release.set()
        await asyncio.gather(stopping, return_exceptions=True)


async def test_hub_health_includes_delivery_and_speech_failures(make_hub):
    from orb_hub.link import Link

    hub = await make_hub()
    link = Link(hub, None, "fixture", "127.0.0.1")
    link.device_id = "fixture-device"
    hub.online[link.device_id] = link
    subscriber = hub.sessions.attach(link)
    try:
        subscriber.failed = True
        health = hub.health()
        assert not health["healthy"]
        assert health["services"]["sessions"]["delivery_failures"] == 1
        subscriber.failed = False

        async def fail():
            raise RuntimeError("private speech text must stay out of diagnostics")

        task = link.speech.supervisor.spawn(fail(), name="fixture-speech")
        await asyncio.gather(task, return_exceptions=True)
        health = hub.health()
        assert not health["healthy"]
        assert health["services"]["speech:0"]["failures"]["fixture-speech"]["type"] == "RuntimeError"
    finally:
        hub.online.pop(link.device_id)
        hub.sessions.unsubscribe(link)
        await link.lifecycle.stop()
        await link.speech.close()
