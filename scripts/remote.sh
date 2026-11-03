#!/usr/bin/env bash
# Flash and monitor the board while it is plugged into the MacBook Pro.
# Builds happen here (Mac mini); the build output is copied over SSH and esptool runs on the MacBook Pro.
#
#   scripts/remote.sh flash      copy build/ and write bootloader, partition table and app
#   scripts/remote.sh flash-app BUILD_DIR   signed ota_0 + OTA selector only; preserve NVS/table
#   scripts/remote.sh monitor    serial log stream (Ctrl-C to quit)
#   scripts/remote.sh info       chip, flash and security info
#   scripts/remote.sh restore    write back the original Waveshare/Xiaozhi flash image
# Paths below are remote: quoted tildes are expanded by orb_dev_remote.py.
# shellcheck disable=SC2016,SC2029,SC2088
set -euo pipefail
# Personal dev settings (SSH host, board id, backup path) live outside the repo in
# ~/.config/orb/dev.env as KEY=VALUE lines; variables already set win.
DEV_ENV="${ORB_DEV_ENV:-$HOME/.config/orb/dev.env}"
if [[ -f "$DEV_ENV" ]]; then
    while IFS='=' read -r key value; do
        [[ "$key" =~ ^[A-Z_][A-Z0-9_]*$ && -z "${!key:-}" ]] && export "$key=$value"
    done < "$DEV_ENV"
fi

HOST="${ESP_REMOTE_HOST:-my-mac.local}"
DEST='$HOME/esp-remote/esp32s3-firmware'
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$(dirname "$0")/../firmware"

case "${1:-}" in
flash-app)
    # Round-3 comparisons keep the installed bootloader/table (the renderer's
    # historical storage layout differs). Reset only the OTA selector so the
    # existing bootloader selects ota_0. Never write NVS, assets or storage.
    [[ $# == 2 ]] || { echo 'flash-app needs an absolute build directory' >&2; exit 2; }
    [[ "$2" == /* && -f "$2/orb.bin" && -f "$2/ota_data_initial.bin" ]] || exit 2
    [[ $(wc -c <"$2/ota_data_initial.bin") -eq 8192 ]] || exit 2
    [[ $(LC_ALL=C tr -d '\377' <"$2/ota_data_initial.bin" | wc -c) -eq 0 ]] || exit 2
    umask 077
    stage="$(mktemp -d "${TMPDIR:-/tmp}/orb-flash-app.XXXXXX")"
    trap 'rm -rf "$stage"' EXIT
    trap 'exit 130' HUP INT TERM
    cp "$2/orb.bin" "$stage/orb.bin"
    cp "$2/ota_data_initial.bin" "$stage/ota_data_initial.bin"
    "$ROOT/scripts/sign-app.sh" "$stage/orb.bin"
    [[ $(wc -c <"$stage/orb.bin") -le 4194304 ]] || { echo 'app exceeds ota_0' >&2; exit 1; }
    printf '%s\n' '--flash-mode keep --flash-freq keep --flash-size keep' \
        '0xf000 ota_data_initial.bin' '0x20000 orb.bin' >"$stage/flash_args"
    ssh "$HOST" "mkdir -p $DEST"
    rsync -a "$stage/" "$HOST:${DEST#\$HOME/}/"
    "$ROOT/scripts/orb-transport.sh" --exec --cwd '~/esp-remote/esp32s3-firmware' \
        '~/.venvs/esptool/bin/esptool' --chip esp32s3 --port '{port}' -b 921600 write-flash @flash_args
    ;;
flash)
    [[ -f build/flash_args ]] || { echo "No build/flash_args; run scripts/idf.sh build first" >&2; exit 1; }
    # OTA trusts the running image's first signature block; USB must carry it.
    app_bin="$(awk '$1 == "0x20000" {print $2}' build/flash_args)"
    [[ -n "$app_bin" && -f "build/$app_bin" ]] || { echo "No app at ota_0 in flash_args" >&2; exit 1; }
    # A normal app flash cannot preserve state across a partition relocation.
    # Only the explicit migration tool backs up and verifies NVS under one lease.
    if [[ -f build/partition_table/partition-table.bin ]] &&
       [[ -z "${ORB_PARTITIONS_MIGRATED:-}" ]]; then
        echo 'Wave 2 layout: use scripts/migrate-partitions.sh for the first install.' >&2
        echo 'After verified migration, set ORB_PARTITIONS_MIGRATED=1 for routine USB flashing.' >&2
        exit 1
    fi
    "$ROOT/scripts/sign-app.sh" "build/$app_bin"
    ssh "$HOST" "mkdir -p $DEST"
    rsync -a --delete --include='*/' --include='*.bin' --include='*.elf' --include='flash_args' \
        --exclude='*' --prune-empty-dirs build/ "$HOST:${DEST#\$HOME/}/"
    "$ROOT/scripts/orb-transport.sh" --exec --cwd '~/esp-remote/esp32s3-firmware' \
        '~/.venvs/esptool/bin/esptool' --chip esp32s3 --port '{port}' -b 921600 write-flash @flash_args
    ;;
monitor)
    "$ROOT/scripts/orb-transport.sh" --logs 0 ''
    ;;
info)
    "$ROOT/scripts/orb-transport.sh" --exec '~/.venvs/esptool/bin/esptool' --port '{port}' flash-id
    "$ROOT/scripts/orb-transport.sh" --exec '~/.venvs/esptool/bin/esptool' --port '{port}' get-security-info
    ;;
restore)
    "$ROOT/scripts/orb-transport.sh" --exec '~/.venvs/esptool/bin/esptool' \
        --chip esp32s3 --port '{port}' -b 921600 write-flash 0 \
        "${ESP_REMOTE_BACKUP:?Set ESP_REMOTE_BACKUP to your remote factory backup path}"
    ;;
*)
    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
