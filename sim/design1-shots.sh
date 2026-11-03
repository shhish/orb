#!/bin/sh
# Lossless design | real-SDL-render review pairs. No board or hub activity.
set -eu
root=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
out=${1:-"$root/work/design-1.0/shell"}
mkdir -p "$out"
for screen in Face FaceWaiting AlwaysOn Stack Notifications NotificationDetail Offline Charging Apps ControlCenter Compass PowerSheet Settings Setup Updating; do
    for variant in default ax3-bold-rt; do
        frames="$out/frames/$screen-$variant"
        if [ "$variant" = ax3-bold-rt ]; then
            nice "$root/sim/build/orb_emu" --design-scene "$screen" --headless-frames 60 --out "$frames" --text-size ax3 --bold --reduce-transparency
        else
            nice "$root/sim/build/orb_emu" --design-scene "$screen" --headless-frames 60 --out "$frames"
        fi
        cp "$frames/frame-000060.png" "$out/$screen-$variant.png"
    done
done
# The same integrated retained surface proves redaction, discrete minute shift,
# empty-state centring and bounded sleep/wake, including Reduce Motion.
for scene in AlwaysOnEmpty AlwaysOnShifted Sleep Wake; do
    for variant in default ax3-bold-rt reduce-motion; do
        frames="$out/power-flows/$scene-$variant"
        case "$variant" in
            ax3-bold-rt) nice "$root/sim/build/orb_emu" --design-scene "$scene" --headless-frames 60 --out "$frames" --text-size ax3 --bold --reduce-transparency ;;
            reduce-motion) nice "$root/sim/build/orb_emu" --design-scene "$scene" --headless-frames 60 --out "$frames" --reduce-motion ;;
            *) nice "$root/sim/build/orb_emu" --design-scene "$scene" --headless-frames 60 --out "$frames" ;;
        esac
    done
done
nice swift -module-cache-path /tmp/orb-design1-swift-cache "$root/sim/design1_contact.swift" "$root/docs/design/1.0/png" "$out"
