"""Agents 2.0 stream delivery: deterministic clocks and synthetic payloads."""

import asyncio
import json
from types import SimpleNamespace

import pytest
from test_sessions import session

from orb_hub import envelope
from orb_hub.envelope import LinkError
from orb_hub.link import Link
from orb_hub.sessions import SessionHub


class CaptureLink:
    def __init__(self, device_id="orb-first", caps=("cards", "usage")):
        self.device_id = device_id
        self.caps = set(caps)
        self.events = []

    async def notify(self, method, params):
        self.events.append((method, params))

    async def close(self, _code, _reason):
        pass


def stream(identity="s", text="Synthetic reply.", **changes):
    return {
        "session": identity,
        "seq": 0,
        "at": 10000,
        "state": "busy",
        "stale": False,
        "activity": "Ran idf.py -j4 build",
        "steps": [{"id": "t1", "kind": "tool", "label": "Ran idf.py -j4 build", "state": "running"}],
        "assistant": {"id": "m1", "text": text, "final": False},
        **changes,
    }


async def drain(hub, link):
    await asyncio.wait_for(hub._subscribers[link].queue.join(), 1)
    events, link.events = link.events, []
    return events


def streams(events):
    return [params for name, params in events if name == "agent.stream"]


def usage(percent=1):
    return {
        "windows": [
            {
                "provider": "codex",
                "label": "5 hours",
                "used_percent": percent,
                "resets_at": 20000,
                "observed_at": 10000,
                "stale": False,
                "source": "rollout",
            }
        ],
        "unknown": ["claude"],
    }


@pytest.fixture
async def coordinator(short_tmp):
    now = [10000.0]
    hub = SessionHub(short_tmp, clock=lambda: now[0])
    yield hub, now
    await hub.close()


async def test_only_eight_focused_enabled_streams_and_bounded_envelopes(coordinator):
    hub, _ = coordinator
    for index in range(24):
        identity = f"s{index:02}"
        await hub.upsert_session(session(identity, state="busy", updated=10000 - index))
        hub.upsert_stream(stream(identity, text="界" * 53, activity="界" * 21))
    link = CaptureLink()
    await hub.subscribe(link)
    events = await drain(hub, link)
    assert [value["session"] for value in streams(events)] == [f"s{i:02}" for i in range(8)]
    assert all(len(envelope.encode(envelope.event(name, params)).encode()) <= 4096 for name, params in events)
    last_snapshot = max(index for index, event in enumerate(events) if event[0] == "sessions.snapshot")
    first_stream = next(index for index, event in enumerate(events) if event[0] == "agent.stream")
    assert first_stream > last_snapshot
    await hub.set_prefs(link.device_id, {"sources": {"codex": False}})
    assert not streams(await drain(hub, link))


async def test_live_streams_coalesce_to_latest_and_consecutive_published_sequence(coordinator):
    hub, now = coordinator
    await hub.upsert_session(session(state="busy", updated=10000))
    hub.upsert_stream(stream())
    link = CaptureLink()
    await hub.subscribe(link)
    first = streams(await drain(hub, link))[0]
    for index in range(100):
        hub.upsert_stream(stream(text=f"Synthetic update {index}."))
    now[0] += 0.249
    hub._refresh(hub._subscribers[link])
    assert not streams(await drain(hub, link))
    now[0] += 0.002
    hub._refresh(hub._subscribers[link])
    second = streams(await drain(hub, link))
    assert len(second) == 1
    assert second[0]["assistant"]["text"] == "Synthetic update 99."
    assert second[0]["seq"] == first["seq"] + 1


async def test_resubscribe_repairs_gap_after_snapshot_and_preserves_rate_limit(coordinator):
    hub, now = coordinator
    await hub.upsert_session(session(state="busy", updated=10000))
    hub.upsert_stream(stream())
    link = CaptureLink()
    await hub.subscribe(link)
    first = streams(await drain(hub, link))[0]
    hub.upsert_stream(stream(text="Current synthetic snapshot."))
    now[0] += 0.1
    await hub.subscribe(link)
    early = await drain(hub, link)
    assert any(name == "sessions.snapshot" and params["reset"] for name, params in early)
    assert not streams(early)
    now[0] += 0.151
    hub._refresh(hub._subscribers[link])
    repaired = streams(await drain(hub, link))
    assert len(repaired) == 1
    assert repaired[0]["assistant"]["text"] == "Current synthetic snapshot."
    assert repaired[0]["seq"] > first["seq"]


async def test_unchanged_snapshot_replays_same_sequence_after_rate_interval(coordinator):
    hub, now = coordinator
    await hub.upsert_session(session(state="busy", updated=10000))
    hub.upsert_stream(stream())
    link = CaptureLink()
    await hub.subscribe(link)
    first = streams(await drain(hub, link))
    now[0] += 0.251
    await hub.subscribe(link)
    assert streams(await drain(hub, link)) == first


async def test_usage_is_independent_and_limited_even_on_resubscribe(coordinator):
    hub, now = coordinator
    hub.set_usage(usage())
    link = CaptureLink(caps=("usage",))
    hub.subscribe_usage(link)
    assert [value for name, value in await drain(hub, link) if name == "usage.snapshot"] == [usage()]
    for index in range(2, 20):
        hub.set_usage(usage(index))
        now[0] += 0.1
        hub.subscribe_usage(link)
    assert not await drain(hub, link)
    now[0] = 10030
    hub._refresh_usage(hub._subscribers[link])
    events = await drain(hub, link)
    assert events == [("usage.snapshot", usage(19))]
    assert not streams(events)
    other = CaptureLink("orb-second", caps=("cards",))
    await hub.subscribe(other)
    assert not any(name == "usage.snapshot" for name, _ in await drain(hub, other))


