"""PNG previews of Orb's fonts (docs/screens/fonts).

Glyphs are rasterised by Pillow (FreeType) from the same instanced TrueType files and
pixel sizes that lv_font_conv converted, then quantised to 4 bpp and laid out with
LVGL's rules: advances in 1/16 px, kerning as lv_font_conv stores it, each glyph's
advance rounded to whole pixels, fallback fonts for missing glyphs, letter_space after
every glyph and line pitch = font line height + line_space. So the previews show what
the firmware draws, and render_all() checks Pillow's glyphs against lv_font_conv's.
"""

from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

import pack
import spec

WHITE = (255, 255, 255)
GREY = (142, 142, 147)  # systemGray
ORANGE = (255, 146, 48)
BLACK = (0, 0, 0)

SAMPLES = ["Sessions", "Needs your approval", "The quick brown fox"]
TABULAR_SAMPLE = "Elapsed: 12" + spec.TIME_SEP + "45 · 3,601.25 · −18%"
ICON_ROW = [
    "MIC",
    "MIC_OFF",
    "WAVEFORM",
    "CHECK",
    "X",
    "CHEVRON_RIGHT",
    "GEAR",
    "WIFI_HIGH",
    "BATTERY_CHARGING",
]
ICON_ROW += ["BELL", "MOON", "TERMINAL", "SPARKLE", "WARNING"]


def icon(macro: str) -> str:
    for m, cp, _, _ in spec.icon_codepoints():
        if m == macro:
            return chr(cp)
    raise KeyError(macro)


