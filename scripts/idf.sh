#!/usr/bin/env bash
# Run idf.py with the ESP-IDF v6.1 install in ~/esp (its Python env was built with python3.13).
# Flash and monitor with scripts/remote.sh (the board is on the MacBook Pro).
set -euo pipefail

export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf6.1_py3.13_env"

# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" >/dev/null
cd "$(dirname "$0")/../firmware"

# Preserve the familiar -D PROJECT_VER=... spelling without storing release
# identity in CMakeCache.txt. Only this invocation inherits the override.
idf_args=()
orb_channel=dev
orb_jobs=4
while (($#)); do
    case "$1" in
        -j|--jobs)
            if (($# < 2)); then
                echo "-j/--jobs requires an integer from 1 to 4" >&2
                exit 2
            fi
            orb_jobs="$2"
            shift 2
            continue
            ;;
        -j*|--jobs=*)
            orb_jobs="${1#-j}"
            orb_jobs="${orb_jobs#--jobs=}"
            shift
            continue
            ;;
        --channel)
            if (($# < 2)) || [[ "$2" != dev && "$2" != stable ]]; then
                echo "--channel requires dev or stable" >&2
                exit 2
            fi
            orb_channel="$2"
            shift 2
            continue
            ;;
        -D|--define-cache-entry)
            if (($# >= 2)) && [[ "$2" == PROJECT_VER=* || "$2" == PROJECT_VER:*=* ]]; then
                export ORB_PROJECT_VER="${2#*=}"
                shift 2
                continue
            fi
            ;;
        -DPROJECT_VER=*|-DPROJECT_VER:*=*|--define-cache-entry=PROJECT_VER=*|--define-cache-entry=PROJECT_VER:*=*)
            orb_version_argument="${1#--define-cache-entry=}"
            export ORB_PROJECT_VER="${orb_version_argument#*=}"
            shift
            continue
            ;;
    esac
    idf_args+=("$1")
    shift
done

# IDF 6.1 invokes Ninja directly and reads IDF_PY_BUILD_JOBS, while nested
# cmake --build commands read CMAKE_BUILD_PARALLEL_LEVEL. Bound both because
# this Mac also hosts the owner's live service.
if [[ ! "$orb_jobs" =~ ^[1-4]$ ]]; then
    echo "-j/--jobs requires an integer from 1 to 4" >&2
    exit 2
fi
export IDF_PY_BUILD_JOBS="$orb_jobs"
export CMAKE_BUILD_PARALLEL_LEVEL="$orb_jobs"

# Stable configuration is isolated from the saved development sdkconfig. The
# CMake guard also rejects a stale or manually overridden config with devtools.
if [[ "$orb_channel" == stable ]]; then
    idf_args=(-B build-stable
        -D "SDKCONFIG=$PWD/build-stable/sdkconfig"
        -D "SDKCONFIG_DEFAULTS=$PWD/sdkconfig.defaults;$PWD/sdkconfig.stable"
        "${idf_args[@]}")
fi

# Dev builds share one compile cache across worktrees. Stable builds compile fresh.
if [[ "$orb_channel" == dev ]]; then
    export IDF_CCACHE_ENABLE=1
    CCACHE_BASEDIR="$(cd .. && pwd)"
    export CCACHE_BASEDIR
    export CCACHE_NOHASHDIR=1
else
    export IDF_CCACHE_ENABLE=0
fi

orb_revision="$(git -c maintenance.auto=false rev-parse --short=7 HEAD 2>/dev/null || true)"
orb_version="${ORB_PROJECT_VER:-0.3.0-${orb_revision:-unknown}}"
exec idf.py -D "ORB_BUILD_VERSION=$orb_version" -D "ORB_BUILD_CHANNEL=$orb_channel" "${idf_args[@]}"
