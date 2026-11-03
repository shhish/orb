"""Command line entry points; never changes owner agent or launchd settings."""

import argparse
import asyncio
import sys
from pathlib import Path

from aiohttp import web

from .auth import Tokens, provision_tokens
from .backend import FakeDeviceBackend
from .events import Events
from .server import create_app
from .service import OrbService
from .stdio import run_stdio
from .storage import connect
from .tunnel import register_parser


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="orb-mcp")
    sub = parser.add_subparsers(dest="command", required=True)
    serve = sub.add_parser(
        "serve", help="Run the loopback server with an isolated fake device"
    )
    serve.add_argument("--data-dir", type=Path, required=True)
    serve.add_argument("--port", type=int, default=8445)
    serve.add_argument("--fake-device", action="store_true")
    stdio = sub.add_parser("stdio", help="Forward local or tunnel NDJSON to the hub")
    stdio.add_argument("--url", default="http://127.0.0.1:8445/mcp")
    stdio.add_argument("--token-file", type=Path, required=True)
    register_parser(sub)
    args = parser.parse_args(argv)
    try:
        if hasattr(args, "handler"):
            return args.handler(args)
        if args.command == "stdio":
            return asyncio.run(run_stdio(args.url, args.token_file))
        if not args.fake_device:
            parser.error(
                "serve requires --fake-device; the hub embeds create_app with its DeviceBackend"
            )
        if not 0 <= args.port <= 65535:
            parser.error("port must be between 0 and 65535")
        args.data_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        tokens = Tokens(provision_tokens(args.data_dir / "tokens"))
        conn = connect(args.data_dir / "orb-mcp.sqlite3")
        try:
            events = Events(
                conn,
                authorize=lambda principal, name, arguments: tokens.active(principal),
            )
            service = OrbService(conn, FakeDeviceBackend(), events)
            app = create_app(service, tokens, fake_device=True)
            web.run_app(
                app,
                host="127.0.0.1",
                port=args.port,
                access_log=None,
                print=lambda text: print(text, file=sys.stderr),
                handler_cancellation=True,
            )
        finally:
            conn.close()
        return 0
    except (ValueError, OSError):
        print(
            "orb-mcp: configuration or I/O error; check private files and local service",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
