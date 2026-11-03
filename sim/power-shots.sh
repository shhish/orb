#!/bin/sh
# Stream C public-data fixtures and unchanged Face/list/card design-gate context.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
out=${1:-"$sim_dir/build/power-gate"}
power=${ORB_POWER_SIM:-"$sim_dir/build/orb_power_sim"}
shell=${ORB_SHELL_SIM:-"$sim_dir/build/orb_shell_sim"}
mkdir -p "$out"
"$power" "$out/default"
"$power" "$out/ax3-bold-rt" ax3 bold rt
"$power" "$out/reduce-motion" rm
"$power" "$out/ax3-bold-rt-rm" ax3 bold rt rm
"$power" "$out/increase-contrast" ax3 bold rt ic
for scene in face agents approval; do
    "$shell" "$scene" "$out/default/$scene-context.png"
    "$shell" "$scene" "$out/ax3-bold-rt/$scene-context.png" ax3 bold rt
    "$shell" "$scene" "$out/ax3-bold-rt-rm/$scene-context.png" ax3 bold rt rm
done
printf 'Power design gate screenshots: %s\n' "$out"
