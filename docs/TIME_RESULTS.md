# Time — design round 3 (2026-10-27)

Round 3 follows the lead's round-2 contact review. Timer and Stopwatch now use
Apple Watch style duration formats, the counter clears the ring's inner edge,
and laps have a compact pinned live header with fully disappearing controls.

No Git writes, board access, owner configuration changes or live-service
operations were performed. The preexisting untracked root `Makefile` was left
untouched. Builds use four jobs. All evidence is host/simulator evidence; no
physical-panel performance, power, sound or contrast measurement is claimed.

## Behaviour and rationale

- Main Timer/Stopwatch counters show `M:SS` or `MM:SS` below one hour (`5:00`,
  `3:30`, `1:22`, `59:59`), then `H:MM:SS` (`1:00:00`, `24:00:00`,
  `99:59:59`). There are no hundredths in the main counter or pinned header.
  Lap splits retain hundredths, including long splits such as `98:59:58.69`.
  The pure, bounded `orb_time_format_duration()` API is shared by laps/header
  and tested at format boundaries and every output capacity.
- **Hours reveal by carrying the minute adjustment beyond 59:59.** This keeps
  the frequent five-minute picker free of another permanent affordance. Rim
  input uses its unwrapped angle, so it crosses the seam in either direction
  and can reverse during the same gesture. Tapping the selected minute digits
  also carries into hours, preserving a visible button alternative. Once an
  hour group appears, it can be selected and adjusted directly. An in-flight
  hour turn keeps its original unit even if the hour group disappears; after
  release, the next gesture selects visible minutes.
- Digit groups omit redundant leading zeros. Their centring changes with the
  format, but their **figure size stays fixed for each mode** across states,
  9:59/10:00, 59:59/1:00:00 and the supported maximum. For each mode the
  measured widest format constrains one shared clock-font raster scale. The
  scale must fit `orb_chord_width()` at both actual line-box edges, inside
  radius `212 - ceil(13/2) - 16 = 189 px`: ring inner edge plus a full 16 px
  text margin. Alarm uses its own fixed HH:MM size. This resolves per-format
  fit without changing the figure size while counting.
- Alarm remains **24-hour `HH:MM`**. The protocol, firmware prefs model and the
  integration hub's current Prefs implementation provide sources, idle timeout
  and resolved temperature units; `hello` provides timezone/time. They do not
  provide locale or hour cycle. Temperature and timezone cannot safely stand
  in for a 12-hour preference. No unsupported preference or protocol extension
  is invented in this UI polish round.
- Laps remain in Stopwatch's scrolling viewport and use
  `orb_round_list_apply()` plus updates on parent scroll. Fastest retains
  green/up and slowest red/down, with real space reserved between the arrows
  and long values. A long AX3 value uses the shared Footnote 1 tabular style
  when Body cannot fit on one line.
- The scrolling controls dissolve from their material and caption ink before
  any part reaches the viewport's top, then their complete wrappers are hidden.
  **HIG §§2/9 adaptation:** the requested fade is implemented by changing the
  glass amount, rather than alpha-fading glass. The top content dissolve starts
  with scrolling so the departing counter softens before the clipping edge.
- The compact pinned header contains one Stopwatch title and the live elapsed
  time in secondary tabular text. Its 260×92 px target returns to the counter.
  The duplicate bottom Stopwatch footer is removed. The lap viewport begins
  at y140, leaving both header lines clear at AX3 + Bold.
- Background service semantics, latched completion, sound policy, Face/Stack
  integration, page gestures, visible alternatives and Reduce Motion remain.
  Resting/paused/finished/list cases still produce zero idle frames.

## Files changed in this round

- `firmware/components/orb_time_app/orb_time_view.c`
- `firmware/components/orb_time_app/orb_time.c`
- `firmware/components/orb_time_app/include/orb_time.h`
- `firmware/components/orb_time_app/tests/host/test_time.c`
- `firmware/components/orb_time_app/README.md`
- `sim/time_main.c`
- `sim/time_contact.swift`
- `docs/TIME_RESULTS.md`

## Validation

| Check | Result |
| --- | --- |
| `cmake --build sim/build -j4` (`-Werror`) | Pass |
| `ctest --test-dir sim/build -j4 --timeout 30 --output-on-failure` | **618/618 pass**, 26.61 s |
| Time host ASan/UBSan, `make … -j4 test` | **1,809 checks + 10,000 concurrent snapshot invariants** pass |
| Time view Default | **3,311 assertions** pass |
| Time view AX3 + Bold + RT | **3,306 assertions** pass |
| Time view AX3 + Bold + RT + RM | **3,477 assertions** pass |
| Time shell Default / AX3 + Bold + RT / with RM | **70 / 72 / 72 assertions** pass |
| `IDF_PY_BUILD_JOBS=4 scripts/idf.sh build` | Pass, unsigned build; Ninja invocation `-j 4` |
| `git -c maintenance.auto=false diff --check` | Pass |

