#!/usr/bin/env bash
# Lead-only round 3: build, profile both bench phases, compare orb/render, restore.
# Usage: scripts/speed_profile.sh --bench-version CURRENT_DEV_FEED_VERSION
# Resume: repeat the same command; artifacts default to work/w2-speed-r3/.
# Requires sole development-board ownership and ORB_SIGNING_KEY (existing key).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON="${ORB_PYTHON:-$ROOT/work/host-venv/bin/python}"
[[ -x "$PYTHON" ]] || PYTHON=python3
exec "$PYTHON" "$ROOT/scripts/speed_profile.py" "$@"
