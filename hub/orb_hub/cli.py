"""orb-hub: runs the hub (serve) and drives it.

Commands that act on devices talk to the running hub over its control socket. devices and unpair also work while
the hub is stopped, straight from the device database.
"""

import argparse
import asyncio
import base64
import binascii
import json
import logging
import logging.handlers
import os
import re
import resource
import signal
import sys
import tempfile
import time
from pathlib import Path

from orb_hub import __version__, control
from orb_hub.config import CHANNELS, Config, ensure_private_dir
from orb_hub.control import ControlError, NotRunning
from orb_hub.devtools import COMMANDS, validate
from orb_hub.firmware import FirmwareError, Releases, is_newer, read_app_info
from orb_hub.provision.session import ProvisioningError
from orb_hub.signing import SigningError

# hub/orb_hub/cli.py → the repository root, which has firmware/.
REPO_ROOT = Path(__file__).resolve().parents[2]
# macOS starts processes with 256 open files; the hub needs one per connection.
FD_LIMIT = 4096
LOG_MAX_BYTES = 5 * 1024 * 1024
LOG_BACKUPS = 3


def _config() -> Config:
    return Config.load()


def _call(cmd: str, **args):
    return control.call(_config().control_socket, cmd, {k: v for k, v in args.items() if v is not None})


def cmd_statusline(args: argparse.Namespace) -> int:
    # Avoid Config.load's Mac lookups on Claude's frequent statusline updates.
    from orb_hub.config import DEFAULT_DATA_DIR
    from orb_hub.statusline import run

    directory = Path(args.data_dir or os.environ.get("ORB_HUB_DATA", DEFAULT_DATA_DIR)).expanduser()
    return run(directory, sys.stdin, sys.stdout, sys.stderr)


# -- serve


def raise_fd_limit(want: int = FD_LIMIT) -> None:
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    target = want if hard == resource.RLIM_INFINITY else min(want, hard)
    if soft != resource.RLIM_INFINITY and soft < target:
        resource.setrlimit(resource.RLIMIT_NOFILE, (target, hard))


def cmd_serve(args: argparse.Namespace) -> int:
    if args.log_file:
        # launchd never rotates the files it points stdout at, and anyone on the LAN can make the hub log.
        log_file = Path(args.log_file).expanduser()
        log_file.parent.mkdir(parents=True, exist_ok=True)
        handler = logging.handlers.RotatingFileHandler(
            log_file, maxBytes=LOG_MAX_BYTES, backupCount=LOG_BACKUPS, encoding="utf-8"
        )
    else:
        handler = logging.StreamHandler(sys.stderr)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        handlers=[handler],
    )
    raise_fd_limit()
    cfg = Config.load(
        bind=args.bind,
        tls_port=args.port,
        pair_port=args.pair_port,
        hub_name=args.name,
        mdns=False if args.no_mdns else None,
        agents_enabled=args.agents,
        enable_codex_queue=args.codex_queue,
        hooks_port=args.hooks_port,
        mcp_enabled=args.dot,
        mcp_port=args.mcp_port,
        home_enabled=args.home,
        speech_enabled=args.speech,
        nowplaying_enabled=args.nowplaying,
    )
    # A stored HomeRelay token means the owner set Home up; don't make them
    # also edit the LaunchAgent's arguments to turn it on.
    if not cfg.home_enabled and (cfg.data_dir / "home" / "homerelay.token").is_file():
        cfg.home_enabled = True
    asyncio.run(_serve(cfg))
    return 0


async def _serve(cfg: Config) -> None:
    from orb_hub.server import Hub

    hub = Hub(cfg)
    await hub.start()
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    loop.add_signal_handler(signal.SIGHUP, hub.request_preferences_refresh)
    try:
        await stop.wait()
    finally:
        loop.remove_signal_handler(signal.SIGHUP)
        logging.getLogger(__name__).info("stopping")
        await hub.stop()


# -- devices


def cmd_pair(args: argparse.Namespace) -> int:
    code = re.sub(r"[\s-]", "", args.code)
    result = _call("pair", code=code)
    print(f"Paired {result['name']} ({result['device_id']}), {result['channel']} channel.")
    return 0


