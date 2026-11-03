"""Framework7 5.0.5 icons, offset to match each Inter instance's stem.

Framework7 supplies filled paths at one weight, not stroked centre lines. The
round-joined Minkowski offset in grow() expands their exterior and contracts
counters. A four-unit reference stem (the circle border on its 56-unit grid)
is matched to 0.9 of Inter's measured stem, including optical-size changes.
Native *_fill artwork keeps its geometry and receives the same weight offset;
fill bounds are registered to the outline so toggling state never moves it.
Only explicit phosphor: assets use the older multi-weight fallback pipeline.
"""

import re
from pathlib import Path
from xml.etree import ElementTree

import pathops
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.bezierTools import splitQuadraticAtT
from fontTools.pens.basePen import BasePen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path

import spec

UPM = 2048
GRID = 56
FRAMEWORK7_CIRCLE = 47.8126
FRAMEWORK7_STEM = 4.0
PHOSPHOR_STROKES = {"thin": 8, "light": 12, "regular": 16, "bold": 24}
PHOSPHOR_CIRCLE = 208  # outer diameter of Phosphor's regular circle


class ExtremaPen(BasePen):
    """Split quadratic curves at extrema so FreeType's control box is tight.

    Two equal silhouettes can have different off-curve control boxes. FreeType
    allocates its glyph bitmap using that box, leaving different blank rows in
    outline and filled variants unless the extrema become on-curve points.
    """

    def __init__(self, pen):
        super().__init__(None)
        self.pen = pen

    def _moveTo(self, p):
        self.pen.moveTo(p)

    def _lineTo(self, p):
        self.pen.lineTo(p)

    def _qCurveToOne(self, p1, p2):
        p0 = self._getCurrentPoint()
        extrema = set()
        for axis in (0, 1):
            denominator = p0[axis] - 2 * p1[axis] + p2[axis]
            if abs(denominator) > 1e-12:
                t = (p0[axis] - p1[axis]) / denominator
                if 1e-6 < t < 1 - 1e-6:
                    extrema.add(t)
        for _, control, end in splitQuadraticAtT(p0, p1, p2, *sorted(extrema)):
            self.pen.qCurveTo(control, end)

    def _closePath(self):
        self.pen.closePath()

    def _endPath(self):
        self.pen.endPath()


def _svg_path(assets: Path, weight: str, name: str) -> pathops.Path:
    file = assets / weight / (f"{name}.svg" if weight == "regular" else f"{name}-{weight}.svg")
    svg = file.read_text()
    if "<path" not in svg or re.search(r"<(circle|rect|line|polyline|polygon|ellipse)\b", svg):
        raise SystemExit(f"{file}: expected path elements only")
    path = pathops.Path()
    for d in re.findall(r'<path[^>]*\sd="([^"]+)"', svg):
        parse_path(d, path.getPen())
    return path


def framework7_path(assets: Path, name: str) -> pathops.Path:
    """Read upstream SVG paths, preserving per-path transforms and fill rules."""
    file = assets / "framework7" / f"{name}.svg"
    root = ElementTree.fromstring(file.read_text())
    if root.attrib.get("viewBox") != "0 0 56 56":
        raise ValueError(f"{file}: unexpected Framework7 viewBox")
    out = pathops.Path()
    for element in root:
        if element.tag.rsplit("}", 1)[-1] != "path":
            raise ValueError(f"{file}: expected path elements only")
        path = pathops.Path()
        if element.attrib.get("fill-rule") == "evenodd":
            path.fillType = pathops.FillType.EVEN_ODD
        pen = path.getPen()
        transform = element.attrib.get("transform")
        if transform:
            match = re.fullmatch(r"translate\(\s*([-\d.]+)[ ,]+([-\d.]+)\s*\)", transform)
            if not match:
                raise ValueError(f"{file}: unsupported SVG transform {transform}")
            dx, dy = map(float, match.groups())
            pen = TransformPen(pen, (1, 0, 0, 1, dx, dy))
        parse_path(element.attrib["d"], pen)
        out = pathops.op(out, path, pathops.PathOp.UNION)
    return out


def fit_bounds(path: pathops.Path, bounds: tuple[float, ...]) -> pathops.Path:
    """Register a native filled variant to the outline's box before weighting."""
    x0, y0, x1, y1 = path.bounds
    a, b, c, d = bounds
    sx, sy = (c - a) / (x1 - x0), (d - b) / (y1 - y0)
    out = pathops.Path()
    path.draw(TransformPen(out.getPen(), (sx, 0, 0, sy, a - sx * x0, b - sy * y0)))
    return out


