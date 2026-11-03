#!/usr/bin/env bash
# Pair the fresh CTest renders with the canonical exported Design 1.0 frames.
# Run the simulator build and complete CTest suite first; no live services.
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-sim/build/polish-shell/sheets}
mkdir -p "$out"
for mode in default ax3-bold-rt; do
    for screen in Face Apps ControlCenter Stack Settings AlwaysOn Charging Setup Updating Notifications NotificationDetail; do
        python3 sim/png_tools.py concat "docs/design/1.0/png/$screen.png" \
            "sim/build/design1/$screen-$mode/frame-000060.png" "$out/$screen-$mode.png"
    done
    for screen in Agents SessionLive Talk TalkSend Sessions Dot Approval; do
        python3 sim/png_tools.py concat "docs/design/1.0/png/$screen.png" \
            "sim/build/agents-$mode/$screen.png" "$out/$screen-$mode.png"
    done
    for screen in NowPlaying SpokenReply SoundSettings; do
        python3 sim/png_tools.py concat "docs/design/1.0/png/$screen.png" \
            "sim/build/sound-ui/emu/$screen-$mode/frame-000060.png" "$out/$screen-$mode.png"
    done
    for card in usage timer; do
        python3 sim/png_tools.py concat docs/design/1.0/png/Stack.png \
            "sim/build/shell-stack-$card-$mode.png" "$out/Stack-$card-$mode.png"
    done
done
