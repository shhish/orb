"""Faces, Lock/Always On and system-state polish regressions on real renders.

Each case runs the production presenters through orb_parity (one fresh process
per scenario/profile) and checks the semantic trace and RGB pixels. The review
items are in docs/agent-briefs/wave2/polish-faces-results.md.
"""

import argparse
import concurrent.futures
import importlib.util
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("orb_parity", Path(__file__).parents[1] / "parity.py")
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)

# Framework7 codepoints from orb_icons.h.
HOUSE, LOCK, PROMPT, CODE = "", "", "", ""
CLOCK, CLOUD, PLUG, BATTERY_LOW = "", "", "", ""
BATTERY_HIGH, HOURGLASS, WARNING, DESKTOP = "", "", "", ""
ARROW_CLOCKWISE, DOTS_THREE = "", ""
BINARY: Path | None
ROOT: Path
CACHE: dict = {}

FACES = [
    f"face-{face}{state}"
    for face in ("orb", "meridian", "activity")
    for state in ("", "-offline", "-unknown", "-weather", "-weather-stale", "-colour", "-gallery")
] + [
    "face-lock",
    "face-always-on",
    "face-meridian-lock",
    "face-meridian-always-on",
    "face-activity-lock",
    "face-activity-always-on",
    "face-orb-weather-unavailable",
    "face-orb-colour-save-error",
    "face-orb-provider-next",
]
SYSTEM = [f"charging.{s}" for s in ("default", "full", "low", "unknown", "unplugged")] + [
    f"idle.{s}"
    for s in ("default", "empty", "error", "loading", "more", "offline", "allocation-error")
]
CASES = [(scenario, profile) for profile in ("default", "ax3") for scenario in FACES + SYSTEM]


def run(case):
    scenario, profile = case
    out = ROOT / scenario / profile
    if BINARY:
        out.mkdir(parents=True)
        subprocess.run([str(BINARY), scenario, profile, str(out)], check=True, capture_output=True)
    width, _, rgb = parity.read_png(out / "settled.png")
    return case, (parity.read_json(out / "settled.json"), (width, rgb))


def trace(scenario, profile="default"):
    return CACHE[(scenario, profile)][0]


def texts(scenario, profile="default"):
    return [node["text"] for node in trace(scenario, profile)["controls"] if node["text"]]


def has(scenario, needle, profile="default"):
    return any(needle in text for text in texts(scenario, profile))


def ink(scenario, points, profile="default"):
    """Brightest channel over a set of (x, y) pixels."""
    width, rgb = CACHE[(scenario, profile)][1]
    return max(max(rgb[(y * width + x) * 3 : (y * width + x) * 3 + 3]) for x, y in points)


def disc(cx, cy, radius):
    return [
        (x, y)
        for y in range(cy - radius, cy + radius + 1)
        for x in range(cx - radius, cx + radius + 1)
        if (x - cx) ** 2 + (y - cy) ** 2 <= radius * radius and 0 <= x < 466 and 0 <= y < 466
    ]


