"""Independent HTTP and real stdio-process boundary checks on loopback only."""

import asyncio
import json
import os
import stat
import sys
from contextlib import asynccontextmanager

import aiohttp
import pytest
from aiohttp import web

from orb_mcp.auth import Tokens, local_url, provision_tokens, read_token
from orb_mcp.backend import FakeDeviceBackend
from orb_mcp.events import Events
from orb_mcp.protocol import (
    CAPABILITIES_KEY,
    MODERN_VERSION,
    VERSION_KEY,
    request_headers,
)
from orb_mcp.server import MAX_REQUEST, create_app, load_json
from orb_mcp.service import OrbService
from orb_mcp.storage import connect


def request(method="ping", *, request_id=1, modern=True, **params):
    if modern:
        params["_meta"] = {VERSION_KEY: MODERN_VERSION, CAPABILITIES_KEY: {}}
    return {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}


@pytest.fixture
async def hub(tmp_path):
    paths = provision_tokens(tmp_path / "tokens")
    tokens = Tokens(paths)
    conn = connect(tmp_path / "state.sqlite3")
    backend = FakeDeviceBackend()
    events = Events(conn)
    service = OrbService(conn, backend, events)
    app = create_app(service, tokens, fake_device=True, worker_interval=0.01)
    runner = web.AppRunner(app, handler_cancellation=True, access_log=None)
    await runner.setup()
    site = web.TCPSite(runner, "127.0.0.1", 0)
    await site.start()
    port = site._server.sockets[0].getsockname()[1]
    async with aiohttp.ClientSession(trust_env=False) as client:
        try:
            yield {
                "url": f"http://127.0.0.1:{port}/mcp",
                "client": client,
                "paths": paths,
                "tokens": tokens,
                "service": service,
                "backend": backend,
                "conn": conn,
            }
        finally:
            await runner.cleanup()
            conn.close()


def headers(hub, message, principal="dot"):
    return {
        "Authorization": "Bearer " + read_token(hub["paths"][principal]),
        "Accept": "application/json, text/event-stream",
        **request_headers(message),
    }


async def post(hub, message, *, principal="dot", overrides=None):
    combined = headers(hub, message, principal)
    combined.update(overrides or {})
    response = await hub["client"].post(hub["url"], json=message, headers=combined)
    return response.status, await response.json()


def test_tokens_are_distinct_private_persistent_and_revocable(tmp_path):
    paths = provision_tokens(tmp_path / "tokens")
    original = {name: read_token(path) for name, path in paths.items()}
    assert set(original) == {"dot", "claude", "codex"}
    assert len(set(original.values())) == 3
    assert all(stat.S_IMODE(path.stat().st_mode) == 0o600 for path in paths.values())
    assert provision_tokens(tmp_path / "tokens") == paths
    assert {name: read_token(path) for name, path in paths.items()} == original
    tokens = Tokens(paths)
    for principal, token in original.items():
        assert tokens.authenticate("Bearer " + token) == principal
    assert tokens.authenticate("Bearer " + "wrong") is None
    assert tokens.authenticate(original["dot"]) is None
    paths["dot"].unlink()
    assert tokens.authenticate("Bearer " + original["dot"]) is None
    assert tokens.active("dot") is False


@pytest.mark.parametrize("mode", [0o644, 0o640, 0o400, 0o666, 0o700])
def test_read_token_requires_exact_private_mode(tmp_path, mode):
    path = tmp_path / "token"
    path.write_text("a" * 64)
    path.chmod(mode)
    with pytest.raises(ValueError, match="0600"):
        read_token(path)


def test_read_token_rejects_symlink_fifo_and_nonowner(tmp_path, monkeypatch):
    path = tmp_path / "token"
    path.write_text("a" * 64)
    path.chmod(0o600)
    link = tmp_path / "link"
    link.symlink_to(path)
    with pytest.raises(OSError):
        read_token(link)
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo, 0o600)
    with pytest.raises(ValueError, match="regular"):
        read_token(fifo)
    owner = os.getuid()
    monkeypatch.setattr(os, "getuid", lambda: owner + 1)
    with pytest.raises(ValueError, match="owned"):
        read_token(path)


def test_tokens_reject_duplicate_client_credentials(tmp_path):
    paths = provision_tokens(tmp_path / "tokens")
    paths["codex"].write_text(read_token(paths["dot"]))
    with pytest.raises(ValueError, match="distinct"):
        Tokens(paths)


