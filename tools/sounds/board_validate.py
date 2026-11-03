#!/usr/bin/env python3
"""Lead-only device validation; never flashes, publishes or contacts board two.

Run after the lead installs a matched signed app/assets pair. The default
output is work/sound/board-run-1. --plan is entirely offline.
"""

import argparse
import base64
import json
import os
import re
import subprocess
import time
import wave
import zlib
from pathlib import Path


def _load_dev_env() -> None:
    """Personal dev settings live outside the repo in ~/.config/orb/dev.env
    (KEY=VALUE lines). Variables already set in the environment win."""
    path = Path(os.environ.get("ORB_DEV_ENV", Path.home() / ".config/orb/dev.env"))
    if path.is_file():
        for line in path.read_text().splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip().isidentifier() and not line.lstrip().startswith("#"):
                os.environ.setdefault(key.strip(), value.strip())


_load_dev_env()

ROOT = Path(__file__).resolve().parents[2]
SOUND = re.compile(r"@@ORBDEV SOUND (.+)")
MIC_HEADER = re.compile(
    r"@@ORBDEV MIC rate=16000 channels=2 frames=80000 bytes=320000 crc32=([0-9a-f]{8})"
)
MIC_CHUNK = re.compile(r"@@ORBDEV MIC_CHUNK seq=(\d+) data=([A-Za-z0-9+/]+={0,2})")
MIC_END = re.compile(r"@@ORBDEV MIC_END chunks=(\d+) bytes=320000 crc32=([0-9a-f]{8})")


def decode_microphones(text: str) -> bytes:
    """Reject missing/duplicate/reordered/truncated chunks before writing WAV."""
    crc = None
    ended = False
    raw = bytearray()
    sequence = 0
    for line in text.splitlines():
        if match := MIC_HEADER.fullmatch(line):
            if crc is not None:
                raise ValueError("duplicate microphone header")
            crc = int(match[1], 16)
        elif match := MIC_CHUNK.fullmatch(line):
            if crc is None or ended or int(match[1]) != sequence:
                raise ValueError("microphone chunks are out of order")
            chunk = base64.b64decode(match[2], validate=True)
            if len(chunk) != min(192, 320000 - len(raw)) or not chunk:
                raise ValueError("microphone chunk has wrong length")
            raw.extend(chunk)
            sequence += 1
        elif match := MIC_END.fullmatch(line):
            if ended or int(match[1]) != sequence or int(match[2], 16) != crc:
                raise ValueError("invalid microphone trailer")
            ended = True
        elif line.startswith("@@ORBDEV MIC"):
            raise ValueError("malformed microphone transfer")
    if not ended or len(raw) != 320000 or zlib.crc32(raw) != crc:
        raise ValueError("microphone transfer is incomplete or CRC failed")
    return bytes(raw)


def sound_status(text: str) -> dict[str, int]:
    for line in text.splitlines():
        if match := SOUND.fullmatch(line):
            return {
                key: int(value) for key, value in (field.split("=") for field in match[1].split())
            }
    raise ValueError("missing sound status")