async def test_source_change_rechecks_stream_under_actual_link_send_lock(coordinator):
    hub, _ = coordinator
    await hub.upsert_session(session(state="busy", updated=10000))
    hub.upsert_stream(stream())

    class Socket:
        def __init__(self):
            self.frames = []

        async def send_str(self, text):
            self.frames.append(json.loads(text))

    socket = Socket()
    runtime = SimpleNamespace(sessions=hub, cfg=SimpleNamespace(request_timeout=2))
    link = Link(runtime, socket, "127.0.0.1", "127.0.0.1")
    link.device_id = "orb-first"
    link.caps = {"cards"}
    await link._send_lock.acquire()
    try:
        await hub.subscribe(link)
        # Let worker dequeue the first old snapshot and await the real lock.
        await asyncio.sleep(0)
        await hub.set_prefs(link.device_id, {"sources": {"codex": False}})
    finally:
        link._send_lock.release()
    await asyncio.wait_for(hub._subscribers[link].queue.join(), 1)
    assert not any(frame["m"] == "agent.stream" for frame in socket.frames)
    assert not any(frame["m"] == "sessions.snapshot" and frame["p"]["sessions"] for frame in socket.frames)


async def test_stream_protocol_rejects_byte_overflow_and_nonfocus_stays_local(coordinator):
    hub, _ = coordinator
    await hub.upsert_session(session(state="busy", updated=10000))
    with pytest.raises(LinkError, match="160 UTF-8 bytes"):
        hub.upsert_stream(stream(text="界" * 54))
    with pytest.raises(LinkError, match="unknown stream session"):
        hub.upsert_stream(stream("missing"))


async def test_stalled_stream_has_one_outstanding_event_and_no_recovery_burst(coordinator):
    hub, now = coordinator
    await hub.upsert_session(session(state="busy", updated=10000))
    hub.upsert_stream(stream())
    reached = asyncio.Event()
    release = asyncio.Event()

    class BlockedLink(CaptureLink):
        async def notify(self, method, params):
            if method == "agent.stream":
                reached.set()
                await release.wait()
            await super().notify(method, params)

    link = BlockedLink(caps=("cards",))
    await hub.subscribe(link)
    await asyncio.wait_for(reached.wait(), 1)
    for index in range(10):
        now[0] += 0.3
        hub.upsert_stream(stream(text=f"Synthetic pending {index}."))
        hub._refresh(hub._subscribers[link])
    assert hub._subscribers[link].queue.qsize() == 0
    release.set()
    first = streams(await drain(hub, link))
    assert len(first) == 1
    hub._refresh(hub._subscribers[link])
    assert not streams(await drain(hub, link))
    now[0] += 0.249
    hub._refresh(hub._subscribers[link])
    assert not streams(await drain(hub, link))
    now[0] += 0.002
    hub._refresh(hub._subscribers[link])
    latest = streams(await drain(hub, link))
    assert len(latest) == 1
    assert latest[0]["assistant"]["text"] == "Synthetic pending 9."
    assert latest[0]["seq"] == first[0]["seq"] + 1


def test_step_line_counts_preserve_known_zero_and_omit_unknown():
    from orb_hub.models import agent_stream

    payload = stream()
    assert "added" not in agent_stream(payload)["steps"][0]
    payload["steps"][0].update(added=42, removed=0)
    assert agent_stream(payload)["steps"][0] == payload["steps"][0]


async def test_private_dot_streams_replay_only_to_targeted_device(coordinator):
    hub, now = coordinator
    await hub.upsert_session(session("dot", agent="dot", updated=10000))
    hub.upsert_stream(stream("dot", text="First device reply.", state="idle"), device_id="orb-first")
    hub.upsert_stream(stream("dot", text="Second device reply.", state="idle"), device_id="orb-second")
    first, second = CaptureLink(), CaptureLink("orb-second")
    await hub.subscribe(first)
    await hub.subscribe(second)
    assert streams(await drain(hub, first))[0]["assistant"]["text"] == "First device reply."
    assert streams(await drain(hub, second))[0]["assistant"]["text"] == "Second device reply."
    hub.unsubscribe(first)
    now[0] += 1
    reconnected = CaptureLink()
    await hub.subscribe(reconnected)
    replay = streams(await drain(hub, reconnected))
    assert len(replay) == 1 and replay[0]["assistant"]["text"] == "First device reply."
    await hub.remove_session("dot")
    assert all("dot" not in values for values in hub._device_streams.values())


@pytest.mark.parametrize("value", [-1, 1.5, True, None, 2147483648])
def test_step_line_counts_reject_invalid_values(value):
    from orb_hub.models import agent_stream

    payload = stream()
    payload["steps"][0]["added"] = value
    with pytest.raises(LinkError):
        agent_stream(payload)


async def test_private_stream_speech_hook_keeps_device_scope(coordinator):
    hub, _ = coordinator
    calls = []
    hub.on_speech_trigger = lambda target, text, device_id: calls.append((target, text, device_id))
    await hub.upsert_session(session("dot", agent="dot", state="busy", updated=10000))
    value = stream("dot", assistant={"id": "m1", "text": "Private reply.", "final": True})
    hub.upsert_stream(value, device_id="orb-first")
    assert calls == [("dot", "Private reply.", "orb-first")]
    link = Link(SimpleNamespace(sessions=hub), None, "test-only", "127.0.0.1")
    link.device_id = "orb-first"
    link._arm_speech("dot")
    assert "Private reply." in link._speech_baseline