def cmd_unpair(args: argparse.Namespace) -> int:
    try:
        _call("unpair", device_id=args.device_id)
    except NotRunning:
        from orb_hub.store import Store

        cfg = _config()
        if not cfg.db_path.exists():
            raise ControlError(f"no paired device {args.device_id}") from None
        store = Store(cfg.db_path)
        try:
            if not store.unpair(args.device_id):
                raise ControlError(f"no paired device {args.device_id}") from None
        finally:
            store.close()
    print(f"Unpaired {args.device_id}. Its next hello is refused and it starts pairing again.")
    return 0


def _ago(ms: int | None) -> str:
    if not ms:
        return "never seen"
    seconds = max(0, time.time() - ms / 1000)
    for unit, size in (("d", 86400), ("h", 3600), ("m", 60)):
        if seconds >= size:
            return f"seen {int(seconds // size)}{unit} ago"
    return "seen just now"


def cmd_devices(args: argparse.Namespace) -> int:
    running = True
    try:
        result = _call("devices")
    except NotRunning:
        from orb_hub.server import describe
        from orb_hub.store import Store

        running = False
        cfg = _config()
        devices = []
        if cfg.db_path.exists():
            store = Store(cfg.db_path)
            devices = [describe(d, online=False) for d in store.all()]
            store.close()
        result = {"devices": devices, "pending": []}
    if args.json:
        print(json.dumps(result, indent=2))
        return 0

    if not running:
        print("(orb-hub isn't running; showing the last known state)")
    rows = [("DEVICE", "NAME", "CHANNEL", "FIRMWARE", "STATUS", "BATTERY", "RSSI", "IP")]
    for d in result["devices"]:
        battery = "" if d["battery_pct"] is None else f"{d['battery_pct']}%{' +' if d['charging'] else ''}"
        rows.append(
            (
                d["device_id"],
                d["name"],
                d["channel"],
                " ".join(filter(None, (d["fw"], d["build"]))) or "?",
                "online" if d["online"] else _ago(d["last_seen"]),
                battery,
                "" if d["rssi"] is None else f"{d['rssi']} dBm",
                d["ip"] or "",
            )
        )
    if len(rows) == 1:
        print("No paired devices.")
    else:
        widths = [max(len(str(r[i])) for r in rows) for i in range(len(rows[0]))]
        for row in rows:
            print("  ".join(str(v).ljust(w) for v, w in zip(row, widths)).rstrip())
    for p in result["pending"]:
        print(
            f"\nWaiting to pair: {p['name']} ({p['device_id']}) from {p['peer']}, {p['expires_in']} s left."
            "\nConfirm with `orb-hub pair <code>`, using the code on the Orb's screen."
        )
    return 0


def _duration(seconds: float) -> str:
    total = max(0, round(seconds))
    hours, rest = divmod(total, 3600)
    minutes, seconds = divmod(rest, 60)
    if hours:
        return f"{hours}h {minutes:02d}m"
    if minutes:
        return f"{minutes}m"
    return f"{seconds}s"


def _mv(value: int | None) -> str:
    return "? mV" if value is None else f"{value} mV"