def write_waves(directory: Path, pcm: bytes) -> None:
    with wave.open(str(directory / "both-mics-5s.wav"), "wb") as wav:
        wav.setparams((2, 2, 16000, 80000, "NONE", "not compressed"))
        wav.writeframes(pcm)
    for channel in range(2):
        mono = bytearray()
        for offset in range(channel * 2, len(pcm), 4):
            mono.extend(pcm[offset : offset + 2])
        with wave.open(str(directory / f"mic-{channel + 1}-5s.wav"), "wb") as wav:
            wav.setparams((1, 2, 16000, 80000, "NONE", "not compressed"))
            wav.writeframes(mono)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "work/sound/board-run-1")
    parser.add_argument("--plan", action="store_true", help="print offline plan, no board access")
    args = parser.parse_args()
    palette = json.loads((ROOT / "tools/sounds/out/contact.json").read_text())["sounds"]
    if args.plan:
        print("Existing matched signed firmware/assets; lead leases only the dev board.")
        print("Record initial heap, sound policy, gyro/tap and battery. Play all 19 cues.")
        print("Test volumes 0,20,40,60,80,100; Silent ordinary/alert; Always On.")
        print("Record 5 s stereo via devtools, validate CRC and write stereo/separate WAVs.")
        print("Restore volume/Silent/Always On; retain submission latency and final heap.")
        print(f"Output: {args.out}")
        return
    if args.out.exists():
        raise SystemExit("output already exists; choose a fresh run path to preserve evidence")
    args.out.mkdir(parents=True)
    counter = 0

    def run(command: str, *, suppressed: bool = False) -> str:
        nonlocal counter
        counter += 1
        # An idle display is a quiet state that drops ordinary cues, and the
        # validator's own input never counts as activity: wake before each step.
        subprocess.run(
            [str(ROOT / "scripts/orb-dev.sh"), "wake"],
            text=True,
            capture_output=True,
            timeout=100,
            check=False,
        )
        result = subprocess.run(
            [str(ROOT / "scripts/orb-dev.sh"), *command.split()],
            text=True,
            capture_output=True,
            timeout=100,
            check=False,
        )
        text = result.stdout + result.stderr
        (args.out / f"{counter:03d}-{command.replace(' ', '-')}.txt").write_text(text)
        if suppressed:
            if (
                result.returncode != 1
                or "device rejected command:" not in text
                or "reason=unavailable_or_suppressed" not in text
                or "TimeoutError" in text
            ):
                raise RuntimeError(f"expected suppressed cue: {command}")
        elif result.returncode or "@@ORBDEV ERROR" in text:
            raise RuntimeError(f"device command failed: {command}; see run transcript")
        return text

    original = sound_status(run("soundstatus"))
    if not original["ready"] or original["face_down"]:
        raise SystemExit("sound/asset worker must be ready and device face up before validation")
    identity = run("senses")
    device = os.environ.get("ORB_DEV_DEVICE_ID", "orb-020000abcdef")
    if not re.search(
        rf"^@@ORBDEV SENSES device={re.escape(device)}(?: |$)", identity, re.MULTILINE
    ):
        raise SystemExit(
            "refusing tests: selected transport is not the designated development board"
        )
    try:
        run("heap")
        print(run("senses"))
        run("soundsilent 0")
        run("soundalwayson 0")
        run("soundvolume 60")
        for sound in palette:
            cue = sound["id"]
            print(f"Play {cue}: {sound['name']}")
            run(f"soundplay {cue}")
            time.sleep(2.5 if sound["name"] in ("alarm", "timer") else 0.5)
            if sound["name"] in ("alarm", "timer"):
                run("soundstop")
        # A full 22 s alarm audition verifies the fade subjectively; a scope/
        # loopback recording is still needed to establish physical onset.
        run("soundplay 13")
        time.sleep(22)
        run("soundstop")
        for volume in (0, 20, 40, 60, 80, 100):
            run(f"soundvolume {volume}")
            run("soundplay 5", suppressed=volume == 0)
            time.sleep(0.5)
        run("soundvolume 60")
        run("soundsilent 1")
        run("soundplay 0", suppressed=True)
        run("soundplay 12")
        time.sleep(2)
        run("soundstop")
        run("soundplay 13")
        time.sleep(2)
        run("soundstop")
        run("soundsilent 0")
        run("soundalwayson 1")
        # Always On is a quiet state: ordinary cues drop, alarms still ring.
        run("soundplay 0", suppressed=True)
        run("soundplay 13")
        time.sleep(2)
        run("soundstop")
        run("soundalwayson 0")
        raw = decode_microphones(run("micrecord"))
        write_waves(args.out, raw)
        print(run("senses"))
        run("soundstatus")
        run("heap")
    finally:
        restore_errors = []
        for command in (
            "soundstop",
            f"soundvolume {original['volume']}",
            f"soundsilent {original['silent']}",
            f"soundalwayson {original['always_on']}",
        ):
            try:
                run(command)
            except (OSError, subprocess.SubprocessError, RuntimeError) as error:
                restore_errors.append(str(error))
        time.sleep(1)
        try:
            restored = sound_status(run("soundstatus"))
            if (restored["volume"], restored["silent"], restored["always_on"]) != (
                original["volume"],
                original["silent"],
                original["always_on"],
            ):
                restore_errors.append("sound policy restoration readback did not match")
        except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as error:
            restore_errors.append(str(error))
        if restore_errors:
            (args.out / "RESTORE_FAILED").write_text("\n".join(restore_errors) + "\n")
            raise RuntimeError("sound policy restoration failed; see RESTORE_FAILED")
    (args.out / "COMPLETE").write_text(
        "Automated commands and restoration completed. Lead must review sound quality, both mic WAVs, "
        "gyro/tap behavior and physical onset latency; no acoustic gate implied.\n"
    )


if __name__ == "__main__":
    main()
