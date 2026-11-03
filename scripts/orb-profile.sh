#!/usr/bin/env bash
# Capture LVGL profiling through orb-dev.sh, or convert an existing console log.
# Usage: orb-profile.sh [milliseconds [trace.json]]
#        orb-profile.sh --input console.log [trace.json] [converter options...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON="${ORB_PYTHON:-$ROOT/.venv/bin/python}"
[[ -x "$PYTHON" ]] || PYTHON=python3
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi
if [[ "${1:-}" == --input ]]; then
    [[ $# -ge 2 ]] || { echo '--input needs a console log path' >&2; exit 2; }
    input="$2"
    output="${3:-$ROOT/profiles/trace.json}"
    shift 2
    if [[ $# -gt 0 ]]; then shift; fi
else
    ms="${1:-400}"
    if [[ ! "$ms" =~ ^[0-9]{1,4}$ ]]; then
        echo 'milliseconds must be 1..5000' >&2; exit 2
    fi
    if ((10#$ms < 1 || 10#$ms > 5000)); then
        echo 'milliseconds must be 1..5000' >&2; exit 2
    fi
    output="${2:-$ROOT/profiles/$(date +%Y%m%d-%H%M%S).json}"
    input="${output%.json}.ftrace"
    mkdir -p "$(dirname "$input")"
    "$ROOT/scripts/orb-dev.sh" profile "$ms" >"$input"
    if [[ $# -gt 0 ]]; then shift; fi
    if [[ $# -gt 0 ]]; then shift; fi
fi
mkdir -p "$(dirname "$output")"
"$PYTHON" "$ROOT/scripts/orb-profile-convert.py" "$input" "$output" "$@" | tee "${output%.json}.summary.txt"
