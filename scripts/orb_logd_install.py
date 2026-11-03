"""Install/uninstall only Orb's serial agent, explicitly on the board's Mac."""

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LABEL = "com.orb.logd"


def render_template(template: bytes, replacements: dict[str, str]) -> bytes:
    def replace(value):
        if isinstance(value, str):
            for key, item in replacements.items():
                value = value.replace(f"@{key}@", item)
            return value
        if isinstance(value, list):
            return [replace(item) for item in value]
        if isinstance(value, dict):
            return {key: replace(item) for key, item in value.items()}
        return value

    return plistlib.dumps(replace(plistlib.loads(template)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "uninstall"])
    parser.add_argument(
        "--load", action="store_true", help="also load the new agent (install only)"
    )
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--prefix", type=Path, default=Path.home() / ".local/share/orb-logd")
    parser.add_argument("--agents-dir", type=Path, default=Path.home() / "Library/LaunchAgents")
    parser.add_argument("--log-dir", type=Path, default=Path.home() / "Library/Logs/orb-logd")
    parser.add_argument("--socket", type=Path, help="override prefix/orb.sock")
    args = parser.parse_args(argv)
    prefix = args.prefix.expanduser().resolve()
    agent = args.agents_dir.expanduser().resolve() / f"{LABEL}.plist"
    socket_path = (args.socket or prefix / "orb.sock").expanduser().resolve()
    if len(os.fsencode(socket_path)) > 103:
        parser.error("Unix socket path must be at most 103 bytes on macOS")
    domain = f"gui/{os.getuid()}"
    target = f"{domain}/{LABEL}"
    if args.action == "uninstall":
        if args.load:
            parser.error("--load is only valid with install")
        if agent.exists():
            result = subprocess.run(
                ["launchctl", "bootout", target], check=False, capture_output=True
            )
            if result.returncode:
                loaded = subprocess.run(
                    ["launchctl", "print", target], check=False, capture_output=True
                )
                if loaded.returncode == 0:
                    parser.error(
                        "could not stop loaded Orb agent; preserving its plist and runtime"
                    )
            agent.unlink()
        # Avoid recursively deleting an arbitrary user-provided prefix. Only the
        # dedicated venv is ours; historical logs and sockets are not data to purge.
        venv = prefix / "venv"
        if (venv / ".orb-logd-owned").is_file():
            shutil.rmtree(venv)
        print("Orb agent removed. Logs and incident files were preserved.")
        return 0
    if agent.exists():
        parser.error("agent already exists; uninstall first (logs are preserved)")
    prefix.mkdir(parents=True, exist_ok=True, mode=0o700)
    args.log_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    venv = prefix / "venv"
    if venv.exists() and not (venv / ".orb-logd-owned").is_file():
        parser.error("refusing to replace an unowned venv")
    subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True)
    (venv / ".orb-logd-owned").touch(mode=0o600)
    python = venv / "bin/python"
    subprocess.run([str(python), "-m", "pip", "install", "--no-cache-dir", str(ROOT)], check=True)
    rendered = render_template(
        (ROOT / "deploy/com.orb.logd.plist.in").read_bytes(),
        {
            "PYTHON": str(python),
            "SOCKET": str(socket_path),
            "PORT": args.port,
            "LOGDIR": str(args.log_dir.resolve()),
        },
    )
    agent.parent.mkdir(parents=True, exist_ok=True)
    agent.write_bytes(rendered)
    agent.chmod(0o600)
    print(f"Installed {agent}; socket {socket_path}")
    if args.load:
        subprocess.run(["launchctl", "bootstrap", domain, str(agent)], check=True)
        print("Agent loaded; use orb-logd status to verify the serial connection.")
    else:
        print(f"Not loaded. Start with: launchctl bootstrap {domain} '{agent}'")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
