#!/bin/sh
# Fresh native evidence for UX review F14–F16/F29–F32. No board or hub access.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_GAMES_SIM:-"$sim_dir/build/orb_games_sim"}
shell_binary=${ORB_SHELL_SIM:-"$sim_dir/build/orb_shell_sim"}
out=${1:-"$sim_dir/../docs/screens/ux-fixes/gameux"}

capture() {
    mode=$1
    shift
    mkdir -p "$out/$mode"
    for game in marble orbit ring-snake pop; do
        for scene in start play paused end; do
            "$binary" --game "$game" --headless --scene "$scene" --best 120 \
                --score 180 --shot "800=$out/$mode/$game-$scene.png" "$@"
        done
        for scene in play paused end; do
            "$binary" --game "$game" --headless --scene "$scene" --save-error \
                --best 120 --score 180 --shot "800=$out/$mode/$game-$scene-save-error.png" "$@"
        done
        mkdir -p "$out/$mode/$game-result-motion"
        "$binary" --game "$game" --headless --scene play --best 120 --score 180 --end 900 \
            --shot "899=$out/$mode/$game-result-motion/00-before.png" \
            --shot "933=$out/$mode/$game-result-motion/01-result.png" \
            --shot "1000=$out/$mode/$game-result-motion/02-score.png" \
            --shot "1100=$out/$mode/$game-result-motion/03-score.png" \
            --shot "1500=$out/$mode/$game-result-motion/04-settled.png" "$@"
        mkdir -p "$out/$mode/$game-motion"
        "$binary" --game "$game" --headless --scene play --reward 900 \
            --shot "0=$out/$mode/$game-motion/00-live.png" \
            --shot "600=$out/$mode/$game-motion/01-drift.png" \
            --shot "899=$out/$mode/$game-motion/02-before.png" \
            --shot "933=$out/$mode/$game-motion/03-response.png" \
            --shot "1000=$out/$mode/$game-motion/04-response.png" \
            --shot "1100=$out/$mode/$game-motion/05-response.png" \
            --shot "1400=$out/$mode/$game-motion/06-settled.png" "$@"
    done
    "$binary" --game marble --headless --scene play --controls \
        --shot "800=$out/$mode/marble-touch-controls.png" "$@"
    mkdir -p "$out/$mode/pop-direct"
    "$binary" --game pop --headless --scene play --bubble-tap 900 --bubble-tap 1100 \
        --shot "899=$out/$mode/pop-direct/00-before.png" \
        --shot "933=$out/$mode/pop-direct/01-burst.png" \
        --shot "1000=$out/$mode/pop-direct/02-droplets.png" \
        --shot "1133=$out/$mode/pop-direct/03-combo.png" \
        --shot "1200=$out/$mode/pop-direct/04-score.png" \
        --shot "1700=$out/$mode/pop-direct/05-next-target.png" "$@"
}
capture default
# Motion stays enabled in the principal accessibility profile.
capture ax3-bold-rt --text-size ax3 --bold --reduce-transparency
capture reduce-motion --text-size ax3 --bold --reduce-transparency --reduce-motion

for mode in default ax3-bold-rt opaque; do
    mkdir -p "$out/gate/$mode"
    for scene in face agents approval; do
        case "$mode" in
            default) "$shell_binary" "$scene" "$out/gate/$mode/$scene.png" ;;
            ax3-bold-rt) "$shell_binary" "$scene" "$out/gate/$mode/$scene.png" ax3 bold rt ;;
            opaque) "$shell_binary" "$scene" "$out/gate/$mode/$scene.png" rt ;;
        esac
    done
done
printf 'Native game UX evidence: %s\n' "$out"
