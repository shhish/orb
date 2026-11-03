"""Source-integrity and compact-font contracts, without touching a device or service."""

import hashlib
import io

import pytest
from fontTools.ttLib import TTFont

import build
import spec
import status_fonts


@pytest.fixture
def download(monkeypatch, tmp_path):
    payload = b"isolated-font-source"
    monkeypatch.setattr(build, "CACHE", tmp_path)
    monkeypatch.setattr(
        build,
        "SOURCES",
        {
            "fixture": {
                "url": "https://example.invalid/font.tgz",
                "sha256": hashlib.sha256(payload).hexdigest(),
                "file": "font.tgz",
            }
        },
    )
    return payload, tmp_path / "downloads" / "font.tgz"


def test_download_verified_and_reused(download, monkeypatch):
    payload, path = download
    calls = []

    def response(url, timeout):
        calls.append((url, timeout))
        return io.BytesIO(payload)

    monkeypatch.setattr(build.urllib.request, "urlopen", response)
    assert build.fetch("fixture") == payload
    assert path.read_bytes() == payload
    assert not path.with_suffix(".part").exists()
    assert build.fetch("fixture") == payload
    assert len(calls) == 1


@pytest.mark.parametrize("cached", [False, True])
def test_corrupted_archive_rejected(download, monkeypatch, cached):
    _, path = download
    if cached:
        path.parent.mkdir()
        path.write_bytes(b"corrupt-cache")
    monkeypatch.setattr(
        build.urllib.request, "urlopen", lambda *_a, **_k: io.BytesIO(b"corrupt-download")
    )
    with pytest.raises(SystemExit, match="SHA-256"):
        build.fetch("fixture")


def test_every_status_size_has_both_builtin_weights():
    fonts, sets = build.plan()
    for fs in sets:
        for row in fs.styles:
            px = status_fonts.size_for_px(row["style"].px)
            for weight in ("semibold", "bold"):
                font = fonts[f"orb_status_{px}_{weight}"]
                assert font.home == "app"
                assert font.codepoints == [*status_fonts.DIGITS, status_fonts.BOLT]
                assert font.fallback is None
    assert status_fonts.size_for_px(28) == 12
    assert status_fonts.size_for_px(31) == 14
    assert status_fonts.size_for_px(38) == 17


def test_always_on_clock_keeps_low_ink_weights_in_every_content_size():
    fonts, sets = build.plan()
    for fs in sets:
        face = next(c for c in fs.clocks if c["clock"].enum == "ALWAYS_ON")
        assert face["clock"].px == 128
        assert face["clock"].weight == (400 if fs.bold else 300)
        font = fonts[face["font"]]
        assert font.home == "pack"
        assert font.codepoints == spec.CLOCK_CODEPOINTS


@pytest.mark.parametrize("weight", ["semibold", "bold"])
def test_generated_status_font_has_tabular_digits_and_real_bolt(weight):
    # Integration test of the generator output used by the host LVGL check.
    path = build.WORK / "ttf" / f"orb_status_12_{weight}.ttf"
    assert path.exists(), "Run scripts/fonts.sh --no-previews before pytest"
    with TTFont(path) as font:
        cmap = font.getBestCmap()
        assert set(cmap) == {*status_fonts.DIGITS, status_fonts.BOLT}
        advances = {font["hmtx"][cmap[cp]][0] for cp in range(48, 58)}
        assert len(advances) == 1
        for cp in (*range(48, 58), status_fonts.BOLT):
            glyph = font["glyf"][cmap[cp]]
            assert glyph.numberOfContours != 0  # composites have -1 contours
            assert glyph.xMax > glyph.xMin
            assert glyph.yMax > glyph.yMin


def test_status_selector_emits_current_style_and_weight(tmp_path):
    fonts, sets = build.plan()
    path = tmp_path / "selector.c"
    status_fonts.emit_selector(path, fonts, sets)
    text = path.read_text()
    assert "orb_text_style_info(style)" in text
    assert "ORB_TEXT_EMPHASIZED" in text
    assert "cap.box_h" in text
    for px in {f.px for f in fonts.values() if f.kind == "status"}:
        assert f"if (px >= {px})" in text
    first = path.read_bytes()
    status_fonts.emit_selector(path, fonts, sets)
    assert path.read_bytes() == first
