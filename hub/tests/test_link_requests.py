"""Bounded hub-to-device requests, including real async backpressure and TLS reconnect."""

import asyncio
import gc
import json
from types import SimpleNamespace

import pytest
from fake_device import FakeDevice, paired_device

from orb_hub import envelope
from orb_hub.envelope import Envelope, LinkError
from orb_hub.link import MAX_INFLIGHT_REQUESTS, Link


class ControlledSocket:
    """Only the transport is synthetic; Link's lock, deadlines and Futures are real."""

    def __init__(self):
        self.messages = asyncio.Queue()
        self.writable = asyncio.Event()
        self.writable.set()
        self.failure = None
        self.active_sends = 0

    async def send_str(self, text):
        self.active_sends += 1
        try:
            await self.messages.put(json.loads(text))
            await self.writable.wait()
            if self.failure:
                raise self.failure
        finally:
            self.active_sends -= 1


@pytest.fixture
async def request_link():
    socket = ControlledSocket()
    hub = SimpleNamespace(cfg=SimpleNamespace(request_timeout=0.04))
    link = Link(hub, socket, "test-only", "127.0.0.1")
    link.state = "online"
    return link


async def next_sent(link):
    async with asyncio.timeout(1):
        return await link.ws.messages.get()


def resolve(link, request, *, result=None, error=None):
    link._resolve(Envelope(t="res", id=request["id"], ok=error is None, r=result or {}, e=error or {}))


async def assert_reusable(link):
    link.ws.writable.set()
    link.ws.failure = None
    call = asyncio.create_task(link.request("device.ping", timeout=1))
    message = await next_sent(link)
    resolve(link, message, result={"uptime_s": 12})
    assert await call == {"uptime_s": 12}
    assert not link._waiting
    assert not link._send_lock.locked()
    assert link.ws.active_sends == 0


async def test_stalled_send_caps_admission_and_obeys_deadline(request_link):
    link = request_link
    link.ws.writable.clear()
    calls = [asyncio.create_task(link.request("device.ping", timeout=0.5))]
    first = await next_sent(link)
    # Keep the transport owner alive past all lock-waiter deadlines. Equal
    # relative deadlines start at slightly different times: under load a later
    # waiter can legitimately acquire the lock just before its own deadline.
    calls.extend(asyncio.create_task(link.request("device.ping", timeout=0.05)) for _ in range(63))
    await asyncio.sleep(0)  # Admit the remaining calls before inspecting the cap.
    # All admissions happen before their first await; no unbounded semaphore queue.
    assert len(link._waiting) == MAX_INFLIGHT_REQUESTS
    assert sum(call.done() for call in calls) == 64 - MAX_INFLIGHT_REQUESTS
    async with asyncio.timeout(1):
        results = await asyncio.gather(*calls, return_exceptions=True)
    assert sum(isinstance(result, LinkError) and result.code == envelope.TIMEOUT for result in results) == 16
    assert sum(isinstance(result, LinkError) and result.code == envelope.BUSY for result in results) == 48
    assert not link._waiting
    assert link.ws.active_sends == 0
    assert not link._send_lock.locked()
    assert link.ws.messages.empty()  # Lock waiters timed out without reaching the transport.
    resolve(link, first, error={"code": "internal", "msg": "late"})
    await assert_reusable(link)


async def test_send_lock_wait_is_inside_request_deadline(request_link):
    link = request_link
    await link._send_lock.acquire()
    try:
        async with asyncio.timeout(1):
            with pytest.raises(LinkError) as error:
                await link.request("device.ping", timeout=0.01)
        assert error.value.code == envelope.TIMEOUT
        assert not link._waiting
        assert link.ws.messages.empty()
    finally:
        link._send_lock.release()
    await assert_reusable(link)


