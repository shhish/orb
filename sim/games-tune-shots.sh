#!/bin/sh
# Design 1.0 | real firmware renders. No board, hub, or owner state is touched.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$root/sim/build/games-tune"}
binary="$root/sim/build/orb_games_sim"
mkdir -p "$out/default" "$out/ax3-bold-rt-rm" "$out/sheets"

capture() {
    variant=$1
    shift
    for game in marble beads orbit ring-snake pop; do
        case "$game" in
            marble) frame=Marble; score=0 ;;
            beads) frame=Beads; score=0 ;;
            orbit) frame=Orbit; score=12 ;;
            ring-snake) frame=RingSnake; score=24 ;;
            pop) frame=Pop; score=128 ;;
        esac
        best=120
        if [ "$game" = ring-snake ]; then best=31; fi
        for scene in start play paused; do
            shot_at=800
            if [ "$game" = beads ] && [ "$scene" = play ]; then shot_at=4000; fi
            "$binary" --game "$game" --headless --scene "$scene" --design-frame \
                --score "$score" --best "$best" --shot "$shot_at=$out/$variant/$game-$scene.png" "$@"
        done
        python3 "$root/sim/png_tools.py" concat "$root/docs/design/1.0/png/$frame.png" \
            "$out/$variant/$game-play.png" "$out/sheets/$game-play-$variant.png"
        case "$game" in
            orbit|ring-snake|pop)
                "$binary" --game "$game" --headless --scene end --score 128 --best 96 \
                    --shot "800=$out/$variant/$game-end.png" "$@"
                python3 "$root/sim/png_tools.py" concat "$root/docs/design/1.0/png/GameOver.png" \
                    "$out/$variant/$game-end.png" "$out/sheets/$game-end-$variant.png"
                ;;
        esac
    done
    "$binary" --game beads --headless --scene play --shot "4000=$out/$variant/beads-clock.png" "$@"
    "$binary" --game beads --headless --scene play --tilt 400=20,10 --tilt 1000=0,0 \
        --shot "999=$out/$variant/beads-flow.png" \
        --shot "3200=$out/$variant/beads-settling-early.png" \
        --shot "3500=$out/$variant/beads-settling-late.png" "$@"
}

capture default
capture ax3-bold-rt-rm --text-size ax3 --bold --reduce-transparency --reduce-motion
for game in marble beads orbit ring-snake pop; do
    for scene in start paused; do
        python3 "$root/sim/png_tools.py" concat "$out/default/$game-$scene.png" \
            "$out/ax3-bold-rt-rm/$game-$scene.png" "$out/sheets/$game-$scene-modes.png"
    done
done
python3 "$root/sim/png_tools.py" concat "$out/default/beads-clock.png" \
    "$out/ax3-bold-rt-rm/beads-clock.png" "$out/sheets/beads-clock-modes.png"
for state in flow settling-early settling-late; do
    python3 "$root/sim/png_tools.py" concat "$out/default/beads-$state.png" \
        "$out/ax3-bold-rt-rm/beads-$state.png" "$out/sheets/beads-$state-modes.png"
done
printf 'Games tuning sheets: %s\n' "$out/sheets"
