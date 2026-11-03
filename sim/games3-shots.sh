#!/bin/sh
# Repeatable synthetic gravity/gyro captures of production game logic/view.
set -eu
out=${1:-sim/build/games3}
for mode in default ax3-bold-rt; do
    mkdir -p "$out/$mode"
    set --
    if [ "$mode" = ax3-bold-rt ]; then
        set -- --text-size ax3 --bold --reduce-transparency
    fi
    sim/build/orb_games_sim --game beads --scene play --headless "$@" \
        --handheld-until 6000 --tilt 0=-35,0 --tilt 3000=35,0 --tilt 6000=0,0 \
        --shot "3000=$out/$mode/beads-left.png" \
        --shot "3800=$out/$mode/beads-crossing.png" \
        --shot "6000=$out/$mode/beads-right.png" \
        --shot "9000=$out/$mode/beads-settling.png" \
        --shot "12000=$out/$mode/beads-clock.png"
    sim/build/orb_games_sim --game marble --scene play --headless "$@" \
        --shot "400=$out/$mode/marble-rim-hint.png"
    sim/build/orb_games_sim --game orbit --scene play --headless "$@" \
        --tap 800=350,300 --shot "850=$out/$mode/orbit-switch.png"
    sim/build/orb_games_sim --game ring-snake --scene play --controls --headless "$@" \
        --shot "800=$out/$mode/snake-controls.png"
    for game in marble beads orbit ring-snake pop; do
        sim/build/orb_games_sim --game "$game" --scene paused --headless "$@" \
            --shot "900=$out/$mode/$game-paused.png"
    done
done