def cmd_battery(args: argparse.Namespace) -> int:
    from orb_hub.battery import average_ma, discharge_segments, display_state_shares, iso_time
    from orb_hub.store import Store

    if args.hours <= 0:
        raise ControlError("--hours must be positive")
    if args.capacity_mah is not None and args.capacity_mah <= 0:
        raise ControlError("--capacity-mah must be positive")
    now = int(time.time() * 1000)
    since = now - args.hours * 60 * 60 * 1000
    samples = []
    cfg = _config()
    if cfg.db_path.exists():
        store = Store(cfg.db_path)
        try:
            samples = store.battery_samples(device_id=args.device, since_ms=since, until_ms=now)
        finally:
            store.close()

    grouped = {}
    for sample in samples:
        grouped.setdefault(sample.device_id, []).append(sample)
    devices = []
    for device_id, rows in sorted(grouped.items()):
        segments = discharge_segments(rows, args.capacity_mah)
        devices.append(
            {
                "device_id": device_id,
                "sample_count": len(rows),
                "segments": [
                    {
                        "start": iso_time(segment.start_at),
                        "end": iso_time(segment.end_at),
                        "duration_s": round(segment.duration_s),
                        "start_pct": segment.start_pct,
                        "end_pct": segment.end_pct,
                        "start_mv": segment.start_mv,
                        "end_mv": segment.end_mv,
                        "pct_per_hour": round(segment.pct_per_hour, 3),
                        "hours_per_full_charge": (
                            None if segment.hours_per_full_charge is None else round(segment.hours_per_full_charge, 3)
                        ),
                        "estimated_ma": None if segment.estimated_ma is None else round(segment.estimated_ma, 1),
                        "reliable": segment.reliable,
                        "caveats": list(segment.caveats),
                    }
                    for segment in segments
                ],
                "display_state_shares": {state: round(share, 4) for state, share in display_state_shares(rows).items()},
                "estimated_average_ma": (
                    None if (mean := average_ma(segments, args.capacity_mah)) is None else round(mean, 1)
                ),
            }
        )
    if args.json:
        print(
            json.dumps(
                {
                    "hours": args.hours,
                    "since": iso_time(since),
                    "until": iso_time(now),
                    "capacity_mah": args.capacity_mah,
                    "devices": devices,
                },
                indent=2,
            )
        )
        return 0

    if not devices:
        print(f"No battery samples in the last {args.hours} h.")
        return 0
    print(f"Battery, last {args.hours} h ({iso_time(since)} → {iso_time(now)})")
    rough = False
    for device in devices:
        print(f"\n{device['device_id']}")
        if not device["segments"]:
            print("  No discharge segments (USB or charging throughout).")
        for segment in device["segments"]:
            caveat = f"  [rough: {', '.join(segment['caveats'])}]" if segment["caveats"] else ""
            rough |= bool(segment["caveats"])
            print(
                f"  {segment['start']} → {segment['end']} ({_duration(segment['duration_s'])}): "
                f"{segment['start_pct']}% → {segment['end_pct']}%, "
                f"{_mv(segment['start_mv'])} → {_mv(segment['end_mv'])}"
            )
            details = f"{segment['pct_per_hour']:.2f} %/h"
            if segment["hours_per_full_charge"] is not None:
                details += f", {segment['hours_per_full_charge']:.1f} h per full charge"
            if segment["estimated_ma"] is not None:
                details += f", ~{segment['estimated_ma']:.0f} mA"
            print(f"    {details}{caveat}")
        if device["display_state_shares"]:
            shares = ", ".join(f"{state} {share * 100:.0f}%" for state, share in device["display_state_shares"].items())
            print(f"  Display time: {shares}")
        if device["estimated_average_ma"] is not None:
            print(f"  Estimated average draw ({args.capacity_mah:g} mAh): {device['estimated_average_ma']:.0f} mA")
    if rough:
        print("\nRough segments are under 20 minutes or moved under 2 percent; don't over-trust them.")
    return 0


def cmd_identify(args: argparse.Namespace) -> int:
    _call("identify", device_id=args.device_id)
    print(f"{args.device_id} is showing its name and code for 10 seconds.")
    return 0


def cmd_reboot(args: argparse.Namespace) -> int:
    _call("reboot", device_id=args.device_id)
    print(f"{args.device_id} is rebooting.")
    return 0


def cmd_config(args: argparse.Namespace) -> int:
    result = _call("config", device_id=args.device_id, name=args.name, channel=args.channel, brightness=args.brightness)
    print(f"Saved; {args.device_id} applies it {result['applied']}.")
    return 0


def _print_prefs(device_id: str, prefs: dict, *, as_json: bool) -> None:
    if as_json:
        print(json.dumps(prefs, indent=2))
        return
    sources = ", ".join(f"{source} {'on' if enabled else 'off'}" for source, enabled in prefs["sources"].items())
    print(f"{device_id}: {sources}; recent state changes {prefs['idle_minutes']} min.")
    if units := prefs.get("units"):
        print(f"Temperature: °{units['temperature']} ({units['source']}).")