@pytest.mark.parametrize(
    "url",
    [
        "https://127.0.0.1:8445/mcp",
        "http://localhost:8445/mcp",
        "http://example.com:8445/mcp",
        "http://0.0.0.0:8445/mcp",
        "http://127.0.0.1:8445/mcp?key=x",
        "http://127.0.0.1:8445/mcp#x",
        "http://key@127.0.0.1:8445/mcp",
        "http://127.0.0.1:8445/elsewhere",
    ],
)
def test_adapter_url_cannot_escape_loopback(url):
    with pytest.raises(ValueError):
        local_url(url)


async def test_http_discover_modern_metadata_and_legacy_initialize(hub):
    status, body = await post(hub, request("server/discover"))
    assert status == 200
    assert body["jsonrpc"] == "2.0" and body["id"] == 1
    assert body["result"]["resultType"] == "complete"
    assert body["result"]["capabilities"] == {"tools": {}, "events": {}}
    assert MODERN_VERSION in body["result"]["supportedVersions"]
    assert len(body["result"]["instructions"]) <= 512
    status, body = await post(
        hub,
        request(
            "initialize",
            modern=False,
            protocolVersion="2025-11-25",
            capabilities={},
            clientInfo={"name": "test", "version": "1"},
        ),
    )
    assert status == 200
    assert body["result"]["protocolVersion"] == "2025-11-25"
    assert "resultType" not in body["result"]


@pytest.mark.parametrize("authorization", [None, "", "Bearer wrong", "Basic ignored"])
async def test_http_auth_missing_and_wrong(hub, authorization):
    message = request()
    values = headers(hub, message)
    if authorization is None:
        values.pop("Authorization")
    else:
        values["Authorization"] = authorization
    response = await hub["client"].post(hub["url"], json=message, headers=values)
    assert response.status == 401
    assert response.headers["WWW-Authenticate"] == "Bearer"
    assert await response.json() == {"error": "Unauthorized"}


@pytest.mark.parametrize(
    "overrides",
    [
        {"Origin": "null"},
        {"Origin": "http://127.0.0.1"},
        {"Origin": ""},
        {"Host": "evil.example"},
        {"Host": "localhost:8445"},
        {"Host": "0.0.0.0:8445"},
        {"Host": "127.0.0.10:8445"},
        {"Host": "127.0.0.1.evil.example"},
    ],
)
async def test_http_rejects_origin_and_nonliteral_loopback_host(hub, overrides):
    status, body = await post(hub, request(), overrides=overrides)
    assert status == 403
    assert body == {"error": "Forbidden"}


@pytest.mark.parametrize(
    "header",
    ["Authorization", "Host", "Mcp-Method", "Mcp-Name", "MCP-Protocol-Version"],
)
async def test_duplicate_security_headers_rejected(hub, header):
    message = request()
    values = list(headers(hub, message).items())
    if header.lower() not in {name.lower() for name, _ in values}:
        values.append((header, "first"))
    values.append((header, "second"))
    response = await hub["client"].post(hub["url"], json=message, headers=values)
    assert response.status == 403


@pytest.mark.parametrize(
    "overrides",
    [
        {"Mcp-Method": "tools/list"},
        {"MCP-Protocol-Version": "2025-11-25"},
        {"Mcp-Name": "orb_cancel"},
    ],
)
async def test_modern_mirrored_headers_must_match_body(hub, overrides):
    status, body = await post(
        hub, request("tools/call", name="orb_status", arguments={}), overrides=overrides
    )
    assert status == 400
    assert body["error"]["code"] == -32020


async def test_modern_required_headers_and_capabilities(hub):
    message = request()
    values = headers(hub, message)
    values.pop("Mcp-Method")
    response = await hub["client"].post(hub["url"], json=message, headers=values)
    assert response.status == 400
    assert (await response.json())["error"]["code"] == -32020
    message["params"]["_meta"].pop(CAPABILITIES_KEY)
    status, body = await post(hub, message)
    assert body["error"]["code"] == -32602


async def test_http_accept_media_type_size_and_method_contract(hub):
    message = request()
    status, _ = await post(hub, message, overrides={"Accept": "application/json"})
    assert status == 406
    response = await hub["client"].post(
        hub["url"], data="{}", headers=headers(hub, message)
    )
    assert response.status == 415
    for method in ("get", "delete"):
        response = await getattr(hub["client"], method)(
            hub["url"], headers=headers(hub, message)
        )
        assert response.status == 405
    response = await hub["client"].post(
        hub["url"],
        data=b" " * (MAX_REQUEST + 1),
        headers={**headers(hub, message), "Content-Type": "application/json"},
    )
    assert response.status == 413


