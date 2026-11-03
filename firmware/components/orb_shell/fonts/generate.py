"""Reproduce the rounded Face subsets without changing the main font pipeline.

Use tools/fonts' hash-pinned environment and lv_font_conv 1.5.3; see README.md.
The original Nunito file and licence are pinned and checked before generation.
"""

import argparse
import hashlib
import json
import subprocess
import tempfile
from pathlib import Path

import uharfbuzz as hb
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.ttLib.removeOverlaps import removeOverlaps
from fontTools.varLib.instancer import instantiateVariableFont

SOURCE_SHA = "bb55a5ca5c2042335b3991af27c4d0705d0ef41cac6164ac737fd8f2a1e85207"
LICENSE_SHA = "580df76c95a1ec5ab878ceb25bb3d85c6a076804e9c970c8c6972aea775fdf65"
TEXT = "0123456789waitingMacDemoUSB- "
# orb_fonts_table.c FOOTNOTE2: Default, Larger, AX3. Track like the semantic style.
STYLES = ((21, 16), (25, 4), (28, -4))
WEIGHTS = ((600, "semibold"), (700, "bold"))


def check_hash(path: Path, expected: str) -> None:
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f"Source hash mismatch: {path.name}: {actual}")


def instance(source: Path, weight: int, tracking: int) -> TTFont:
    font = instantiateVariableFont(TTFont(source), {"wght": weight}, updateFontNames=False)
    face = hb.Face(source.read_bytes())
    shaper = hb.Font(face)
    shaper.set_variations({"wght": weight})
    names = font.getGlyphOrder()
    # LVGL cannot apply OpenType tnum; freeze the shaped digit glyphs into cmap.
    digits = {}
    for char in "0123456789":
        buf = hb.Buffer()
        buf.add_str(char)
        buf.guess_segment_properties()
        hb.shape(shaper, buf, {"tnum": True})
        assert len(buf.glyph_infos) == 1
        digits[ord(char)] = names[buf.glyph_infos[0].codepoint]
    for cmap in font["cmap"].tables:
        if cmap.isUnicode():
            cmap.cmap.update(digits)
    assert len({font["hmtx"][name][0] for name in digits.values()}) == 1, "Non-tabular digits"
    options = subset.Options()
    options.layout_features = ["kern"]
    options.hinting = False
    options.name_IDs = ["*"]
    options.name_languages = ["*"]
    options.notdef_outline = True
    sub = subset.Subsetter(options)
    sub.populate(unicodes=map(ord, TEXT))
    sub.subset(font)
    removeOverlaps(font)
    # Fractional tracking is part of advances, as in Orb's Inter generator.
    units = round(font["head"].unitsPerEm * tracking / 1000)
    for name in font.getGlyphOrder():
        glyph = font["glyf"][name]
        if glyph.numberOfContours > 0:
            glyph.coordinates.translate((int(units / 2), 0))
        advance, bearing = font["hmtx"][name]
        if glyph.numberOfContours:
            glyph.recalcBounds(font["glyf"])
            bearing = glyph.xMin
        font["hmtx"][name] = (advance + units if advance else 0, bearing)
    # A derived subset has its own family name; retain copyright/licence names.
    for record in font["name"].names:
        if record.nameID in (1, 3, 4, 6, 16):
            record.string = f"Orb Rounded Complications {weight}".encode(record.getEncoding())
    font.recalcTimestamp = False
    return font


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--converter", type=Path, required=True, help="lv_font_conv.js from tools/fonts"
    )
    args = parser.parse_args()
    converter = args.converter.resolve()
    package = json.loads((converter.parent / "package.json").read_text())
    if package["version"] != "1.5.3":
        raise SystemExit("Use the pinned lv_font_conv 1.5.3")
    out = Path(__file__).resolve().parent
    source = out / "Nunito-wght.ttf"
    check_hash(source, SOURCE_SHA)
    check_hash(out / "Nunito-OFL.txt", LICENSE_SHA)
    with tempfile.TemporaryDirectory(prefix="orb-complication-fonts-") as tmp:
        work = Path(tmp)
        for size, tracking in STYLES:
            for weight, weight_name in WEIGHTS:
                name = f"orb_nunito_{size}_{weight_name}"
                ttf = f"Nunito-{size}-{weight}.ttf"
                instance(source, weight, tracking).save(work / ttf)
                subprocess.run(
                    [
                        "node",
                        str(converter),
                        "--font",
                        ttf,
                        "--autohint-off",
                        "--symbols",
                        TEXT,
                        "--size",
                        str(size),
                        "--bpp",
                        "4",
                        "--format",
                        "lvgl",
                        "--lv-include",
                        "lvgl.h",
                        "--lv-font-name",
                        name,
                        "-o",
                        f"{name}.c",
                        "--no-compress",
                    ],
                    cwd=work,
                    check=True,
                )
                text = (work / f"{name}.c").read_text()
                header = (
                    "/* Rounded Face complications (hig.md §§4/7).\n"
                    " * Generated by fonts/generate.py from Nunito 3.602; do not edit.\n"
                    " * Copyright 2014 The Nunito Project Authors. SIL OFL 1.1:\n"
                    " * see Nunito-OFL.txt and README.md for source, hashes and reproduction.\n"
                    " */\n\n"
                )
                (out / f"{name}.c").write_text(header + text)
                print(name)


if __name__ == "__main__":
    main()