def source_path(assets: Path, name: str) -> pathops.Path:
    """Framework7 artwork, with derived legacy battery/Wi-Fi font states.

    Live status must use orb_status; these shapes preserve existing font call sites.
    """
    name, _, state = name.partition("@")
    src = framework7_path(assets, name)
    if name == "arrow_up" and state == "upload":
        # Design 1.0 is an arrow OVER a separate bottom bar. Framework7's
        # arrow_up_to_line instead points at a top bar, so compose native
        # arrow/minus paths without adding a box or changing their strokes.
        arrow = fit_bounds(src, (17, 6, 39, 39))
        bar = fit_bounds(framework7_path(assets, "minus"), (10, 47, 46, 51))
        return pathops.op(arrow, bar, pathops.PathOp.UNION)
    if name == "wifi" and state:
        pieces = sorted(components(src), key=lambda p: p.bounds[1], reverse=True)
        keep = max(1, int(state))
        out = pathops.Path()
        for piece in pieces[:keep]:
            out = pathops.op(out, piece, pathops.PathOp.UNION)
        # Empty reception keeps the fan's bottom wedge outlined, distinguishable
        # from a one-bar connection, while preserving its position in the group.
        return pathops.op(out, grow(out, -1.1), pathops.PathOp.DIFFERENCE) if state == "0" else out
    if name == "battery_100" and state:
        body = framework7_path(assets, "battery_0")
        fill = pathops.op(src, body, pathops.PathOp.DIFFERENCE)
        if state.isdecimal():
            x0, y0, x1, y1 = fill.bounds
            fill = fit_bounds(fill, (x0, y0, x0 + (x1 - x0) * int(state) / 100, y1))
        elif state in ("charging", "warning"):
            mark = framework7_path(
                assets, "bolt_fill" if state == "charging" else "exclamationmark"
            )
            mark = fit_bounds(mark, (21, 20.5, 29, 35.5))
            fill = pathops.op(fill, mark, pathops.PathOp.DIFFERENCE)
        else:
            raise ValueError(f"unknown battery state {state}")
        return pathops.op(body, fill, pathops.PathOp.UNION)
    if state:
        raise ValueError(f"unknown icon state {name}@{state}")
    return src


def weighted_battery(assets: Path, asset: str, offset: float) -> pathops.Path:
    """Keep the compatibility battery's clearance and knockout open at bold weights.

    Emboldening the assembled outline would close both its narrow separator and
    the charging bolt. Offset the body and fill independently, then cut the bolt
    out at the current weight. The clearance is about 0.75 px at Body size.
    """
    name, _, state = asset.partition("@")
    regular_body = framework7_path(assets, "battery_0")
    body = grow(regular_body, offset)
    if name == "battery_0":
        return body
    fill = pathops.op(
        framework7_path(assets, "battery_100"), regular_body, pathops.PathOp.DIFFERENCE
    )
    if state.isdecimal():
        x0, y0, x1, y1 = fill.bounds
        fill = fit_bounds(fill, (x0, y0, x0 + (x1 - x0) * int(state) / 100, y1))
    clearance = grow(pathops.op(silhouette(body), body, pathops.PathOp.DIFFERENCE), -1.45)
    fill = pathops.op(grow(fill, offset), clearance, pathops.PathOp.INTERSECTION)
    if state in ("charging", "warning"):
        mark = framework7_path(assets, "bolt_fill" if state == "charging" else "exclamationmark")
        mark = grow(fit_bounds(mark, (21, 20.5, 29, 35.5)), offset)
        fill = pathops.op(fill, mark, pathops.PathOp.DIFFERENCE)
    return pathops.op(body, fill, pathops.PathOp.UNION)


def base_weight(stroke: float) -> str:
    """The heaviest Phosphor weight that isn't heavier than `stroke` grid units."""
    fitting = [w for w, s in PHOSPHOR_STROKES.items() if s <= stroke + 1e-6]
    return max(fitting, key=PHOSPHOR_STROKES.get) if fitting else "thin"


def grow(path: pathops.Path, d: float) -> pathops.Path:
    """The outline moved out by d on every side (in by -d when d is negative), with
    round joins."""
    if abs(d) <= 0.01:
        return pathops.simplify(path)
    ring = pathops.Path()
    path.draw(ring.getPen())
    ring.stroke(2 * abs(d), pathops.LineCap.ROUND_CAP, pathops.LineJoin.ROUND_JOIN, 4)
    ring.convertConicsToQuads(0.05)
    op = pathops.PathOp.UNION if d > 0 else pathops.PathOp.DIFFERENCE
    return pathops.op(path, ring, op)


def _same_contour(a: pathops.Path, b: pathops.Path) -> bool:
    return all(abs(x - y) <= 0.5 for x, y in zip(a.bounds, b.bounds)) and abs(
        abs(a.area) - abs(b.area)
    ) <= 0.01 * abs(b.area)


