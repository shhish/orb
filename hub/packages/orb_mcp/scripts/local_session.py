"""Isolated scripted integration session; requires the package's [dev] extras.

Owner: orb-mcp tests. Lifetime: one invocation; all state is temporary and all
listeners/processes stop in finally blocks. No scheduler or real config used.
The child invokes the real serve --fake-device CLI with an injected transport
that maps one test HTTPS identity to our local receiver. Production CLI exposes
no callback safety bypass. No tunnel or OpenAI credentials are involved.
"""

import argparse
import asyncio
import base64
import json
import secrets
import socket
import sys
import tempfile
from pathlib import Path

import aiohttp
from aiohttp import web
from standardwebhooks.webhooks import Webhook

from orb_mcp.auth import read_token
from orb_mcp.events import Events, WebhookResponse
from orb_mcp.protocol import CAPABILITIES_KEY, MODERN_VERSION, VERSION_KEY

CALLBACK_IDENTITY = "https://orb-test.invalid/callback"


def child(args):
    from orb_mcp import cli

    async def local_test_transport(url, body, headers):
        if url != CALLBACK_IDENTITY:
            raise ValueError("Unexpected test callback identity")
        async with aiohttp.ClientSession(trust_env=False) as session:
            async with session.post(
                f"http://127.0.0.1:{args.callback_port}/callback",
                data=body,
                headers=headers,
                allow_redirects=False,
            ) as response:
                return WebhookResponse(response.status, await response.read())

    def events(conn, **kwargs):
        return Events(conn, transport=local_test_transport, **kwargs)

    cli.Events = events
    return cli.main(
        [
            "serve",
            "--fake-device",
            "--data-dir",
            args.data_dir,
            "--port",
            str(args.port),
        ]
    )


async def stop_process(process):
    if process is None or process.returncode is not None:
        return
    process.terminate()
    try:
        await asyncio.wait_for(process.communicate(), 5)
    except TimeoutError:
        process.kill()
        await process.communicate()


