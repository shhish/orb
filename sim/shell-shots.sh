#!/bin/sh
# Synthetic fixtures only. Build first with cmake --build sim/build.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_SHELL_SIM:-"$sim_dir/build/orb_shell_sim"}
out=${1:-"$sim_dir/../docs/screens/shell"}
mkdir -p "$out"
for surface in face demo apps apps-next agents agents-next session session-summary stack approval approval-actions choice choice-options choice-options-next message attention notifications \
    control-center control-silent control-motion settings settings-next display accessibility access-bold access-motion access-transparency access-contrast about about-licenses dot voice play play-registered updates \
    setup setup-retry pairing connecting offline identify service-error update \
    downloading verifying ready update-error \
    voice-starting voice-recording voice-transcribing voice-confirming voice-sending voice-error \
    voice-no-speech voice-network voice-unsupported \
    agents-idle agents-idle-row idle-loading idle-page idle-more idle-end idle-empty idle-error idle-more-pending idle-more-error idle-detail idle-reply session-outside-focus \
    sessions-settings sessions-codex sessions-dot sessions-recent sessions-pending sessions-error sessions-off sessions-touched sessions-recent-off sessions-recent-five sessions-recent-thirty \
    answer-queued answer-delivered answer-undelivered answer-other answer-rejected card-cancelled \
    message-delivered message-timeout; do
    "$binary" "$surface" "$out/$surface-default.png" default
    "$binary" "$surface" "$out/$surface-ax3.png" ax3
    "$binary" "$surface" "$out/$surface-ax3-bold-rt.png" ax3 bold rt
done
# HIG design gate: opaque glass and larger, heavier text on representative views.
for surface in face agents approval approval-actions; do
    "$binary" "$surface" "$out/$surface-rt.png" rt
    "$binary" "$surface" "$out/$surface-ax3-bold-rt-rm.png" ax3 bold rt rm
done
"$binary" --flow "$out"
printf 'Shell screenshots: %s\n' "$out"
