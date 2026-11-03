"""Quota snapshots use independent per-link negotiation and a 30-second gate."""

import asyncio
import json

import pytest
from fake_device import paired_device
from orb_agents.usage import UsageCollector

from orb_hub import envelope, models
from orb_hub.envelope import LinkError
from orb_hub.sessions import SessionHub


class UsageLink:
    def __init__(self, device_id, caps):
        self.device_id = device_id
        self.caps = set(caps)
        self.events = []

    async def notify(self, method, params):
        self.events.append((method, params))

    async def close(self, _code, _reason):
        pass


@pytest.fixture
async def usage_hub(short_tmp):
    now = [1000.0]
    coordinator = SessionHub(short_tmp, clock=lambda: now[0])
    yield coordinator, now
    await coordinator.close()


async def flush(coordinator, link):
    await asyncio.wait_for(coordinator._subscribers[link].queue.join(), 1)
    events, link.events = link.events, []
    return [params for method, params in events if method == "usage.snapshot"]


def snapshot(used=25, stale=False):
    return {
        "windows": [
            {
                "provider": "codex",
                "label": "5 hours",
                "used_percent": used,
                "resets_at": 10000,
                "observed_at": 1000,
                "stale": stale,
                "source": "rollout",
            }
        ],
        "unknown": ["claude"],
    }


async def test_usage_only_subscription_is_immediate_and_independent(usage_hub):
    coordinator, _ = usage_hub
    link = UsageLink("usage-only", {"usage"})
    coordinator.subscribe_usage(link)
    assert await flush(coordinator, link) == [{"windows": [], "unknown": ["claude", "codex"]}]
    assert coordinator._subscribers[link].sessions_enabled is False
    assert link.events == []


async def test_cards_without_usage_never_receives_account_quota(usage_hub):
    coordinator, _ = usage_hub
    link = UsageLink("cards-only", {"cards"})
    coordinator.set_usage(snapshot())
    await coordinator.subscribe(link)
    assert await flush(coordinator, link) == []
    coordinator.subscribe_usage(link)
    assert await flush(coordinator, link) == []
    assert not coordinator.delivery_allowed(link, "usage.snapshot", snapshot())


async def test_usage_changes_coalesce_and_resubscribe_does_not_reset_gate(usage_hub):
    coordinator, now = usage_hub
    link = UsageLink("first", {"usage", "cards"})
    coordinator.subscribe_usage(link)
    await flush(coordinator, link)
    coordinator.set_usage(snapshot(26))
    now[0] += 10
    coordinator._refresh_usage(coordinator._subscribers[link])
    await coordinator.subscribe(link)
    assert await flush(coordinator, link) == []
    coordinator.set_usage(snapshot(27))
    now[0] = 1029.999
    coordinator._refresh_usage(coordinator._subscribers[link])
    assert await flush(coordinator, link) == []
    now[0] = 1030
    coordinator._refresh_usage(coordinator._subscribers[link])
    assert await flush(coordinator, link) == [snapshot(27)]
    now[0] = 1060
    coordinator._refresh_usage(coordinator._subscribers[link])
    assert await flush(coordinator, link) == []


async def test_new_link_gets_current_snapshot_while_another_is_rate_limited(usage_hub):
    coordinator, now = usage_hub
    first = UsageLink("first", {"usage"})
    coordinator.subscribe_usage(first)
    await flush(coordinator, first)
    now[0] += 1
    coordinator.set_usage(snapshot(29))
    second = UsageLink("second", {"usage"})
    coordinator.subscribe_usage(second)
    coordinator._refresh_usage(coordinator._subscribers[first])
    assert await flush(coordinator, first) == []
    assert await flush(coordinator, second) == [snapshot(29)]


async def test_stalled_quota_write_cannot_accumulate_a_burst(usage_hub):
    coordinator, now = usage_hub
    entered, release = asyncio.Event(), asyncio.Event()

    class StalledUsageLink(UsageLink):
        async def notify(self, method, params):
            if method == "usage.snapshot":
                entered.set()
                await release.wait()
            await super().notify(method, params)

    link = StalledUsageLink("stalled", {"usage"})
    coordinator.subscribe_usage(link)
    await asyncio.wait_for(entered.wait(), 1)
    subscriber = coordinator._subscribers[link]
    # Advance only the injected clock: no real sleep or network activity.
    for offset, used in ((31, 26), (61, 27), (100, 28)):
        now[0] = 1000 + offset
        coordinator.set_usage(snapshot(used))
        coordinator._refresh_usage(subscriber)
        assert subscriber.queue.empty()
        assert subscriber.usage_pending
    release.set()
    assert await flush(coordinator, link) == [{"windows": [], "unknown": ["claude", "codex"]}]
    assert subscriber.usage_sent == 1100
    coordinator._refresh_usage(subscriber)
    assert await flush(coordinator, link) == []
    now[0] = 1129.999
    coordinator._refresh_usage(subscriber)
    assert await flush(coordinator, link) == []
    now[0] = 1130
    coordinator._refresh_usage(subscriber)
    assert await flush(coordinator, link) == [snapshot(28)]


async def test_staleness_and_expired_windows_change_wire_truth_without_new_reading(usage_hub):
    coordinator, now = usage_hub
    collector = UsageCollector()
    assert collector.observe_codex(
        {
            "type": "event_msg",
            "timestamp": 1000,
            "payload": {
                "type": "token_count",
                "rate_limits": {
                    "primary": {"used_percent": 40, "window_minutes": 300, "resets_at": 3000},
                    "secondary": None,
                },
            },
        }
    )
    coordinator.set_usage(collector.snapshot(now[0]))
    link = UsageLink("first", {"usage"})
    coordinator.subscribe_usage(link)
    assert (await flush(coordinator, link))[0]["windows"][0]["stale"] is False
    now[0] = 1901
    coordinator.set_usage(collector.snapshot(now[0]))
    coordinator._refresh_usage(coordinator._subscribers[link])
    stale = (await flush(coordinator, link))[0]
    assert stale["windows"][0]["stale"] is True
    assert stale["windows"][0]["observed_at"] == 1000
    now[0] = 3000
    coordinator.set_usage(collector.snapshot(now[0]))
    coordinator._refresh_usage(coordinator._subscribers[link])
    assert await flush(coordinator, link) == [{"windows": [], "unknown": ["claude"]}]


def test_full_usage_snapshot_stays_within_envelope_and_rejects_false_certainty():
    value = snapshot()
    value["unknown"] = []
    value["windows"] *= 2
    value["windows"] += [dict(window, provider="claude") for window in value["windows"]]
    cleaned = models.usage_snapshot(value)
    assert len(json.dumps(envelope.event("usage.snapshot", cleaned)).encode()) < 4096
    with pytest.raises(LinkError):
        models.usage_snapshot(dict(value, unknown=["codex"]))
    with pytest.raises(LinkError):
        models.usage_snapshot(dict(value, windows=value["windows"] + [snapshot()["windows"][0]]))


async def test_usage_only_hello_negotiates_and_sends_snapshot_over_pinned_tls(hub):
    hub.sessions.set_usage(snapshot())
    device = await paired_device(hub)
    try:
        await device.disconnect()
        await device.connect()
        result = await device.hello(caps=["usage"])
        assert result["ok"] and result["r"]["caps"] == ["usage"]
        assert await device.event() == ("usage.snapshot", snapshot())
        assert not hub.sessions._subscribers[hub.online[device.device_id]].sessions_enabled
    finally:
        await device.aclose()
