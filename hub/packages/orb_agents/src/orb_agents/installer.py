"""Generate a reviewable Claude settings fragment; never install it.

The generated file contains a secret. Keep it outside source control and pass
the same header value to :class:`orb_agents.hooks.HookServer`.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
from pathlib import Path
from urllib.parse import urlsplit

HEADER = "X-Orb-Secret"
HOOK_PATH = "/hooks/claude"
EVENTS = (
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "Notification",
    "Stop",
    "StopFailure",
    "PermissionRequest",
    "PostToolUse",
    "PostToolUseFailure",
)


def validate_secret(secret: str) -> None:
    if (
        not isinstance(secret, str)
        or not 32 <= len(secret) <= 256
        or "$" in secret
        or any(not 33 <= ord(character) <= 126 for character in secret)
    ):
        raise ValueError(
            "Secret must be 32–256 printable ASCII characters without spaces or dollar signs"
        )


def generate_hooks(url: str, secret: str | None = None, timeout: float = 130) -> dict:
    """Return a settings fragment with an independent secret if none is supplied.

    The receiver's approval timeout must be shorter than ``timeout``. Status
    hooks use a ten-second timeout. Existing hooks must be merged by the owner;
    this function does not read, replace, or apply any settings.
    """
    parsed = urlsplit(url)
    if (
        parsed.scheme != "http"
        or parsed.hostname != "127.0.0.1"
        or parsed.username is not None
        or parsed.password is not None
        or parsed.port is None
        or not 1 <= parsed.port <= 65535
        or parsed.path != HOOK_PATH
        or parsed.query
        or parsed.fragment
        or url != f"http://127.0.0.1:{parsed.port}{HOOK_PATH}"
    ):
        raise ValueError("Hook URL must be http://127.0.0.1:<port>/hooks/claude")
    if not 1 <= timeout <= 600:
        raise ValueError("Hook timeout must be between 1 and 600 seconds")
    secret = secrets.token_urlsafe(32) if secret is None else secret
    validate_secret(secret)
    return {
        "hooks": {
            event: [
                {
                    "hooks": [
                        {
                            "type": "http",
                            "url": url,
                            "headers": {HEADER: secret},
                            "timeout": timeout if event == "PermissionRequest" else 10,
                        }
                    ]
                }
            ]
            for event in EVENTS
        }
    }


def write_snippet(path: str | Path, snippet: dict) -> None:
    """Create a new mode-0600 file; refuse existing files, including symlinks."""
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        json.dump(snippet, stream, indent=2)
        stream.write("\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=130)
    args = parser.parse_args(argv)
    try:
        snippet = generate_hooks(args.url, timeout=args.timeout)
        write_snippet(args.output, snippet)
    except (OSError, ValueError):
        # Neither user-supplied URL nor file data is echoed into logs.
        parser.exit(
            2, "Could not create snippet; check endpoint and use a new output file.\n"
        )
    print(
        "Created private hooks snippet. Review and merge it manually; nothing was installed."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
