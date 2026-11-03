#!/usr/bin/env python3
"""Refresh the Home, Time and Faces live-value evidence from the local build.

Run after building: nice -n 10 python3 sim/live_capture.py
All fixtures are synthetic. Captures run sequentially to leave the Mac's live
services headroom; this script performs no build, network or device access.
"""

import argparse
import json
import re
import subprocess
from pathlib import Path

from home_contact_sheets import SCENES


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("sim/build"))
    parser.add_argument("--out", type=Path, default=Path("sim/build/live-review"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    scenes = tuple(dict.fromkeys(scene for group in SCENES.values() for scene in group))
    scenes += (
        "cover",
        "cover-zero",
        "cover-full",
        "room-thermostat-negative",
        "room-thermostat-three-digit",
    )
    receipts = []
    for profile, flags in (("default", []), ("ax3-bold-rt", ["ax3", "bold", "rt"])):
        for group in ("home", "time", "faces"):
            (args.out / group / profile).mkdir(parents=True, exist_ok=True)
        commands = [
            (
                f"home/{profile}/{scene}",
                [
                    str(args.build / "orb_home_sim"),
                    scene,
                    str(args.out / "home" / profile / f"{scene}.png"),
                    *flags,
                ],
            )
            for scene in scenes
        ]
        commands.extend(
            [
                (
                    f"time/{profile}",
                    [
                        str(args.build / "orb_time_sim"),
                        str(args.out / "time" / profile),
                        *flags,
                        "capture",
                    ],
                ),
                (
                    f"faces/{profile}",
                    [
                        str(args.build / "test_faces_layout"),
                        str(args.out / "faces" / profile),
                        profile,
                    ],
                ),
            ]
        )
        for index, (name, command) in enumerate(commands):
            log = args.out / f"{name.replace('/', '_')}.capture.log"
            with log.open("w") as stream:
                result = subprocess.run(
                    command, stdout=stream, stderr=subprocess.STDOUT, check=False
                )
            content = log.read_text()
            # Time's atlas keeps capturing after a failed fit so all defects
            # are reviewable. Its diagnostic receipt, not exit zero, gates it.
            diagnostics = re.findall(r"(\d+) failed|(?<!\d)(\d+) layout failures", content)
            failed = result.returncode != 0 or any(int(a or b) for a, b in diagnostics)
            receipts.append(
                {
                    "name": name,
                    "command": command,
                    "status": result.returncode,
                    "failed": failed,
                    "log": str(log),
                }
            )
            if failed:
                print(f"FAILED {name}: {log}", flush=True)
            elif index % 20 == 0:
                print(f"Captured {profile}: {index + 1}/{len(commands)}", flush=True)
    (args.out / "capture-receipts.json").write_text(json.dumps(receipts, indent=2) + "\n")
    failures = sum(receipt["failed"] for receipt in receipts)
    print(f"Live capture: {len(receipts)} commands, {failures} failures; {args.out}")
    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
