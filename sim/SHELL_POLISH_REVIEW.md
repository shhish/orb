# Orb shell polish review

Branch: `orb/w2-polish-shell`. No commits, signing, publishing or board activity.

The reference is the exported Design 1.0 PNG, with `hig.md` controlling safe
chords, minimum type, material budgets and interaction. The audit's hollow
digits, Battery and karaoke claims were intentionally excluded.

## Comparison sheets

Run `bash sim/polish-shell-shots.sh` after the simulator build and CTest suite.
Every sheet uses `python3 sim/png_tools.py concat`: reference left, firmware
right. Files are under `sim/build/polish-shell/sheets/` with the suffixes
`-default.png` and `-ax3-bold-rt.png`.

The [local gallery](build/polish-shell/index.html) indexes all 46 comparison
sheets. Both variants of every listed screen were visually inspected.

| Item | Change | Sheet stem(s) |
| --- | --- | --- |
| 1 | Apps uses plain prompt, upload above a bottom bar, sun, and black glyph ink on every identity disc. | Apps |
| 2 | Control Center uses torch and sun; all six controls share the same static material and quiet rim. AX keeps the same six controls. | ControlCenter |
| 3 | Inline Stack actions retain one glass capsule with the outside shadow clipped to the capsule. Hit areas and physical input gates remain. | Stack, Stack-usage, Stack-timer |
| 4 | Settings Accessibility uses a black accessibility figure on its blue identity disc. | Settings |
| 5 | Always On uses 128 px Inter Light (300), Regular (400) with Bold Text, approximately −4 px tracking. | AlwaysOn |
| 6 | Approval has a single-line source when it fits the chord, AX reflow, and normal command tracking. | Approval |
| 7 | Sessions row widths follow the circle chord; offscreen rows retain a usable source width until scrolled into view. HomeRooms requires the pending file-scope exception. | Sessions |
| 8 | Known receipt ages appear on list cards and the detail sash. Numbers and time units use nonbreaking spaces. AX reflows metadata below the title. | Notifications, NotificationDetail |
| 9 | Updating and Setup use balanced two-line instructions; TalkSend balances the actual transcript; Agents subtitle and SessionLive title use available chord space. Reminder requires the pending file-scope exception. | Updating, Setup, TalkSend, Agents, SessionLive |
| 10 | Talk and Dot use centered solid provider discs with smaller black artwork. | Talk, Dot |
| 11 | Now Playing has its pink music note and seek double triangles; layout follows its own full-screen frame. AX preserves the time counters. | NowPlaying |
| 12 | Spoken Reply resolves Dot/Claude/Codex from the explicit voice-request target, with unknown-source fallback. The footer selects the complete instruction when it fits. | SpokenReply |
| 13 | Spoken replies subtitle is “Dot and agents read aloud”; AX uses “Read aloud”. | SoundSettings |
| 14 | Verified Charging already sizes digits and percent labels to their content and places them adjacent; no additional source change was needed. | Charging |

## Design gate

This review applies to both variants of every sheet listed above; Face is also
captured as the unchanged baseline for the gate's Face/list/card requirement.

- **Two layers:** content stays flat; glass remains on controls. Stack's clip
  is ordinary content. Control Center uses a common static material to honor
  the glass-shape budget; Now Playing has three glass controls, one tinted.
- **Canvas:** true black inside the physical display circle. No gradients or
  glow were added. The emulator's gray exterior corners are outside the panel.
- **Round-first:** labels and row widths use measured chords. Sessions measures
  capsule end caps and caps AX source rectangles to avoid the retained renderer
  shrinking accessible type a second time. AX reflows rather than running text
  into the bezel. HomeRooms is explicitly pending below.
- **Sizes:** existing semantic text faces remain; the added 128 px Light clock
  is the owner's explicit Always On exception to the general weight/size rule.
- **Colour:** flat identity colours, black disc ink as requested, semantic
  accents and only one tinted glass action. Existing source-identity exceptions
  remain those documented in HIG §5.
