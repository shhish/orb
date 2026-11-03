# Orb Stack polish review — 2026-10-28

Branch: `orb/w2-stack-polish`. Changes are uncommitted. No board was flashed,
no owner configuration was changed, and no hub telemetry was invented.

## Changes

1. **Smart Stack:** an allocation-free selector orders unanswered agent cards,
   running/finished Time activities or today's next alarm, capability-backed
   Now Playing, lights on in Home at any hour, relevant Claude/Codex usage, and
   a quiet date/next-alarm floor. Home rises above media at night. Paused Time
   activities remain below live context. Usage requires a known, current window
   at least 70% used or resetting within 30 minutes; unknown/stale/expired
   observations are omitted. Unavailable Home/media are omitted. The genuine
   empty state needs no useful activity and no known date/clock. Neighboring
   previews describe actual entries; they navigate correctly, and removing an
   earlier request preserves the activity currently being viewed.
2. **Empty states:** Notifications, Stack, Sessions and Dot share a quiet
   Framework7 glyph, regular Title3 title and Footnote2 explanation. Measured
   wrapping handles AX3 + Bold. Empty views use a compact top title; Dot reserves
   room for its microphone action and states connection limitations honestly.
3. **Settings values:** a shared row-local accessory anchors Sounds and Volume
   at the right inset and true vertical centre after resolving font metrics.
   AX3 Volume reflows its icon/title to leave a trailing value lane. All eleven
   Settings rows were captured at both sizes, including scrolled positions.
4. **Light dial:** the semantic glyph is retained and recoloured on state
   updates. Off matches the secondary value; the arc is explicitly invalidated
   on power changes, even when brightness/hue stay constant. Forty on/off
   transitions pass retained-versus-full-redraw checks at both sizes, without
   adding glass for the icon.
5. **Sound persistence:** Silent uses bit 19 of the existing NVS preference
   word and the existing debounced storage worker. Preferences load before the
   sound tasks start; workspace creation restores through
   `orb_shell_set_silent`. Face-down/idle muting stays transient. Volume,
   spoken replies and Quiet Hours were already persisted. Silent also shows
   the existing save-failure feedback. See `orb_sound/README.md`.
6. **Microphones:** all ADC users, including the lead's raw recording tool,
   apply PGA 36 dB and digital +16 dB on both ES7210 channels (+22 dB relative
   to the supplied recording). This projects its -31 dBFS peak to -9 dBFS;
   it is not a new acoustic measurement. Noise suppression floors, echo
   coupling and talk-to-interrupt levels share the new gain domain. The old
   interrupt floor exceeded the supplied speech RMS, so the new sustained
   detector floor is -34.5 dBFS, below projected -30 dBFS speech RMS, with the
   existing both-mic/noise/echo guards. Register map, rationale and physical
   validation procedure are in `orb_voice/README.md`.

## Screenshots and comparison sheets

Every path below is relative to the repository root. Each named surface has
both `default` and `ax3-bold-rt` PNGs at
`work/stack-polish/<profile>/<surface>.png`, and a design-left/render-right
sheet at `work/stack-polish/<surface>-<profile>-sheet.png`.
Sheets were made with `python3 sim/png_tools.py concat DESIGN RENDER SHEET`.

| Surface names | Design reference | Review result |
| --- | --- | --- |
| `Stack`, `StackDay` | `Stack.png` | Same matte card structure; actual night and daytime Home content. |
| `StackTimer`, `StackStopwatch` | `Stack.png` | Activity, state and action have separate lanes at both sizes. |
| `StackDate`, `StackUsage` | `Stack.png` | Honest floor/pressure cards; date uses month/day plus weekday. |
| `NowPlaying` | `Stack.png` | Stack geometry, complete two-line AX3 idle title, three transport controls. |
| `StackEmpty` | `Stack.png` | Genuine no-data fixture; shared glyph/title/footnote treatment. |
| `NotificationsEmpty` | `Notifications.png` | Shared empty treatment; AX3 heading stays above content. |
| `SessionsEmpty` | `Sessions.png` | Shared empty treatment; no fabricated sessions. |
| `DotEmpty` | `Dot.png` | Shared empty treatment with one BOOT instruction and visible mic action. |
| `Settings`, `SettingsNext` | `Settings.png` | Values centre and right-align, including faded/scrolled rows. |
| `SoundSettings` | `SoundSettings.png` | Volume centre/right-aligns; AX3 reflows icon/title. |
| `HomeDial`, `HomeDialOff`, `HomeDialCycle` | `HomeDial.png` | Retained glyph follows power; Off is explicit and dimmed. |
| `Face` | `Face.png` | Unchanged representative face for the accessibility gate. |

All Settings rows also have individual captures and sheets:

- `work/stack-polish/<profile>/Settings-<row>.png`
- `work/stack-polish/Settings-<row>-<profile>-sheet.png`
- Rows: `Display`, `Sounds`, `Accessibility`, `Mac`, `About`, `Sessions`,
  `Updates`, `Units`, `Controls`, `Compass`, `Battery`.
