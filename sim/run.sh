#!/usr/bin/env bash
# Builds Orb's desktop simulator (sim/) and runs it, in a window unless
# --headless is given. Arguments go to orb_sim (orb_sim --help lists them).
set -euo pipefail

sim="$(cd "$(dirname "$0")" && pwd)"
build="$sim/build"

if ! pkg-config --exists sdl2 2>/dev/null; then
    if ! command -v brew >/dev/null; then
        echo "run.sh: SDL2 is missing, and Homebrew to install it" >&2
        exit 1
    fi
    echo "run.sh: installing SDL2 with Homebrew" >&2
    brew install sdl2
fi

if [[ ! -f "$build/CMakeCache.txt" ]]; then
    generator=()
    if command -v ninja >/dev/null; then
        generator=(-G Ninja)
    fi
    cmake -S "$sim" -B "$build" "${generator[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
fi
# Quiet when it works, the whole log when it doesn't.
if ! log="$(cmake --build "$build" --target orb_sim 2>&1)"; then
    echo "$log" >&2
    exit 1
fi

exec "$build/orb_sim" "$@"