async def run_session() -> dict:
    secret = "whsec_" + base64.b64encode(secrets.token_bytes(32)).decode()
    signer = Webhook(secret)
    received = asyncio.Queue()
    verifications = []

    async def callback(request):
        body = await request.read()
        payload = signer.verify(body, dict(request.headers))
        if payload.get("type") == "verification":
            verifications.append(True)
            return web.json_response({"challenge": payload["challenge"]})
        assert request.headers["webhook-id"] == payload["eventId"]
        assert request.headers["X-MCP-Subscription-Id"].startswith("sub_")
        await received.put(payload)
        return web.Response(status=204)

    app = web.Application()
    app.router.add_post("/callback", callback)
    runner = web.AppRunner(app, access_log=None)
    server = adapter = None
    await runner.setup()
    try:
        with socket.socket() as webhook_socket:
            webhook_socket.bind(("127.0.0.1", 0))
            webhook_socket.listen()
            webhook_socket.setblocking(False)
            callback_port = webhook_socket.getsockname()[1]
            # The runner owns this duplicate until cleanup, beyond the with block.
            site = web.SockSite(runner, webhook_socket.dup())
            await site.start()
            with tempfile.TemporaryDirectory(prefix="orb-mcp-session-") as directory:
                data_dir = Path(directory) / "state"
                # Only the CLI owns its listener; reserve an available port briefly.
                with socket.socket() as candidate:
                    candidate.bind(("127.0.0.1", 0))
                    port = candidate.getsockname()[1]
                server = await asyncio.create_subprocess_exec(
                    sys.executable,
                    str(Path(__file__).resolve()),
                    "--child",
                    "--data-dir",
                    str(data_dir),
                    "--port",
                    str(port),
                    "--callback-port",
                    str(callback_port),
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE,
                )
                token_file = data_dir / "tokens" / "dot.token"
                async with aiohttp.ClientSession(trust_env=False) as session:
                    async with asyncio.timeout(10):
                        while True:
                            if server.returncode is not None:
                                raise RuntimeError(
                                    "Fake server exited before readiness"
                                )
                            try:
                                token = read_token(token_file)
                                async with session.get(
                                    f"http://127.0.0.1:{port}/health",
                                    headers={"Authorization": "Bearer " + token},
                                ) as response:
                                    if response.status == 200:
                                        break
                            except (OSError, aiohttp.ClientError):
                                pass
                            await asyncio.sleep(0.03)
                    adapter = await asyncio.create_subprocess_exec(
                        sys.executable,
                        "-m",
                        "orb_mcp",
                        "stdio",
                        "--url",
                        f"http://127.0.0.1:{port}/mcp",
                        "--token-file",
                        str(token_file),
                        stdin=asyncio.subprocess.PIPE,
                        stdout=asyncio.subprocess.PIPE,
                        stderr=asyncio.subprocess.PIPE,
                    )
                    request_id = 0

                    async def call(method, params=None):
                        nonlocal request_id
                        request_id += 1
                        message = {
                            "jsonrpc": "2.0",
                            "id": request_id,
                            "method": method,
                            "params": {
                                **(params or {}),
                                "_meta": {
                                    VERSION_KEY: MODERN_VERSION,
                                    CAPABILITIES_KEY: {},
                                },
                            },
                        }
                        adapter.stdin.write(json.dumps(message).encode() + b"\n")
                        await adapter.stdin.drain()
                        response = json.loads(
                            await asyncio.wait_for(adapter.stdout.readline(), 10)
                        )
                        assert response["id"] == request_id and "error" not in response
                        return response["result"]

                    discover = await call("server/discover")
                    assert "events" in discover["capabilities"]
                    await call(
                        "events/subscribe",
                        {
                            "name": "card.answered",
                            "arguments": {"card_id": "session-question"},
                            "delivery": {
                                "mode": "webhook",
                                "url": CALLBACK_IDENTITY,
                                "secret": secret,
                            },
                        },
                    )
                    shown = await call(
                        "tools/call",
                        {
                            "name": "orb_show",
                            "arguments": {
                                "card_id": "session-greeting",
                                "text": "Hello from the isolated local session",
                            },
                        },
                    )
                    assert shown["structuredContent"]["status"] == "showing"
                    asked = await call(
                        "tools/call",
                        {
                            "name": "orb_ask",
                            "arguments": {
                                "card_id": "session-question",
                                "text": "Continue this test?",
                                "options": [
                                    {"id": "yes", "label": "Continue"},
                                    {"id": "no", "label": "Stop"},
                                ],
                            },
                        },
                    )
                    assert asked["structuredContent"]["status"] == "showing"
                    async with session.post(
                        f"http://127.0.0.1:{port}/__fake/answer",
                        json={"card_id": "session-question", "option_id": "yes"},
                        headers={"Authorization": "Bearer " + token},
                    ) as response:
                        assert response.status == 200
                    event = await asyncio.wait_for(received.get(), 5)
                    assert event["name"] == "card.answered"
                    assert event["data"]["option_id"] == "yes"
                    card = await call(
                        "tools/call",
                        {
                            "name": "orb_get_card",
                            "arguments": {"card_id": "session-question"},
                        },
                    )
                    assert card["structuredContent"]["status"] == "answered"
                    adapter.stdin.close()
                    await asyncio.wait_for(adapter.wait(), 5)
                    assert adapter.returncode == 0
                    result = {
                        "discover": True,
                        "cards_shown": 2,
                        "callback_verified": len(verifications) == 1,
                        "signed_events_received": 1,
                        "answer_persisted": True,
                    }
                await stop_process(server)
                server = None
            return result
    finally:
        await stop_process(adapter)
        await stop_process(server)
        await runner.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--data-dir")
    parser.add_argument("--port", type=int)
    parser.add_argument("--callback-port", type=int)
    args = parser.parse_args()
    if args.child:
        return child(args)
    print(json.dumps(asyncio.run(run_session()), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