async def test_send_and_reply_share_one_deadline(request_link):
    link = request_link
    link.ws.writable.clear()
    loop = asyncio.get_running_loop()
    started = loop.time()
    call = asyncio.create_task(link.request("device.ping", timeout=0.15))
    message = await next_sent(link)
    await asyncio.sleep(0.09)
    link.ws.writable.set()
    async with asyncio.timeout(1):
        with pytest.raises(LinkError) as error:
            await call
    elapsed = loop.time() - started
    assert error.value.code == envelope.TIMEOUT
    # A second timeout after send would end at ~240 ms; tolerate scheduler noise.
    assert 0.12 <= elapsed < 0.22
    assert not link._waiting
    resolve(link, message, result={"too_late": True})
    await assert_reusable(link)


@pytest.mark.parametrize("timeout", [0, -0.1])
async def test_nonpositive_deadline_does_not_send(request_link, timeout):
    link = request_link
    with pytest.raises(LinkError) as error:
        await link.request("device.reboot", timeout=timeout)
    assert error.value.code == envelope.TIMEOUT
    assert not link._waiting
    assert link.ws.messages.empty()


async def test_none_uses_configured_deadline(request_link):
    link = request_link
    link.ws.writable.clear()
    async with asyncio.timeout(1):
        with pytest.raises(LinkError) as error:
            await link.request("device.ping")
    assert error.value.code == envelope.TIMEOUT
    assert not link._waiting
    assert not link._send_lock.locked()


async def test_direct_response_send_also_has_a_deadline(request_link):
    link = request_link
    link.ws.writable.clear()
    call = asyncio.create_task(link._send(envelope.success(1, {})))
    await next_sent(link)
    async with asyncio.timeout(1):
        with pytest.raises(ConnectionError, match="stalled"):
            await call
    assert not link._send_lock.locked()
    assert link.ws.active_sends == 0
    await assert_reusable(link)


@pytest.mark.parametrize("stage", ["lock", "send", "response"])
async def test_cancellation_releases_request_and_lock(request_link, stage):
    link = request_link
    if stage == "lock":
        await link._send_lock.acquire()
    elif stage == "send":
        link.ws.writable.clear()
    call = asyncio.create_task(link.request("device.ping", timeout=5))
    if stage == "lock":
        await asyncio.sleep(0)
    else:
        await next_sent(link)
    future = next(iter(link._waiting.values()))
    call.cancel()
    with pytest.raises(asyncio.CancelledError):
        await call
    assert future.cancelled()
    assert not link._waiting
    assert link.ws.active_sends == 0
    if stage == "lock":
        link._send_lock.release()
    assert not link._send_lock.locked()
    await assert_reusable(link)


@pytest.mark.parametrize("failure", [ConnectionError("broken"), TimeoutError("transport timeout")])
async def test_send_failure_preserves_timeout_error_and_cleanup(request_link, failure):
    link = request_link
    link.ws.failure = failure
    with pytest.raises(LinkError) as error:
        await link.request("device.ping")
    assert error.value.code == envelope.TIMEOUT
    assert not link._waiting
    assert not link._send_lock.locked()
    await next_sent(link)
    await assert_reusable(link)


async def test_device_errors_and_out_of_order_replies_are_preserved(request_link):
    link = request_link
    calls = [asyncio.create_task(link.request("device.ping", {"index": i}, timeout=1)) for i in range(16)]
    messages = [await next_sent(link) for _ in calls]
    for message in reversed(messages):
        index = message["p"]["index"]
        if index == 7:
            resolve(link, message, error={"code": envelope.UNKNOWN_METHOD, "msg": "unsupported"})
        else:
            resolve(link, message, result={"index": index})
    results = await asyncio.gather(*calls, return_exceptions=True)
    for index, result in enumerate(results):
        if index == 7:
            assert isinstance(result, LinkError)
            assert (result.code, result.msg) == (envelope.UNKNOWN_METHOD, "unsupported")
        else:
            assert result == {"index": index}
    assert not link._waiting
    await assert_reusable(link)


