#!/usr/bin/env python3
"""Repository entry point for the dependency-free LVGL trace converter."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "devtools"))
from orb_devtools.profile import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
