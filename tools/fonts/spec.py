"""What Orb's fonts contain: text styles at each content size, Bold Text, glyph sets,
clock faces and icons.

Everything the generator makes is derived from this file. Sizes come from Apple's
watchOS Dynamic Type tables (HIG, Typography > Specifications) converted at
1.634 px/pt, as docs/hig.md section 4 sets out. Icon codepoints are part of the
firmware ABI: append new icons, never renumber.
"""

from dataclasses import dataclass

# 466 px over 1.75 in is 266.3 ppi; Apple's point is 1/163 in (docs/hig.md section 3).
PX_PER_PT = 266.3 * 2 / 326

WEIGHT_NAMES = {
    300: "light",
    400: "regular",
    500: "medium",
    600: "semibold",
    700: "bold",
    800: "extrabold",
}

# Bold Text (docs/hig.md section 4) moves every weight one step up Apple's ladder
# (Regular, Medium, Semibold, Bold, Heavy); Inter's 800 is its Heavy.
BOLD_TEXT = {300: 400, 400: 500, 500: 600, 600: 700, 700: 800}

# Inter's opsz axis runs continuously from 14 to 32. Like SF's dynamic optical sizes,
# every size gets the optical size of its point size (Text at 14 pt and below,
# Display at 32 pt and up, in between at that point size).
OPSZ_MIN = 14
OPSZ_MAX = 32


def opsz_for_pt(pt: float) -> int:
    return min(OPSZ_MAX, max(OPSZ_MIN, round(pt)))


# Glyph bitmaps of fonts at least this big are RLE-compressed. Small sizes stay plain:
# they are drawn many times per frame (lists, body text) and decoding plain 4 bpp is
# several times cheaper than LVGL's bit-by-bit RLE decoder.
COMPRESS_MIN_PX = 33

# SF Compact's tracking by point size, 1/1000 em (HIG, Typography > Tracking values >
# watchOS). Apple tracks by size, not by style, so every content size uses this table.
SF_COMPACT_TRACKING = {
    6: 50, 7: 30, 8: 30, 9: 30, 10: 30, 11: 24, 12: 20, 13: 16, 14: 14, 15: 4, 16: 0,
    17: -4, 18: -8, 19: -12, 20: 0, 21: -2, 22: -4, 23: -6, 24: -8, 25: -10, 26: -11,
    27: -12, 28: -12, 29: -14, 30: -14, 31: -15, 32: -16, 33: -17, 34: -18, 35: -18,
    36: -20, 37: -20, 38: -20, 39: -20, 40: -20, 41: -20, 42: -20, 43: -20, 44: -20,
    45: -20, 46: -20, 47: -20, 48: -20, 49: -21, 50: -21, 51: -21, 52: -21, 53: -22,
    54: -22, 56: -22, 58: -22, 60: -22, 62: -22, 64: -23, 66: -24, 68: -24, 70: -24,
    72: -24, 76: -25, 80: -26, 84: -26, 88: -26, 92: -28, 96: -28,
}  # fmt: skip


def tracking_for_pt(pt: float) -> int:
    assert pt == int(pt) and int(pt) in SF_COMPACT_TRACKING, f"no SF Compact tracking for {pt} pt"
    return SF_COMPACT_TRACKING[int(pt)]


@dataclass(frozen=True)
class Style:
    """One of Apple's text styles, independent of size."""

    enum: str  # ORB_TEXT_<enum>
    name: str
    weight: int
    emphasized_weight: int  # Apple: Large Title becomes Bold, every other style Semibold


STYLES = [
    Style("LARGE_TITLE", "Large Title", 400, 700),
    Style("TITLE1", "Title 1", 400, 600),
    Style("TITLE2", "Title 2", 400, 600),
    Style("TITLE3", "Title 3", 400, 600),
    Style("HEADLINE", "Headline", 600, 600),
    Style("BODY", "Body", 400, 600),
    Style("CAPTION1", "Caption 1", 400, 600),
    Style("CAPTION2", "Caption 2", 400, 600),
    Style("FOOTNOTE1", "Footnote 1", 400, 600),
    Style("FOOTNOTE2", "Footnote 2", 400, 600),
]


@dataclass(frozen=True)
class ContentSize:
    enum: str  # ORB_CONTENT_SIZE_<enum>
    name: str
    apple: str  # Apple's name for the size category
    points: list[tuple[float, float]]  # (size, leading) in pt, in STYLES order