async def test_request_cap_applies_while_waiting_for_responses(request_link):
    link = request_link
    calls = [asyncio.create_task(link.request("device.ping", timeout=1)) for _ in range(16)]
    messages = [await next_sent(link) for _ in calls]
    with pytest.raises(LinkError) as error:
        await link.request("device.reboot")
    assert error.value.code == envelope.BUSY
    assert len(link._waiting) == 16 and link.ws.messages.empty()
    for call in calls:
        call.cancel()
    await asyncio.gather(*calls, return_exceptions=True)
    for message in messages:
        resolve(link, message, error={"code": "internal", "msg": "late"})
    await assert_reusable(link)


@pytest.mark.parametrize("ending", ["cancel", "timeout"])
async def test_error_arriving_during_stalled_send_is_consumed(request_link, ending):
    link = request_link
    loop = asyncio.get_running_loop()
    previous = loop.get_exception_handler()
    unhandled = []
    loop.set_exception_handler(lambda loop, context: unhandled.append(context))
    try:
        link.ws.writable.clear()
        call = asyncio.create_task(link.request("device.ping", timeout=0.04))
        message = await next_sent(link)
        # Covers both _resolve's device error and run's disconnect exception.
        resolve(link, message, error={"code": envelope.TIMEOUT, "msg": "the device disconnected"})
        if ending == "cancel":
            call.cancel()
            with pytest.raises(asyncio.CancelledError):
                await call
        else:
            async with asyncio.timeout(1):
                with pytest.raises(LinkError) as error:
                    await call
            assert error.value.code == envelope.TIMEOUT
        del call
        gc.collect()
        await asyncio.sleep(0)
        assert not unhandled
        assert not link._waiting
    finally:
        loop.set_exception_handler(previous)


async def test_offline_error_still_precedes_admission(request_link):
    request_link.state = "closing"
    with pytest.raises(LinkError) as error:
        await request_link.request("device.ping")
    assert error.value.code == envelope.NOT_FOUND
    assert not request_link._waiting
    assert request_link.ws.messages.empty()


async def test_tls_reconnect_after_cancelled_backpressured_request(hub, monkeypatch):
    device = await paired_device(hub)
    twin = FakeDevice(hub)
    try:
        old = hub.online[device.device_id]
        started = asyncio.Event()

        async def stalled_send(text):
            started.set()
            await asyncio.Event().wait()

        monkeypatch.setattr(old.ws, "send_str", stalled_send)
        call = asyncio.create_task(old.request("device.ping", timeout=5))
        async with asyncio.timeout(1):
            await started.wait()
        call.cancel()
        with pytest.raises(asyncio.CancelledError):
            await call
        assert not old._waiting and not old._send_lock.locked()
        twin.pem, twin.token = device.pem, device.token
        await twin.connect()
        assert (await twin.hello())["ok"]
        assert await device.closed() == 1001
        new = hub.online[device.device_id]
        assert new is not old
        assert await new.request("device.ping") == {"uptime_s": 12}
        assert not old._waiting and not new._waiting
    finally:
        await device.aclose()
        await twin.aclose()


async def test_tls_disconnect_fails_pending_reply_and_next_connection_works(hub, monkeypatch):
    device = await paired_device(hub)
    try:
        old = hub.online[device.device_id]

        async def drop_reply(text):
            return None

        monkeypatch.setattr(device.ws, "send_str", drop_reply)
        call = asyncio.create_task(old.request("device.ping", timeout=5))
        async with asyncio.timeout(1):
            await device.hub_requests.get()
            await device.disconnect()
            with pytest.raises(LinkError) as error:
                await call
        assert (error.value.code, error.value.msg) == (envelope.TIMEOUT, "the device disconnected")
        assert not old._waiting
        await device.connect()
        assert (await device.hello())["ok"]
        new = hub.online[device.device_id]
        assert new is not old
        assert await new.request("device.ping") == {"uptime_s": 12}
        assert not new._waiting
    finally:
        await device.aclose()
