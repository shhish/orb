#!/usr/bin/env bash
# Real shell/game input against synthetic offline state; no board or hub access.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
build="${ORB_SIM_BUILD:-$repo/sim/build}"
output="${1:-$repo/docs/screens/ux-fixes/offline}"
capture() {
    profile=$1
    shift
    mkdir -p "$output/$profile/game-flow"
    "$build/test_shell" --offline "$output/$profile" "$@"
    "$build/orb_game_flow_sim" "$output/$profile/game-flow" "$@"
}
capture default
capture ax3-bold-rt ax3 bold rt
capture default-rt rt
"$build/orb_game_flow_sim" "$repo/docs/screens/ux-fixes/gameux/flow/default"
"$build/orb_game_flow_sim" "$repo/docs/screens/ux-fixes/gameux/flow/ax3-bold-rt" ax3 bold rt