@pytest.mark.parametrize(
    "body", ['{"jsonrpc":"2.0",', '{"x":1,"x":2}', '{"x":NaN}', '{"x":Infinity}']
)
async def test_strict_json_parse_errors(hub, body):
    response = await hub["client"].post(
        hub["url"],
        data=body,
        headers={**headers(hub, request()), "Content-Type": "application/json"},
    )
    assert response.status == 400
    assert (await response.json())["error"]["code"] == -32700


async def test_notifications_have_no_response_and_never_mutate(hub):
    message = request(
        "tools/call",
        name="orb_show",
        arguments={"card_id": "silent", "text": "must not show"},
    )
    del message["id"]
    response = await hub["client"].post(
        hub["url"], json=message, headers=headers(hub, message)
    )
    assert response.status == 202 and await response.read() == b""
    assert hub["conn"].execute("SELECT COUNT(*) FROM cards").fetchone()[0] == 0


async def test_http_clients_cannot_read_or_cancel_each_others_cards(hub):
    status, body = await post(
        hub,
        request(
            "tools/call",
            name="orb_show",
            arguments={"card_id": "private", "text": "hello"},
        ),
    )
    assert status == 200 and not body["result"]["isError"]
    for name in ("orb_get_card", "orb_cancel"):
        _, result = await post(
            hub,
            request("tools/call", name=name, arguments={"card_id": "private"}),
            principal="claude",
        )
        assert result["result"]["isError"]
        assert "hello" not in json.dumps(result)
    assert hub["conn"].execute("SELECT status FROM cards").fetchone()[0] == "showing"


@asynccontextmanager
async def stdio_process(hub):
    process = await asyncio.create_subprocess_exec(
        sys.executable,
        "-m",
        "orb_mcp.cli",
        "stdio",
        "--url",
        hub["url"],
        "--token-file",
        str(hub["paths"]["dot"]),
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        yield process
    finally:
        process.stdin.close()
        try:
            await asyncio.wait_for(process.wait(), 2)
        except TimeoutError:
            process.terminate()
            try:
                await asyncio.wait_for(process.wait(), 2)
            except TimeoutError:
                process.kill()
                await process.wait()
        assert (
            read_token(hub["paths"]["dot"])
            not in (await process.stderr.read()).decode()
        )


async def send(process, message):
    process.stdin.write((json.dumps(message) + "\n").encode())
    await process.stdin.drain()


async def receive(process):
    line = await asyncio.wait_for(process.stdout.readline(), 4)
    assert line, "stdio exited without a response"
    return load_json(line)


async def test_stdio_end_to_end_legacy_then_modern_and_fake_answer(hub):
    async with stdio_process(hub) as process:
        await send(
            process,
            request(
                "initialize",
                modern=False,
                protocolVersion="2025-11-25",
                capabilities={},
                clientInfo={"name": "test", "version": "1"},
            ),
        )
        assert (await receive(process))["result"]["protocolVersion"] == "2025-11-25"
        await send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        await send(process, request("server/discover", request_id=2))
        assert (await receive(process))["result"]["resultType"] == "complete"
        await send(
            process,
            request(
                "tools/call",
                request_id=3,
                name="orb_ask",
                arguments={
                    "card_id": "stdio-card",
                    "text": "Pick one",
                    "options": [{"id": "yes", "label": "Proceed"}],
                },
            ),
        )
        result = await receive(process)
        assert result["result"]["structuredContent"]["status"] == "showing"
        response = await hub["client"].post(
            hub["url"].removesuffix("/mcp") + "/__fake/answer",
            json={"card_id": "stdio-card", "option_id": "yes"},
            headers=headers(hub, request()),
        )
        assert response.status == 200
        await send(
            process,
            request(
                "tools/call",
                request_id=4,
                name="orb_get_card",
                arguments={"card_id": "stdio-card"},
            ),
        )
        result = await receive(process)
        assert result["result"]["structuredContent"]["answer"]["option_id"] == "yes"
        process.stdin.close()
        assert await asyncio.wait_for(process.wait(), 3) == 0


async def test_stdio_wait_does_not_block_other_calls_and_cancel_stops_wait(hub):
    await hub["service"].call_tool(
        "orb_ask",
        {
            "card_id": "waiting",
            "text": "Wait",
            "options": [{"id": "yes", "label": "Yes"}],
        },
        "dot",
    )
    started, cancelled = asyncio.Event(), asyncio.Event()
    original = hub["service"].wait

    async def observed_wait(*args):
        started.set()
        try:
            return await original(*args)
        except asyncio.CancelledError:
            cancelled.set()
            raise

    hub["service"].wait = observed_wait
    async with stdio_process(hub) as process:
        await send(
            process,
            request(
                "tools/call",
                request_id=20,
                name="orb_wait",
                arguments={"card_id": "waiting", "timeout_s": 50},
            ),
        )
        await asyncio.wait_for(started.wait(), 3)
        await send(process, request(request_id=21))
        assert (await receive(process))["id"] == 21
        await send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "notifications/cancelled",
                "params": {"requestId": 20},
            },
        )
        await asyncio.wait_for(cancelled.wait(), 3)
        await send(process, request(request_id=22))
        assert (await receive(process))["id"] == 22
        assert (
            hub["conn"].execute("SELECT status FROM cards").fetchone()[0] == "showing"
        )


