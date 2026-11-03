#!/usr/bin/env bash
# Synthetic, real-pointer evidence for the shell UX review; never contacts a hub.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
binary="${ORB_SHELL_SIM:-$repo/sim/build/orb_shell_sim}"
output="${1:-$repo/docs/screens/ux-fixes/shellux}"
scenes=(face apps apps-next agents agents-next settings settings-next notifications
    notifications-received notifications-cleared notifications-undo notifications-empty
    control-center control-silent control-motion home-controls home-controls-unavailable home-controls-waiting
    agents-empty dot-empty stack-empty voice voice-idle-session voice-starting voice-recording
    voice-transcribing voice-confirming voice-sending voice-no-speech voice-error voice-network voice-unsupported
    answer-queued answer-delivered answer-undelivered answer-rejected answer-other
    approval choice stack connection connection-maintenance forget-wifi-confirm forget-wifi-confirm-more
    unpair-confirm unpair-confirm-more setup setup-more setup-retry pairing pairing-more connecting offline
    update-check-receipt update-check-result ready downloading verifying update-error display
    accessibility access-text-size access-text-selected access-list-motion access-list-transparency access-list-contrast
    access-bold access-motion access-transparency access-contrast sessions-settings sessions-codex sessions-dot
    sessions-recent sessions-pending sessions-error sessions-off sessions-touched
    about about-device-id about-disclosure about-licenses about-licenses-middle about-licenses-end
    sound sound-volume sound-quiet sound-start sound-end)
for profile in default ax3-bold-rt; do
    flags=(default)
    if [[ "$profile" == ax3-bold-rt ]]; then flags=(ax3 bold rt); fi
    mkdir -p "$output/$profile" "$output/motion/$profile"
    for scene in "${scenes[@]}"; do
        "$binary" "$scene" "$output/$profile/$scene.png" "${flags[@]}"
    done
    "$binary" --delight "$output/motion/$profile" "${flags[@]}"
done
mkdir -p "$output/motion/reduce-motion"
"$binary" --delight "$output/motion/reduce-motion" ax3 bold rt rm
