#!/bin/sh
# Real shell input/model sequences, with virtual time and synthetic data only.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_SHELL_SIM:-"$sim_dir/build/orb_shell_sim"}
out=${1:-"$sim_dir/../docs/screens/delight/motion"}
mkdir -p "$out/default" "$out/ax3-bold-rt-rm" "$out/contrast"
"$binary" --delight "$out/default"
"$binary" --delight "$out/ax3-bold-rt-rm" ax3 bold rt rm
for surface in face agents approval; do
    "$binary" "$surface" "$out/contrast/$surface-ic.png" ic
    "$binary" "$surface" "$out/contrast/$surface-ax3-bold-rt-ic.png" ax3 bold rt ic
done
printf 'Delight motion screenshots: %s\n' "$out"
