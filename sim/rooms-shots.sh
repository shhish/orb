#!/bin/sh
# Seven-room list evidence from the real renderer; all state is process-local.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_HOME_SIM:-"$sim_dir/build/orb_home_sim"}
out=${1:-"$sim_dir/../work/rooms-shots"}
for profile in default larger largest bold reduce-transparency largest-bold; do
    case "$profile" in
        default) set -- default ;;
        larger) set -- larger ;;
        largest) set -- ax3 ;;
        bold) set -- bold ;;
        reduce-transparency) set -- rt ;;
        largest-bold) set -- ax3 bold ;;
    esac
    mkdir -p "$out/$profile"
    for state in top end stale offline empty pending failed; do
        nice "$binary" "rooms-list-$state" "$out/$profile/$state.png" "$@"
    done
    for index in 0 1 2 3 4 5 6; do
        nice "$binary" "rooms-list-centre-$index" "$out/$profile/room-$index.png" "$@"
    done
    nice "$binary" room-all-off-row "$out/$profile/room-all-off-row.png" "$@"
done
printf 'Room-list evidence: %s\n' "$out"