def cmd_weather_prefs(args: argparse.Namespace) -> int:
    from orb_hub.weather_prefs import parse_latlon

    if args.weather is not None:
        city = args.weather.strip()
        if not city or len(city.encode("utf-8")) > 128 or any(ord(char) < 32 or ord(char) == 127 for char in city):
            raise ControlError("weather city must be 1-128 UTF-8 bytes without control characters")
        result = _call("weather.prefs.set", weather=city)
    elif args.weather_latlon is not None:
        try:
            location = parse_latlon(args.weather_latlon)
        except ValueError as exc:
            raise ControlError(str(exc)) from exc
        result = _call("weather.prefs.set", latlon=[location["latitude"], location["longitude"]])
    else:
        result = _call("weather.prefs.show")
    if args.json:
        print(json.dumps(result, indent=2))
    elif location := result.get("location"):
        print(f"Weather: {location['name']} ({location['latitude']:.4f}, {location['longitude']:.4f}).")
    else:
        print('Weather is off. Set a location with: orb-hub prefs --weather "<city>"')
    return 0


def cmd_prefs_show(args: argparse.Namespace) -> int:
    prefs = _call("prefs.show", device_id=args.device_id)
    _print_prefs(args.device_id, prefs, as_json=args.json)
    return 0


def cmd_prefs_set(args: argparse.Namespace) -> int:
    sources = {
        source: value == "on" for source in ("claude", "codex", "dot") if (value := getattr(args, source)) is not None
    }
    patch = {"sources": sources} if sources else {}
    if args.idle_minutes is not None:
        if not 0 <= args.idle_minutes <= 1440:
            raise ControlError("idle minutes must be 0-1440")
        patch["idle_minutes"] = args.idle_minutes
    if args.temperature is not None:
        patch["units"] = {"temperature": args.temperature}
    if not patch:
        raise ControlError("nothing to change: give --claude, --codex, --dot, --idle-minutes or --temperature")
    prefs = _call("prefs.set", device_id=args.device_id, prefs=patch)
    _print_prefs(args.device_id, prefs, as_json=args.json)
    return 0


# -- firmware


def cmd_release(args: argparse.Namespace) -> int:
    from orb_hub import signing

    image = Path(args.bin).read_bytes()
    key_pem = signing.load_signing_key()
    cfg = _config()
    ensure_private_dir(cfg.data_dir)
    ensure_private_dir(cfg.firmware_dir)
    releases = Releases(cfg.firmware_dir)
    previous = releases.current(args.channel, assets=args.assets)
    manifest = releases.publish(
        image,
        args.channel,
        args.notes or "",
        lambda data: signing.ensure_signed(data, key_pem, cfg.firmware_dir),
        assets=args.assets,
    )
    print(
        f"Released {manifest['project']} {manifest['version']} (build {manifest['build'] or '?'}, "
        f"built {manifest['built']}) on {args.channel}: {manifest['size']} bytes, sha256 {manifest['sha256']}"
    )
    if previous and previous["sha256"] != manifest["sha256"] and not is_newer(manifest, previous["version"]):
        if previous["version"] == manifest["version"]:
            warning = f"Warning: the channel's previous release was also {manifest['version']}."
        else:
            warning = f"Warning: {manifest['version']} does not supersede the previous release {previous['version']}."
        requirement = "higher assets content version" if args.assets else "higher numeric major.minor.patch"
        print(
            f"{warning} Automatic offers require a {requirement}; build IDs do not establish precedence. "
            "The artifact was published, but replacing an equal/newer build or rolling back requires explicit recovery."
        )
    try:
        notified = _call("release.notify", channel=args.channel, **({"kind": "assets"} if args.assets else {}))[
            "notified"
        ]
    except NotRunning:
        print("orb-hub isn't running; eligible devices get the offer when they next connect.")
    else:
        print(f"Offered to {', '.join(notified)}." if notified else "No online device on that channel needs it.")
    return 0


def cmd_sign(args: argparse.Namespace) -> int:
    from orb_hub import signing

    source = Path(args.bin)
    output = Path(args.output) if args.output else source.with_name(f"{source.stem}-signed{source.suffix}")
    image = source.read_bytes()
    info = read_app_info(image)
    signed = signing.ensure_signed(image, signing.load_signing_key(), output.parent)
    output.write_bytes(signed)
    print(f"Signed {info.project} {info.version}: {output}")
    return 0


