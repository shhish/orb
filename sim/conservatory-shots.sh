#!/bin/sh
# Fake board only: production Conservatory logic/view and real pointer events.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
binary=${ORB_GAMES_SIM:-"$sim_dir/build/orb_games_sim"}
out=${1:-"$sim_dir/build/conservatory"}

capture() {
    mode=$1
    shift
    mkdir -p "$out/$mode/rewind" "$out/$mode/pause" "$out/$mode/bloom" "$out/$mode/stages" "$out/$mode/garden-flow"
    for scene in start play paused end finished cleared garden garden-empty; do
        shot_ms=900
        if [ "$scene" = cleared ]; then shot_ms=1600; fi
        "$binary" --game marble --headless --scene "$scene" \
            --shot "$shot_ms=$out/$mode/$scene.png" "$@"
    done
    for scene in paused cleared garden; do
        shot_ms=900
        if [ "$scene" = cleared ]; then shot_ms=1600; fi
        "$binary" --game marble --headless --scene "$scene" --save-error \
            --shot "$shot_ms=$out/$mode/$scene-save-error.png" "$@"
    done
    "$binary" --game marble --headless --scene paused \
        --tap 500=233,325 --shot "900=$out/$mode/paused-toggle.png" "$@"
    for stage in 2 3; do
        "$binary" --game marble --headless --scene play --stage "$stage" \
            --shot "900=$out/$mode/stage-$stage.png" "$@"
    done
    "$binary" --game marble --headless --scene garden --keepsakes 12 \
        --shot "900=$out/$mode/garden-full.png" "$@"
    "$binary" --game marble --headless --scene play --controls \
        --shot "900=$out/$mode/controls.png" "$@"
    "$binary" --game marble --headless --scene play \
        --shot "3400=$out/$mode/play-glyph.png" "$@"
    "$binary" --game marble --headless --scene play \
        --tilt 400=25,-15 \
        --press 1800=420,300 --release 3000=420,300 \
        --shot "900=$out/$mode/rewind/00-forward.png" \
        --shot "1750=$out/$mode/rewind/01-before.png" \
        --shot "2250=$out/$mode/rewind/02-hold.png" \
        --shot "2400=$out/$mode/rewind/03-retrace.png" \
        --shot "2650=$out/$mode/rewind/04-retrace.png" \
        --shot "2980=$out/$mode/rewind/05-retrace.png" \
        --shot "3035=$out/$mode/rewind/06-release.png" \
        --shot "3400=$out/$mode/rewind/07-forward.png" "$@"
    "$binary" --game marble --headless --scene play \
        --tap 800=420,300 --tap 1800=233,399 \
        --tap 2800=420,300 --tap 3800=233,155 \
        --shot "600=$out/$mode/pause/00-play.png" \
        --shot "1400=$out/$mode/pause/01-paused.png" \
        --shot "2400=$out/$mode/pause/02-resumed.png" \
        --shot "3400=$out/$mode/pause/03-paused.png" \
        --shot "4400=$out/$mode/pause/04-garden.png" "$@"
    "$binary" --game marble --headless --scene play --reward 900 \
        --shot "890=$out/$mode/bloom/00-before.png" \
        --shot "935=$out/$mode/bloom/01-arrival.png" \
        --shot "1000=$out/$mode/bloom/02-unfurl.png" \
        --shot "1100=$out/$mode/bloom/03-bloom.png" \
        --shot "1235=$out/$mode/bloom/04-open.png" \
        --shot "1300=$out/$mode/bloom/05-lift.png" \
        --shot "1450=$out/$mode/bloom/06-flight.png" \
        --shot "1600=$out/$mode/bloom/07-land.png" \
        --shot "2450=$out/$mode/bloom/08-rest.png" "$@"
    "$binary" --game marble --headless --scene play --reward 900 \
        --tap 2600=233,399 \
        --shot "600=$out/$mode/stages/00-play.png" \
        --shot "935=$out/$mode/stages/01-planted.png" \
        --shot "1100=$out/$mode/stages/02-settle.png" \
        --shot "2450=$out/$mode/stages/03-keepsake.png" \
        --shot "2650=$out/$mode/stages/04-next.png" \
        --shot "3250=$out/$mode/stages/05-stage-two.png" "$@"
    "$binary" --game marble --headless --keepsakes 6 \
        --tap 900=233,329 --tap 1900=233,399 \
        --shot "600=$out/$mode/garden-flow/00-start.png" \
        --shot "950=$out/$mode/garden-flow/01-open.png" \
        --shot "1700=$out/$mode/garden-flow/02-garden.png" \
        --shot "1950=$out/$mode/garden-flow/03-return.png" \
        --shot "2500=$out/$mode/garden-flow/04-start.png" "$@"
}

capture default
capture ax3-bold-rt --text-size ax3 --bold --reduce-transparency
capture reduce-motion --reduce-motion
printf 'Conservatory simulator evidence: %s\n' "$out"