class Lock(unittest.TestCase):
    """#9 / #42: Lock and Always On are the chosen face, dimmed, with its complications."""

    def test_chosen_face(self):
        for profile in ("default", "ax3"):
            for state in ("lock", "always-on"):
                activity, meridian = f"face-activity-{state}", f"face-meridian-{state}"
                orb = "face-lock" if state == "lock" else "face-always-on"
                # Activity keeps its rings' legend; Meridian has no digital clock.
                self.assertTrue(
                    has(activity, f"{PROMPT}\xa0", profile) or has(activity, PROMPT, profile)
                )
                self.assertTrue(
                    any(n["type"] == "arc" for n in trace(activity, profile)["controls"])
                )
                self.assertFalse(has(meridian, "10:09", profile))
                self.assertTrue(
                    any(n["type"] == "line" for n in trace(meridian, profile)["controls"])
                )
                self.assertTrue(has(orb, "10:09", profile))
                for scenario in (activity, meridian, orb):
                    self.assertIn(LOCK, texts(scenario, profile))

    def test_same_complications_low_ink(self):
        for profile in ("default", "ax3"):
            # The Orb face's battery glyph and Home disc survive the lock.
            self.assertIn(BATTERY_HIGH, texts("face-lock", profile))
            self.assertIn(BATTERY_HIGH, texts("face-orb", profile))
            # #42: the household count is withheld; the glyph names the slot.
            self.assertIn(HOUSE, texts("face-lock", profile))
            self.assertFalse(any(t.endswith(" on") for t in texts("face-lock", profile)))
            self.assertTrue(any(t.endswith(" on") for t in texts("face-orb", profile)))
            # Low ink: the locked clock is never drawn at full white.
            self.assertGreater(ink("face-orb", disc(233, 183, 30), profile), 240)
            self.assertLess(ink("face-lock", disc(233, 183, 30), profile), 200)


class Faces(unittest.TestCase):
    def test_weather_rule(self):
        """#14: a stale value is shown dimmed beside the clock mark; '—' only without one."""
        for profile in ("default", "ax3"):
            self.assertIn("72°", texts("face-orb-weather-stale", profile))
            self.assertIn("Last known", texts("face-orb-weather-stale", profile))
            self.assertIn(CLOCK, texts("face-orb-weather-stale", profile))
            self.assertFalse(has("face-orb-weather-stale", "Weather stale", profile))
            self.assertIn(CLOCK, texts("face-meridian-weather-stale", profile))
            self.assertIn("72°", texts("face-meridian-weather-stale", profile))
            self.assertTrue(has("face-activity-weather-stale", f"{CLOCK} 72°", profile))
            self.assertFalse(has("face-orb-weather-unavailable", "Partly cloudy", profile))
            self.assertIn("Weather unavailable", texts("face-orb-weather-unavailable", profile))
            self.assertIn(CLOUD, texts("face-orb", profile))

    def test_offline_cue(self):
        """#15: every face says "Mac offline", in its own text lane."""
        for profile in ("default", "ax3"):
            for face in ("orb", "meridian", "activity"):
                self.assertTrue(has(f"face-{face}-offline", "Mac offline", profile), face)
                self.assertFalse(has(f"face-{face}", "Mac offline", profile), face)

    def test_activity_labels(self):
        """#16: each ring is named by its provider glyph; unknown is not '0 working'."""
        default = texts("face-activity")
        self.assertIn(f"{PROMPT}\xa062", default)
        self.assertIn(f"{CODE}\xa038", default)
        self.assertIn(PROMPT, texts("face-activity", "ax3"))
        self.assertIn(CODE, texts("face-activity", "ax3"))
        for profile in ("default", "ax3"):
            self.assertTrue(has("face-activity-unknown", "No sessions", profile))
            self.assertFalse(has("face-activity-unknown", "0 working", profile))
            # Empty slots keep their own glyph beside the dash.
            self.assertIn(PROMPT, texts("face-meridian-unknown", profile))

    def test_dates(self):
        """#11 / #37: three letters on the subdial faces at AX3; one weekday on Orb."""
        for profile in ("default", "ax3"):
            self.assertIn("WED", texts("face-meridian", profile))
            self.assertIn("WED", texts("face-activity", profile))
            self.assertIn("WEDNESDAY", texts("face-orb", profile))
            self.assertIn("WEDNESDAY", texts("face-lock", profile))

    def test_condition_and_units(self):
        """#13 / #27: a condition is whole or absent; timers use glossary units."""
        for profile in ("default", "ax3"):
            values = texts("face-orb-weather", profile)
            self.assertFalse(any(t.endswith("…") or t.endswith("...") for t in values), values)
            self.assertIn("min", texts("face-orb-provider-next", profile))
            self.assertFalse(has("face-orb-provider-next", "13m", profile))

    def test_colour_preview_gap(self):
        """#17: >= 8px of black between the preview and the selected swatch's focus ring."""
        for profile in ("default", "ax3"):
            for face in ("orb", "meridian", "activity"):
                annulus = [p for p in disc(356, 110, 55) if math.dist(p, (356, 110)) >= 48]
                self.assertLess(ink(f"face-{face}-colour", annulus, profile), 40, face)

    def test_save_error(self):
        """#18: say why, keep the colour name and offer the retry."""
        for profile in ("default", "ax3"):
            values = texts("face-orb-colour-save-error", profile)
            self.assertIn("Couldn’t save", values)
            self.assertIn("Orange", values)
            self.assertTrue(has("face-orb-colour-save-error", "Try again", profile))
            self.assertNotIn("Not saved", values)
            self.assertIn("COLOR", texts("face-orb-colour", profile))

    def test_gallery(self):
        """#38: dots on the rim below Edit; no peek past either end."""
        for profile in ("default", "ax3"):
            # Orb is page 0: the active (white) dot is the left one, 14px pitch.
            self.assertGreater(ink("face-orb-gallery", disc(219, 445, 3), profile), 150)
            self.assertLess(ink("face-orb-gallery", disc(233, 351, 4), profile), 40)
            edge = [(x, y) for x in range(4, 24) for y in range(170, 230)]
            right = [(466 - x, y) for x, y in edge]
            self.assertLess(ink("face-orb-gallery", edge, profile), 40)  # nothing before Orb
            self.assertGreater(ink("face-orb-gallery", right, profile), 60)  # Meridian peeks
            self.assertLess(ink("face-activity-gallery", right, profile), 40)  # nothing after