The final source passes a complete four-way simulator run. Counter format,
actual figure height, 189 px ink clearance, retained/full redraw equality,
hour-seam reversals, live pinned header, complete control disappearance,
long-lap no-wrap and ≥8 px logical arrow/value gaps all have regression gates.
Timer/Stopwatch retain the 4 Hz cap (8 frames over 2 seconds); settled
idle/paused/finished/list probes report zero frames and invalidations.

Two earlier full simulator runs passed 617/618; the already documented Home
board fixtures timed out at 30 s (`home_board_ax3`, then
`home_board_ax3_bold_rt`). Time's six view/shell tests passed on both runs.
The final complete run passed 618/618. No Home source or test was changed;
the intermittent timeout remains an existing limitation rather than a claimed
fix in this round.

ESP-IDF initially failed because sandboxed macOS `sysctl` prevented psutil from
inspecting parent processes. The authorized build outside that restriction
passed; its Ninja invocation was `-j 4`. No flash, upload, sign or release ran.

## Screenshot and motion evidence

The same current-state inventory is rendered in all three directories:

- Default: `sim/build/time-default/`
- AX3 + Bold + Reduce Transparency: `sim/build/time-ax3-bold-rt/`
- The same with Reduce Motion: `sim/build/time-ax3-bold-rt-rm/`

Each contains `contact.jpg` and `motion-laps-scroll-contact.jpg`. The latter
contains all 12 frames, including controls at rest, dissolution, fully hidden
controls, pinned live header and settled curved rows. Individual motion frames
are `motion-laps-scroll-00.png` through `motion-laps-scroll-11.png`.

| Surface | PNG names in each directory |
| --- | --- |
| Timer picker | `timer-idle`, `timer-minute`, `timer-second`, `timer-underhour-boundary`, `timer-rim-hours`, `timer-rim-reverse`, `timer-hour`, `timer-longduration` |
| Timer lifecycle/hour boundary | `timer-running`, `timer-paused`, `timer-finished`, `timer-ring-quarter`, `timer-hour-running`, `timer-hour-crossed`, `timer-background-finished` |
| Stopwatch counter | `stopwatch-idle`, `stopwatch-running`, `stopwatch-paused`, `stopwatch-underhour-boundary`, `stopwatch-hour-running`, `stopwatch-max` |
| Curved laps/header | `stopwatch-laps`, `stopwatch-laps-scroll`, `stopwatch-laps-button`, `stopwatch-laps-rim`, `stopwatch-laps-extremes`, `stopwatch-laps-live`, `stopwatch-laps-long` |
| Alarm | `alarm-idle`, `alarm-hour`, `alarm-minute`, `alarm-enabled`, `alarm-ringing`, `alarm-waiting-for-time`, `alarm-time-arrived` |
| Completion on a peer page | `timer-finish-on-stopwatch`, `alarm-ringing-on-timer` |

Other refreshed sequences retain their `motion-<name>-NN.png` frames and
`motion-<name>-contact.jpg`: start, pause, finish, lap, mode, rim, page-cancel,
page-regrab. The sheet generator uses an explicit inventory, excluding obsolete
round-1/2 captures. Full-suite shell fixtures refresh
`sim/build/time-shell-{default,ax3-bold-rt,ax3-bold-rt-rm}/` as well.

## Critical design-gate review

The following checks cover every surface enumerated above in both Default and
AX3 + Bold + RT, with Reduce Motion additionally tested. An independent agent
reviewed both final contacts, both 12-frame lap-scroll motion sheets and the
long AX3 lap detail after regeneration: **no P0/P1 findings**. Lead review
before integration remains separate.

- **Two layers — pass:** digits, header and lap rows are flat content; glass is
  limited to action controls and the touched rim knob, at most one tinted.
- **Canvas — pass:** true black with no decorative background, gradient, glow
  or card. Scroll dissolves mask content under the pinned header/page dots.
- **Round-first — pass:** actual counter ink stays inside the ring's text-safe
  circle in short, one-hour and maximum formats. The track remains continuous;
  rows scale/fade through the round-list helper. Whole controls disappear
  before reaching the lap viewport boundary; there are no clipped halves or
  bottom footer label.
- **Sizes/type — pass:** shared Inter clock/body/footnote styles, tabular
  figures, fixed counter size across format transitions, full verb captions,
  72 px circles, ≥46 px visible alternatives and AX3/Bold without wrapping.
  Two-digit-hour lap values reserve a separate arrow gap.
- **Colour — pass:** yellow timing/selection, green Start/Resume and neutral
  auxiliaries; only green/red extrema remain on the lap list. Arrows and the
  neutral selection dot supplement colour. Secondary header text stays legible.
- **Approvals/agents — not applicable/pass:** no approval or send path, no
  spinner, and completion remains latched until explicit acknowledgement.
