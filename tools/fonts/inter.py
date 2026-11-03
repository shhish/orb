"""Static Inter instances for lv_font_conv: weight and optical size pinned, OpenType
features LVGL can't apply frozen into the cmap, Apple tracking built into the advances.
"""

from pathlib import Path

import uharfbuzz as hb
from fontTools import subset
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont
from fontTools.ttLib.removeOverlaps import removeOverlaps
from fontTools.ttLib.tables._g_l_y_f import ROUND_XY_TO_GRID
from fontTools.varLib.instancer import instantiateVariableFont


def instance(variable: Path, wght: int, opsz: int) -> TTFont:
    return instantiateVariableFont(
        TTFont(variable), {"wght": wght, "opsz": opsz}, updateFontNames=False
    )


def frozen_glyphs(
    variable: Path, wght: int, opsz: int, contexts: dict[int, str], features: dict
) -> dict[int, str]:
    """The glyph HarfBuzz picks for each codepoint in its context with `features` on.
    `contexts` maps a codepoint to a three-character string whose middle character is
    the one shaped ("0:0" for a colon between figures, "a:a" for one after a word), which
    is what a shaping engine would show there. lv_font_conv applies no OpenType features,
    so these become the codepoints' cmap entries."""
    face = hb.Face(hb.Blob.from_file_path(str(variable)))
    font = hb.Font(face)
    font.set_variations({"wght": wght, "opsz": opsz})
    order = TTFont(variable, lazy=True).getGlyphOrder()
    out = {}
    for cp, context in contexts.items():
        assert len(context) == 3, context
        buf = hb.Buffer()
        buf.add_str(context)
        buf.guess_segment_properties()
        hb.shape(font, buf, features)
        infos = buf.glyph_infos
        assert len(infos) == 3, f"U+{cp:04X} in {context!r} shaped into {len(infos)} glyphs"
        out[cp] = order[infos[1].codepoint]
    return out


def remap_cmap(font: TTFont, glyphs: dict[int, str]) -> None:
    for table in font["cmap"].tables:
        if table.isUnicode():
            for cp, name in glyphs.items():
                if cp in table.cmap:
                    table.cmap[cp] = name


def subset_to(font: TTFont, codepoints: list[int]) -> None:
    """Keep only what lv_font_conv will read: these characters and their pair kerning."""
    cmap = font.getBestCmap()
    missing = [cp for cp in codepoints if cp not in cmap]
    if missing:
        raise SystemExit("Inter lacks " + ", ".join(f"U+{cp:04X}" for cp in missing))
    opts = subset.Options()
    opts.layout_features = ["kern"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.glyph_names = True
    opts.hinting = False
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=codepoints)
    sub.subset(font)
    # Variable fonts keep overlapping contours (and overlapping components). Anti-aliased
    # edges where two contours overlap are drawn too dark by lv_font_conv's FreeType, so
    # merge them: clean outlines rasterise the same everywhere.
    removeOverlaps(font)
    # Inter's composites (;  ,,  Ç  ¼ ...) ask for their component offsets to be rounded
    # to whole pixels when hinted. lv_font_conv's FreeType honours that even with the
    # auto-hinter off and moves those glyphs by up to half a pixel; place them exactly.
    glyf = font["glyf"]
    for name in font.getGlyphOrder():
        glyph = glyf[name]
        if glyph.isComposite():
            for comp in glyph.components:
                comp.flags &= ~ROUND_XY_TO_GRID


def stem_em(font: TTFont) -> float:
    """Width of the lowercase l, Inter's plain vertical stem, in em."""
    gs = font.getGlyphSet()
    pen = BoundsPen(gs)
    gs[font.getBestCmap()[ord("l")]].draw(pen)
    x0, _, x1, _ = pen.bounds
    return (x1 - x0) / font["head"].unitsPerEm


def track(font: TTFont, units: int) -> None:
    """Add `units` to every advance and move every outline right by half of it, so the
    tracking falls evenly on both sides of a glyph and a tracked line stays centred."""
    if not units:
        return
    shift = int(units / 2)
    glyf, hmtx = font["glyf"], font["hmtx"]
    order = font.getGlyphOrder()
    for name in order:
        glyph = glyf[name]
        if glyph.numberOfContours > 0:  # composites move with their components
            glyph.coordinates.translate((shift, 0))
    for name in order:
        glyph = glyf[name]
        adv, lsb = hmtx[name]
        if glyph.numberOfContours != 0:
            glyph.recalcBounds(glyf)
            lsb = glyph.xMin
        hmtx[name] = (adv + units if adv else 0, lsb)


def rename(font: TTFont, family: str, style: str) -> None:
    """Modified instances get their own names so nothing mistakes them for Inter's."""
    name = font["name"]
    full = f"{family} {style}"
    for nid, value in (
        (1, family),
        (2, "Regular"),
        (4, full),
        (6, full.replace(" ", "-")),
        (16, family),
        (17, style),
    ):
        name.setName(value, nid, 3, 1, 0x409)
        name.setName(value, nid, 1, 0, 0)