# Apple's watchOS Dynamic Type sizes (HIG, Typography > Specifications), as chosen in
# docs/hig.md section 4: xLarge by default, xxxLarge for Larger Text, AX3 for the
# largest accessibility size.
CONTENT_SIZES = [
    ContentSize("DEFAULT", "Default", "xLarge", [
        (40, 42.5), (38, 40.5), (30, 32.5), (20, 22.5), (17, 19.5),
        (17, 19.5), (16, 18.5), (15, 17.5), (14, 16.5), (13, 15.5),
    ]),
    ContentSize("LARGER", "Larger", "xxxLarge", [
        (42, 44.5), (40, 42.5), (32, 34.5), (22, 24.5), (19, 21.5),
        (19, 21.5), (18, 20.5), (17, 19.5), (16, 18.5), (15, 17.5),
    ]),
    ContentSize("AX3", "AX3", "AX3", [
        (46, 48.5), (44, 47), (36, 38.5), (26, 28.5), (23, 25.5),
        (23, 25.5), (20, 22.5), (19, 21.5), (18, 20.5), (17, 19.5),
    ]),
]  # fmt: skip


def px(pt: float) -> int:
    return round(pt * PX_PER_PT)


@dataclass(frozen=True)
class TextStyle:
    """A style at one content size, with or without Bold Text."""

    style: Style
    size: ContentSize
    bold_text: bool
    pt: float
    leading_pt: float

    @property
    def enum(self) -> str:
        return self.style.enum

    @property
    def name(self) -> str:
        return self.style.name

    @property
    def px(self) -> int:
        return px(self.pt)

    @property
    def leading_px(self) -> int:
        return px(self.leading_pt)

    @property
    def weight(self) -> int:
        w = self.style.weight
        return BOLD_TEXT[w] if self.bold_text else w

    @property
    def emphasized_weight(self) -> int:
        w = self.style.emphasized_weight
        return BOLD_TEXT[w] if self.bold_text else w

    @property
    def opsz(self) -> int:
        return opsz_for_pt(self.pt)

    @property
    def tracking(self) -> int:
        return tracking_for_pt(self.pt)

    @property
    def tracking_px(self) -> float:
        return self.tracking * self.px / 1000


def text_styles(size: ContentSize, bold_text: bool) -> list[TextStyle]:
    return [
        TextStyle(style, size, bold_text, pt, leading)
        for style, (pt, leading) in zip(STYLES, size.points, strict=True)
    ]


# The firmware's default: Default size, Bold Text off. These fonts live in the app image;
# every other combination is in the font pack in the assets partition.
DEFAULT_SIZE = CONTENT_SIZES[0]
TEXT_STYLES = text_styles(DEFAULT_SIZE, False)


@dataclass(frozen=True)
class ClockFace:
    enum: str  # ORB_CLOCK_<enum>
    px: int
    weight: int
    tracking: int  # 1/1000 em

    def with_bold_text(self, bold_text: bool) -> "ClockFace":
        return ClockFace(
            self.enum, self.px, BOLD_TEXT[self.weight] if bold_text else self.weight, self.tracking
        )


# Large tabular digits for the face, timers and the stopwatch. Clocks don't follow the
# content size (they are already as big as the screen allows); Bold Text makes them heavier.
CLOCK_FACES = [
    ClockFace("LARGE", 140, 600, -10),
    ClockFace("MEDIUM", 96, 600, -10),
    # Owner-requested low-ink Always On exception to HIG's general no-Light rule.
    # The owner permits the frame's needed 128 px size and -4 px tracking;
    # Bold Text moves Light to Regular.
    ClockFace("ALWAYS_ON", 128, 300, -31),
]
CLOCK_OPSZ = OPSZ_MAX


def _chars(*parts: str) -> list[int]:
    out: set[int] = set()
    for part in parts:
        out.update(ord(c) for c in part)
    return sorted(out)


