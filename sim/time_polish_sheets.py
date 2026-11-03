"""Compose Time review sheets after the full simulator test run.

Run from the repository root:
    python3 sim/time_polish_sheets.py

Inputs: sim/build/time-atlas-{default,ax3,ax3-bold-rt,ax3-bold-rt-rm} and frames.
Outputs: sim/build/time-design2-sheets, including reference-map.tsv and contacts.
Every comparison uses png_tools.concat and verifies both decoded native halves.
Shell-owned Save captures are kept under pending-S2, outside the Time gate.
"""

import sys
from functools import cache
from pathlib import Path

sys.path.insert(0, "sim")
import png_tools

base = Path("sim/build/time-design2-sheets")
primary = [
    "timer-idle",
    "timer-running",
    "stopwatch-idle",
    "stopwatch-paused",
    "alarm-design",
    "alarm-ringing",
    "reminder-due",
    "reminders-list",
    "presets-last-used",
    "options-repeat-weekdays",
    "custom-days-selected",
]
stress = [
    "timer-finished",
    "timer-longduration",
    "stopwatch-laps-extremes",
    "stopwatch-laps-long",
    "alarm-waiting-for-time",
    "alarm-24h-midnight",
    "reminders-empty",
    "reminders-detail-long",
    "reminders-list-bottom",
]
storage_errors = [
    "alarm-save-error",
    "reminders-empty-save-error",
    "reminders-save-error",
]
required = {
    "AlarmRinging",
    "timer-idle",
    "timer-running",
    "timer-paused",
    "timer-finished",
    "timer-longduration",
    "presets-list",
    "stopwatch-idle",
    "stopwatch-running",
    "stopwatch-paused",
    "stopwatch-laps",
    "stopwatch-laps-long",
    "alarm-idle",
    "alarm-design",
    "alarm-ringing",
    "alarm-ringing-custom",
    "alarm-ringing-wide",
    "alarm-waiting-for-time",
    "options-repeat-weekdays",
    "options-snooze-seven",
    "custom-days-list",
    "custom-days-selected",
    "reminders-empty",
    "reminders-list",
    "reminders-detail-long",
    "reminders-detail-long-scrolled",
    "reminder-due",
}


def reference(name):
    # IMPLEMENT keeps Design 1.0 where 2.0 is silent. The inherited place
    # frames show composition; the shared kit overrides their old components.
    inherited = "inherited 1.0; kit2 overrides"
    if name.startswith("stopwatch-"):
        return Path("docs/design/1.0/png/Stopwatch.png"), inherited
    if name == "AlarmRinging" or name.startswith("alarm-ringing"):
        return Path("docs/design/1.0/png/AlarmRinging.png"), inherited
    if name.startswith("alarm-"):
        return Path("docs/design/1.0/png/Alarm.png"), inherited
    if name == "reminder-due" or name.startswith("reminders-detail-"):
        return Path("docs/design/1.0/png/Reminder.png"), inherited
    role = "exact" if name == "timer-running" else "system; no exported state frame"
    return Path("docs/design/2.0/png/Timer.png"), role


@cache
def reference_pixels(frame):
    # Frame exports are immutable during the run; decode each shared reference
    # once. Simulator sources are deliberately read fresh for every comparison.
    return png_tools.check(frame)


def pair_matches(target, expected_left, expected_right):
    width, height, actual = png_tools.check(target)
    if (width, height) != (933, 466):
        return False
    native_stride = 466 * 3
    pair_stride = width * 3
    for y in range(height):
        row = actual[y * pair_stride : (y + 1) * pair_stride]
        start = y * native_stride
        if (
            row[:native_stride] != expected_left[start : start + native_stride]
            or row[native_stride : native_stride + 3] != b"\xff\xff\xff"
            or row[native_stride + 3 :] != expected_right[start : start + native_stride]
        ):
            return False
    return True


def comparison(frame, source, target):
    fw, fh, expected_left = reference_pixels(frame)
    sw, sh, expected_right = png_tools.check(source)
    assert (fw, fh) == (sw, sh) == (466, 466), (frame, source)
    # Timestamps are not enough: reuse only after exact decoded pixel equality.
    # A fresh source or altered output is recomposed and independently checked.
    if target.exists() and pair_matches(target, expected_left, expected_right):
        return
    png_tools.concat(str(frame), str(source), str(target))
    assert pair_matches(target, expected_left, expected_right), target