- Overview: `work/stack-polish/Settings-all-default.png` and
  `work/stack-polish/Settings-all-ax3-bold-rt.png`.
- Reproduce row captures with `sim/build/orb_shell_sim --settings EXISTING_DIR
  default` or `... ax3 bold rt`.

The Stack, NowPlaying, SoundSettings and Face emulator captures include 60
headless frames per profile; the sheet uses `frame-000060.png`. The emulator's
grey outside surround is outside the physical circle. The board comparison
inputs remain `work/lead-review/board-*.png`; fresh board evidence is pending.

## Design gate

The following notes apply to every surface listed in each column above,
including both screenshot profiles. Independent visual review covered all
surface captures and all 22 individual Settings row captures. Its two layout
findings (Time metadata clearance and duplicate Dot BOOT copy) were corrected
and reviewed again. A code review also expanded preview hit targets to 72 px
without changing their pixels.

| Gate heading | Stack cards | Notifications / Sessions / Dot empties | Settings / Sound | Home dial |
| --- | --- | --- | --- | --- |
| Two layers | Pass: matte cards, glass actions only; media has three, one tinted. | Pass: plain glyph/text; Dot has one tinted mic action. | Pass: flat rows and plain switches; no glass rows. | Pass: glyph/value are content; power is the glass action. |
| Canvas | Pass: black circle, no gradients/glow. | Pass: black circle, no gradients/glow. | Pass: black circle, no gradients/glow. | Pass: black circle, no gradients/glow. |
| Round-first | Pass: cards/labels fit; neighbor previews intentionally fade. | Pass: measured centred groups; AX3 headers do not collide. | Pass: eleven rows checked at both sizes; scroll peeks are deliberate. | Pass: name, room, value and glyph stay inside chords. |
| Sizes | Pass: semantic Inter, legible AX3 reflow; actions/previews have 72px hit areas. | Pass: Title3/Footnote2, shared icons, AX3/Bold wrapping. | Pass: 72/100px Settings rows, 72/104px Sound rows; values resolve before alignment. | Pass: existing clock/semantic type and controls; glyph is retained. |
| Colour | Pass: semantic provider/app accents and state text; one tinted media action. | Pass: quiet secondary glyph; state is in text. | Pass: identity discs; On/Off/value and switches retain explicit meaning. | Pass: Off text plus dimmed glyph/value/arc. |
| Approvals & agents | Pass: existing explicit decisions, no default approval or spinner; no invented cards. | Pass: no fabricated activity or loading spinner. | Pass: no new decision flow. | Pass: existing deliberate action guards unchanged. |
| Motion | Shared springs/RT/Bold used; settled variants pass, navigation/input suite passes. | Shared type/RT; live Dot content hides the empty group. | Shared native scrolling/RT; geometry regressions pass. | Retained/full-redraw equality passes over 40 power transitions per profile. |
| Navigation alternatives | Pass: physical BOOT/PWR/rim/twist; previews page the real order. | Pass: physical navigation; Dot retains mic/BOOT route. | Pass: physical navigation; no redundant chevrons added. | Pass: existing physical navigation; no extra on-screen navigation. |
| Feels Apple-made | Pass: consistent focused card, restrained context and actual neighbors. | Pass: balanced glyph/title/explanation, no isolated bold headline. | Pass: aligned accessories, consistent grouping and AX3 hierarchy. | Pass: state agrees across icon, number, arc and explicit Off subtitle. |

Frame differences are intentional: no fake completed-agent preview; content
comes from actual fixtures; Now Playing uses the requested Stack geometry;
Sound retains existing Quiet Hours instead of fabricating a hub voice name.
Existing semantic Footnote2 rounds the frame's 20/22px captions to 21px
(28px at AX3); Headline is 28px; empty Title3 is 33px. No new font size was
introduced. HIG takes precedence over exact frame pixels for larger type and
physical hit targets.

## Validation

- `nice cmake --build sim/build -j4`: passed.
- `nice ctest --test-dir sim/build -j4 --output-on-failure`: **774/774 passed**.
- `nice scripts/idf.sh build`: passed, four-job default. App is 0x290000 bytes,
  with 57% of the 6 MiB app slot free (build size, not a device measurement).
- Sound host ASan/UBSan: palette 1,899, voice lease 35, preference storage 140 checks.
- Voice host: state machine 2,322, DSP 624, callbacks 44, requests 97 checks.
- Board host: audio construction/cleanup 1,703 checks; gain register preservation,
  all ten failure stages and retry pass.
- New Python tests pass Ruff check and format. Git diff whitespace check passes.
- Logs: `work/lead-review/polish-sim-build.log`, `polish-ctest.log`,
  `polish-idf-build.log`.

Fresh real-board screenshots, memory-owner budgets, arm's-length stereo speech
peaks/no clipped samples, louder speech and talk-over/typing at real speaker
volumes remain lead validation before release. Simulator evidence does not
establish those physical results.