# U+2236 RATIO is the time separator: a colon raised to the figures' centre, which LVGL
# can't pick by context the way a shaping engine does (orb_fonts.h: ORB_TIME_SEP).
TIME_SEP = "∶"

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
LATIN1 = "".join(chr(c) for c in range(0xA0, 0x100) if c != 0xAD)  # no soft hyphen in Inter
# Windows-1252 extras (Western European names), typographic punctuation, spaces,
# maths and the arrows and keys a coding-agent UI shows. Each one must exist in Inter.
TYPOGRAPHIC = (
    "‘’‚“”„"  # quotes ‘ ’ ‚ “ ” „
    "‐‑‒–—…"  # hyphen, nb hyphen, figure dash, – — …
    "•‹›†‡‰′″⁄"  # • ‹ › † ‡ ‰ ′ ″ ⁄
    "€₹™№ƒ"  # € ₹ ™ № ƒ
    "ŒœŠšŽžŸıˆ˜"  # Œ œ Š š Ž ž Ÿ ı ˆ ˜
    "−≈≠≤≥∞"  # − ≈ ≠ ≤ ≥ ∞
    "←↑→↓↔"  # ← ↑ → ↓ ↔
    "⇧⌘⌥⌃⌫⏎"  # ⇧ ⌘ ⌥ ⌃ ⌫ ⏎
    "✓✗●○★☆"  # ✓ ✗ ● ○ ★ ☆
    "\u2007\u2009\u200a\u202f" + TIME_SEP  # figure, thin, hair and narrow no-break spaces
)
TEXT_CODEPOINTS = _chars(ASCII, LATIN1, TYPOGRAPHIC)

# Tabular companions: these characters come from a tnum-frozen font whose fallback is
# the style's normal font, so "12∶05" never jiggles and everything else is unchanged.
# ':' is the tabular colon on the baseline, as after a word ("Elapsed: 01∶23"); the time
# separator is TIME_SEP. '-' is left to the text font: a tabular label still says "Wi-Fi".
TABULAR_CODEPOINTS = _chars("0123456789:.,+− " + TIME_SEP)
CLOCK_CODEPOINTS = _chars("0123456789: AMP" + TIME_SEP)


@dataclass(frozen=True)
class Icon:
    macro: str  # ORB_ICON_<macro>
    asset: str  # Framework7 5.0.5 asset, or phosphor:<name> for explicit fallbacks
    codepoint: int
    fill: bool = False  # also ORB_ICON_<macro>_FILL at codepoint + FILL_OFFSET
    group: str = ""  # icons that replace each other in place share their width and position


# Framework7 already draws sparkles solid; its public filled alias stays ABI-compatible.
FILL_ASSETS = {"SPARKLE": "sparkles", "RECORD": "smallcircle_fill_circle"}


def icon_asset(asset: str, macro: str, is_fill: bool) -> str:
    """Concrete source asset, retaining a selector for derived compatibility states."""
    if asset.startswith("phosphor:") or not is_fill:
        return asset
    return FILL_ASSETS.get(macro.removesuffix("_FILL"), asset + "_fill")


ICON_BASE = 0xE000
FILL_OFFSET = 0x100

# Icon size: Framework7's circle (the outer edge of circle-based icons) is as tall
# as 1.22 cap heights of the text, and its centre sits at half the cap height, the way
# SF Symbols' medium scale lines up with text. Every text size has its own icon size.
ICON_CIRCLE_CAP_HEIGHTS = 1.22
# Stroke weight: this fraction of the text's vertical stem at the same size and weight.
ICON_STROKE_RATIO = 0.9
# Space on each side of the ink, in em.
ICON_SIDE_BEARING = 0.08