def knockouts(fill: pathops.Path, regular: pathops.Path) -> pathops.Path:
    """The holes of a filled icon that are the regular outline's ink knocked out of the
    shape (check-circle's check, info's "i", lock's keyhole), as filled areas. Other
    holes are counters (lock's shackle, the gap in record) and stay as grown."""
    ink = [c for c in pathops.simplify(regular, clockwise=True).contours if c.clockwise]
    out = pathops.Path()
    for hole in pathops.simplify(fill, clockwise=True).contours:
        if hole.clockwise or not any(_same_contour(hole, c) for c in ink):
            continue
        out = pathops.op(out, hole, pathops.PathOp.UNION)
    return out


def _region(contour: pathops.Path) -> pathops.Path:
    return pathops.op(pathops.Path(), contour, pathops.PathOp.UNION)


def _area(path: pathops.Path) -> float:
    return sum(
        abs(c.area) * (1 if c.clockwise else -1)
        for c in pathops.simplify(path, clockwise=True).contours
    )


def silhouette(path: pathops.Path) -> pathops.Path:
    """The area inside a path's outer contours: the shape with its counters filled."""
    out = pathops.Path()
    for c in pathops.simplify(path, clockwise=True).contours:
        if c.clockwise:
            out = pathops.op(out, c, pathops.PathOp.UNION)
    return out


def components(path: pathops.Path) -> list[pathops.Path]:
    """The connected pieces of a filled path."""
    contours = [(c.clockwise, _region(c)) for c in pathops.simplify(path, clockwise=True).contours]
    out = []
    for i, (outer, region) in enumerate(contours):
        if not outer:
            continue
        piece = region
        for j, (inner, hole) in enumerate(contours):
            if (
                j != i
                and not inner
                and _area(pathops.op(hole, region, pathops.PathOp.DIFFERENCE)) < 1e-3
            ):
                piece = pathops.op(piece, hole, pathops.PathOp.DIFFERENCE)
        out.append(piece)
    return out


def faces(outline: pathops.Path) -> list[tuple[bool, pathops.Path]]:
    """The regions of an outline icon: (is_ink, region) for every stroke and every counter."""
    ink = pathops.simplify(outline)
    counters = pathops.op(silhouette(outline), ink, pathops.PathOp.DIFFERENCE)
    return [(True, c) for c in components(ink)] + [(False, c) for c in components(counters)]


def filled_form(
    fill: pathops.Path, regular: pathops.Path, outline: pathops.Path, stroke: float
) -> pathops.Path:
    """Phosphor's fill (`fill`, drawn over its `regular` outline) as the filled form of
    `outline`, the outline glyph as drawn with `stroke` (grid units)."""
    d_fill = (stroke - PHOSPHOR_STROKES["regular"]) / 2
    # Phosphor's fill and regular outline at this weight: same skeleton, so every region
    # of the outline is either inside the fill or outside it (or, by design, split).
    holes = grow(knockouts(fill, regular), d_fill)
    grown = pathops.op(grow(fill, d_fill), holes, pathops.PathOp.DIFFERENCE)
    reference = []
    for ink, region in faces(grow(regular, d_fill)):
        area = _area(region)
        covered = (
            _area(pathops.op(region, grown, pathops.PathOp.INTERSECTION)) / area if area else 0
        )
        reference.append((ink, region, covered))
    out = pathops.Path()
    for ink, region in faces(outline):
        same = [
            (_area(pathops.op(region, r, pathops.PathOp.INTERSECTION)), covered)
            for r_ink, r, covered in reference
            if r_ink == ink
        ]
        overlap, covered = max(same, default=(0.0, 1.0 if ink else 0.0))
        if overlap <= 0:
            covered = 1.0 if ink else 0.0
        if covered >= 0.99:
            out = pathops.op(out, region, pathops.PathOp.UNION)
        elif covered > 0.01:
            out = pathops.op(
                out, pathops.op(region, grown, pathops.PathOp.INTERSECTION), pathops.PathOp.UNION
            )
    return out


def icon_scale(cap_height: int, circle_cap_heights: float) -> float:
    """Font units per Framework7 grid unit."""
    return circle_cap_heights * cap_height / FRAMEWORK7_CIRCLE