- **Approvals and agents:** Deny remains, decisions stay neutral, no automatic
  decisions or work spinners. No physical-input gate was relaxed.
- **Motion:** the existing press/spring/material behavior and Reduce Motion
  paths remain. Reduce Transparency uses opaque materials.
- **Navigation:** physical BOOT/PWR and existing gestures remain; no redundant
  navigation arrows were added.
- **Visual review:** every listed default/AX sheet was inspected; independent
  lead review and real-board screenshots remain the release gate.

## Deliberate differences and pending scope

- The signed text pack's minimum face is 21 px; the frames use some 15–19 px
  text. Headers therefore move down when the larger face needs a wider chord.
  AX uses fewer visible rows and scrolling prose. Long variable step text can
  still ellipsize; session titles and the requested status subtitle fit.
- AX Setup and Updating retain concise instructions so both fit in two lines;
  the full Updating sentence is retained at Default.
- Approval commands use signed Inter with normal tracking, rather than the
  frame's JetBrains Mono. No new text family was introduced.
- Always On retains the clock API's raised colon. It uses filled light digits,
  not the audit's incorrectly requested hollow digits.
- Replayed notifications have no source timestamp in protocol v1.1. Their age
  stays unknown; fresh arrivals use locally observed receipt ages. Design
  scenes explicitly set synthetic receipt ages without changing live data.
- Spoken Reply's full footer is about 306 px in the smallest signed face,
  wider than its lower chord. Default uses “Double-tap or talk”; AX uses
  “Talk or double-tap to stop”. Captions remain uniform, without karaoke.
- Sound settings retains the actual Quiet Hours/Always On policy rows rather
  than inventing the frame's unsupported voice selector.
- Apps retains 80 px discs for the HIG's 20 px gaps; AX Apps uses a list.
- Notification detail gives the age its own header line, and AX can use the
  provider name when the project does not fit. List ages use the metadata lane.
- Charging retains truthful charging state rather than an unsupported PMU
  time-to-full estimate.
- Talk retains “Release BOOT to review” to describe confirm-before-send.
- HomeRooms rows and the Reminder voice caption live in
  `orb_home/orb_home_view.c` and `orb_time_app/orb_time_view.c`, outside the
  requested shell scope. They remain unapplied pending the two-file exception.
  [The prepared patch](polish-shell-scope.patch) passes `git apply --check`;
  it has not been applied, rendered or tested.

## Assets and validation

The font pack is 5,761,412 bytes. Combined fonts/sounds signed-envelope footprint
is 6,111,232 bytes, leaving 2,277,376 bytes in the 8 MiB partition. The generated
identity changed: the lead must re-release owner-signed assets with this app.
No signing key was accessed.

Captured LVGL clock rasters have 8,283 lit pixels versus 16,713 previously;
Bold has 10,363 versus 18,836. These include the size and weight change and
measure pixels, not device power or burn-in.

Font validation: 68 tests passed; all six settings passed the host self-test,
2,136 status checks passed, and 40,000 concurrent glyph decodes had no failures.
Python lint/format and shellcheck passed.

Final simulator build passed with `cmake --build sim/build -j4`; the full
`ctest --test-dir sim/build -j4 --output-on-failure` suite passed **776/776**
in 49.39 seconds. Both commands ran under `nice` outside the sandbox's
priority restriction. The shell contract passed 1,516 checks; agent fixtures
passed 632 Default and 629 AX checks. Long-title assertions remain intact;
the notification tap helper now reacquires live rows after timestamp refreshes.

`nice scripts/idf.sh build` passed, with its four-job cap. The full log is
[idf-build.log](build/polish-shell/idf-build.log). It reports one unused-function
warning in the untouched `orb_devtools.c` (`command_reply_printf_until`).
`orb.bin` is `0x2a0000` bytes in the `0x600000` app slot, leaving 56% free.

All 46 comparison PNGs passed `sim/png_tools.py check`. Final diff whitespace,
shellcheck and the unapplied scope patch's `git apply --check` passed. The
working branch remains `orb/w2-polish-shell`; no commit was created.
