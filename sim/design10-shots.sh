#!/usr/bin/env bash
# Design 1.0 contact sheets: design PNG | firmware render, per screen, in
# Default and AX3 + Bold + Reduce Transparency.
set -eu
sim_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
build="$sim_dir/build"
design="$sim_dir/../docs/design/1.0/png"
out=${1:-"$sim_dir/../work/design-1.0"}
python=${PYTHON:-python3}
outputs=()
mkdir -p "$out"

check_png() {
    "$python" "$sim_dir/png_tools.py" check "$@"
}

pair() { # out_png design_png render_png
    local out_png=$1 design_png=$2 render_png=$3
    # Read the PNGs directly: the old sips/BMP path wrote 32-bit, top-down BMPs
    # that bmp_concat misread and saved under a .png name.
    "$python" "$sim_dir/png_tools.py" concat "$design_png" "$render_png" "$out_png"
    check_png "$out_png"
    outputs+=("$out_png")
}

shot() { # out_png game scene best score variant_args...
    out_png=$1; game=$2; scene=$3; best=$4; score=$5; shift 5
    "$build/orb_games_sim" --game "$game" --headless --scene "$scene" \
        --best "$best" --score "$score" --design-frame "$@" --shot "4000=$out_png"
}

for variant in default ax3-bold-rt; do
    play_args=()
    game_args=()
    if [ "$variant" = ax3-bold-rt ]; then
        play_args=(--ax3 --bold --rt)
        game_args=(--text-size ax3 --bold --reduce-transparency)
    fi
    # Play picker (design Play.png shows the centred Marble card).
    "$build/orb_play_sim" ${play_args[@]+"${play_args[@]}"} --index 0 --shot "$out/play-$variant-render.png"
    pair "$out/play-$variant.png" "$design/Play.png" "$out/play-$variant-render.png"
    rm -f "$out/play-$variant-render.png"
    # Marble, Orbit, Ring Snake, Pop play scenes.
    shot "$out/marble-$variant-render.png" marble play 0 0 ${game_args[@]+"${game_args[@]}"}
    pair "$out/marble-$variant.png" "$design/Marble.png" "$out/marble-$variant-render.png"
    rm -f "$out/marble-$variant-render.png"
    shot "$out/orbit-$variant-render.png" orbit play 31 12 ${game_args[@]+"${game_args[@]}"}
    pair "$out/orbit-$variant.png" "$design/Orbit.png" "$out/orbit-$variant-render.png"
    rm -f "$out/orbit-$variant-render.png"
    shot "$out/ring-snake-$variant-render.png" ring-snake play 31 24 ${game_args[@]+"${game_args[@]}"}
    pair "$out/ring-snake-$variant.png" "$design/RingSnake.png" "$out/ring-snake-$variant-render.png"
    rm -f "$out/ring-snake-$variant-render.png"
    shot "$out/pop-$variant-render.png" pop play 114 128 ${game_args[@]+"${game_args[@]}"}
    pair "$out/pop-$variant.png" "$design/Pop.png" "$out/pop-$variant-render.png"
    rm -f "$out/pop-$variant-render.png"
    # Beads settled into 10:09 after 4 s of stillness.
    "$build/orb_games_sim" --game beads --headless --scene play ${game_args[@]+"${game_args[@]}"} --shot "4000=$out/beads-$variant-render.png"
    pair "$out/beads-$variant.png" "$design/Beads.png" "$out/beads-$variant-render.png"
    rm -f "$out/beads-$variant-render.png"
    # GameOver (Pop accent, new best 128 over last best 114).
    "$build/orb_games_sim" --game pop --headless --scene end --best 114 --score 128 ${game_args[@]+"${game_args[@]}"} --shot "600=$out/game-over-$variant-render.png"
    pair "$out/game-over-$variant.png" "$design/GameOver.png" "$out/game-over-$variant-render.png"
    rm -f "$out/game-over-$variant-render.png"
done
# Fail the run if any sheet is missing, malformed, or entirely black.
check_png ${outputs[@]+"${outputs[@]}"}
printf 'Design 1.0 contact sheets: %s\n' "$out"
