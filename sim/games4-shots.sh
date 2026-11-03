#!/bin/sh
# Production models, accelerometer/gyro and LVGL pointer input; no board state.
set -eu
out=${1:-sim/build/games4}
for mode in default ax3-bold-rt; do
    mkdir -p "$out/$mode"
    set --
    if [ "$mode" = ax3-bold-rt ]; then
        set -- --text-size ax3 --bold --reduce-transparency
    fi
    sim/build/orb_games_sim --game beads --scene play --headless "$@" \
        --handheld-until 6000 --tilt 0=-10,0 --tilt 3000=10,0 --tilt 6000=0,0 \
        --shot "2800=$out/$mode/beads-left.png" \
        --shot "3700=$out/$mode/beads-crossing.png" \
        --shot "5800=$out/$mode/beads-right.png" \
        --shot "10000=$out/$mode/beads-clock.png"
    sim/build/orb_games_sim --game marble --scene play --headless "$@" \
        --tilt 500=15,5 --press 1800=233,300 --release 2500=233,300 \
        --shot "200=$out/$mode/marble-calibration.png" \
        --shot "1600=$out/$mode/marble-rolling.png" \
        --shot "2300=$out/$mode/marble-rewind.png" \
        --shot "2900=$out/$mode/marble-release.png"
    sim/build/orb_games_sim --game orbit --scene play --headless "$@" \
        --tap 800=350,300 --tap 1700=350,300 \
        --shot "500=$out/$mode/orbit-start.png" \
        --shot "850=$out/$mode/orbit-switch.png" \
        --shot "1750=$out/$mode/orbit-switch-again.png" \
        --shot "2600=$out/$mode/orbit-playing.png"
    sim/build/orb_games_sim --game ring-snake --scene play --headless --best 0 "$@" \
        --shot "700=$out/$mode/snake-start.png" \
        --shot "1700=$out/$mode/snake-straight.png" \
        --shot "2500=$out/$mode/snake-rim-warning.png" \
        --shot "3400=$out/$mode/snake-rim-loss.png"
    sim/build/orb_games_sim --game ring-snake --scene play --controls --headless --best 0 "$@" \
        --press 500=380,233 --press 1400=233,380 --press 2300=90,233 \
        --release 3000=90,233 --shot "1200=$out/$mode/snake-touch.png" \
        --shot "2200=$out/$mode/snake-turn.png"
    sim/build/orb_games_sim --game pop --scene play --headless "$@" \
        --bubble-tap 800 --bubble-tap 1600 --bubble-tap 2400 \
        --shot "500=$out/$mode/pop-start.png" \
        --shot "850=$out/$mode/pop-hit.png" \
        --shot "1650=$out/$mode/pop-combo.png" \
        --shot "2450=$out/$mode/pop-playing.png"
    for game in marble beads orbit ring-snake pop; do
        sim/build/orb_games_sim --game "$game" --scene paused --headless "$@" \
            --shot "900=$out/$mode/$game-paused.png"
    done
    sim/build/orb_games_sim --game beads --scene play --controls --headless "$@" \
        --press 500=70,233 --press 3000=396,233 --release 5500=396,233 \
        --shot "2800=$out/$mode/beads-touch-left.png" \
        --shot "5300=$out/$mode/beads-touch-right.png" \
        --shot "8000=$out/$mode/beads-touch-clock.png"
    sim/build/orb_games_sim --game beads --scene paused --controls --headless "$@" \
        --shot "900=$out/$mode/beads-touch-paused.png"
done
python3 sim/games4_sheets.py "$out"
