"""Polish wave 1 regressions, asserted on the real presenter renders.

Each case runs the production shell through orb_parity (one fresh process per
scenario/profile) and checks its semantic trace and RGB pixels. The review
items are in docs/agent-briefs/wave2/polish1-results.md.
"""

import argparse
import concurrent.futures
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("orb_parity", Path(__file__).parents[1] / "parity.py")
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)

DISABLED = 1 << 9  # LV_STATE_DISABLED
CHECK, HAND, MIC, SPEAKER_SLASH = "", "", "", ""
BINARY: Path
ROOT: Path
CACHE: dict = {}

CASES = [
    (scenario, profile)
    for profile in ("default", "ax3")
    for scenario in (
        "approval.default",
        "approval.offline",
        "question.default",
        "question.offline",
        "session.default",
        "session.read-only",
        "agents.default",
        "agents.read-only",
        "stack.default",
        "stack.offline",
        "settings.sources",
        "settings.sources.offline",
        "settings.temperature.offline",
        "voice.review",
        "voice.review.empty-transcript",
        "voice.review.ended",
        "voice.review.read-only",
        "voice.review.offline",
        "voice.time.saved",
        "voice.time.failed",
        "voice.transcribing",
        "dot.loading",
        "settings.sound.saving",
        "approval.expired",
        "approval.refreshing",
        "notifications.default",
        "notifications.detail",
        "system.connection",
        "system.connection.connecting",
        "system.connection.searching",
        "system.connection.offline",
        "system.connection.unauthorized",
        "system.connection.link-connecting",
        "system.connection.failed",
        "system.connection.unprovisioned",
        "system.connection.no-name",
        "system.setup",
        "system.setup.error",
        "system.pairing",
        "system.pairing.long-name",
        "updates.mandatory.offered",
        "updates.mandatory.failed",
        "updates.ordinary.error",
        "updates.mandatory.downloading.complete",
        "updates.mandatory.ready",
        "volume-hud.silent",
    )
] + [
    (card, profile)
    for card in ("stack.card.approval", "stack.card.message", "stack.card.question")
    for profile in ("default", "reduce-motion")
]


def run(case):
    scenario, profile = case
    out = ROOT / scenario / profile
    out.mkdir(parents=True)
    subprocess.run([str(BINARY), scenario, profile, str(out)], check=True, capture_output=True)
    frames = {}
    for frame in ("settled", "late-1600"):
        width, height, rgb = parity.read_png(out / f"{frame}.png")
        frames[frame] = (parity.read_json(out / f"{frame}.json"), (width, rgb))
    return case, frames


def frame(scenario, profile="default", which="settled"):
    return CACHE[(scenario, profile)][which]


def trace(scenario, profile="default", which="settled"):
    return frame(scenario, profile, which)[0]


def texts(scenario, profile="default"):
    return [node["text"] for node in trace(scenario, profile)["controls"] if node["text"]]


def has(scenario, needle, profile="default"):
    return any(needle in text for text in texts(scenario, profile))


def label(scenario, text, profile="default", which="settled"):
    """Exact label, or one LVGL ellipsised from text ("Which checks sho...")."""
    for node in trace(scenario, profile, which)["controls"]:
        shown = node["text"]
        stem = shown[:-3] if shown.endswith("...") else shown[:-1] if shown.endswith("…") else None
        if node["type"] == "label" and (shown == text or (stem and text.startswith(stem))):
            return node
    raise AssertionError(f"{scenario}/{profile}: no label {text!r}")


def owner(scenario, bounds, profile="default"):
    """Smallest non-label node containing a rectangle's centre."""
    cx, cy = (bounds[0] + bounds[2]) // 2, (bounds[1] + bounds[3]) // 2
    best = None
    for node in trace(scenario, profile)["controls"]:
        x1, y1, x2, y2 = node["bounds"]
        if node["type"] == "label" or not (x1 <= cx <= x2 and y1 <= cy <= y2):
            continue
        area = (x2 - x1) * (y2 - y1)
        if area < 466 * 466 and (best is None or area < best[0]):
            best = (area, node)
    return best and best[1]