# fmt: off
ICONS = [
    # Actions and common UI
    Icon("CHECK", "checkmark", 0xE000),
    Icon("X", "xmark", 0xE001),
    Icon("PLUS", "plus", 0xE002),
    Icon("MINUS", "minus", 0xE003),
    Icon("CHECK_CIRCLE", "checkmark_circle", 0xE004, fill=True, group="toggle"),
    Icon("X_CIRCLE", "xmark_circle", 0xE005, fill=True, group="toggle"),
    Icon("CIRCLE", "circle", 0xE006, fill=True, group="toggle"),
    Icon("DOTS_THREE", "ellipsis", 0xE007),
    Icon("PROHIBIT", "slash_circle", 0xE008),
    Icon("TRASH", "trash", 0xE009),
    Icon("PENCIL", "pencil", 0xE00A),
    Icon("COPY", "doc_on_doc", 0xE00B),
    Icon("SHARE", "square_arrow_up", 0xE00C),
    Icon("MAGNIFYING_GLASS", "search", 0xE00D),
    Icon("LIST", "line_horizontal_3", 0xE00E),
    Icon("SLIDERS", "slider_horizontal_3", 0xE00F),
    Icon("GEAR", "gear_alt", 0xE010, fill=True),
    Icon("HOUSE", "house", 0xE011, fill=True),
    Icon("APPS", "circle_grid_3x3", 0xE012),
    Icon("STAR", "star", 0xE013, fill=True),
    Icon("PUSH_PIN", "pin", 0xE014, fill=True),
    Icon("LINK", "link", 0xE015),
    Icon("EYE", "eye", 0xE016, group="eye"),
    Icon("EYE_SLASH", "eye_slash", 0xE017, group="eye"),
    # Navigation
    Icon("CHEVRON_LEFT", "chevron_left", 0xE020),
    Icon("CHEVRON_RIGHT", "chevron_right", 0xE021),
    Icon("CHEVRON_UP", "chevron_up", 0xE022, group="disclosure"),
    Icon("CHEVRON_DOWN", "chevron_down", 0xE023, group="disclosure"),
    Icon("ARROW_LEFT", "arrow_left", 0xE024),
    Icon("ARROW_RIGHT", "arrow_right", 0xE025),
    Icon("ARROW_UP", "arrow_up", 0xE026),
    Icon("ARROW_DOWN", "arrow_down", 0xE027),
    Icon("ARROW_UP_RIGHT", "arrow_up_right", 0xE028),
    Icon("ARROW_CLOCKWISE", "arrow_clockwise", 0xE029),
    Icon("ARROW_COUNTER_CLOCKWISE", "arrow_counterclockwise", 0xE02A),
    Icon("ARROWS_CLOCKWISE", "arrow_2_circlepath", 0xE02B),
    Icon("UNDO", "arrow_uturn_left", 0xE02C),
    Icon("ARROW_UP_CIRCLE", "arrow_up_circle", 0xE02D, fill=True),
    Icon("DOWNLOAD", "arrow_down_to_line", 0xE02E),
    # Alerts and security
    Icon("WARNING", "exclamationmark_triangle", 0xE030, fill=True),
    Icon("INFO", "info_circle", 0xE031, fill=True),
    Icon("QUESTION", "question_circle", 0xE032),
    Icon("BELL", "bell", 0xE033, fill=True, group="bell"),
    Icon("BELL_SLASH", "bell_slash", 0xE034, group="bell"),
    Icon("LOCK", "lock", 0xE035, fill=True, group="lock"),
    Icon("LOCK_OPEN", "lock_open", 0xE036, group="lock"),
    Icon("SHIELD_CHECK", "checkmark_shield", 0xE037),
    Icon("HAND", "hand_raised", 0xE038),
    # Connectivity, devices and power
    Icon("WIFI_HIGH", "wifi", 0xE040, group="wifi"),
    Icon("WIFI_MEDIUM", "wifi@2", 0xE041, group="wifi"),
    Icon("WIFI_LOW", "wifi@1", 0xE042, group="wifi"),
    Icon("WIFI_NONE", "wifi@0", 0xE043, group="wifi"),
    Icon("WIFI_SLASH", "wifi_slash", 0xE044, group="wifi"),
    Icon("BLUETOOTH", "phosphor:bluetooth", 0xE045, group="bluetooth"),
    Icon("BLUETOOTH_SLASH", "phosphor:bluetooth-slash", 0xE046, group="bluetooth"),
    Icon("BLUETOOTH_CONNECTED", "phosphor:bluetooth-connected", 0xE047, group="bluetooth"),
    Icon("GLOBE", "globe", 0xE048),
    Icon("CLOUD_ARROW_DOWN", "cloud_download", 0xE049),
    Icon("DESKTOP", "desktopcomputer", 0xE04A),
    Icon("WATCH", "phosphor:watch", 0xE04B),
    Icon("BATTERY_FULL", "battery_100", 0xE04C, group="battery"),
    Icon("BATTERY_HIGH", "battery_100@80", 0xE04D, group="battery"),
    Icon("BATTERY_MEDIUM", "battery_100@50", 0xE04E, group="battery"),
    Icon("BATTERY_LOW", "battery_100@20", 0xE04F, group="battery"),
    Icon("BATTERY_EMPTY", "battery_0", 0xE050, group="battery"),
    Icon("BATTERY_CHARGING", "battery_100@charging", 0xE051, group="battery"),
    Icon("BATTERY_WARNING", "battery_100@warning", 0xE052, group="battery"),
    Icon("LIGHTNING", "bolt", 0xE053, fill=True),
    Icon("POWER", "power", 0xE054),
    # Display and sleep
    Icon("SUN", "sun_max", 0xE060, fill=True, group="sun"),
    Icon("SUN_DIM", "sun_min", 0xE061, group="sun"),
    Icon("MOON", "moon", 0xE062, fill=True),
    # Audio and media
    Icon("MIC", "mic", 0xE070, fill=True, group="mic"),
    Icon("MIC_OFF", "mic_slash", 0xE071, group="mic"),
    Icon("WAVEFORM", "waveform", 0xE072),
    Icon("SPEAKER_NONE", "speaker", 0xE073, group="speaker"),
    Icon("SPEAKER_LOW", "speaker_1", 0xE074, group="speaker"),
    Icon("SPEAKER_HIGH", "speaker_3", 0xE075, fill=True, group="speaker"),
    Icon("SPEAKER_SLASH", "speaker_slash", 0xE076, group="speaker"),
    Icon("PLAY", "play", 0xE077, fill=True, group="transport"),
    Icon("PAUSE", "pause", 0xE078, fill=True, group="transport"),
    Icon("STOP", "stop", 0xE079, fill=True, group="transport"),
    Icon("SKIP_FORWARD", "forward_end", 0xE07A, fill=True),
    Icon("SKIP_BACK", "backward_end", 0xE07B, fill=True),
    Icon("RECORD", "smallcircle_circle", 0xE07C, fill=True),
    # Time
    Icon("CLOCK", "clock", 0xE090),
    Icon("TIMER", "timer", 0xE091),
    Icon("ALARM", "alarm", 0xE092),
    Icon("HOURGLASS", "hourglass", 0xE093),
    Icon("CALENDAR", "calendar", 0xE094),
    # Agents and code
    Icon("TERMINAL", "phosphor:terminal-window", 0xE0A0, fill=True),
    Icon("CODE", "chevron_left_slash_chevron_right", 0xE0A1),
    Icon("SPARKLE", "sparkles", 0xE0A2, fill=True),
    Icon("ROBOT", "phosphor:robot", 0xE0A3),
    Icon("CHAT", "chat_bubble", 0xE0A4, fill=True),
    Icon("PAPER_PLANE", "paperplane", 0xE0A5),
    Icon("GIT_BRANCH", "arrow_branch", 0xE0A6),
    Icon("GIT_PULL_REQUEST", "phosphor:git-pull-request", 0xE0A7),
    Icon("FILE_TEXT", "doc_text", 0xE0A8),
    Icon("FOLDER", "folder", 0xE0A9),
    Icon("USER_CIRCLE", "person_circle", 0xE0AA, fill=True),
    Icon("GAME_CONTROLLER", "gamecontroller", 0xE0AB),
    # Shell Design 1.0 additions. Append ABI values; never alter existing artwork.
    # Framework7 has no plain prompt, torch or accessibility figure.
    Icon("PROMPT", "phosphor:terminal", 0xE0AC),
    Icon("UPLOAD", "arrow_up@upload", 0xE0AD),
    Icon("FLASHLIGHT", "phosphor:flashlight", 0xE0AE),
    Icon("ACCESSIBILITY", "phosphor:person-arms-spread", 0xE0AF),
    Icon("SEEK_BACK", "backward_fill", 0xE0B0),
    Icon("SEEK_FORWARD", "forward_fill", 0xE0B1),
    Icon("MUSIC_NOTE", "music_note", 0xE0B2),
    # Weather: one glyph for each Open-Meteo condition, so the Face never falls back to a word.
    Icon("CLOUD", "cloud", 0xE0B3, fill=True, group="weather"),
    Icon("CLOUD_SUN", "cloud_sun", 0xE0B4, fill=True, group="weather"),
    Icon("CLOUD_MOON", "cloud_moon", 0xE0B5, fill=True, group="weather"),
    Icon("CLOUD_FOG", "cloud_fog", 0xE0B6, fill=True, group="weather"),
    Icon("CLOUD_DRIZZLE", "cloud_drizzle", 0xE0B7, fill=True, group="weather"),
    Icon("CLOUD_RAIN", "cloud_rain", 0xE0B8, fill=True, group="weather"),
    Icon("CLOUD_SNOW", "cloud_snow", 0xE0B9, fill=True, group="weather"),
    Icon("CLOUD_BOLT", "cloud_bolt", 0xE0BA, fill=True, group="weather"),
    # Home accessory identities, Time and game controls the polish lanes had to approximate (assets v6).
    Icon("LIGHTBULB", "lightbulb", 0xE0BB, fill=True, group="home"),
    Icon("FAN", "phosphor:fan", 0xE0BC, group="home"),
    Icon("THERMOMETER", "thermometer", 0xE0BD, group="home"),
    Icon("DROP", "drop", 0xE0BE, fill=True, group="home"),
    Icon("FLAME", "flame", 0xE0BF, fill=True, group="home"),
    Icon("SNOWFLAKE", "snow", 0xE0C0, group="home"),
    Icon("BLINDS", "rectangle_split_3x1", 0xE0C1, fill=True, group="home"),
    Icon("DOOR", "phosphor:door", 0xE0C2, group="home"),
    Icon("GARAGE", "phosphor:garage", 0xE0C3, group="home"),
    Icon("PLUG", "phosphor:plug", 0xE0C4, group="home"),
    Icon("TOGGLE", "phosphor:toggle-left", 0xE0C5, group="home"),
    Icon("WALK", "phosphor:person-simple-walk", 0xE0C6, group="home"),
    Icon("SENSOR", "phosphor:broadcast", 0xE0C7, group="home"),
    Icon("FLAG", "flag", 0xE0C8, fill=True),
    Icon("ZZZ", "moon_zzz", 0xE0C9, fill=True),
    Icon("REPEAT", "repeat", 0xE0CA),
    Icon("STOPWATCH", "stopwatch", 0xE0CB, fill=True),
    Icon("HAND_TAP", "phosphor:hand-tap", 0xE0CC),
    Icon("LIST_BULLET", "list_bullet", 0xE0CD),
    Icon("RECENTS", "phosphor:clock-counter-clockwise", 0xE0CE),
]
# fmt: on