class LvFont:
    """One generated font as LVGL sees it (metrics from lv_font_conv's dump), with glyph
    images from Pillow."""

    def __init__(self, name: str, build: Path, fonts: dict, cache: dict):
        f = fonts[name]
        self.name = name
        self.f = f
        info = f.info["dump"]
        self.glyphs = {g["code"]: g for g in info["glyphs"]}
        c = f.info["c"]
        self.line_height, self.base_line = c["line_height"], c["base_line"]
        # Advances and kerning exactly as lv_font_conv stored them for LVGL.
        self.lv = LvTables(pack.parse_c((build / "c" / f"{name}.c").read_text()))
        ttf = str(build / "ttf" / f"{name}.ttf")
        self.pil = ImageFont.truetype(ttf, f.px * SUPERSAMPLE, layout_engine=ImageFont.Layout.BASIC)
        self.fallback = LvFont.get(f.fallback, build, fonts, cache) if f.fallback else None
        self._img: dict[int, tuple] = {}

    @staticmethod
    def get(name: str, build: Path, fonts: dict, cache: dict) -> "LvFont":
        if name not in cache:
            cache[name] = LvFont(name, build, fonts, cache)
        return cache[name]

    def resolve(self, cp: int) -> "LvFont | None":
        f = self
        while f and cp not in f.glyphs:
            f = f.fallback
        return f

    def advance(self, cp: int, nxt: int) -> int:
        return self.lv.advance(cp, nxt)

    def image(self, cp: int) -> tuple[Image.Image | None, int, int]:
        """4 bpp coverage image and its offset from the pen position on the baseline."""
        if cp not in self._img:
            g = self.glyphs[cp]
            if g["bbox"]["width"] == 0 or g["bbox"]["height"] == 0:
                self._img[cp] = (None, 0, 0)
            else:
                mask, ox, oy = _mask(self.pil, cp)
                self._img[cp] = (mask.point(_Q4) if mask else None, ox, oy)
        return self._img[cp]

    def parity(self) -> list[int]:
        """How far Pillow's glyphs are from lv_font_conv's: counts of inked pixels by
        difference in 4 bpp steps (index 0: identical), on the same baseline origin."""
        steps = [0] * 16
        for cp, g in self.glyphs.items():
            b = g["bbox"]
            mask, ox, oy = _mask(self.pil, cp)
            if not b["width"] or not b["height"]:
                steps[15] += 1 if mask else 0
                continue
            m = 8
            frame = (b["width"] + 2 * m, b["height"] + 2 * m)
            top = b["y"] + b["height"]  # rows above the baseline
            ours = Image.new("L", frame)
            if mask:
                ours.paste(mask, (ox - b["x"] + m, oy + top + m))
            ref = Image.new("L", frame)
            ref.paste(
                Image.frombytes("L", (b["width"], b["height"]), bytes(sum(g["pixels"], []))),
                (m, m),
            )
            ours, ref = ours.point(_Q4), ref.point(_Q4)
            inked = ImageChops.lighter(ours, ref).point(lambda v: 255 if v else 0)
            diff = ImageChops.difference(ours, ref).histogram(mask=inked)
            for v, n in enumerate(diff):
                steps[v // 17] += n
        return steps


class LvTables:
    """A font's glyph ids, advances and kerning as LVGL's fmt_txt code reads them
    (lv_font_get_glyph_dsc_fmt_txt), so preview layout matches the firmware to the pixel."""

    def __init__(self, font: dict):
        self.font = font
        self.gid: dict[int, int] = {}
        for c in font["cmaps"]:
            if c["type"] in (2, 0):  # format 0 tiny / full
                for rcp in range(c["range_length"]):
                    ofs = c["glyph_id_ofs_list"][rcp] if c["type"] == 0 else rcp
                    self.gid[c["range_start"] + rcp] = c["glyph_id_start"] + ofs
            else:  # sparse tiny / full
                for k, u in enumerate(c["unicode_list"]):
                    ofs = c["glyph_id_ofs_list"][k] if c["type"] == 1 else k
                    self.gid[c["range_start"] + u] = c["glyph_id_start"] + ofs
        k = font["kern"]
        self.pairs = {}
        if k and k["type"] == 2:
            ids = k["ids"]
            self.pairs = {(ids[2 * i], ids[2 * i + 1]): v for i, v in enumerate(k["values"])}

    def kern(self, left: int, right: int) -> int:
        k = self.font["kern"]
        if not k:
            return 0
        if k["type"] == 1:
            lc, rc = k["left"][left], k["right"][right]
            return k["values"][(lc - 1) * k["right_cnt"] + rc - 1] if lc and rc else 0
        return self.pairs.get((left, right), 0)

    def advance(self, cp: int, nxt: int) -> int:
        gid = self.gid[cp]
        adv = self.font["glyphs"][gid][1]
        if nxt and nxt in self.gid:
            adv += (self.kern(gid, self.gid[nxt]) * self.font["kern_scale"]) >> 4
        return (max(0, adv) + 8) >> 4


def _Q4(v: int) -> int:
    """lv_font_conv's 8 to 4 bpp step (floor), as LVGL expands it back (x 17)."""
    return (v >> 4) * 17


SUPERSAMPLE = 16


def _mask(big: ImageFont.FreeTypeFont, cp: int) -> tuple[Image.Image | None, int, int]:
    """Pillow's coverage of one glyph, unhinted, and its top-left relative to the pen on
    the baseline. lv_font_conv renders unhinted (--autohint-off) but Pillow always hints,
    so `big` is the font at 16x the size: the glyph is drawn with the pen on a pixel corner
    and box-filtered down: the exact area coverage FreeType computes unhinted, to within the
    hinter's movement at 16x (under 1/32 px)."""
    ss = SUPERSAMPLE
    left, top, right, bottom = big.getbbox(chr(cp), anchor="ls")
    if right <= left or bottom <= top:
        return None, 0, 0
    x0, y0 = (left // ss - 1) * ss, (top // ss - 1) * ss
    x1, y1 = (-(-right // ss) + 1) * ss, (-(-bottom // ss) + 1) * ss
    canvas = Image.new("L", (x1 - x0, y1 - y0), 0)
    ImageDraw.Draw(canvas).text((-x0, -y0), chr(cp), font=big, fill=255, anchor="ls")
    small = canvas.resize(((x1 - x0) // ss, (y1 - y0) // ss), Image.BOX)
    box = small.getbbox()
    if not box:
        return None, 0, 0
    return small.crop(box), x0 // ss + box[0], y0 // ss + box[1]


@dataclass
class Run:
    text: str
    font: LvFont
    color: tuple = WHITE
    letter_space: int = 0


def measure(run: Run) -> int:
    x = 0
    cps = [ord(c) for c in run.text]
    for i, cp in enumerate(cps):
        f = run.font.resolve(cp)
        if not f:
            raise SystemExit(f"{run.font.name}: no glyph for U+{cp:04X}")
        nxt = cps[i + 1] if i + 1 < len(cps) else 0
        x += f.advance(cp, nxt if nxt in f.glyphs else 0) + run.letter_space
    return x - (run.letter_space if cps else 0)


def draw(img: Image.Image, x: int, baseline: int, run: Run) -> int:
    cps = [ord(c) for c in run.text]
    solid = Image.new("RGB", (1, 1), run.color)
    for i, cp in enumerate(cps):
        f = run.font.resolve(cp)
        nxt = cps[i + 1] if i + 1 < len(cps) else 0
        mask, ox, oy = f.image(cp)
        if mask is not None:
            img.paste(solid.resize(mask.size), (x + ox, baseline + oy), mask)
        x += f.advance(cp, nxt if nxt in f.glyphs else 0) + run.letter_space
    return x


def ascent(font: LvFont) -> int:
    return font.line_height - font.base_line


class Ctx:
    def __init__(self, build: Path, fonts: dict, sets: list):
        self.build, self.fonts, self.sets = build, fonts, sets
        self.styles, self.clocks = sets[0].styles, sets[0].clocks  # the default set
        self.cache: dict = {}

    def font(self, name: str) -> LvFont:
        return LvFont.get(name, self.build, self.fonts, self.cache)

    def style(self, enum: str, fs=None) -> dict:
        return next(r for r in (fs or self.sets[0]).styles if r["style"].enum == enum)

    def text(self, enum: str, variant: str = "regular", kind: str = "text", fs=None) -> LvFont:
        return self.font(self.style(enum, fs)[variant][kind])


def style_sheet(ctx: Ctx, row: dict) -> Image.Image:
    s = row["style"]
    note = ctx.text("FOOTNOTE2")
    columns = []
    for variant in ("regular", "emphasized"):
        v = row[variant]
        text, tnum, icons = (
            ctx.font(v["text"]),
            ctx.font(v["tnum"]),
            ctx.font(v["icons"]),
        )
        label = (
            f"{s.name} · {s.px} px · Inter {spec.WEIGHT_NAMES[v['weight']].title()}, opsz {s.opsz}"
        )
        detail = (
            f"leading {s.leading_px} · tracking {s.tracking:+d}/1000 em ({s.tracking_px:+.2f} px)"
        )
        if variant == "emphasized":
            label += " (emphasized)"
        lines = [Run(label, note, GREY), Run(detail, note, GREY)]
        ls = row["letter_space"]
        lines += [Run(t, text, WHITE, ls) for t in SAMPLES]
        lines += [Run(TABULAR_SAMPLE, tnum, WHITE, ls)]
        lines += [Run(" ".join(icon(m) for m in ICON_ROW), icons, WHITE, 0)]
        lines += [Run(icon("TERMINAL") + " Sessions " + icon("CHEVRON_RIGHT"), text, WHITE, ls)]
        columns.append(lines)
    pad, gap, note_pitch = 24, 48, 25
    width = max(measure(r) for col in columns for r in col)
    rows = len(columns[0]) - 2
    first = max(ascent(r.font) for col in columns for r in col[2:3])
    height = pad + 2 * note_pitch + 8 + first + (rows - 1) * s.leading_px + s.px // 3 + pad
    img = Image.new("RGB", (pad * 2 + width * 2 + gap, height), BLACK)
    for ci, col in enumerate(columns):
        x0 = pad + ci * (width + gap)
        y = pad
        for r in col[:2]:
            draw(img, x0, y + ascent(note), r)
            y += note_pitch
        # Lines are leading_px apart, baseline to baseline, as orb_text_apply() spaces them.
        baseline = y + 8 + first
        for r in col[2:]:
            draw(img, x0, baseline, r)
            baseline += s.leading_px
    return img


def clock_sheet(ctx: Ctx) -> Image.Image:
    pad = 24
    note = ctx.text("FOOTNOTE2")
    rows = []
    clocks = ctx.clocks + ctx.sets[1].clocks  # and with Bold Text
    for i, c in enumerate(clocks):
        face, f = c["clock"], ctx.font(c["font"])
        label = (
            f"Clock {face.enum.lower()} · {face.px} px · Inter {spec.WEIGHT_NAMES[face.weight].title()}"
            + (" (Bold Text)" if i >= len(ctx.clocks) else "")
        )
        rows.append(
            (
                Run(label, note, GREY),
                [
                    Run(t, f, WHITE, c["letter_space"])
                    for t in ("10:09", "12:45 PM", "00" + spec.TIME_SEP + "00")
                ],
            )
        )
    width = max(sum(measure(r) for r in rs) + 60 * (len(rs) - 1) for _, rs in rows)
    height = pad * 2 + sum(40 + ctx.font(c["font"]).line_height + 24 for c in clocks)
    img = Image.new("RGB", (width + pad * 2, height), BLACK)
    y = pad
    for (label, rs), c in zip(rows, clocks):
        draw(img, pad, y + ascent(note), label)
        y += 40
        x = pad
        f = ctx.font(c["font"])
        for r in rs:
            draw(img, x, y + ascent(f), r)
            x += measure(r) + 60
        y += f.line_height + 24
    return img


def icon_sheet(ctx: Ctx) -> Image.Image:
    """Every icon at Body size, regular and emphasized, next to text of the same weight."""
    # Each icon with its filled form right after it.
    glyphs = sorted(
        spec.icon_codepoints(), key=lambda g: (g[1] - (spec.FILL_OFFSET if g[3] else 0), g[3])
    )
    reg, emph = ctx.text("BODY", "regular", "icons"), ctx.text("BODY", "emphasized", "icons")
    body, headline = ctx.text("BODY"), ctx.text("BODY", "emphasized")
    note = ctx.text("FOOTNOTE2")
    cols, cell_w, cell_h, pad = 6, 270, 70, 24
    rows = -(-len(glyphs) // cols)
    head = 70
    img = Image.new("RGB", (pad * 2 + cols * cell_w, pad * 2 + head + rows * cell_h), BLACK)
    draw(
        img,
        pad,
        pad + ascent(body),
        Run("Body Regular " + icon("GEAR") + icon("MIC") + icon("CHECK_CIRCLE"), body),
    )
    x = (
        pad
        + measure(
            Run(
                "Body Regular " + icon("GEAR") + icon("MIC") + icon("CHECK_CIRCLE"),
                body,
            )
        )
        + 40
    )
    draw(
        img,
        x,
        pad + ascent(body),
        Run("Semibold " + icon("GEAR") + icon("MIC") + icon("CHECK_CIRCLE"), headline),
    )
    for i, (macro, cp, _, _) in enumerate(glyphs):
        cx, cy = pad + (i % cols) * cell_w, pad + head + (i // cols) * cell_h
        draw(img, cx, cy + ascent(reg), Run(chr(cp), reg))
        draw(img, cx + 40, cy + ascent(emph), Run(chr(cp), emph))
        draw(img, cx, cy + 34 + ascent(note), Run(macro.lower(), note, GREY))
    return img


def type_sizes_sheet(ctx: Ctx, bold: bool) -> Image.Image:
    """Every style at each content size (Default, Larger, AX3), as Dynamic Type shows them."""
    sets = [fs for fs in ctx.sets if fs.bold == bold]
    note = ctx.text("FOOTNOTE2")
    pad, gap, col_w = 24, 40, 0
    columns = []
    for fs in sets:
        runs = []
        for r in fs.styles:
            s = r["style"]
            f = ctx.font(r["regular"]["text"])
            text = s.name + " " + icon("CHECK_CIRCLE")
            runs.append((s, Run(text, f, WHITE, r["letter_space"])))
        col_w = max(col_w, max(measure(run) for _, run in runs))
        columns.append((fs, runs))
    col_w = max(col_w, 300)
    height = (
        pad
        + 30
        + sum(
            max(fs.styles[i]["style"].leading_px for fs in sets) + 6
            for i in range(len(spec.STYLES))
        )
    )
    img = Image.new(
        "RGB", (pad * 2 + len(sets) * col_w + (len(sets) - 1) * gap, height + pad), BLACK
    )
    for ci, (fs, runs) in enumerate(columns):
        x = pad + ci * (col_w + gap)
        body = next(r["style"] for r in fs.styles if r["style"].enum == "BODY")
        apple = f" ({fs.size.apple})" if fs.size.apple != fs.size.name else ""
        draw(img, x, pad + ascent(note), Run(f"{fs.title}{apple} · Body {body.px} px", note, GREY))
        y = pad + 30
        for i, (s, run) in enumerate(runs):
            pitch = max(f.styles[i]["style"].leading_px for f in sets) + 6
            draw(img, x, y + ascent(run.font), run)
            y += pitch
    return img


def round_screen(img: Image.Image) -> Image.Image:
    """Black outside the 466 px circle, as the panel shows it."""
    mask = Image.new("L", (466 * 4, 466 * 4), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, 466 * 4 - 1, 466 * 4 - 1), fill=255)
    mask = mask.resize((466, 466), Image.LANCZOS)
    out = Image.new("RGB", (466, 466), BLACK)
    out.paste(img, (0, 0), mask)
    return out


def centred(img: Image.Image, y_baseline: int, run: Run, cx: int = 233) -> None:
    draw(img, cx - measure(run) // 2, y_baseline, run)


def sessions_screen(ctx: Ctx) -> Image.Image:
    img = Image.new("RGB", (466, 466), BLACK)
    title = ctx.text("TITLE2", "emphasized")
    head = ctx.text("HEADLINE")
    foot = ctx.text("FOOTNOTE1")
    icons = ctx.text("HEADLINE", "regular", "icons")
    centred(img, 92, Run("Sessions", title, WHITE, ctx.style("TITLE2")["letter_space"]))
    rows = [
        ("TERMINAL", "esp32s3-firmware", "Needs your approval", ORANGE),
        ("SPARKLE", "alex-site", "Writing the blog post…", GREY),
        ("CODE", "demo-project", "Idle · 12 min", GREY),
    ]
    y = 160
    for ic, name, sub, colour in rows:
        draw(img, 70, y + 4, Run(icon(ic), icons, colour if colour != GREY else WHITE))
        draw(img, 118, y, Run(name, head))
        draw(
            img,
            118,
            y + 30,
            Run(sub, foot, colour, ctx.style("FOOTNOTE1")["letter_space"]),
        )
        y += 82
    pill = Run(icon("MIC") + " Hold to talk", ctx.text("BODY", "emphasized"))
    w = measure(pill) + 56
    ImageDraw.Draw(img).rounded_rectangle(
        (233 - w // 2, 392, 233 + w // 2, 392 + 56), 28, fill=(28, 28, 30)
    )
    centred(img, 392 + 38, pill)
    return round_screen(img)


def render_all(out: Path, build: Path, fonts: dict, sets: list) -> None:
    ctx = Ctx(build, fonts, sets)
    styles = ctx.styles
    out.mkdir(parents=True, exist_ok=True)
    # Only overwrite this renderer's outputs. The face and live status sheet are
    # produced by the real LVGL host renderer after this step (scripts/fonts.sh).
    for row in styles:
        style_sheet(ctx, row).save(out / f"{row['style'].enum.lower()}.png", optimize=True)
    clock_sheet(ctx).save(out / "clock.png", optimize=True)
    icon_sheet(ctx).save(out / "icons.png", optimize=True)
    type_sizes_sheet(ctx, False).save(out / "content-sizes.png", optimize=True)
    type_sizes_sheet(ctx, True).save(out / "bold-text.png", optimize=True)
    sessions_screen(ctx).save(out / "screen-sessions.png", optimize=True)
    steps = [0] * 16
    for name in sorted(fonts):
        steps = [a + b for a, b in zip(steps, ctx.font(name).parity())]
    total = sum(steps) or 1
    worst = max(i for i, n in enumerate(steps) if n) if any(steps) else 0
    print(
        f"previews: {out}\n  Pillow vs lv_font_conv, inked glyph pixels: {steps[0] / total:.1%} identical, "
        f"{(steps[0] + steps[1]) / total:.2%} within one 4 bpp step, worst {worst} steps"
    )
