"""Icon ABI, outline offsets, source fidelity and generated font integration."""

import hashlib
import json
import tarfile
from pathlib import Path

import pytest
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

import build
import icons
import spec

FALLBACKS = {
    "DOOR",
    "FAN",
    "GARAGE",
    "HAND_TAP",
    "PLUG",
    "RECENTS",
    "SENSOR",
    "TOGGLE",
    "WALK",
    "BLUETOOTH",
    "BLUETOOTH_SLASH",
    "BLUETOOTH_CONNECTED",
    "WATCH",
    "TERMINAL",
    "ROBOT",
    "GIT_PULL_REQUEST",
    "PROMPT",
    "FLASHLIGHT",
    "ACCESSIBILITY",
}


@pytest.fixture(scope="module")
def assets(tmp_path_factory):
    """Use verified downloaded upstream SVGs, unpacked into an isolated test directory.

    Offline synthetic tests always run. Run scripts/fonts.sh first to supply these
    pinned upstream archives for the integration tests.
    """
    root = tmp_path_factory.mktemp("icon-sources")
    for key in ("framework7", "phosphor"):
        pinned = build.SOURCES[key]
        archive = build.CACHE / "downloads" / pinned["file"]
        if not archive.exists():
            pytest.skip("Run scripts/fonts.sh first for pinned-source integration tests")
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == pinned["sha256"]
        prefix = "package/svg/" if key == "framework7" else "package/assets/"
        with tarfile.open(archive) as source:
            for member in source.getmembers():
                if (
                    member.isfile()
                    and member.name.startswith(prefix)
                    and member.name.endswith(".svg")
                ):
                    relative = Path(member.name.removeprefix(prefix))
                    assert not relative.is_absolute() and ".." not in relative.parts
                    target = root / key / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source.extractfile(member).read())
    return root


def test_public_icon_abi_is_unchanged():
    expected = json.loads((Path(__file__).parent / "icon_abi.json").read_text())
    actual = {macro: cp for macro, cp, _, _ in spec.icon_codepoints()}
    assert {macro: actual[macro] for macro in expected} == expected
    assert len(spec.ICONS) == 137
    assert len(spec.icon_codepoints()) == 179
    spec.check()


def test_fallbacks_are_explicit_and_limited():
    assert {i.macro for i in spec.ICONS if i.asset.startswith("phosphor:")} == FALLBACKS
    assert len([a for _, _, a, _ in spec.icon_codepoints() if a.startswith("phosphor:")]) == 20


@pytest.mark.parametrize(
    ("macro", "expected"),
    [
        ("CHECK", "checkmark"),
        ("GEAR", "gear_alt"),
        ("LIGHTNING", "bolt"),
        ("SPARKLE", "sparkles"),
        ("MIC", "mic"),
        ("WAVEFORM", "waveform"),
        ("GIT_BRANCH", "arrow_branch"),
    ],
)
def test_framework7_semantic_mapping(macro, expected):
    assert next(i.asset for i in spec.ICONS if i.macro == macro) == expected


def test_fill_artwork_selection():
    assert spec.icon_asset("bolt", "LIGHTNING_FILL", True) == "bolt_fill"
    assert spec.icon_asset("sparkles", "SPARKLE_FILL", True) == "sparkles"
    assert spec.icon_asset("smallcircle_circle", "RECORD_FILL", True) == "smallcircle_fill_circle"


def test_every_mapped_framework7_source_exists_and_has_ink(assets):
    for macro, _, asset, fill in spec.icon_codepoints():
        if not asset.startswith("phosphor:"):
            path = icons.source_path(assets, spec.icon_asset(asset, macro, fill))
            assert icons._area(path) > 0, macro


def _svg(tmp_path, path, *, view_box="0 0 56 56"):
    folder = tmp_path / "framework7"
    folder.mkdir(exist_ok=True)
    (folder / "test.svg").write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{view_box}">{path}</svg>'
    )
    return tmp_path


def test_svg_translation_and_evenodd_counters(tmp_path):
    root = _svg(
        tmp_path,
        '<path fill-rule="evenodd" transform="translate(4 5)" d="M0 0H10V10H0Z M2 2H8V8H2Z"/>',
    )
    path = icons.framework7_path(root, "test")
    assert path.bounds == pytest.approx((4, 5, 14, 15))
    assert icons._area(path) == pytest.approx(64)


@pytest.mark.parametrize(
    "element",
    [
        '<g><path d="M0 0H10V10H0Z"/></g>',
        '<circle r="5"/>',
        '<path transform="rotate(10)" d="M0 0H10V10H0Z"/>',
    ],
)
def test_svg_unsupported_geometry_fails_closed(tmp_path, element):
    with pytest.raises(ValueError):
        icons.framework7_path(_svg(tmp_path, element), "test")


def test_unexpected_source_grid_fails_closed(tmp_path):
    with pytest.raises(ValueError, match="viewBox"):
        icons.framework7_path(
            _svg(tmp_path, '<path d="M0 0H10V10H0Z"/>', view_box="0 0 24 24"), "test"
        )


@pytest.mark.parametrize("offset", [-0.3, 0, 0.2, 1, 2])
def test_outline_offset_tracks_stem_width(assets, offset):
    circle = icons.framework7_path(assets, "circle")
    grown = icons.grow(circle, offset)
    contours = sorted(grown.contours, key=lambda c: c.bounds[2] - c.bounds[0], reverse=True)
    original = sorted(circle.contours, key=lambda c: c.bounds[2] - c.bounds[0], reverse=True)
    old_stem = original[1].bounds[0] - original[0].bounds[0]
    new_stem = contours[1].bounds[0] - contours[0].bounds[0]
    assert new_stem == pytest.approx(old_stem + 2 * offset, abs=0.015)