async def test_stdio_malformed_input_is_one_envelope_then_recovers(hub):
    async with stdio_process(hub) as process:
        process.stdin.write(b'{"invalid":\n')
        await process.stdin.drain()
        assert (await receive(process))["error"]["code"] == -32700
        await send(process, request(request_id=2))
        assert (await receive(process))["id"] == 2


def test_database_file_and_parent_permissions_and_special_files(tmp_path):
    path = tmp_path / "state.sqlite3"
    conn = connect(path)
    try:
        assert stat.S_IMODE(path.stat().st_mode) == 0o600
        assert conn.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
        assert conn.execute("PRAGMA synchronous").fetchone()[0] == 2
        assert conn.execute("PRAGMA foreign_keys").fetchone()[0] == 1
    finally:
        conn.close()
    path.chmod(0o644)
    with pytest.raises(ValueError, match="0600"):
        connect(path)
    fifo = tmp_path / "fifo"
    os.mkfifo(fifo, 0o600)
    with pytest.raises(ValueError, match="0600"):
        connect(fifo)
    link = tmp_path / "link"
    link.symlink_to(path)
    with pytest.raises(OSError):
        connect(link)
    public = tmp_path / "public"
    public.mkdir(mode=0o755)
    with pytest.raises(ValueError, match="0700"):
        connect(public / "db")


async def test_stdio_capacity_still_accepts_cancel_and_deduplicates_ids(hub):
    await hub["service"].call_tool(
        "orb_show", {"card_id": "capacity", "text": "wait"}, "dot"
    )
    original = hub["service"].wait
    started = asyncio.Event()
    freed = asyncio.Event()
    count = 0

    async def observed_wait(*args):
        nonlocal count
        count += 1
        if count == 32:
            started.set()
        try:
            return await original(*args)
        except asyncio.CancelledError:
            freed.set()
            raise

    hub["service"].wait = observed_wait
    async with stdio_process(hub) as process:
        for number in range(32):
            await send(
                process,
                request(
                    "tools/call",
                    request_id=number,
                    name="orb_wait",
                    arguments={"card_id": "capacity", "timeout_s": 50},
                ),
            )
        await asyncio.wait_for(started.wait(), 3)
        await send(process, request(request_id=0))
        duplicate = await receive(process)
        assert duplicate["id"] == 0 and duplicate["error"]["code"] == -32600
        await send(process, request(request_id=100))
        capacity = await receive(process)
        assert capacity["id"] == 100 and capacity["error"]["code"] == -32000
        await send(process, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        await send(
            process,
            {
                "jsonrpc": "2.0",
                "method": "notifications/cancelled",
                "params": {"requestId": 0},
            },
        )
        await asyncio.wait_for(freed.wait(), 3)
        await send(process, request(request_id=101))
        assert (await receive(process))["id"] == 101
        for number in range(1, 32):
            await send(
                process,
                {
                    "jsonrpc": "2.0",
                    "method": "notifications/cancelled",
                    "params": {"requestId": number},
                },
            )


async def test_background_delivery_retries_sanitized_and_card_worker_is_independent(
    tmp_path, caplog
):
    paths = provision_tokens(tmp_path / "tokens")
    conn = connect(tmp_path / "state.sqlite3")
    events = Events(conn)
    service = OrbService(conn, FakeDeviceBackend(), events)
    cards_progressed = asyncio.Event()
    outbox_started = asyncio.Event()
    block_outbox = asyncio.Event()
    calls = 0

    async def cards():
        nonlocal calls
        calls += 1
        if calls == 1:
            raise RuntimeError("a-secret-value")
        cards_progressed.set()

    async def outbox():
        outbox_started.set()
        await block_outbox.wait()

    service.deliver_pending = cards
    events.deliver_due = outbox
    runner = web.AppRunner(create_app(service, Tokens(paths), worker_interval=0.01))
    try:
        await runner.setup()
        await asyncio.wait_for(outbox_started.wait(), 2)
        await asyncio.wait_for(cards_progressed.wait(), 2)
        assert calls >= 2
        assert "RuntimeError" in caplog.text
        assert "a-secret-value" not in caplog.text
    finally:
        await runner.cleanup()
        conn.close()