class System(unittest.TestCase):
    def test_charging(self):
        """#36: Low says Low; the subtitle never repeats the title; full shows power."""
        for profile in ("default", "ax3"):
            self.assertIn("Low battery", texts("charging.low", profile))
            self.assertIn(BATTERY_LOW, texts("charging.low", profile))
            self.assertIn("Fully charged", texts("charging.full", profile))
            self.assertIn(PLUG, texts("charging.full", profile))
            self.assertIn("On battery", texts("charging.unplugged", profile))
            for state in ("default", "full", "low", "unplugged"):
                self.assertEqual(texts(f"charging.{state}", profile).count("Battery"), 0, state)

    def test_idle(self):
        """#20: distinct glyphs, one error pattern, offline is not also loading."""
        glyphs = {}
        for profile in ("default", "ax3"):
            offline = texts("idle.offline", profile)
            self.assertIn("Mac offline", offline)
            self.assertFalse(any(t.startswith("Loading") for t in offline))
            self.assertIn("Couldn’t load sessions", texts("idle.error", profile))
            self.assertIn("Try again", texts("idle.error", profile))
            self.assertIn("Couldn’t load sessions", texts("idle.allocation-error", profile))
            for state, glyph in (
                ("loading", HOURGLASS),
                ("error", ARROW_CLOCKWISE),
                ("more", DOTS_THREE),
                ("offline", DESKTOP),
                ("allocation-error", WARNING),
            ):
                self.assertIn(glyph, texts(f"idle.{state}", profile), state)
                glyphs[state] = glyph
        self.assertEqual(len(set(glyphs.values())), 5)


def main() -> int:
    global BINARY, ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    # An existing parity capture (e.g. a pre-change baseline) instead of rendering.
    parser.add_argument("--renders", type=Path)
    args, rest = parser.parse_known_args()
    if not args.binary and not args.renders:
        parser.error("--binary or --renders is required")
    BINARY = args.binary
    with tempfile.TemporaryDirectory(prefix="orb-polish-faces-") as temporary:
        ROOT = Path(temporary) if BINARY else args.renders
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            for case, frames in pool.map(run, CASES):
                CACHE[case] = frames
        program = unittest.main(argv=[sys.argv[0], *rest], exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