def peak(scenario, bounds, profile="default"):
    width, rgb = frame(scenario, profile)[1]
    x1, y1, x2, y2 = bounds
    return max(
        max(rgb[(y * width + x) * 3 : (y * width + x) * 3 + 3])
        for y in range(y1, y2 + 1)
        for x in range(x1, x2 + 1)
    )


def bright(scenario, bounds, profile="default"):
    """Pixels whose brightest channel is near full ink: dimmed ink has none."""
    width, rgb = frame(scenario, profile)[1]
    x1, y1, x2, y2 = bounds
    return sum(
        max(rgb[(y * width + x) * 3 : (y * width + x) * 3 + 3]) > 200
        for y in range(y1, y2 + 1)
        for x in range(x1, x2 + 1)
    )


def pixel(scenario, x, y, profile="default"):
    width, rgb = frame(scenario, profile)[1]
    return tuple(rgb[(y * width + x) * 3 : (y * width + x) * 3 + 3])


class Offline(unittest.TestCase):
    """Review #12: unavailable controls look unavailable, say why, and are disabled."""

    def assert_dimmed(self, scenario, normal, text, profile):
        node = label(scenario, text, profile)
        control = owner(scenario, node["bounds"], profile)
        self.assertTrue(control and control["state"] & DISABLED, f"{scenario} {text} enabled")
        self.assertGreater(bright(normal, label(normal, text, profile)["bounds"], profile), 20)
        self.assertEqual(
            bright(scenario, node["bounds"], profile), 0, f"{scenario} {text} not dimmed"
        )

    def test_cards(self):
        for profile in ("default", "ax3"):
            for name, normal, controls in (
                ("approval.offline", "approval.default", ("Deny", "Allow")),
                ("question.offline", "question.default", ("Deny",)),
            ):
                self.assertTrue(has(name, "Mac offline", profile))
                self.assertFalse(has(normal, "Mac offline", profile))
                for text in controls:
                    self.assert_dimmed(
                        name,
                        normal,
                        "Dismiss" if name.startswith("question.") and text == "Deny" else text,
                        profile,
                    )
            self.assertFalse(has("approval.offline", "Hold BOOT to reply", profile))

    def test_sessions(self):
        mic = [193, 340, 273, 420]
        for profile in ("default", "ax3"):
            self.assertTrue(has("session.read-only", "Read-only", profile))
            self.assertGreater(bright("session.default", mic, profile), 20)
            self.assertLess(bright("session.read-only", mic, profile), 5)
            self.assertTrue(has("agents.read-only", "Read-only", profile))
            self.assertTrue(has("agents.default", "Hold BOOT to talk", profile))
            self.assertFalse(has("agents.read-only", "Hold BOOT to talk", profile))

    def test_stack_and_settings(self):
        for profile in ("default", "ax3"):
            self.assertTrue(has("stack.offline", "Mac offline", profile))
            self.assertFalse(has("stack.default", "Mac offline", profile))
            sources = texts("settings.sources.offline", profile)
            # The three providers fit at rest. AX3 places Recent activity's
            # subtitle below the viewport; its retained Unknown projection is
            # checked separately by settings_sources.
            self.assertGreaterEqual(sources.count("Unknown"), 3)
            self.assertNotIn("Off", sources)
            self.assertNotIn("On", sources)
            self.assertTrue(has("settings.sources", "Claude", profile))
            self.assertNotIn("Unknown", texts("settings.sources", profile))
            self.assertTrue(has("settings.sources.offline", "Mac offline", profile))
            self.assertTrue(has("settings.temperature.offline", "Mac offline", profile))
            self.assertNotIn(CHECK, texts("settings.temperature.offline", profile))


class VoiceReview(unittest.TestCase):
    """Review #13: Send is disabled, untinted and explained when it cannot send."""

    def test_send(self):
        hints = {
            "voice.review.empty-transcript": "Hold BOOT to talk",
            "voice.review.ended": "Can’t send to an ended session",
            "voice.review.read-only": "This session is read-only",
            "voice.review.offline": "Reconnect your Mac to send",
        }
        for profile in ("default", "ax3"):
            send = label("voice.review", "Send", profile)
            self.assertFalse(owner("voice.review", send["bounds"], profile)["state"] & DISABLED)
            for scenario, hint in hints.items():
                node = label(scenario, "Send", profile)
                self.assertTrue(
                    owner(scenario, node["bounds"], profile)["state"] & DISABLED, scenario
                )
                self.assertEqual(bright(scenario, node["bounds"], profile), 0, scenario)
                self.assertTrue(has(scenario, hint, profile), scenario)
                self.assertFalse(has(scenario, "Pick a session", profile))