def icon_codepoints() -> list[tuple[str, int, str, bool]]:
    """(macro, codepoint, asset name, is_fill) for every glyph, in codepoint order."""
    out = []
    for icon in ICONS:
        out.append((icon.macro, icon.codepoint, icon.asset, False))
        if icon.fill:
            out.append(
                (
                    icon.macro + "_FILL",
                    icon.codepoint + FILL_OFFSET,
                    icon.asset,
                    True,
                )
            )
    out.sort(key=lambda t: t[1])
    return out


def icon_groups() -> dict[str, str]:
    """Glyph macro -> metrics group. A filled icon shares its outline's metrics, so
    swapping BELL for BELL_FILL (or WIFI_LOW for WIFI_HIGH) never moves anything."""
    out = {}
    for icon in ICONS:
        group = icon.group or icon.macro
        out[icon.macro] = group
        if icon.fill:
            out[icon.macro + "_FILL"] = group
    return out


# docs/hig.md section 4 lists these px values; the tables above must reproduce them.
HIG_DEFAULT_PX = [65, 62, 49, 33, 28, 28, 26, 25, 23, 21]
HIG_DEFAULT_LEADING_PX = [69, 66, 53, 37, 32, 32, 30, 29, 27, 25]
HIG_BODY_PX = {"DEFAULT": 28, "LARGER": 31, "AX3": 38}


