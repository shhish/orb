#!/usr/bin/env python3
"""Render a private hub LaunchAgent for the owner to install; never run launchctl."""

import argparse
import os
import plistlib
import re
import sys
from pathlib import Path
from xml.sax.saxutils import escape


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--logs", type=Path, default=Path.home() / "Library/Logs/orb-hub")
    args = parser.parse_args(argv)
    prefix = os.environ.get("ORB_LAUNCHD_PREFIX", "org.orb")
    if not re.fullmatch(r"[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)*", prefix):
        parser.error("ORB_LAUNCHD_PREFIX must be a reverse-DNS label prefix")
    values = {
        "__LABEL_PREFIX__": prefix,
        "__REPO_PATH__": str(args.repo.expanduser().resolve()),
        "__PYTHON__": str(args.python.expanduser().absolute()),
        "__LOG_DIR__": str(args.logs.expanduser().resolve()),
    }
    template = Path(__file__).resolve().parents[1] / "hub/launchd/orb-hub.plist.template"
    text = template.read_text()
    text = re.sub(r"__[A-Z_]+__", lambda match: escape(values[match[0]]), text)
    data = plistlib.loads(text.encode())
    output = args.output.expanduser()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Generated machine-specific paths must stay private, even under a loose umask.
    with os.fdopen(os.open(output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600), "wb") as stream:
        os.fchmod(stream.fileno(), 0o600)
        plistlib.dump(data, stream)
    print(f"Rendered {data['Label']}; install the output manually (see hub/README.md).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