def cmd_keys_init(args: argparse.Namespace) -> int:
    from orb_hub import signing

    if args.pub_out:
        pub_out = Path(args.pub_out)
    elif (REPO_ROOT / "firmware").is_dir():
        pub_out = REPO_ROOT / "firmware" / "keys" / "orb_signing_pub.pem"
    else:
        raise SigningError("can't find the repository's firmware/ directory; give --pub-out")
    if signing.keys_init(pub_out):
        print(f"Created the signing key in the login Keychain ({signing.KEYCHAIN_SERVICE}).")
    else:
        print(f"The login Keychain already has the signing key ({signing.KEYCHAIN_SERVICE}); kept it.")
    print(f"Public key: {pub_out}")
    return 0


# -- Wi-Fi


def cmd_setup(args: argparse.Namespace) -> int:
    from orb_hub.provision.client import run_setup

    name, status = run_setup(name=args.name, ssid=args.ssid)
    if status.state == "connected":
        print(f"{name} joined the network at {status.ip}. It now looks for this hub and shows a pairing code:")
        print("confirm it with `orb-hub pair <code>`.")
        return 0
    if status.state == "failed":
        raise ProvisioningError(f"{name} couldn't join: {status.reason or 'unknown reason'}")
    raise ProvisioningError(f"{name} hasn't joined the network yet; check its screen")


def cmd_agents_status(args):
    print(json.dumps(_call("agents.status"), indent=2))
    return 0


def cmd_hooks_snippet(args):
    from orb_hub.agents import hooks_snippet

    print(json.dumps(hooks_snippet(args.port), indent=2))
    return 0


def cmd_agents_start(args):
    result = _call(
        "agents.start", agent=args.agent, cwd=str(Path(args.cwd).resolve()), prompt=args.prompt, model=args.model
    )
    print(json.dumps(result, indent=2))
    return 0


def cmd_demo_cards(args):
    print(json.dumps(_call("demo-cards", device_id=args.device_id), indent=2))
    return 0


def cmd_dot(args):
    if args.args == ["status"]:
        print(json.dumps(_call("dot.status"), indent=2))
        return 0
    from orb_mcp.cli import main as mcp_main

    return mcp_main(args.args)


def cmd_home(args):
    params = {"device_id": args.device_id} if hasattr(args, "device_id") else {}
    if hasattr(args, "id"):
        params["id"] = args.id
    print(json.dumps(_call(args.home_command, **params), indent=2))
    return 0


