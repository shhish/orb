#!/usr/bin/env bash
# Internal transport. ORB_DEV_LOCAL=1 runs the same helper locally for PTY tests.
set -euo pipefail
# Personal dev settings (SSH host, board id, backup path) live outside the repo in
# ~/.config/orb/dev.env as KEY=VALUE lines; variables already set win.
DEV_ENV="${ORB_DEV_ENV:-$HOME/.config/orb/dev.env}"
if [[ -f "$DEV_ENV" ]]; then
    while IFS='=' read -r key value; do
        [[ "$key" =~ ^[A-Z_][A-Z0-9_]*$ && -z "${!key:-}" ]] && export "$key=$value"
    done < "$DEV_ENV"
fi
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${ESP_REMOTE_PORT:-/dev/cu.usbmodem1101}"
if [[ "${ORB_DEV_LOCAL:-0}" == 1 ]]; then
    exec "${ORB_PYTHON:-$ROOT/.venv/bin/python}" "$ROOT/scripts/orb_dev_remote.py" "$PORT" "$@"
fi
HOST="${ESP_REMOTE_HOST:-my-mac.local}"
printf -v args '%q ' "$PORT" "$@"
prefix=''
if [[ -n "${ORB_LOGD_SOCKET:-}" ]]; then
    printf -v prefix 'ORB_LOGD_SOCKET=%q ' "$ORB_LOGD_SOCKET"
fi
# shellcheck disable=SC2029
exec ssh "$HOST" "${prefix}\$HOME/.venvs/esptool/bin/python - $args" <"$ROOT/scripts/orb_dev_remote.py"
