#!/usr/bin/env bash
# Actual firmware UI, synthetic data, exact 60 Hz virtual samples.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:?usage: sim/alive-shots.sh OUT}"
export TZ=UTC LC_ALL=C
shopt -s nullglob dotglob
existing=("$OUT"/*)
if (( ${#existing[@]} )); then
    echo "Capture output must be new or empty: $OUT" >&2
    exit 2
fi
if [[ ! -x "$ROOT/sim/build/orb_alive_shots" ]]; then
    cmake -S "$ROOT/sim" -B "$ROOT/sim/build" -G Ninja
    nice cmake --build "$ROOT/sim/build" -j4 --target orb_alive_shots
fi
mkdir -p "$OUT"
for scenario in rooms-arrival row-light accessory-light detail-light brightness thermostat scroll page-dots-kit scene-requested loading; do
    nice "$ROOT/sim/build/orb_alive_shots" "$OUT/$scenario" "$scenario"
done
for mode in ax3 rt rm; do
    mkdir -p "$OUT/$mode"
    for scenario in rooms-arrival detail-light brightness loading; do
        nice "$ROOT/sim/build/orb_alive_shots" "$OUT/$mode/$scenario" "$scenario" "$mode"
    done
done

# Whole-OS sequences run in fresh parity processes with synthetic models.
# Wait sequences sample their first four seconds and their endpoint past 30 s.
if [[ ! -x "$ROOT/sim/build/orb_parity" ]]; then
    cmake -S "$ROOT/sim" -B "$ROOT/sim/build" -G Ninja
    nice cmake --build "$ROOT/sim/build" -j4 --target orb_parity
fi
mkdir -p "$OUT/os"
for scenario in settings-brightness volume usage usage-follow timer-open timer-action cc-toggle cc-follow face-wake face-minute roll-weather roll-battery charging-follow volume-hud-follow apps-arrival compass-arrival receipt agent-step-check update-ready ota-follow empty-state breathe-running breathe-turning-off breathe-dot breathe-checking breathe-loading breathe-transcribing breathe-buffering breathe-captions breathe-saving breathe-temp-saving time-pending-still breathe-weather-loading breathe-control-loading breathe-boot-starting breathe-setup-code breathe-pairing-code breathe-sources-saving activity-placeholder-still breathe-syncing breathe-idle-loading breathe-starting breathe-sending breathe-queued breathe-time breathe-verifying breathe-installing; do
    nice "$ROOT/sim/build/orb_parity" --alive-capture "$scenario" default "$OUT/os/$scenario"
done
for mode in ax3-bold-rt reduce-motion; do
    mkdir -p "$OUT/os/$mode"
    for scenario in settings-brightness volume usage timer-open cc-toggle cc-follow face-wake roll-battery apps-arrival compass-arrival receipt agent-step-check update-ready breathe-loading breathe-transcribing breathe-saving; do
        nice "$ROOT/sim/build/orb_parity" --alive-capture "$scenario" "$mode" "$OUT/os/$mode/$scenario"
    done
done
