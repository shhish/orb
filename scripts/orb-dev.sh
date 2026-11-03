#!/usr/bin/env bash
# Talk to orb_devtools on the board (on the MacBook Pro's USB) from here.
#
#   scripts/orb-dev.sh shot [out.png]         screenshot (default: shots/<time>.png)
#   scripts/orb-dev.sh tap X Y
#   scripts/orb-dev.sh hold X Y MS
#   scripts/orb-dev.sh swipe X1 Y1 X2 Y2 MS
#   scripts/orb-dev.sh arc R A0 A1 MS         circular touch; 0° top, clockwise positive
#   scripts/orb-dev.sh tilt X Y MS           specific-force g, game input only; wakes first
#   scripts/orb-dev.sh top                    CPU per task over one second
#   scripts/orb-dev.sh power                  clock policy and mode residency
#   scripts/orb-dev.sh bench MS               bounded live renderer stress scene
#   scripts/orb-dev.sh bench_transition MS    synthetic two-screen transition baseline
#   scripts/orb-dev.sh bench_quiet MS         synthetic quiet screen after SPI drain
#   scripts/orb-dev.sh bench_open SURFACE     play|dot|agents|apps|back; repeat for warm
#   scripts/orb-dev.sh bench_game ID MS       marble|orbit|ring-snake|pop; 1000..60000 ms
#   scripts/orb-dev.sh bench_nav GESTURE MS   cached slide/layer/back/iris/rotary; 1000..60000 ms
#   scripts/orb-dev.sh heap                   memory by kind
#   scripts/orb-dev.sh wake                   wake an idle display (injected input never does)
#   scripts/orb-dev.sh goto SURFACE           jump to face, apps, agents, home, time, play, settings, sound, ...
#   scripts/orb-dev.sh --wifi [--device DEVICE] shot [out.png]
#   scripts/orb-dev.sh --wifi [--device DEVICE] tap|hold|swipe|arc|tilt|bench|heap|top|power ...
# Wi-Fi uses orb-hub's private control socket and the paired TLS link. ORB_DEVICE
# selects a device; otherwise exactly one online dev-channel device is required.
# ORB_HUB_BIN may name an alternate installed orb-hub executable.
#
# Coordinates are panel pixels, 0-465 each way.
# Arc: centre (233,233), R 1..232 px, A0/A1 -3600..3600°, MS 1..5000.
# Angles are unwrapped: `arc 216 350 370 400` crosses top clockwise;
# `arc 216 160 200 400` follows the brightness arc clockwise. Gestures wake first.
# Tilt: finite X/Y -1..1g, X²+Y² <= 1, MS 1..5000; z is positive sqrt(1-X²-Y²).
# It expires automatically, never affects power/tap/twist, and exists only with devtools.
# Start a tilt game, hold `tilt 0 0 600` through neutral calibration, then e.g.
# `tilt -0.20 0 1000` rolls right; `tilt 0 0.20 1000` rolls down. Resume re-zeros.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

[[ $# -ge 1 ]] || { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

if [[ "$1" == --wifi ]]; then
    shift
    hub="${ORB_HUB_BIN:-orb-hub}"
    device="${ORB_DEVICE:-}"
    if [[ "${1:-}" == --device ]]; then
        [[ $# -ge 3 && -n "$2" ]] || { echo 'usage: orb-dev.sh --wifi --device DEVICE COMMAND ...' >&2; exit 2; }
        device="$2"
        shift 2
    fi
    [[ $# -ge 1 ]] || { echo 'usage: orb-dev.sh --wifi [--device DEVICE] COMMAND ...' >&2; exit 2; }
    if [[ -z "$device" ]]; then
        device="$("$hub" devices --json | python3 -c '
import json, sys
devices = [d["device_id"] for d in json.load(sys.stdin)["devices"]
           if d.get("online") is True and d.get("channel") == "dev"]
if len(devices) != 1:
    sys.exit("Wi-Fi devtools needs exactly one online dev device; set ORB_DEVICE or --device.")
print(devices[0])
')"
    fi
    if [[ "$1" == shot ]]; then
        [[ $# -le 2 ]] || { echo 'usage: orb-dev.sh --wifi shot [out.png]' >&2; exit 2; }
        out="${2:-$ROOT/shots/$(date +%Y%m%d-%H%M%S).png}"
        mkdir -p "$(dirname "$out")"
        exec "$hub" dev "$device" shot "$out"
    fi
    exec "$hub" dev "$device" "$@"
fi

if [[ "$1" == shot ]]; then
    out="${2:-$ROOT/shots/$(date +%Y%m%d-%H%M%S).png}"
    mkdir -p "$(dirname "$out")"
    trap 'rm -f "$out.part"' EXIT
    "$ROOT/scripts/orb-transport.sh" shot >"$out.part"
    mv "$out.part" "$out"
    echo "$out"
else
    "$ROOT/scripts/orb-transport.sh" "$@"
fi
