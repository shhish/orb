#!/usr/bin/env python3
"""Deterministic Orb SoundSenses palette. No runtime synthesizer or dependencies.

PCM is mono signed little-endian 16-bit at 32 kHz. Each source has exactly zero
integer DC and a -1 dBFS peak. Runtime mixing adds headroom; codec volume sets SPL.
"""

import argparse
import json
import math
import random
import struct
import wave
from pathlib import Path

RATE = 32000
PEAK = round(32767 * 10 ** (-1 / 20))
PALETTE = [
    ("detent", 8),
    ("tap", 12),
    ("open", 90),
    ("close", 90),
    ("needs_you", 240),
    ("done", 180),
    ("sent", 120),
    ("listening_start", 70),
    ("listening_stop", 70),
    ("failed", 160),
    ("home_on", 10),
    ("home_off", 10),
    ("timer", 2000),
    ("alarm", 2000),
    ("game_roll", 60),
    ("game_bead", 80),
    ("game_pop", 45),
    ("game_switch", 35),
    ("game_collect", 90),
]
HEADER = struct.Struct("<8sIIII")
ENTRY = struct.Struct("<IIII")


def source(name: str, milliseconds: int) -> list[float]:
    count = RATE * milliseconds // 1000
    rng = random.Random(0x0BDB000 + PALETTE.index((name, milliseconds)))
    low = previous = 0.0
    out = []
    for index in range(count):
        t = index / RATE
        length = count / RATE
        noise = rng.uniform(-1, 1)
        low += 0.19 * (noise - low)
        high = low - previous
        previous = low
        edge = min(1.0, t / 0.0015, (length - 1 / RATE - t) / 0.004)
        envelope = max(0.0, edge) ** 2
        # Inharmonic partials avoid a melody; a short FM strike supplies warmth.
        if name in {"needs_you", "done", "game_collect"}:
            second = name == "needs_you" and t >= 0.105
            local = t - (0.105 if second else 0)
            hz = 747 if second else (563 if name == "done" else 618)
            bell = math.sin(
                2 * math.pi * hz * local
                + 0.65 * math.exp(-local * 48) * math.sin(2 * math.pi * hz * 1.413 * local)
            )
            bell += 0.19 * math.sin(2 * math.pi * hz * 2.73 * local) * math.exp(-local * 42)
            attack = min(1.0, local / 0.004) ** 2
            value = bell * math.exp(-local * 22) * attack
        elif name in {"timer", "alarm"}:
            # A two-second zero-ended loop. Engine applies the 20-second ramp.
            phase = t % 1.0
            on = min(1.0, phase / 0.035, max(0, (0.48 - phase) / 0.12))
            value = (
                (math.sin(2 * math.pi * 524 * t) + 0.16 * math.sin(2 * math.pi * 1048 * t))
                * on
                * on
            )
        elif name in {"open", "close", "sent", "listening_start", "listening_stop"}:
            rise = name in {"open", "sent", "listening_start"}
            sweep = (t / length) if rise else (1 - t / length)
            value = low * (0.35 + 0.65 * sweep) + high * sweep * 0.2
            value *= math.sin(math.pi * t / length) ** 1.5
        elif name == "failed":
            local = t % 0.08
            value = (low * 0.5 + math.sin(2 * math.pi * 179 * local)) * math.exp(-local * 85)
            value *= min(1, local / 0.002) ** 2
        else:
            hz = 940 * (2 ** (1 / 12) if name == "home_on" else 1)
            value = low + 0.3 * high
            if name in {"home_on", "home_off", "game_pop", "game_switch"}:
                value += 0.25 * math.sin(2 * math.pi * hz * t + 0.8 * math.exp(-t * 160))
            value *= math.exp(-t * (260 if milliseconds <= 12 else 70))
        out.append(value * envelope)
    return out


def pcm_normalize(values: list[float]) -> list[int]:
    # Remove DC behind a smooth window to keep both endpoints exactly zero.
    weights = [math.sin(math.pi * i / (len(values) - 1)) ** 2 for i in range(len(values))]
    dc = sum(values) / sum(weights)
    values = [v - dc * w for v, w in zip(values, weights, strict=True)]
    scale = PEAK / max(abs(v) for v in values)
    pcm = [round(v * scale) for v in values]
    pcm[0] = pcm[-1] = 0
    # Quantization can leave a few LSBs of residual DC. Spread corrections over
    # interior samples with headroom; the quantized stream then sums to zero.
    residual = sum(pcm)
    step = 1 if residual > 0 else -1
    for index in range(1, len(pcm) - 1):
        if residual == 0:
            break
        if abs(pcm[index]) < PEAK - 1:
            pcm[index] -= step
            residual -= step
    assert residual == 0 and max(abs(v) for v in pcm) == PEAK
    return pcm


def wav(path: Path, pcm: list[int]) -> None:
    with wave.open(str(path), "wb") as stream:
        stream.setparams((1, 2, RATE, len(pcm), "NONE", "not compressed"))
        stream.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))


def generate(output: Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    blob = bytearray(HEADER.size + ENTRY.size * len(PALETTE))
    listing = []
    for sound_id, (name, milliseconds) in enumerate(PALETTE):
        pcm = pcm_normalize(source(name, milliseconds))
        offset = len(blob)
        blob.extend(struct.pack(f"<{len(pcm)}h", *pcm))
        ENTRY.pack_into(
            blob,
            HEADER.size + sound_id * ENTRY.size,
            sound_id,
            offset,
            len(pcm),
            int(name in {"timer", "alarm"}),
        )
        wav(output / f"{name}.wav", pcm)
        listing.append(
            {
                "id": sound_id,
                "name": name,
                "duration_ms": milliseconds,
                "samples": len(pcm),
                "pcm_bytes": len(pcm) * 2,
                "peak_dbfs": -1,
                "dc_lsb": 0,
                "loop": name in {"timer", "alarm"},
            }
        )
        if name == "alarm":
            preview = [round(pcm[i % len(pcm)] * min(1, i / (20 * RATE))) for i in range(22 * RATE)]
            wav(output / "alarm_fade_preview.wav", preview)
    HEADER.pack_into(blob, 0, b"ORBSOUND", 1, RATE, len(PALETTE), len(blob))
    (output / "palette.orbs").write_bytes(blob)
    contact = {
        "format": "s16le",
        "channels": 1,
        "sample_rate": RATE,
        "pack_bytes": len(blob),
        "sounds": listing,
    }
    (output / "contact.json").write_text(json.dumps(contact, indent=2) + "\n")
    lines = [
        "Orb sound contact sheet — mono PCM16, 32000 Hz",
        "",
        "Moment                 Duration  PCM bytes  Notes",
    ]
    lines.extend(
        f"{s['name']:<22} {s['duration_ms']:>5} ms {s['pcm_bytes']:>9}  "
        + ("loop; runtime 20 s fade-up" if s["loop"] else "-1 dBFS, zero DC")
        for s in listing
    )
    lines.extend(
        [
            "",
            "Spoken reply: streamed by hub; not a generated system cue.",
            f"Total signed sound-section payload: {len(blob)} bytes.",
        ]
    )
    (output / "contact.txt").write_text("\n".join(lines) + "\n")
    return contact


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=Path(__file__).parent / "out")
    args = parser.parse_args()
    print(json.dumps({key: val for key, val in generate(args.out).items() if key != "sounds"}))


if __name__ == "__main__":
    main()
