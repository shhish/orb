#!/usr/bin/env python3
"""Run Orb UI scenarios with the worktree's devtools package."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "devtools"))

from orb_devtools.ui import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
