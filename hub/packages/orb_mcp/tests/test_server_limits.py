import asyncio

import aiohttp
import pytest
from aiohttp import web

from orb_mcp.auth import Tokens, provision_tokens, read_token
from orb_mcp.backend import FakeDeviceBackend
from orb_mcp.events import Events
from orb_mcp.protocol import CAPABILITIES_KEY, VERSION_KEY, request_headers
from orb_mcp.server import create_app
from orb_mcp.service import OrbService
from orb_mcp.storage import connect


@pytest.fixture
async def limited(tmp_path):
    paths = provision_tokens(tmp_path / "tokens")
    conn = connect(tmp_path / "state.sqlite3")
    service = OrbService(conn, FakeDeviceBackend(), Events(conn))
    now = [100.0]
    app = create_app(
        service,
        Tokens(paths),
        max_inflight=1,
        requests_per_minute=2,
        monotonic=lambda: now[0],
    )
    runner = web.AppRunner(app, handler_cancellation=True, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    url = f"http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}/mcp"
    async with aiohttp.ClientSession(trust_env=False) as client:

        async def call(principal="dot", method="ping", params=None):
            message = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": method,
                "params": {
                    **(params or {}),
                    "_meta": {VERSION_KEY: "2026-07-28", CAPABILITIES_KEY: {}},
                },
            }
            headers = {
                "Authorization": "Bearer " + read_token(paths[principal]),
                "Accept": "application/json, text/event-stream",
                **request_headers(message),
            }
            async with client.post(url, json=message, headers=headers) as response:
                return response.status, dict(response.headers), await response.json()

        try:
            yield service, now, call
        finally:
            await runner.cleanup()
            conn.close()


async def test_rate_limit_is_per_principal_and_recovers(limited):
    _, now, call = limited
    assert (await call())[0] == 200
    assert (await call())[0] == 200
    result = await call()
    assert result[0] == 429 and result[1]["Retry-After"] == "1"
    assert (await call("claude"))[0] == 200
    now[0] += 60
    assert (await call())[0] == 200


async def test_inflight_limit_releases_after_wait(limited):
    service, _, call = limited
    await service.call_tool(
        "orb_ask",
        {"card_id": "q", "text": "Wait?", "options": [{"id": "yes", "label": "Yes"}]},
        "dot",
    )
    entered = asyncio.Event()
    original = service.wait

    async def waiting(*args):
        entered.set()
        return await original(*args)

    service.wait = waiting
    pending = asyncio.create_task(
        call(
            method="tools/call",
            params={"name": "orb_wait", "arguments": {"card_id": "q", "timeout_s": 1}},
        )
    )
    await asyncio.wait_for(entered.wait(), 1)
    assert (await call())[0] == 429
    await service.backend.answer("q", "yes")
    assert (await pending)[0] == 200
    assert (await call())[0] == 200