def test_framework7_translated_gear_is_centred(assets):
    for name in ("gear_alt", "gear_alt_fill"):
        x0, y0, x1, y1 = icons.framework7_path(assets, name).bounds
        assert (x0 + x1) / 2 == pytest.approx(28, abs=0.1)
        assert (y0 + y1) / 2 == pytest.approx(28, abs=0.1)


def test_battery_and_wifi_states_remain_distinct(assets):
    battery_areas = [
        icons._area(icons.source_path(assets, f"battery_100@{n}")) for n in (20, 50, 80)
    ]
    assert battery_areas == sorted(set(battery_areas))
    wifi_areas = [icons._area(icons.source_path(assets, f"wifi@{n}")) for n in (0, 1, 2)]
    wifi_areas += [icons._area(icons.source_path(assets, "wifi"))]
    assert wifi_areas == sorted(set(wifi_areas))


def test_upload_arrow_is_above_its_separate_bar(assets):
    arrow, bar = sorted(
        icons.components(icons.source_path(assets, "arrow_up@upload")),
        key=lambda path: path.bounds[1],
    )
    assert arrow.bounds[3] < bar.bounds[1]
    assert arrow.bounds[0] > bar.bounds[0]
    assert arrow.bounds[2] < bar.bounds[2]


def _font(assets, target, stroke, px):
    return icons.build(
        assets,
        spec.icon_codepoints(),
        spec.icon_groups(),
        stroke,
        icons.icon_scale(1490, spec.ICON_CIRCLE_CAP_HEIGHTS),
        745,
        164,
        1984,
        -494,
        576,
        "Orb Test Icons",
        px,
        target,
    )


@pytest.mark.parametrize(("stroke", "px"), [(4.22, 28), (6.12, 28), (8.2, 75)])
def test_font_metrics_fills_baseline_and_reproducibility(assets, tmp_path, stroke, px):
    first, second = tmp_path / "a.ttf", tmp_path / "b.ttf"
    report = _font(assets, first, stroke, px)
    _font(assets, second, stroke, px)
    assert first.read_bytes() == second.read_bytes()
    assert report["glyphs"] == 179
    assert report["fill_bounds_px"] < 0.5
    font = TTFont(first)
    cmap, glyf, metrics = font.getBestCmap(), font["glyf"], font["hmtx"]
    widths = {}
    for macro, cp, _, filled in spec.icon_codepoints():
        name = cmap[cp]
        group = spec.icon_groups()[macro]
        advance = metrics[name][0]
        assert advance == widths.setdefault(group, advance)
        glyph = glyf[name]
        assert glyph.numberOfContours > 0
        if filled:
            # TrueType control-point boxes need not match on differently curved
            # paths; compare actual ink extrema (as FreeType rasterises them).
            glyph_set = font.getGlyphSet()
            filled_bounds, outline_bounds = BoundsPen(glyph_set), BoundsPen(glyph_set)
            glyph_set[name].draw(filled_bounds)
            glyph_set[cmap[cp - spec.FILL_OFFSET]].draw(outline_bounds)
            assert filled_bounds.bounds == pytest.approx(outline_bounds.bounds, abs=2)
            # FreeType also uses the control-point box to allocate bitmap rows.
            # Quadratic extrema splitting prevents otherwise identical speaker
            # silhouettes from receiving different blank padding at large sizes.
            outline = glyf[cmap[cp - spec.FILL_OFFSET]]
            assert tuple(
                getattr(glyph, axis) for axis in ("xMin", "yMin", "xMax", "yMax")
            ) == pytest.approx(
                tuple(getattr(outline, axis) for axis in ("xMin", "yMin", "xMax", "yMax")), abs=2
            )
    circle = glyf[cmap[0xE006]]
    assert (circle.yMax + circle.yMin) / 2 == pytest.approx(745, abs=1)


def test_fit_bounds_preserves_counter(assets):
    original = icons.framework7_path(assets, "gear_alt_fill")
    fitted = icons.fit_bounds(original, (0, 0, 100, 100))
    assert fitted.bounds == pytest.approx((0, 0, 100, 100))
    assert len(list(fitted.contours)) == len(list(original.contours))
    assert icons._area(fitted) < icons._area(icons.silhouette(fitted))


@pytest.mark.parametrize("offset", [0.1, 1.06, 2.1])
def test_emphasized_battery_retains_clearance_and_charging_knockout(assets, offset):
    full = icons.weighted_battery(assets, "battery_100", offset)
    charging = icons.weighted_battery(assets, "battery_100@charging", offset)
    warning = icons.weighted_battery(assets, "battery_100@warning", offset)
    # Body and fill remain separate connected pieces even at Heavy. Both marks
    # remain visible negative space instead of vanishing during emboldening.
    assert len(icons.components(full)) >= 2
    assert icons._area(full) - icons._area(charging) > 40
    assert icons._area(full) - icons._area(warning) > 30
    assert charging.bounds == pytest.approx(full.bounds)
    assert warning.bounds == pytest.approx(full.bounds)


def test_quadratic_extrema_keep_freetype_bitmap_boxes_tight():
    sink = TTGlyphPen(None)
    pen = icons.ExtremaPen(sink)
    pen.moveTo((0, 0))
    pen.qCurveTo((5, 20), (10, 0))
    pen.closePath()
    glyph = sink.glyph()
    glyph.recalcBounds(None)
    assert (glyph.xMin, glyph.yMin, glyph.xMax, glyph.yMax) == (0, 0, 10, 10)
