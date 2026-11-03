#!/usr/bin/env bash
# Build a signed assets image using the same key as the running signed application.
# Usage: scripts/sign-assets.sh --version <uint32> [-o <assets-signed.bin>] [font-pack]
# Defaults: firmware/components/orb_fonts/pack/orb_fonts.pack -> firmware/build/orb_assets-signed.bin
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
version="" output="$root/firmware/build/orb_assets-signed.bin" pack=""
while (($#)); do
    case "$1" in
    --version)
        [[ $# -ge 2 ]] || { echo "sign-assets: --version requires a value" >&2; exit 2; }
        version="$2"; shift 2 ;;
    -o|--output)
        [[ $# -ge 2 ]] || { echo "sign-assets: -o requires a path" >&2; exit 2; }
        output="$2"; shift 2 ;;
    -*) echo "Usage: $0 --version <uint32> [-o <assets-signed.bin>] [font-pack]" >&2; exit 2 ;;
    *)
        [[ -z "$pack" ]] || { echo "sign-assets: only one font pack is accepted" >&2; exit 2; }
        pack="$1"; shift ;;
    esac
done
[[ -n "$version" ]] || { echo "sign-assets: --version is required" >&2; exit 2; }
pack="${pack:-$root/firmware/components/orb_fonts/pack/orb_fonts.pack}"
umask 077
scratch="$(mktemp -d "${TMPDIR:-/tmp}/orb-assets-sign.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
trap 'exit 130' HUP INT TERM
"${ORB_PYTHON:-python3.13}" "$root/tools/sounds/make_sounds.py"
"${ORB_PYTHON:-python3.13}" "$root/tools/fonts/assets.py" envelope "$pack" \
    --sounds "$root/tools/sounds/out/palette.orbs" \
    --version "$version" --output "$scratch/unsigned.bin"
"$root/scripts/sign-app.sh" --assets-envelope "$scratch/unsigned.bin" -o "$output"
"${ORB_PYTHON:-python3.13}" "$root/tools/fonts/assets.py" inspect --signed "$output"
