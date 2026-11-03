#!/bin/sh
# Real pointer traces in virtual time; no live hub or development board.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_SHELL_SIM:-"$sim_dir/build/orb_shell_sim"}
python=${ORB_PYTHON:-python3}
out=${1:-"$sim_dir/../docs/screens/nav"}
mkdir -p "$out/default" "$out/ax3-bold-rt" "$out/reduce-motion" "$out/gate"
"$binary" --nav "$out/default"
"$binary" --nav "$out/ax3-bold-rt" ax3 bold rt
"$binary" --nav "$out/reduce-motion" rm
for surface in face agents approval; do
    "$binary" "$surface" "$out/gate/$surface-default.png"
    "$binary" "$surface" "$out/gate/$surface-ax3.png" ax3
    "$binary" "$surface" "$out/gate/$surface-rt.png" rt
    "$binary" "$surface" "$out/gate/$surface-ax3-bold-rt.png" ax3 bold rt
    "$binary" "$surface" "$out/gate/$surface-ax3-bold-rt-rm.png" ax3 bold rt rm
done
"$python" "$sim_dir/nav_gifs.py" "$out"
printf 'Navigation PNG sequences and GIFs: %s\n' "$out"