- **Motion — pass:** direct rim/page/scroll input, reversible seam crossing,
  material dissolution before clipping, early content dissolve at the pinned
  header, existing spring/RM transitions and opaque RT surfaces.
- **Visible alternatives — pass:** digit taps cover rim setting, page dots
  cover paging, Laps reveals rows, the pinned title returns to the counter,
  and system PWR exits.
- **Apple/watch glance — pass:** significant time leads in familiar formats;
  the lap header preserves context without duplicate labels or orphaned actions.

Review iteration caught a fractional retained-damage edge, insufficient AX3
header space, a counter that reached the clip before its dissolve began, and
cramped spacing before the longest AX3 lap split. The implementation and
regression captures address these. Lead review and physical-panel validation
remain separate; nothing was merged.

## orb_nav merge verification (2026-10-27)

The reported failure at `sim/time_shell_main.c:247` is the check **after a hub
card is added and the crown turns**, rather than the initial Stack navigation
check at line 238. The initial live card, Face complication and Stack ordering
already work: with no hub cards or Home context, Time occupies page zero.

The new shell's rotary adapter incorrectly preferred every card's prose
viewport over Stack paging. The short background card has no scroll range, so
`nav_bind_rotary()` unbound the crown instead of exposing the second Stack page.
Long card prose would incorrectly consume the crown as scrolling instead.
This is not a stale snapshot or a delayed live-tree build.

`orb_workspace_nav.inc:386` now separates the rotary scroll owner from the
native touch scroll owner. Both binding (line 431) and change callbacks (line
397) use Stack's page index and `stack_count()`, including Time and Home local
activities. Native card prose scrolling remains available. The existing tests
were not modified or weakened.

| Check | Result |
| --- | --- |
| `cmake --build sim/build -j4` | Pass |
| `ctest --test-dir sim/build -j4 --output-on-failure` | **638/638 pass**, 21.46 s |
| Time shell Default / AX3 + Bold + RT / with RM | **101 / 103 / 103 checks** pass |
| `make -B -C firmware/components/orb_time_app/tests/host -j4 test` | **1,809 checks + 10,000 concurrent snapshot invariants** pass, ASan/UBSan |
| `IDF_PY_BUILD_JOBS=4 scripts/idf.sh build` | Pass, `ninja -j 4 all`; unsigned `orb.bin` is 0x270000 bytes |
| `git -c maintenance.auto=false diff --check` | Pass |

All three Time shell variants cover hub-card insertion, crown paging to Time,
card removal, finished and alarm states, retained/full redraw equality, and
zero redraws when settled. ESP-IDF's sandboxed process inspection failed in
psutil; the identical four-job build passed with that restriction lifted.
Test executables rebuilt inside the source tree were removed after validation;
the PNG evidence and firmware build remain. No Git writes, flash, signing,
owner configuration changes or live-service operations were performed. The
merge index remains for the owner to resolve/stage and commit.

### Merge screenshot gate

Fresh screenshots were reviewed by the implementing agent and independently by
a second agent. Each filename below exists under all three directories:
`sim/build/time-shell-default/`, `sim/build/time-shell-ax3-bold-rt/`, and
`sim/build/time-shell-ax3-bold-rt-rm/`.

- Face: `04-face-running.png` and `04b-face-updated.png`.
- Apps/list: `01-apps.png` (the AX3 variants use the accessible list).
- Stack card: `05-stack-running.png`, `05b-stack-card-removed.png` and
  `06-stack-finished.png`.

| Gate | Face | Apps/list | Stack card |
| --- | --- | --- | --- |
| Two layers | Pass: content plus Apps glass | Pass: flat rows/icons, navigation glass | Pass: plain activity, navigation/action glass |
| Canvas | Pass: black, no gradient/glow | Pass: black, flat identity discs | Pass: black, no gradient/glow |
| Round-first | Pass: complications fit | Pass: rows fit circle chords | Pass: heading, Back and action fit |
| Sizes/type | Pass: Inter default/AX3/Bold | Pass: accessible rows and controls | Pass: readable type and 72 px controls |
| Colour | Pass: meaningful yellow timer | Pass: permitted launcher colours | Pass: yellow timer, orange transient attention |
| Approvals/agents | Pass: truthful static offline state | Pass: explicit destinations | Pass: explicit running/finished state; no approval/spinner |
| Motion/accessibility | Pass: AX3/Bold/RT fit | Pass: AX3/Bold/RT fit | Pass: AX3/Bold/RT/RM; settled idle gates pass |
| Visible alternatives | Pass: Apps, Alerts and Stack | Pass: Back and paging arrows | Pass: Back, paging arrows while needed and Open Time |
| Craft | Pass: clear hierarchy | Pass: consistent icon/list presentation | Pass: clear countdown and completion |

This verifies simulator behavior and pixels, not physical-panel performance.
