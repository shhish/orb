#!/bin/sh
# Real firmware Home views, synthetic data and deterministic pointer input.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_HOME_SIM:-"$sim_dir/build/orb_home_sim"}
out=${1:-"$sim_dir/build/home-gate"}
mkdir -p "$out/default" "$out/ax3" "$out/ax3-bold-rt" \
    "$out/motion-default" "$out/motion-ax3-bold-rt" "$out/motion-reduced"
for surface in home morning evening night rooms rooms-scrolled rooms-next room-living room-living-next room-bedroom \
    room-kitchen room-office room-dining climate light colour white scenes \
    thermostat modes fan lock unlocked switch outlet temperature humidity motion \
    low-battery contact wire-temperature wire-humidity wire-motion wire-contact \
    no-response offline unavailable unavailable-lock waiting unknown-lock error \
    hub-home hub-rooms hub-room hub-climate hub-light hub-colour hub-lock hub-fan \
    no-favourites lights-0 lights-1 lights-2 switch-light-summary offline-lock offline-unlocked \
    long-scene wide-scene long-error wide-error offline-climate offline-rooms offline-sensors \
    empty-home loading-home busy-rooms busy-room busy-room-scrolled busy-room-last empty-room room-actions \
    all-off-pending all-off-accepted all-off-failed \
    pending-light pending-switch pending-room pending-lock scene-pending scene-accepted scene-failed \
    capsule-pending capsule-accepted capsule-failed thermostat-f-heat thermostat-f-cool thermostat-f-auto \
    thermostat-f-off thermostat-f-modes sensor-aged sensor-unknown-age sensor-future-age; do
    "$binary" "$surface" "$out/default/$surface.png"
    "$binary" "$surface" "$out/ax3/$surface.png" ax3
    "$binary" "$surface" "$out/ax3-bold-rt/$surface.png" ax3 bold rt
done
"$binary" --sequences "$out/motion-default"
"$binary" --sequences "$out/motion-ax3-bold-rt" ax3 bold rt
"$binary" --sequences "$out/motion-reduced" ax3 bold rt rm
"$binary" --ux-sequences "$out/motion-default"
"$binary" --ux-sequences "$out/motion-ax3-bold-rt" ax3 bold rt
"$binary" --ux-sequences "$out/motion-reduced" ax3 bold rt rm
printf 'Home design gate screenshots: %s\n' "$out"
