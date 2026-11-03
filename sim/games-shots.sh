#!/bin/sh
# Production game logic/view, deterministic fake input, no board or hub access.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_GAMES_SIM:-"$sim_dir/build/orb_games_sim"}
out=${1:-"$sim_dir/build/games-gate"}

capture() {
    mode=$1
    shift
    mkdir -p "$out/$mode"
    for game in marble orbit ring-snake pop; do
        for scene in start play paused end; do
            "$binary" --game "$game" --headless --scene "$scene" --best 120 \
                --score 180 --tilt 50=12,5 --shot "600=$out/$mode/$game-$scene.png" "$@"
        done
        mkdir -p "$out/$mode/$game-sequence"
        tilt=18,8
        if [ "$game" = ring-snake ]; then tilt=0,0; fi
        "$binary" --game "$game" --headless --best 120 --score 180 \
            --tap 400=233,390 --reward 900 --tilt "420=$tilt" --end 1800 \
            --shot "0=$out/$mode/$game-sequence/00-start.png" \
            --shot "160=$out/$mode/$game-sequence/01-attract.png" \
            --shot "600=$out/$mode/$game-sequence/02-play.png" \
            --shot "932=$out/$mode/$game-sequence/03-action.png" \
            --shot "1200=$out/$mode/$game-sequence/04-motion.png" \
            --shot "1830=$out/$mode/$game-sequence/05-new-best.png" \
            --shot "1950=$out/$mode/$game-sequence/06-reward.png" \
            --shot "2400=$out/$mode/$game-sequence/07-result.png" "$@"
    done
    "$binary" --game marble --headless --scene play --controls \
        --shot "600=$out/$mode/marble-controls-play.png" "$@"
}

capture default
capture ax3 --text-size ax3
capture opaque --reduce-transparency
capture ax3-bold-rt-rm --text-size ax3 --bold --reduce-transparency --reduce-motion
printf 'Games design gate screenshots: %s\n' "$out"
