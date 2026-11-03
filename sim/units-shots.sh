#!/bin/sh
# Fahrenheit Home content plus the mandatory Face/list/card accessibility gate.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
home_binary=${ORB_HOME_SIM:-"$sim_dir/build/orb_home_sim"}
shell_binary=${ORB_SHELL_SIM:-"$sim_dir/build/orb_shell_sim"}
out=${1:-"$sim_dir/build/units-gate"}

capture() {
    variant=$1
    shift
    mkdir -p "$out/$variant"
    for scene in home rooms climate thermostat temperature; do
        "$home_binary" "$scene" "$out/$variant/$scene.png" fahrenheit "$@"
    done
    for scene in face agents approval units temperature-settings; do
        "$shell_binary" "$scene" "$out/$variant/$scene.png" fahrenheit "$@"
    done
}

capture default
capture ax3 ax3
capture rt rt
capture ax3-bold-rt ax3 bold rt
capture ax3-bold-rt-rm ax3 bold rt rm
printf 'Units design gate screenshots: %s\n' "$out"
