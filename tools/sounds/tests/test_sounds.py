"""The deliverable WAVs and binary pack have the same deterministic PCM contract."""

import importlib.util
import json
import struct
import wave
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "make_sounds.py"
SPEC = importlib.util.spec_from_file_location("make_sounds", SCRIPT)
sounds = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sounds)


def test_generated_palette_contract(tmp_path):
    info = sounds.generate(tmp_path)
    pack = (tmp_path / "palette.orbs").read_bytes()
    assert sounds.HEADER.unpack_from(pack) == (b"ORBSOUND", 1, 32000, 19, len(pack))
    assert info["pack_bytes"] == 344008
    assert json.loads((tmp_path / "contact.json").read_text()) == info
    for sound_id, item in enumerate(info["sounds"]):
        record_id, offset, count, flags = sounds.ENTRY.unpack_from(pack, 24 + sound_id * 16)
        assert record_id == sound_id and bool(flags) == item["loop"]
        pcm = struct.unpack_from(f"<{count}h", pack, offset)
        assert sum(pcm) == 0 and max(map(abs, pcm)) == sounds.PEAK
        assert pcm[0] == pcm[-1] == 0
        assert count == item["duration_ms"] * 32
        with wave.open(str(tmp_path / f"{item['name']}.wav")) as wav:
            assert wav.getparams()[:4] == (1, 2, 32000, count)
            assert wav.readframes(count) == pack[offset : offset + count * 2]
    second = tmp_path / "second"
    sounds.generate(second)
    assert (second / "palette.orbs").read_bytes() == pack


def test_alarm_preview_fades_over_twenty_seconds(tmp_path):
    sounds.generate(tmp_path)
    with wave.open(str(tmp_path / "alarm_fade_preview.wav")) as wav:
        pcm = struct.unpack(f"<{wav.getnframes()}h", wav.readframes(wav.getnframes()))

    def peak(start):
        return max(map(abs, pcm[start * 32000 : (start + 2) * 32000]))

    assert peak(0) < peak(10) < peak(18) <= peak(20)
    assert len(pcm) == 22 * 32000
