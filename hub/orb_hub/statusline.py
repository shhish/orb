"""Claude's stdin statusline integration; no provider settings are modified."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import TextIO

from orb_agents.usage import MAX_STATUSLINE_BYTES, write_claude_statusline


def run(data_dir: Path, stdin: TextIO, stdout: TextIO, stderr: TextIO) -> int:
    try:
        raw = stdin.read(MAX_STATUSLINE_BYTES + 1)
        if len(raw.encode("utf-8")) > MAX_STATUSLINE_BYTES:
            raise ValueError("Statusline input is too large")
        payload = json.loads(raw)
        if not isinstance(payload, dict):
            raise TypeError("Statusline input must be an object")
    except (TypeError, ValueError, UnicodeError, RecursionError):
        print("Claude | context ?", file=stdout)
        print("orb-hub: invalid statusline input", file=stderr)
        return 1

    model = payload.get("model")
    name = model.get("display_name") or model.get("id") if isinstance(model, dict) else None
    # The provider's model display name is terminal content, never ANSI/OSC code.
    name = re.sub(r"[^\w .()-]", "", name)[:40] if isinstance(name, str) else "Claude"
    context = payload.get("context_window")
    used = context.get("used_percentage") if isinstance(context, dict) else None
    context_label = "?"
    if isinstance(used, (int, float)) and not isinstance(used, bool) and 0 <= used <= 100:
        context_label = f"{used:.0f}%"
    print(f"{name or 'Claude'} | context {context_label}", file=stdout)
    try:
        write_claude_statusline(data_dir, payload)
    except (OSError, ValueError):
        print("orb-hub: statusline usage cache unavailable", file=stderr)
        return 1
    return 0
