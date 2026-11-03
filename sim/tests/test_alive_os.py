"""Whole-OS idle guard using the parity catalogue's production presenters.

One fresh process per fixture avoids retained model and storage state. The
representative set includes every catalogue family and additional value,
outcome, empty, restore and named-wait states across the apps. Active gameplay
is deliberately represented by its picker, pause and result surfaces: moving
gameplay is an explicit interaction, not an idle arrival.
"""

import argparse
import json
import subprocess
from pathlib import Path

# These are real state words, not presentation exemptions. Each active check
# waits out the same 30 s budget, then demands a stopped clock and zero frames.
WAITS = {
    "agents.loading",
    "control.home.loading",
    "idle.loading",
    "session.loading",
    "session.waiting",
    "dot.loading",
    "dot.empty",
    "dot.checking",
    "receipt.pending",
    "receipt.recorded",
    "voice.starting",
    "voice.transcribing",
    "voice.sending",
    "send.queued",
    "spoken.buffering",
    "spoken.empty",
    "settings.temperature.pending",
    "settings.sources.pending",
    "settings.sound.saving",
    "settings.quiet.saving",
    "updates.ordinary.pending",
    "updates.mandatory.verifying",
    "updates.mandatory.ready",
    "system.starting",
    "system.setup.loading",
    "system.setup.connecting",
    "system.pairing.loading",
    "home-rooms-loading",
    "home-scene-pending",
    "home-light-pending",
    "home-light-scenes-pending",
    "home-light-waiting",
    "timer-pending",
    "alarm-pending",
    "reminders-pending",
    "alarm-no-clock",
    "weather.loading",
}

# Local timer/stopwatch second ticks may render, but may never keep the shared
# motion clock running between seconds. Every other case requires zero frames.
TICKING = {"timer-running", "stopwatch-running"}

EXTRA = {
    "apps.default",
    "stack.empty",
    "stack.timer.paused",
    "notifications.empty",
    "control.default",
    "control.volume",
    "control.home",
    "control.silent",
    "control.brightness.turn",
    "agents.empty",
    "agents.default",
    "agents.step-counts",
    "sessions.default",
    "session.default",
    "usage.default",
    "usage.codex",
    "usage.unknown",
    "usage.maximum",
    "dot.default",
    "dot.empty",
    "approval.default",
    "question.default",
    "receipt.delivered",
    "receipt.failed",
    "voice.review",
    "send.delivered",
    "send.failed",
    "settings.display",
    "settings.sound.volume",
    "settings.battery",
    "settings.battery.unknown",
    "updates.mandatory.downloading",
    "updates.mandatory.ready",
    "onboarding.default",
    "power-sheet.default",
    "compass.default",
    "offline.default",
    "face-orb",
    "face-meridian",
    "face-activity",
    "face-orb-unknown",
    "face-orb-weather",
    "face-orb-gallery",
    "home-rooms",
    "home-rooms-empty",
    "home-room",
    "home-room-restore",
    "home-climate",
    "home-sensors",
    "home-scenes",
    "home-light",
    "home-thermostat",
    "home-switch",
    "home-lock",
    "home-fan",
    "home-cover",
    "home-temperature",
    "home-humidity",
    "home-light-reading-unknown",
    "timer-idle",
    "timer-paused",
    "timer-idle-restore",
    "stopwatch-idle",
    "stopwatch-paused",
    "stopwatch-laps-paused",
    "alarm-idle",
    "alarm-enabled",
    "alarm-options",
    "reminders-empty",
    "reminders-list",
    "reminders-detail",
    "play-picker",
    "marble-pause",
    "orbit-pause",
    "snake-pause",
    "pop-pause",
    "beads-pause",
    "marble-result",
    "orbit-result",
    "snake-result",
    "pop-result",
    "weather.current",
    "weather.unavailable",
    "weather.from-face-back",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--mode", choices=("active", "dimmed", "always-on"), required=True)
    args = parser.parse_args()
    catalogue = json.loads(subprocess.check_output([str(args.binary), "--list"], text=True))
    ids = {case["id"] for case in catalogue}
    # First ordinary fixture in every family also protects new app families
    # from being silently omitted. Transition timing is covered separately by
    # the parity capture gate; this guard checks arrived, idle surfaces.
    first = {}
    for case in catalogue:
        if case["id"].startswith(("transition.", "privacy.")):
            continue
        first.setdefault(case["family"], case["id"])
    selected = set(first.values()) | WAITS | TICKING | EXTRA
    missing = selected - ids
    if missing:
        raise SystemExit(f"Alive idle guard references absent scenarios: {sorted(missing)}")
    failures = []
    for scenario in sorted(selected):
        mode = args.mode
        if mode == "active" and scenario in WAITS:
            mode = "active-wait"
        elif scenario in TICKING:
            # Quiet policy also suppresses tweening; a direct Time fixture
            # still receives its local authoritative second ticks.
            mode = "active-ticking" if mode == "active" else f"{mode}-ticking"
        result = subprocess.run(
            [str(args.binary), "--settle-check", scenario, mode],
            text=True,
            capture_output=True,
            check=False,
            timeout=30,
        )
        if result.returncode:
            failures.append(f"{scenario}/{mode}:\n{result.stdout}\n{result.stderr}")
    if failures:
        raise SystemExit("\n".join(failures))
    print(
        f"Alive whole-OS {args.mode}: {len(selected)} representative scenarios, "
        f"{len(first)} catalogue families; stopped clocks and two idle seconds checked."
    )


if __name__ == "__main__":
    main()