def check() -> None:
    assert [s.px for s in TEXT_STYLES] == HIG_DEFAULT_PX, [s.px for s in TEXT_STYLES]
    assert [s.leading_px for s in TEXT_STYLES] == HIG_DEFAULT_LEADING_PX
    for size in CONTENT_SIZES:
        for bold in (False, True):
            rows = text_styles(size, bold)
            body = next(s for s in rows if s.enum == "BODY")
            assert body.px == HIG_BODY_PX[size.enum], (size.enum, body.px)
            for s in rows:
                assert s.weight in WEIGHT_NAMES and s.emphasized_weight in WEIGHT_NAMES, s
                assert s.leading_px > s.px, s
            # Larger styles are never smaller.
            assert all(a.px >= b.px for a, b in zip(rows, rows[1:])), size.enum
    # One pixel size per point size, so fonts can be named by px.
    sizes = {s.px: s.pt for c in CONTENT_SIZES for s in text_styles(c, False)}
    for c in CONTENT_SIZES:
        for s in text_styles(c, False):
            assert sizes[s.px] == s.pt, f"{s.px} px is both {sizes[s.px]} and {s.pt} pt"
    for c in CLOCK_FACES:
        assert c.weight in WEIGHT_NAMES and BOLD_TEXT[c.weight] in WEIGHT_NAMES, c
    seen: dict[int, str] = {}
    for macro, cp, _, _ in icon_codepoints():
        assert 0xE000 <= cp <= 0xF8FF, macro
        assert cp not in seen, f"{macro} reuses U+{cp:04X} of {seen[cp]}"
        seen[cp] = macro
    macros = [m for m, _, _, _ in icon_codepoints()]
    assert len(macros) == len(set(macros)), "duplicate icon macro"
