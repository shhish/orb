#!/usr/bin/env bash
# Pull and decode a core dump, or symbolize a saved panic after checking its ELF SHA.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$ROOT/devtools${PYTHONPATH:+:$PYTHONPATH}"
exec "${ORB_PYTHON:-$ROOT/.venv/bin/python}" -m orb_devtools.crash "$@"