def build(
    assets: Path,
    glyphs: list[tuple[str, int, str, bool]],
    groups: dict[str, str],
    stroke: float,
    scale: float,
    centre_y: float,
    side_bearing: int,
    ascent: int,
    descent: int,
    space: int,
    family: str,
    px: int,
    out: Path,
) -> dict:
    """Write a TrueType font. Stroke and scale use Framework7's 56-unit grid.
    Glyph tuples hold (macro, codepoint, asset name, is_fill). Shared metrics groups
    get identical advances and source centres; native fill artwork is retained.
    """
    d = (stroke - FRAMEWORK7_STEM) / 2
    placed: dict[str, pathops.Path] = {}
    for macro, _, asset, is_fill in glyphs:
        if asset.startswith("phosphor:"):
            name = asset.removeprefix("phosphor:")
            factor = FRAMEWORK7_CIRCLE / PHOSPHOR_CIRCLE
            fallback_stroke = stroke / factor
            weight = base_weight(fallback_stroke)
            outline = grow(
                _svg_path(assets / "phosphor", weight, name),
                (fallback_stroke - PHOSPHOR_STROKES[weight]) / 2,
            )
            src = (
                filled_form(
                    _svg_path(assets / "phosphor", "fill", name),
                    _svg_path(assets / "phosphor", "regular", name),
                    outline,
                    fallback_stroke,
                )
                if is_fill
                else outline
            )
            normalized = pathops.Path()
            src.draw(
                TransformPen(
                    normalized.getPen(),
                    (factor, 0, 0, factor, GRID / 2 - 128 * factor, GRID / 2 - 128 * factor),
                )
            )
            src = normalized
        elif asset.startswith("battery_"):
            src = weighted_battery(assets, asset, d)
        else:
            outline = source_path(assets, asset)
            if is_fill:
                native = source_path(assets, spec.icon_asset(asset, macro, True))
                native = fit_bounds(native, outline.bounds)
                holes = grow(knockouts(native, outline), d)
                src = pathops.op(grow(native, d), holes, pathops.PathOp.DIFFERENCE)
                # Offsetting can move extrema on slanted paths by a fraction of a
                # unit; register to the actual weighted outline once more.
                src = fit_bounds(src, grow(outline, d).bounds)
            else:
                src = grow(outline, d)
        # SVG y runs down; every source centre goes to (0, centre_y).
        path = pathops.Path()
        src.draw(
            TransformPen(
                path.getPen(),
                (scale, 0, 0, -scale, -GRID / 2 * scale, centre_y + GRID / 2 * scale),
            )
        )
        placed[macro] = pathops.simplify(path, clockwise=True)
    # A filled form covers exactly its outline, so swapping them moves and resizes nothing.
    worst_fill = 0.0
    for macro, _, _, is_fill in glyphs:
        if is_fill:
            outline = macro.removesuffix("_FILL")
            err = max(abs(a - b) for a, b in zip(placed[macro].bounds, placed[outline].bounds))
            err_px = err * px / UPM
            assert err_px <= 0.5, f"{family}: {macro} is {err_px:.2f} px off {outline}'s bounds"
            worst_fill = max(worst_fill, err_px)
    # Horizontal extent of each metrics group: the union of its members' ink.
    extent: dict[str, tuple[float, float]] = {}
    for macro, path in placed.items():
        x0, _, x1, _ = path.bounds
        g = groups.get(macro, macro)
        old = extent.get(g, (x0, x1))
        extent[g] = (min(old[0], x0), max(old[1], x1))

    names, cmap, outlines, metrics = [".notdef", "space"], {0x20: "space"}, {}, {}
    outlines[".notdef"] = TTGlyphPen(None).glyph()
    metrics[".notdef"] = (UPM // 2, 0)
    outlines["space"] = TTGlyphPen(None).glyph()  # so "ICON ICON" works in icon-only labels
    metrics["space"] = (space, 0)
    for macro, cp, _, _ in glyphs:
        x0, x1 = extent[groups.get(macro, macro)]
        pen = TTGlyphPen(None)
        placed[macro].draw(
            TransformPen(
                Cu2QuPen(ExtremaPen(pen), max_err=1.0, reverse_direction=False),
                (1, 0, 0, 1, side_bearing - x0, 0),
            )
        )
        name = macro.lower()
        names.append(name)
        cmap[cp] = name
        outlines[name] = pen.glyph()
        metrics[name] = (round(x1 - x0 + 2 * side_bearing), 0)

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(outlines)
    glyf = fb.font["glyf"]
    for name in names:
        glyph = glyf[name]
        glyph.recalcBounds(glyf)
        metrics[name] = (metrics[name][0], glyph.xMin if glyph.numberOfContours else 0)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=ascent, descent=descent)
    fb.setupOS2(
        sTypoAscender=ascent,
        sTypoDescender=descent,
        usWinAscent=ascent,
        usWinDescent=-descent,
    )
    fb.setupPost()
    fb.setupNameTable({"familyName": family, "styleName": "Regular"})
    fb.setupMaxp()
    fb.font.recalcTimestamp = False
    fb.font["head"].created = fb.font["head"].modified = 2082844800
    fb.font.save(out)
    return {
        "base": "Framework7 regular",
        "grow": round(d, 2),
        "grow_fill": round(d, 2),
        "fill_bounds_px": round(worst_fill, 3),
        "glyphs": len(names) - 2,
    }
