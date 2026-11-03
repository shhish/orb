#!/bin/sh
# Design 2.0 Home family: every profile, state, kind and frame-left comparison.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_HOME_SIM:-"$sim_dir/build/orb_home_sim"}
out=${1:-"$sim_dir/home_design2"}
exec python3 "$sim_dir/home_contact_sheets.py" --binary "$binary" --out "$out"
