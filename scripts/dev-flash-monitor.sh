#!/usr/bin/env bash
# Build, flash the board on the MacBook Pro, then capture its console for a while.
#
#   scripts/dev-flash-monitor.sh [seconds] [pattern]
#
# Prints only console lines containing one of the |-separated patterns (default
# "main:"), plus errors and crashes. Refuses to flash when the build fails, so a
# stale image never gets measured by mistake. Only one checkout may use the
# board at a time.
set -euo pipefail

SECS="${1:-20}"
PATTERN="${2:-main:}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="${TMPDIR:-/tmp}/orb-build-$$.log"

if ! "$ROOT/scripts/idf.sh" build >"$LOG" 2>&1; then
    grep -Ei "error|undefined|denied|not permitted" "$LOG" | cut -c1-300 | head -20 || true
    echo "BUILD FAILED (full log: $LOG)"
    exit 1
fi
grep -E "binary size" "$LOG" | tail -1 || true
if ! "$ROOT/scripts/remote.sh" flash >"$LOG.flash" 2>&1; then
    tail -5 "$LOG.flash"
    echo "FLASH FAILED"
    exit 1
fi

"$ROOT/scripts/orb-transport.sh" --logs "$SECS" "$PATTERN|E (|Guru|panic|Backtrace|abort|assert"
