"""HomeRelay discovery is exercised without opening a socket or browsing the LAN."""

import asyncio
from types import SimpleNamespace

import pytest

from orb_hub import home
from orb_hub.envelope import LinkError


class Discovery:
    def __init__(self, monkeypatch, *, address="192.0.2.10", properties=None, resolved=True, announce=True):
        self.addresses = [address] if address else []
        self.properties = {b"api": b"/api/v1"} if properties is None else properties
        self.resolved = resolved
        self.announce = announce
        self.cancelled = False
        self.closed = False
        self.created = asyncio.Event()
        self.requests = []
        self.instance = "HomeRelay." + home.SERVICE
        self.zc = object()
        monkeypatch.setattr(home, "AsyncZeroconf", self.zeroconf)
        monkeypatch.setattr(home, "AsyncServiceBrowser", self.browser)
        monkeypatch.setattr(home, "AsyncServiceInfo", self.info)

    def zeroconf(self):
        async def close():
            self.closed = True

        return SimpleNamespace(zeroconf=self.zc, async_close=close)

    def browser(self, zeroconf, service, *, handlers):
        assert zeroconf is self.zc and service == home.SERVICE
        self.created.set()
        if self.announce:
            handlers[0](zeroconf, service, self.instance, home.ServiceStateChange.Added)

        async def cancel():
            self.cancelled = True

        return SimpleNamespace(async_cancel=cancel)

    def info(self, service, instance):
        assert service == home.SERVICE and instance == self.instance

        async def request(zeroconf, timeout):
            assert zeroconf is self.zc
            self.requests.append(timeout)
            return self.resolved

        return SimpleNamespace(
            async_request=request,
            parsed_scoped_addresses=lambda: self.addresses,
            properties=self.properties,
            port=18796,
        )

    def assert_clean(self):
        assert self.cancelled and self.closed


@pytest.mark.parametrize(
    "address,properties,expected",
    [
        ("192.0.2.10", {b"api": b"/relay/v1"}, "http://192.0.2.10:18796/relay/v1"),
        ("2001:db8::10", {b"api": b"/api/v1"}, "http://[2001:db8::10]:18796/api/v1"),
        ("fe80::10%en0", {}, "http://[fe80::10%en0]:18796/api/v1"),
    ],
)
async def test_bonjour_uses_api_txt_and_brackets_ipv6(monkeypatch, address, properties, expected):
    discovery = Discovery(monkeypatch, address=address, properties=properties)
    assert await home.discover_url() == expected
    assert discovery.requests and all(0 < timeout <= 2000 for timeout in discovery.requests)
    discovery.assert_clean()


@pytest.mark.parametrize(
    "settings",
    [
        {"resolved": False},
        {"address": None},
        {"properties": {b"api": b"not-an-absolute-path"}},
        {"properties": {b"api": b"//different-host/api/v1"}},
        {"properties": {b"api": b"\xff"}},
        {"properties": {b"api": None}},
        {"properties": {b"api": "/api/v1"}},
        {"properties": {b"api": b"/api/v1?token=never"}},
        {"properties": {b"api": b"/api/v1#fragment"}},
        {"properties": {b"api": b"/api/v1\r\n"}},
    ],
)
async def test_unusable_advertisement_falls_back_and_closes_discovery(monkeypatch, settings):
    discovery = Discovery(monkeypatch, **settings)
    assert await home.discover_url() == home.DEFAULT_URL
    discovery.assert_clean()


async def test_resolution_failure_falls_back_and_cleans_up(monkeypatch):
    discovery = Discovery(monkeypatch)

    def failing_info(*args):
        async def request(*args):
            raise OSError("test-only Bonjour failure")

        return SimpleNamespace(async_request=request)

    monkeypatch.setattr(home, "AsyncServiceInfo", failing_info)
    assert await home.discover_url() == home.DEFAULT_URL
    discovery.assert_clean()


@pytest.mark.parametrize("constructor", ["AsyncZeroconf", "AsyncServiceBrowser"])
async def test_bonjour_construction_failure_uses_default_and_closes_created_resources(monkeypatch, constructor):
    discovery = Discovery(monkeypatch)

    def failing_constructor(*args, **kwargs):
        raise OSError("test-only Bonjour startup failure")

    monkeypatch.setattr(home, constructor, failing_constructor)
    assert await home.discover_url() == home.DEFAULT_URL
    assert not discovery.cancelled
    assert discovery.closed is (constructor == "AsyncServiceBrowser")


async def test_no_advertisement_times_out_and_cleans_up(monkeypatch):
    discovery = Discovery(monkeypatch, announce=False)
    # Keep the production timeout path, shortening only this isolated browse.
    monkeypatch.setattr(home, "asyncio", SimpleNamespace(Event=asyncio.Event, timeout=lambda _: asyncio.timeout(0.01)))
    assert await home.discover_url() == home.DEFAULT_URL
    assert not discovery.requests
    discovery.assert_clean()


async def test_cancellation_closes_browser_and_zeroconf(monkeypatch):
    discovery = Discovery(monkeypatch, announce=False)
    task = asyncio.create_task(home.discover_url())
    await asyncio.wait_for(discovery.created.wait(), 1)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    discovery.assert_clean()


@pytest.mark.parametrize("explicit", [None, "http://127.0.0.1:23457/explicit/"])
async def test_environment_url_bypasses_bonjour_without_network(monkeypatch, short_tmp, explicit):
    monkeypatch.setenv("HOMERELAY_URL", "http://127.0.0.1:23456/from-env/")

    async def unexpected_discovery():
        pytest.fail("an explicit relay URL must bypass Bonjour")

    monkeypatch.setattr(home, "discover_url", unexpected_discovery)
    bridge = home.HomeBridge(None, short_tmp, url=explicit)
    monkeypatch.setattr(bridge, "_token", lambda: "hr_TEST_ONLY_discovery")
    called = asyncio.Event()
    requests = []

    class NoNetworkClient:
        def get(self, url, **kwargs):
            requests.append(url)
            called.set()
            raise LinkError("unsupported", "test stopped before networking")

        async def close(self):
            pass

    bridge._client = NoNetworkClient()
    bridge._tasks = [asyncio.create_task(bridge._run())]
    try:
        await asyncio.wait_for(called.wait(), 1)
        expected = explicit or "http://127.0.0.1:23456/from-env/"
        assert requests == [expected.rstrip("/") + "/events"]
        assert bridge.url == expected.rstrip("/")
    finally:
        await bridge.stop()
