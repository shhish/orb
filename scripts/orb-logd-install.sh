#!/usr/bin/env bash
# Run manually on the MacBook Pro; never invoked by development/test commands.
# install stages the agent without loading it; --load opts into launchctl.
# uninstall stops/removes only this agent; recorded logs and incidents remain.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON="${ORB_INSTALL_PYTHON:-/opt/homebrew/bin/python3.13}"
exec "$PYTHON" "$ROOT/scripts/orb_logd_install.py" "$@"
