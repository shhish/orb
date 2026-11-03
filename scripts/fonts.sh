#!/usr/bin/env bash
# Orb's fonts (firmware/components/orb_fonts, see its README.md).
#
#   scripts/fonts.sh                   regenerate fonts, tables, README sizes and previews
#   scripts/fonts.sh --no-previews     the same without docs/screens/fonts
#   scripts/fonts.sh check             run the self-test against LVGL on this Mac
#   scripts/fonts.sh status            redraw live status / face previews with LVGL
#   scripts/fonts.sh assets --version <uint32> [-o <signed.bin>]  generate PCM palette, wrap/sign fonts + sounds
#   scripts/fonts.sh test-app build    idf.py in the component's test app (also size-components, ...)
#
# Needs python3.13 and Node. Python packages (hash-pinned in requirements.txt) live in
# tools/fonts/.venv, lv_font_conv (pinned by package-lock.json) in tools/fonts/node_modules
# and downloads in tools/fonts/.cache; each is reinstalled when its lock file changes.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
tools="$root/tools/fonts"

if [ "${1:-}" = "assets" ]; then
    shift
    exec "$root/scripts/sign-assets.sh" "$@"
fi

if [ "${1:-}" = "test-app" ]; then
    shift
    export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
    export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf6.1_py3.13_env"
    # shellcheck disable=SC1091
    . "$IDF_PATH/export.sh" >/dev/null
    exec idf.py -C "$root/firmware/components/orb_fonts/test_app" "$@"
fi

python="${ORB_FONTS_PYTHON:-python3.13}"
venv="$tools/.venv"
if [ ! -x "$venv/bin/python" ]; then
    "$python" -m venv "$venv"
fi
if ! cmp -s "$tools/requirements.txt" "$venv/.requirements"; then
    "$venv/bin/python" -m pip install --quiet --disable-pip-version-check --require-hashes -r "$tools/requirements.txt"
    cp "$tools/requirements.txt" "$venv/.requirements"
fi
command -v npm >/dev/null || { echo "fonts.sh: Node.js (npm) is needed for lv_font_conv" >&2; exit 1; }
if ! cmp -s "$tools/package-lock.json" "$tools/node_modules/.package-lock.orb"; then
    (cd "$tools" && npm ci --ignore-scripts --no-audit --no-fund --silent)
    cp "$tools/package-lock.json" "$tools/node_modules/.package-lock.orb"
fi

if [ "${1:-}" = "check" ]; then
    shift
    exec "$venv/bin/python" "$tools/lvgl_check.py" "$@"
fi
if [ "${1:-}" = "status" ]; then
    shift
    exec "$venv/bin/python" "$tools/lvgl_check.py" --status-only "$@"
fi
"$venv/bin/python" "$tools/build.py" "$@"
for arg in "$@"; do
    if [ "$arg" = "--no-previews" ] || [ "$arg" = "--help" ] || [ "$arg" = "-h" ]; then
        exit 0
    fi
done
exec "$venv/bin/python" "$tools/lvgl_check.py" --status-only
