#!/usr/bin/env bash
# Explicit USB migration; --dry-run never opens a transport or reads a key.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "${ORB_PYTHON:-python3.13}" "$ROOT/scripts/migrate_partitions.py" "$@"