def contact(source, names, output, columns=3):
    names = [n for n in names if (source / f"{n}.png").exists()]
    images = [png_tools.decode(source / f"{n}.png") for n in names]
    if not images:
        return names
    rows = (len(images) + columns - 1) // columns
    cell_width, cell_height, _ = images[0]
    width, height = cell_width * columns, cell_height * rows
    scanlines = bytearray()
    for y in range(height):
        scanlines.append(0)
        strip = bytearray(width * 3)
        grid_y, local_y = divmod(y, cell_height)
        for column in range(columns):
            index = grid_y * columns + column
            if index >= len(images):
                continue
            iw, ih, rgb = images[index]
            assert (iw, ih) == (cell_width, cell_height)
            strip[column * cell_width * 3 : (column + 1) * cell_width * 3] = rgb[
                local_y * cell_width * 3 : (local_y + 1) * cell_width * 3
            ]
        scanlines.extend(strip)
    output.write_bytes(png_tools.encode_rgb(width, height, scanlines))
    return names


mapping = []
pending = []
pages = []
for mode in ["default", "ax3", "ax3-bold-rt", "ax3-bold-rt-rm"]:
    # Dedicated atlas directories keep CTest's fail-fast captures from racing
    # a complete diagnostic suite or leaving older fixture variants behind.
    source = Path(f"sim/build/time-atlas-{mode}")
    output = base / mode
    output.mkdir(parents=True, exist_ok=True)
    captures = [(path.stem, path) for path in sorted(source.glob("*.png"))]
    missing = required - {name for name, _ in captures}
    if missing:
        raise FileNotFoundError(f"{source}: missing required Time captures: {sorted(missing)}")
    storage = Path(f"sim/build/time-storage-{mode}")
    captures += [(name, storage / f"{name}.png") for name in storage_errors]
    captured_names = []
    for name, path in captures:
        if name.startswith(("motion-", "FAILED-")):
            continue
        frame, role = reference(name)
        target = output / f"{name}.png"
        comparison(frame, path, target)
        mapping.append(f"{mode}\t{name}\t{frame}\t{role}\t{path.as_posix()}\t{target.as_posix()}")
        captured_names.append(name)
    # The overview repeats the comparison pair for every row: reference left,
    # firmware right. Individual comparison PNGs remain at native resolution.
    contact(output, primary, output / "contact.png")
    contact(output, stress, output / "stress-contact.png")
    contact(output, storage_errors, output / "storage-errors-contact.png", columns=2)
    current_outputs = {f"{name}.png" for name in captured_names}
    current_outputs.update({"contact.png", "stress-contact.png", "storage-errors-contact.png"})
    for start in range(0, len(captured_names), 6):
        names = captured_names[start : start + 6]
        target = output / f"all-contact-{start // 6 + 1:02}.png"
        contact(output, names, target, columns=2)
        current_outputs.add(target.name)
        pages.append(f"{mode}\t{target.as_posix()}\t{','.join(names)}")
    # This dedicated directory contains only generator-owned review PNGs.
    # Retire stale fixtures/pages after a successful current-mode render; the
    # raw simulator captures and every manifested comparison stay untouched.
    for obsolete in output.glob("*.png"):
        if obsolete.name not in current_outputs:
            obsolete.unlink()
    # S2 is being migrated on a separate branch. Preserve its integration
    # evidence without treating shell-owned legacy UI as a Time gate result.
    shell = Path(f"sim/build/time-shell-atlas-{mode}")
    pending_output = base / "pending-S2" / mode
    pending_output.mkdir(parents=True, exist_ok=True)
    pending_names = []
    for name, capture in [
        ("save-review", "14-time-voice-review"),
        ("save-saved", "15-time-voice-saved"),
        ("save-error", "16-time-voice-error"),
    ]:
        path = shell / f"{capture}.png"
        if not path.exists():
            continue
        target = pending_output / f"{name}.png"
        comparison(Path("docs/design/2.0/png/Timer.png"), path, target)
        pending_names.append(name)
        pending.append(f"{mode}\t{name}\tpending S2; shell-owned\t{path}\t{target}")
    contact(pending_output, pending_names, pending_output / "contact.png", columns=1)
    pending_files = {f"{name}.png" for name in pending_names}
    if pending_names:
        pending_files.add("contact.png")
    for obsolete in pending_output.glob("*.png"):
        if obsolete.name not in pending_files:
            obsolete.unlink()
base.joinpath("reference-map.tsv").write_text(
    "mode\tsurface\tdesign_reference\treference_role\tsource\tsheet\n" + "\n".join(mapping) + "\n"
)
base.joinpath("all-contacts.tsv").write_text(
    "mode\tcontact_sheet\tsurfaces\n" + "\n".join(pages) + "\n"
)
base.joinpath("pending-S2.tsv").write_text(
    "mode\tsurface\tstatus\tsource\tsheet\n" + "\n".join(pending) + "\n"
)
print(f"Verified {len(mapping)} Time comparisons / {len(pages)} contact pages in {base}")
print(f"Kept {len(pending)} shell-owned Save comparisons separately under pending-S2")