class Copy(unittest.TestCase):
    """Review #14: each state says what is true for that state."""

    def test_copy(self):
        for profile in ("default", "ax3"):
            self.assertTrue(has("voice.time.saved", "Find it in Time", profile))
            self.assertFalse(has("voice.time.saved", "Check the session", profile))
            self.assertFalse(has("voice.time.failed", "Session inbox", profile))
            self.assertTrue(has("voice.transcribing", "Transcribing", profile))
            self.assertFalse(has("voice.transcribing", "or hold BOOT", profile))
            self.assertFalse(has("dot.loading", "shortly", profile))
            self.assertTrue(has("dot.loading", "Waiting for your Mac to sync", profile))
            # The Sound root reports its own save; no Quiet Hours stepper.
            self.assertTrue(has("settings.sound.saving", "Saving", profile))
            self.assertTrue(has("settings.sound.saving", "Volume", profile))
            self.assertFalse(has("settings.sound.saving", "Starts", profile))
            self.assertTrue(has("approval.expired", "Expired", profile))
            self.assertFalse(has("approval.expired", "Refreshing from Mac", profile))
            self.assertTrue(has("approval.refreshing", "Loading…", profile))


class StackAndNotifications(unittest.TestCase):
    def test_stack_cards_settle_like_reduce_motion(self):
        """Review #10: the arrival spring no longer strands a 27 px offset."""
        titles = {
            "stack.card.approval": "Run this command?",
            "stack.card.message": "Build complete",
            "stack.card.question": "Which checks should run?",
        }
        for scenario, title in titles.items():
            reduced = label(scenario, title, "reduce-motion")["bounds"]
            for which in ("settled", "late-1600"):
                self.assertEqual(
                    label(scenario, title, "default", which)["bounds"], reduced, scenario
                )

    def test_newest_notification_is_focused(self):
        """Review #7: the newest row rests in the focus lane; Clear All follows it."""
        for profile in ("default", "ax3"):
            row = [
                n
                for n in trace("notifications.default", profile)["controls"]
                if n["type"] == "label" and n["text"].startswith("Build com")
            ][0]["bounds"]
            clear = label("notifications.default", "Clear all", profile)["bounds"]
            self.assertGreaterEqual(row[1], 126)
            self.assertLess((row[1] + row[3]) // 2, 233)
            self.assertGreater(clear[1], row[3])
            age = label("notifications.default", "now", profile)["bounds"]
            self.assertLess(age[1], clear[1])

    def test_no_unlabelled_hand(self):
        """Review #8: the transient hand glyph is gone everywhere."""
        for scenario in ("notifications.detail", "stack.card.approval", "stack.card.message"):
            self.assertNotIn(HAND, texts(scenario))


class System(unittest.TestCase):
    STATES = (
        "connecting",
        "searching",
        "offline",
        "unauthorized",
        "link-connecting",
        "failed",
        "unprovisioned",
    )

    def test_connection_states(self):
        """Review #5/#6: distinct status and fix per state; none once connected."""
        for profile in ("default", "ax3"):
            seen = set()
            for state in self.STATES:
                scenario = f"system.connection.{state}"
                rows = tuple(texts(scenario, profile))
                self.assertNotIn(rows, seen, scenario)
                seen.add(rows)
            for state in ("",) + tuple("." + s for s in self.STATES) + (".no-name",):
                values = texts("system.connection" + state, profile)
                self.assertFalse(any("unavailable" in t or t == "Retry" for t in values), state)
            connected = texts("system.connection", profile)
            self.assertFalse(any("retr" in t.lower() or "Automatic" in t for t in connected))

    def reader(self, scenario, profile):
        nodes = trace(scenario, profile)["controls"]
        labels = [n["bounds"] for n in nodes if n["type"] == "label" and n["role"] == "scroll"]

        def inside(b, r):
            return b[0] >= r[0] and b[2] <= r[2] and r[1] <= b[1] < r[3]

        # The reader is the bounded content viewport holding the most copy;
        # action capsules (role "primary"/single caption) are not readers.
        readers = [
            (sum(inside(b, n["bounds"]) for b in labels), n["bounds"])
            for n in nodes
            if n["type"] == "object"
            and n["role"] == "control"
            and n["bounds"][2] - n["bounds"][0] < 400
        ]
        count, bounds = max(readers)
        self.assertGreater(count, 0, scenario)
        return bounds

    def test_readers_never_hard_cut(self):
        """Review #1/#2: bounded copy ends on a whole line and fades; no glyph
        is sliced by the viewport's bottom edge."""
        for scenario, profile in (
            ("system.setup", "default"),
            ("system.setup.error", "default"),
            ("system.pairing", "ax3"),
            ("system.pairing.long-name", "default"),
            ("updates.mandatory.failed", "default"),
            ("updates.mandatory.failed", "ax3"),
            ("updates.ordinary.error", "ax3"),
            ("updates.mandatory.offered", "ax3"),
        ):
            x1, _, x2, y2 = self.reader(scenario, profile)
            self.assertLess(peak(scenario, [x1, y2, x2, y2], profile), 64, scenario)

    def test_pairing_names(self):
        """Review #7/#10: orb-hub never breaks at its hyphen (U+2011). A long Mac
        name stays whole in the scrolling, fading reader (HIG §4), never hard-cut."""
        for profile in ("default", "ax3"):
            self.assertTrue(has("system.pairing", "orb‑hub", profile))
            self.assertFalse(has("system.pairing", "orb-hub", profile))
        # orb_link ends an over-long name at a word (see its host test_proto.c).
        self.assertIn(
            "Found A Mac computer with a maximum length name and many complete\u2026",
            texts("system.pairing.long-name"),
        )

    def test_update_states(self):
        """Review #22: one consistent line per stage, the version kept, red failure."""
        for profile in ("default", "ax3"):
            complete = texts("updates.mandatory.downloading.complete", profile)
            self.assertIn("Downloaded 0.5.3", complete)
            self.assertFalse(any(t.startswith("Downloading") for t in complete))
            ready = texts("updates.mandatory.ready", profile)
            self.assertIn("Installing 0.5.3", ready)
            self.assertFalse(any("Keep using" in t or "Ready to restart" in t for t in ready))
            failed = trace("updates.mandatory.failed", profile)["controls"]
            self.assertFalse(
                any(n["type"] == "arc" and n["bounds"] == [10, 10, 455, 455] for n in failed)
            )
            r, g, b = pixel(
                "updates.mandatory.failed", 200, 124, profile
            )  # disc fill, beside the glyph
            self.assertTrue(r > 200 and g < 120 and b < 120, (r, g, b))


class VolumeHud(unittest.TestCase):
    def test_silent(self):
        """home-control #10: Silent shows speaker.slash, "Silent" and grey ink over the kept level."""
        for profile in ("default", "ax3"):
            values = texts("volume-hud.silent", profile)
            self.assertIn("Silent", values)
            self.assertIn(SPEAKER_SLASH, values)
            # The kept level stays visible, greyed, so unmuting holds no surprise.
            node = label("volume-hud.silent", "Silent", profile)
            width, rgb = frame("volume-hud.silent", profile)[1]
            x1, y1, x2, y2 = node["bounds"]
            brightest = max(
                (
                    rgb[(y * width + x) * 3 : (y * width + x) * 3 + 3]
                    for y in range(y1, y2 + 1)
                    for x in range(x1, x2 + 1)
                ),
                key=sum,
            )
            self.assertLess(max(brightest) - min(brightest), 24, brightest)


def main() -> int:
    global BINARY, ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    args, rest = parser.parse_known_args()
    BINARY = args.binary
    with tempfile.TemporaryDirectory(prefix="orb-polish1-") as temporary:
        ROOT = Path(temporary)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            for case, frames in pool.map(run, CASES):
                CACHE[case] = frames
        program = unittest.main(argv=[sys.argv[0], *rest], exit=False)
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