def cmd_dev(args: argparse.Namespace) -> int:
    values = [getattr(args, f"a{i}") for i in range(len(COMMANDS[args.dev_command]))]
    validate(args.dev_command, values)
    result = control.call(
        _config().control_socket,
        "dev",
        {"device_id": args.device_id, "command": args.dev_command, "args": values},
        timeout=95,
    )
    if args.dev_command != "shot":
        print(result["text"], end="")
        return 0
    try:
        png = base64.b64decode(result["png"], validate=True)
    except (KeyError, TypeError, binascii.Error):
        raise ControlError("the hub returned an invalid screenshot") from None
    if len(png) > 2 * 1024 * 1024 or not png.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ControlError("the hub returned an invalid screenshot")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.", delete=False) as file:
            temporary = Path(file.name)
            file.write(png)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(output)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="orb-hub", description="The Mac mini hub for Orb devices.")
    parser.add_argument("--version", action="version", version=f"orb-hub {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("statusline", help="read Claude statusline JSON from stdin and cache private usage")
    p.add_argument("--data-dir", help="hub data directory (default: ORB_HUB_DATA or ~/.local/share/orb-hub)")
    p.set_defaults(func=cmd_statusline)

    p = sub.add_parser("serve", help="run the hub")
    p.add_argument("--bind", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    p.add_argument("--port", type=int, default=None, help="TLS port for the link and firmware (default 8443)")
    p.add_argument("--pair-port", type=int, default=None, help="plain-HTTP certificate port (default 8080)")
    p.add_argument("--name", default=None, help="hub name shown on devices (default: this Mac's name)")
    p.add_argument("--no-mdns", action="store_true", help="don't advertise over mDNS")
    p.add_argument("--agents", action="store_true", help="observe coding agents and serve loopback Claude hooks")
    p.add_argument(
        "--codex-queue", action="store_true", help="allow confirmed messages through codex queue for observed sessions"
    )
    p.add_argument("--hooks-port", type=int, default=8447, help="loopback hook port (default 8447)")
    p.add_argument("--dot", action="store_true", help="serve the production MCP backend on loopback")
    p.add_argument("--mcp-port", type=int, default=8445, help="loopback MCP port (default 8445)")
    p.add_argument(
        "--home",
        action="store_true",
        help="bridge HomeRelay through the paired device link (on automatically when home/homerelay.token exists)",
    )
    p.add_argument("--speech", action="store_true", help="synthesise spoken replies via macOS 'say'")
    p.add_argument(
        "--nowplaying",
        action="store_true",
        help="observe media playback and output volume via AppleScript / nowplaying-cli",
    )
    p.add_argument("--log-file", help="log to this file, rotated at 5 MB with 3 old ones kept (default: stderr)")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(func=cmd_serve)

    p = sub.add_parser("pair", help="confirm a pairing request with the code on the Orb's screen")
    p.add_argument("code")
    p.set_defaults(func=cmd_pair)

    p = sub.add_parser("unpair", help="forget a device")
    p.add_argument("device_id")
    p.set_defaults(func=cmd_unpair)

    p = sub.add_parser("devices", help="list paired devices and pending pairing requests")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_devices)

    p = sub.add_parser("battery", help="show measured battery drain from logged state reports")
    p.add_argument("--device", metavar="ID", help="limit to one paired device")
    p.add_argument("--hours", type=int, default=24, help="look back this many hours (default 24)")
    p.add_argument("--capacity-mah", type=float, metavar="N", help="battery capacity for an average mA estimate")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_battery)

    for name, func, text in (
        ("identify", cmd_identify, "make a device show its name"),
        ("reboot", cmd_reboot, "reboot a device"),
    ):
        p = sub.add_parser(name, help=text)
        p.add_argument("device_id")
        p.set_defaults(func=func)

    p = sub.add_parser("dev", help="developer commands through the paired TLS link (dev channel only)")
    p.add_argument("device_id")
    developer = p.add_subparsers(dest="dev_command", required=True)
    for command, specification in COMMANDS.items():
        p = developer.add_parser(command)
        if command == "shot":
            p.add_argument("output", help="destination PNG; replaced only after CRC-verified completion")
        for index, permitted in enumerate(specification):
            if isinstance(permitted[0], int):
                p.add_argument(f"a{index}", type=int)
            elif isinstance(permitted[0], float):
                p.add_argument(f"a{index}", type=float)
            else:
                p.add_argument(f"a{index}", choices=permitted)
        p.set_defaults(func=cmd_dev)

    p = sub.add_parser("config", help="change a device's name, channel or brightness")
    p.add_argument("device_id")
    p.add_argument("--name")
    p.add_argument("--channel", choices=CHANNELS)
    p.add_argument("--brightness", type=int, metavar="0-100")
    p.set_defaults(func=cmd_config)

    p = sub.add_parser("prefs", help="set the weather location or show/change per-device preferences")
    weather = p.add_mutually_exclusive_group()
    weather.add_argument("--weather", metavar="CITY|off", help='save a city, or disable weather with "off"')
    weather.add_argument(
        "--weather-latlon",
        metavar="LAT,LON",
        help="save explicit latitude and longitude, including negative coordinates",
    )
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_weather_prefs)
    prefs = p.add_subparsers()
    p = prefs.add_parser("show", help="show saved session preferences")
    p.add_argument("device_id")
    p.add_argument("--json", action="store_true", default=argparse.SUPPRESS)
    p.set_defaults(func=cmd_prefs_show)
    p = prefs.add_parser("set", help="save a partial preference update, including while the device is offline")
    p.add_argument("device_id")
    for source in ("claude", "codex", "dot"):
        p.add_argument(f"--{source}", choices=("on", "off"))
    p.add_argument("--idle-minutes", type=int, metavar="0-1440")
    p.add_argument("--temperature", choices=("auto", "F", "C"), help="temperature units for this device")
    p.add_argument("--json", action="store_true", default=argparse.SUPPRESS)
    p.set_defaults(func=cmd_prefs_set)

    home = sub.add_parser("home", help="HomeRelay status and per-device Home favourites").add_subparsers(required=True)
    p = home.add_parser("status", help="show Home bridge connectivity without credentials")
    p.set_defaults(func=cmd_home, home_command="home.status")
    favourites = home.add_parser("favourites", help="manage a paired device's favourites").add_subparsers(required=True)
    for action in ("list", "add", "remove"):
        p = favourites.add_parser(action, help=f"{action} saved Home favourites")
        p.add_argument("device_id", help="paired Orb device ID; the device may be offline")
        if action != "list":
            p.add_argument("id", help="short accessory or scene alias from Home status/favourites")
        p.set_defaults(func=cmd_home, home_command=f"home.favourites.{action}")

    p = sub.add_parser("release", help="sign and publish a firmware image to a channel")
    p.add_argument("bin", help="the app or versioned assets image")
    p.add_argument("--assets", action="store_true", help="publish a separately signed assets image")
    p.add_argument("--channel", required=True, choices=CHANNELS)
    p.add_argument("--notes", default="")
    p.set_defaults(func=cmd_release)

    p = sub.add_parser("sign", help="sign an app image for flashing over USB")
    p.add_argument("bin")
    p.add_argument("-o", "--output", help="default: <name>-signed.bin next to the input")
    p.set_defaults(func=cmd_sign)

    keys = sub.add_parser("keys", help="manage the firmware signing key").add_subparsers(dest="keys", required=True)
    p = keys.add_parser("init", help="create the signing key in the login Keychain and write its public key")
    p.add_argument("--pub-out", help="default: firmware/keys/orb_signing_pub.pem in this repository")
    p.set_defaults(func=cmd_keys_init)

    p = sub.add_parser("setup", help="give an unprovisioned Orb your Wi-Fi over Bluetooth")
    p.add_argument("--name", help="the Orb's Bluetooth name (Orb-XXXX), if several are nearby")
    p.add_argument("--ssid", help="the network; asked for if not given")
    p.set_defaults(func=cmd_setup)

    agents = sub.add_parser("agents", help="coding-agent discovery and owner-installed hooks").add_subparsers(
        required=True
    )
    p = agents.add_parser("status", help="show sessions, provider health and voice readiness")
    p.set_defaults(func=cmd_agents_status)
    p = agents.add_parser("hooks-snippet", help="print owner-merge JSON with a private-token placeholder")
    p.add_argument("--port", type=int, default=8447)
    p.set_defaults(func=cmd_hooks_snippet)
    p = agents.add_parser("start", help="start a new controllable hub-owned session")
    p.add_argument("agent", choices=["claude", "codex"])
    p.add_argument("--cwd", required=True)
    p.add_argument("--prompt", required=True)
    p.add_argument("--model")
    p.set_defaults(func=cmd_agents_start)
    p = sub.add_parser("demo-cards", help="push bounded sample sessions/cards to a paired v1.1 device")
    p.add_argument("device_id")
    p.set_defaults(func=cmd_demo_cards)
    p = sub.add_parser("dot", help="Dot status, stdio bridge and tunnel artifact commands")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(func=cmd_dot)
    return parser


def main(argv: list[str] | None = None) -> int:
    from orb_hub.weather_prefs import parse_latlon

    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments[:1] == ["prefs"]:
        index = 1
        while index + 1 < len(arguments):
            if arguments[index] == "--weather-latlon":
                try:
                    parse_latlon(arguments[index + 1])
                except ValueError:
                    pass
                else:
                    # argparse mistakes a negative coordinate pair for an option.
                    # Join only a validated value so actual options keep their meaning.
                    arguments[index : index + 2] = [f"--weather-latlon={arguments[index + 1]}"]
            index += 1
    args = build_parser().parse_args(arguments)
    try:
        if args.func is not cmd_weather_prefs and (
            getattr(args, "weather", None) is not None or getattr(args, "weather_latlon", None) is not None
        ):
            raise ControlError("weather flags belong directly after prefs, without a show/set subcommand")
        return args.func(args)
    except ImportError:
        print("orb-hub: install the local orb-agents/orb-mcp package for this integration", file=sys.stderr)
        return 1
    except (ControlError, SigningError, FirmwareError, ProvisioningError, OSError) as exc:
        print(f"orb-hub: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
