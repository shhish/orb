"""Authenticated, stateless Streamable HTTP, intentionally loopback-only."""

import asyncio
import ipaddress
import json
import logging
import time
from collections import defaultdict, deque

from aiohttp import web

from .auth import Tokens, loopback_host
from .backend import FakeDeviceBackend
from .protocol import Protocol, error_response, http_metadata_error
from .service import OrbService

SERVICE_KEY = web.AppKey("service", OrbService)
PRINCIPAL_KEY = web.RequestKey("principal", str)
MAX_REQUEST = 64 * 1024


def load_json(value: str | bytes):
    def unique(pairs):
        result = {}
        for key, item in pairs:
            if key in result:
                raise ValueError("Duplicate JSON key")
            result[key] = item
        return result

    def invalid(_):
        raise ValueError("Non-finite JSON number")

    return json.loads(value, object_pairs_hook=unique, parse_constant=invalid)


def create_app(
    service: OrbService,
    tokens: Tokens,
    *,
    fake_device: bool = False,
    worker_interval: float = 0.25,
    max_inflight: int = 32,
    requests_per_minute: int = 600,
    monotonic=time.monotonic,
    task_owner=None,
) -> web.Application:
    if max_inflight < 1 or requests_per_minute < 1:
        raise ValueError("Request limits must be positive")
    dispatcher = Protocol(service, service.events)
    inflight = defaultdict(int)
    recent = defaultdict(deque)

    @web.middleware
    async def security(request, handler):
        # Check the boundary even for routes that do not exist.
        guarded = (
            "Host",
            "Authorization",
            "MCP-Protocol-Version",
            "Mcp-Method",
            "Mcp-Name",
        )
        if "Origin" in request.headers or any(
            len(request.headers.getall(key, [])) > 1 for key in guarded
        ):
            return web.json_response({"error": "Forbidden"}, status=403)
        if not loopback_host(request.headers.get("Host", "")):
            return web.json_response({"error": "Forbidden"}, status=403)
        if (
            request.remote is None
            or not ipaddress.ip_address(request.remote).is_loopback
        ):
            return web.json_response({"error": "Forbidden"}, status=403)
        principal = tokens.authenticate(request.headers.get("Authorization", ""))
        if principal is None:
            return web.json_response(
                {"error": "Unauthorized"},
                status=401,
                headers={"WWW-Authenticate": "Bearer"},
            )
        request[PRINCIPAL_KEY] = principal
        if request.path != "/mcp":
            return await handler(request)
        now = monotonic()
        history = recent[principal]
        while history and history[0] <= now - 60:
            history.popleft()
        if inflight[principal] >= max_inflight or len(history) >= requests_per_minute:
            return web.json_response(
                {"error": "Request limit exceeded"},
                status=429,
                headers={"Retry-After": "1"},
            )
        history.append(now)
        inflight[principal] += 1
        try:
            return await handler(request)
        finally:
            inflight[principal] -= 1

    app = web.Application(middlewares=[security], client_max_size=MAX_REQUEST)
    app[SERVICE_KEY] = service

    async def mcp(request):
        if request.content_type != "application/json":
            return web.json_response({"error": "Expected application/json"}, status=415)
        accept = {
            v.strip().split(";")[0]
            for v in request.headers.get("Accept", "").split(",")
        }
        if not {"application/json", "text/event-stream"} <= accept:
            return web.json_response(
                {"error": "Accept must include application/json and text/event-stream"},
                status=406,
            )
        try:
            message = load_json(await request.read())
        except (ValueError, UnicodeError, RecursionError):
            return web.json_response(error_response(-32700, "Parse error"), status=400)
        metadata_error = http_metadata_error(message, request.headers)
        if metadata_error:
            return web.json_response(metadata_error, status=400)
        result = await dispatcher.handle(
            message,
            request[PRINCIPAL_KEY],
            protocol_version=request.headers.get("MCP-Protocol-Version", "2025-03-26"),
        )
        if result is None:
            return web.Response(status=202)
        code = result.get("error", {}).get("code")
        status = (
            404 if code == -32601 else 400 if code in (-32022, -32020, -32600) else 200
        )
        return web.json_response(result, status=status)

    async def health(request):
        return web.json_response({"status": "ok", "fake_device": fake_device})

    app.router.add_post("/mcp", mcp)
    app.router.add_get("/health", health)
    if fake_device:
        if not isinstance(service.backend, FakeDeviceBackend):
            raise ValueError("Fake endpoints require the in-memory backend")

        async def fake(request):
            try:
                args = load_json(await request.read())
                if not isinstance(args, dict):
                    raise TypeError("Expected a JSON object")
                result = await (
                    service.backend.answer(**args)
                    if request.match_info["action"] == "answer"
                    else service.backend.transcribe(**args)
                )
                return web.json_response(result)
            except (ValueError, TypeError, UnicodeError):
                return web.json_response(
                    {"error": "Invalid fake-device input or state"}, status=400
                )

        app.router.add_post("/__fake/{action:answer|transcript}", fake)

    async def worker(operation):
        while True:
            try:
                await operation()
            except Exception as exc:
                if task_owner is not None:
                    raise
                # Never log callback URLs, credentials, payloads, or exception text.
                logging.getLogger(__name__).warning(
                    "Background work failed (%s); retrying", type(exc).__name__
                )
            await asyncio.sleep(worker_interval)

    async def lifecycle(_app):
        if task_owner is None:
            tasks = [
                asyncio.create_task(
                    worker(service.deliver_pending), name="orb-mcp-cards"
                ),
                asyncio.create_task(
                    worker(service.events.deliver_due), name="orb-mcp-outbox"
                ),
            ]
        else:
            tasks = [
                task_owner.run(
                    lambda: worker(service.deliver_pending), name="orb-mcp-cards"
                ),
                task_owner.run(
                    lambda: worker(service.events.deliver_due), name="orb-mcp-outbox"
                ),
            ]
        yield
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    app.cleanup_ctx.append(lifecycle)
    return app
