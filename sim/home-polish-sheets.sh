#!/bin/sh
# Compare the canonical parent frame with each polished Home state. Extended
# states keep their closest parent frame alongside them for layout review.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
renders=${1:-"$sim_dir/build"}
out=${2:-"$sim_dir/build/home-polish-review"}
for profile in default ax3-bold-rt; do
    mkdir -p "$out/$profile"
    for surface in rooms room-living light no-favourites busy-rooms busy-room busy-room-last \
        empty-home loading-home empty-room room-actions all-off-pending all-off-accepted all-off-failed \
        pending-light pending-switch pending-room pending-lock scene-pending scene-accepted scene-failed \
        capsule-pending capsule-accepted capsule-failed thermostat-f-heat thermostat-f-cool \
        thermostat-f-auto thermostat-f-off thermostat-f-modes sensor-aged sensor-unknown-age sensor-future-age; do
        case "$surface" in
            rooms|no-favourites|busy-rooms|empty-home|loading-home|scene-*|capsule-*) frame=HomeRooms ;;
            room-*|busy-room*|empty-room|all-off-*|pending-room) frame=Home ;;
            *) frame=HomeDial ;;
        esac
        python3 "$sim_dir/png_tools.py" concat "$sim_dir/../docs/design/1.0/png/$frame.png" \
            "$renders/home-$surface-$profile.png" "$out/$profile/$surface.png"
    done
done
printf 'Home polish comparison sheets: %s\n' "$out"
