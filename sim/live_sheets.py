#!/usr/bin/env python3
"""Local Design frame (left) / real firmware simulator (right) live-value review."""

import csv
from pathlib import Path

from home_contact_sheets import SCENES as HOME_SCENES
from kit2_sheets import page, pair

BUILD = Path("sim/build")
OUT = BUILD / "live-review" / "comparisons"
D2 = Path("docs/design/2.0/png")
D1 = Path("docs/design/1.0/png")


def reference(name: str, group: str) -> Path:
    if group == "agents":
        if name.startswith("Usage"):
            return D2 / "Usage.png"
        if name.startswith("Agents"):
            return D2 / "Agents.png"
        return D1 / ("SessionLive.png" if name.startswith("SessionLive") else "Sessions.png")
    if group == "power":
        family = (
            "FaceOrb"
            if name.startswith("face-battery")
            else "Settings"
            if name.startswith("battery-")
            else "Lock"
        )
        return D2 / f"{family}.png"
    if group == "gesture-controls":
        return D2 / ("ControlVolume.png" if "volume" in name else "Control.png")
    if group == "sound-ui":
        if name in {"SoundVolume", "VolumeHUD"}:
            return D2 / "ControlVolume.png"
        if name.startswith("NowPlaying"):
            return D1 / "NowPlaying.png"
        base = name.split("-", 1)[0]
        return D1 / f"{base if (D1 / f'{base}.png').exists() else 'SoundSettings'}.png"
    if group == "faces":
        for family in (
            "FaceColor",
            "FaceSlots",
            "FaceGallery",
            "FaceActivity",
            "FaceMeridian",
            "FaceOrb",
            "Lock",
        ):
            if name.startswith(family):
                return D2 / f"{family}.png"
        return D2 / "FaceOrb.png"
    if group == "time":
        if name.startswith("reminders-") and not name.startswith("reminders-detail-"):
            return D1 / "Time.png"
        family = (
            "Stopwatch"
            if name.startswith("stopwatch")
            else "AlarmRinging"
            if "ringing" in name.lower()
            else "Alarm"
            if name.startswith("alarm-")
            else "Reminder"
            if name == "reminder-due" or name.startswith("reminders-detail-")
            else "Timer"
        )
        return (D2 if family == "Timer" else D1) / f"{family}.png"
    if group == "home":
        if name.startswith("room-thermostat-"):
            return D2 / "HomeRoom.png"
        family = next(
            (family for family, scenes in HOME_SCENES.items() if name in scenes),
            next(
                (family for family in HOME_SCENES if name.startswith(family)),
                "HomeLight",
            ),
        )
        return D2 / f"{family}.png"
    if group == "shell" and name == "charging":
        return D1 / "Charging.png"
    family = next(
        (
            v
            for k, v in {
                "agents": "Agents",
                "usage": "Usage",
                "notifications": "Notifications",
                "stack": "Stack",
                "charging": "Control",
                "downloading": "Updates",
                "updates": "Updates",
                "display": "Settings",
            }.items()
            if name.startswith(k)
        ),
        "Settings",
    )
    return D2 / f"{family}.png"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    entries = []
    for profile in ("default", "ax3-bold-rt"):
        required = {
            "gesture-controls": {
                "control-center",
                "brightness-clockwise",
                "brightness-endpoint",
                "display-brightness",
                "always-on-brightness",
                "volume-clockwise",
                "volume-endpoint",
                "volume",
            },
            "sound-ui": {"SoundSettings", "SoundVolume", "VolumeHUD", "Battery", "NowPlaying"},
            "agents": {"Agents", "Usage", "UsageLiveBoundary", "SessionLive", "Sessions"},
            "power": {
                "battery-low",
                "battery-critical",
                "face-battery-low",
                "face-battery-critical",
            },
            "home": {scene for scenes in HOME_SCENES.values() for scene in scenes}
            | {
                "cover",
                "cover-zero",
                "cover-full",
                "room-thermostat-negative",
                "room-thermostat-three-digit",
            },
            "time": {
                "timer-running",
                "timer-longduration",
                "stopwatch-running",
                "stopwatch-laps",
                "alarm-idle",
                "AlarmRinging",
                "reminders-list",
            },
            "faces": {
                "FaceOrb-default",
                "FaceMeridian-default",
                "FaceActivity-default",
                "Lock-default",
            },
        }
        for group, expected in required.items():
            source = (
                BUILD / f"agents-{profile}"
                if group == "agents"
                else BUILD / group / profile
                if group in {"gesture-controls", "sound-ui"}
                else BUILD / "live-review" / group / profile
            )
            captures = sorted(source.glob("*.png"))
            missing = expected - {path.stem for path in captures}
            if missing:
                raise FileNotFoundError(f"{source}: missing required captures: {sorted(missing)}")
            for actual in captures:
                if actual.stem.startswith(("FAILED-", "contact-", "review-")):
                    continue
                if group == "agents" and not actual.stem.startswith(
                    ("Agents", "Usage", "SessionLive", "Sessions")
                ):
                    continue
                if group == "home" and actual.stem not in expected:
                    continue  # Retired/manual captures are not fresh evidence.
                entries.append((profile, group, actual.stem, actual))
        for scene in (
            "agents",
            "usage",
            "notifications",
            "stack",
            "stack-timer",
            "stack-stopwatch",
            "stack-usage",
            "charging",
            "downloading",
            "updates",
            "display",
        ):
            actual = BUILD / f"shell-{scene}-{profile}.png"
            if not actual.exists():
                raise FileNotFoundError(f"Missing required shell capture: {actual}")
            entries.append((profile, "shell", scene, actual))
        actual = BUILD / f"notifications-sources-{profile}.png"
        if not actual.exists():
            raise FileNotFoundError(f"Missing notification source capture: {actual}")
        entries.append((profile, "shell", "notifications-sources", actual))
    rows = []
    for profile in ("default", "ax3-bold-rt"):
        dest = OUT / profile
        dest.mkdir(exist_ok=True)
        paths = []
        for _, group, state, actual in (e for e in entries if e[0] == profile):
            frame = reference(state, group)
            if not frame.exists():
                raise FileNotFoundError(frame)
            target = dest / f"{group}-{state}.png"
            pair(frame, actual, target)
            paths.append(target)
            rows.append(
                (
                    profile,
                    group,
                    state,
                    str(frame),
                    str(actual),
                    str(target),
                    str(dest / f"contact-{(len(paths) - 1) // 4 + 1:02}.png"),
                    "family reference; retired 270-degree gauge replaced by common bottom arc"
                    if frame.stem == "HomeLight"
                    or (group == "sound-ui" and state in {"SoundVolume", "VolumeHUD"})
                    else "family reference; shared kit rules override inherited geometry",
                )
            )
        for offset in range(0, len(paths), 4):
            page(paths[offset : offset + 4], dest / f"contact-{offset // 4 + 1:02}.png")
    with (OUT / "manifest.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t")
        writer.writerow(
            (
                "profile",
                "family",
                "state",
                "frame-left",
                "simulator-right",
                "pair",
                "contact",
                "comparison",
            )
        )
        writer.writerows(rows)
    print(f"Verified {len(rows)} exact frame-left/simulator-right pairs in {OUT}")


if __name__ == "__main__":
    main()
