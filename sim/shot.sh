#!/usr/bin/env bash
# Renders the simulator headless into PNGs, in virtual time: the same pixels
# on every run, however fast the machine.
#
#   sim/shot.sh              the screenshots in docs/screens (demo-*.png)
#   sim/shot.sh -n N [DIR]   the first N frames, one per 16 ms, into DIR
#                            (default sim/build/frames) as frame-0000.png, ...
#
# For other moments or tilts, call sim/run.sh --headless with --shot and --tilt.
set -euo pipefail

sim="$(cd "$(dirname "$0")" && pwd)"
run="$sim/run.sh"

if [[ "${1:-}" == "-n" && $# -ge 2 && $# -le 3 ]]; then
    dir="${3:-$sim/build/frames}"
    mkdir -p "$dir"
    exec "$run" --headless --frames "$2" --out "$dir"
elif [[ $# -ne 0 ]]; then
    echo "usage:" >&2
    sed -n '5,7p' "$0" | sed 's/^#/ /' >&2
    exit 2
fi

out="$(cd "$sim/.." && pwd)/docs/screens"
mkdir -p "$out"

# The Sessions demo scrolls its list down for 4 s and back up for 4 s, then
# rests for 8 s while the light sweeps the glass and the primary action is
# pressed (at 8.5 s, for half a second).
"$run" --headless \
    --shot 0="$out/demo-top.png" \
    --shot 1000="$out/demo-scroll-1s.png" \
    --shot 2000="$out/demo-scroll-2s.png" \
    --shot 4000="$out/demo-scroll-end.png" \
    --shot 8650="$out/demo-pressed.png"

# The moment of demo-scroll-2s.png, tilted 25 degrees from 1.2 s on: the
# highlight swings to the raised side and the content slides downhill.
"$run" --headless --tilt 1200=25,0 --shot 2000="$out/demo-tilt-right.png"
"$run" --headless --tilt 1200=0,25 --shot 2000="$out/demo-tilt-down.png"
