# Games tuning — orb/w2-games-tune2

**Superseded by [GAMES3_REVIEW.md](GAMES3_REVIEW.md) (2026-10-28).** The adaptive-neutral, near-level damping, flow audio and accelerometer-only clock claims below are historical; they were identified as causes of the owner's real-board failures and replaced.

## Resting-pose follow-up (2026-10-28)

This section supersedes the fixed-neutral stillness and pause-sheet claims in
the prior pass below. The lead's physical tilt-direction evidence is retained:
downhill is still `-ax/+ay`. No commit or board operation was performed.

### Changes

- Stillness comes from short-window accelerometer/gyro variance and signed
  pose/rate changes, independently of the calibrated steering angle. A bounded
  200 ms coherent-rate filter distinguishes slow deliberate tilts from typing
  vibration and desk creep. Only distinct valid sensor samples accrue rest time;
  fixed-step catch-up cannot manufacture it.
- Beads starts clock travel after 1.2 seconds of sampled quiet and completes
  exact, zero-velocity target placement within another 0.75 seconds. Motion
  releases the clock in sensor time, including finite impact/free-fall samples
  whose steering/physics is suppressed. Timing gaps and invalid data clear
  quiet accumulation without inventing a pickup.
- After 0.5 seconds of quiet, the full 3D neutral follows the resting pose with
  a two-second time constant. Deliberate motion stops this learning immediately.
  Marble and Ring Snake share it. Their input response curve and fast adaptive
  steering filter remain intact.
- Main supplies bias-corrected gyro rates through the app input. Accelerometer
  capture time remains distinct from service/physics time. Synthetic devtools
  tilt ignores physical gyro, and the accelerometer-only path is traced too.
- Every pause sheet hides the floating ×, uses low-tint Resume glass (opacity
  61) with accent ink, and provides a quiet touch-accessible Done. Marble's
  redundant End Round exit becomes Done; Garden and Touch Controls remain.
  Other games retain End Round for scores. Beads says “Rest to show the time.”
  HIG §§1–5/7/8 govern the controls; no new widgets, assets or heap requests.

### Simulation traces

`sim/tests/test_games_rest.c` runs the real filter and game integrator at
independent sensor/physics cadences. CTest includes it as `games_rest_traces`.
These are synthetic host observations, not new physical-board measurements.

| Trace | Captured result |
| --- | --- |
| Pick up, tilt, set down five degrees from calibration | Exact clock in 2.80–2.84 s, across four poses (flat/35°/85°/120°), 5/10/20 ms samples, with and without gyro |
| Lead's measured rest pose, `(-0.087, 0.040, 0.916) g` | Exact clock in 2.92 s without Resume |
| Desk creeps four degrees in 30 s | Clock stays held; late steering force exactly zero |
| Typing vibration, accelerometer-only and with gyro | Clock forms and stays held |
| Slow deliberate tilt at 1°/s and 2°/s | Releases in 130–390 ms and 50–250 ms respectively; coherent motion stays active |
| 12°/s gyro yaw with unchanged acceleration | Does not settle while rotating; settles after stopping |
| Impact/free fall, then a new resting angle | Releases before a physics step; recovers without Resume |
| Sampling gaps, repeated samples and NaNs | No fabricated quiet time/pickup; subsequent real motion still releases |
| Marble/Snake after resting-pose adaptation | Marble stops drifting; Snake's next flick responds immediately |

Full trace log: `/tmp/orb-rest-traces.log` (**82,574 checks passed**).

### Screenshot evidence and design gate

Each pause comparison has default on the left and AX3 + Bold Text + Reduce
Transparency + Reduce Motion on the right:

- Marble: `sim/build/pause-followup/sheets/marble-pause-modes.png`
- Beads: `sim/build/pause-followup/sheets/beads-pause-modes.png`
- Orbit: `sim/build/pause-followup/sheets/orbit-pause-modes.png`
- Ring Snake: `sim/build/pause-followup/sheets/ring-snake-pause-modes.png`
- Pop: `sim/build/pause-followup/sheets/pop-pause-modes.png`

Fresh supporting default/AX3 comparisons are in the same directory:
`face-gate-modes.png`, `list-gate-modes.png`, `card-gate-modes.png`.
Screenshots were inspected independently, including original-resolution PNGs;
Done's actual ink is present in every pause/profile. Render/navigation tests
exercise the real controls and retained/full redraws.

| Gate heading | Scoped result for every pause surface above |
| --- | --- |
| Two layers | Pass: matte content, one tinted Resume, quiet Done; no stacked glass |
| Canvas | Pass: true black; bounded glass material; no gradient or glow added |
| Round-first | Pass: centred rows, labels and footer fit their chords in both profiles |
| Sizes | Pass: shared Inter; 72 px Resume and 48 px Done targets; AX3/Bold without clipping |
| Colour | Pass: accent ink over low tint, secondary Done and explicit action labels |
| Approvals / agents | Pass: behavior untouched; fresh supporting approval screenshot retains neutral Deny/Allow |
| Motion | Pass: material/pref paths retained; × hidden immediately on Pause; Reduce Transparency uses opaque material |
| Navigation | Pass: touch Resume/Done, Marble Garden/toggle, BOOT/PWR; scored End Round remains elsewhere |
| Craft | Pass: Play/GameOver material consistency, no floating close on Pause, actual touch/render regressions pass |

No HIG conflict was introduced. Independent lead screenshots from the real
board remain the release gate; no new board behavior, cadence, power or sound
measurement is claimed by this follow-up.

### Validation

Final commands ran sequentially under `nice -n 10`, with four workers:

- `cmake --build sim/build -j4`: passed (subsequent check: no work pending).
- `ctest --test-dir sim/build -j4 --output-on-failure`: **782/782 passed**, 32.58 s.
- `scripts/idf.sh build`: passed; `orb.bin` is 2,752,512 bytes, 56% of its app slot free.
- Focused ASan/UBSan game logic and runtime: **26,098,794** and **24,564** checks passed.
- App-input sanitizer checks: **111** passed; gyro validity, sanitation and capture timestamp covered.
- `git -c maintenance.auto=false diff --check`: passed.

Logs: `/tmp/orb-rest-sim-build.log`, `/tmp/orb-rest-ctest.log`,
`/tmp/orb-rest-idf-build.log`, `/tmp/orb-rest-host.log`, `/tmp/orb-rest-apps-host.log`, `/tmp/orb-rest-traces.log`.
The changes remain uncommitted. No flash, OTA, owner configuration or live
service change was made.

## Prior tuning pass (historical)

This pass fixes motion alignment, game feel, and avoidable rendering work. It
does not establish physical-board release readiness. No commits, board access,
live hub changes, owner configuration changes, or new assets were made.

**Motion and runtime**

- Marble, Beads and Ring Snake use the same full-3D held-pose calibration.
  Downhill remains `-ax/+ay` in LVGL coordinates. Start, restart, wake/resume,
  and returning from touch controls capture a fresh 300 ms neutral pose.
- A radial 0.015 dead zone, progressive response and adaptive 4–35 Hz filter
  preserve fine adjustments while responding quickly to a flick. Calibration
  needs distinct, stable, timestamped samples; catch-up steps cannot fabricate
  them. Impacts/free fall are rejected without permanently selecting touch.
  Rejected impact samples do not advance Beads' stillness latch.
- Main reads motion at nominal 100 Hz only while an unpaused tilt game needs
  it; otherwise the existing 50 Hz cadence remains. Capture timestamps are
  taken after I2C reads. Failed game entry clears the fast-poll flag.
- Missing motion enables Marble/Snake touch controls. Beads holds its state,
  explains that motion is unavailable and retries. Snake Resume retries motion
  at the new held pose. Brief calibration text and pause hints explain re-zero.

**Game behaviour**

- Beads: gravity 700→1550 px/s²/g, moving damping .94→.976 per step, maximum
  speed 340→460 px/s; neutral damping .90 arrests residual flow. The rim uses
  .08 restitution. Collisions share approaching velocity rather than injecting
  energy; coincident particles separate deterministically.
- Beads starts clock travel after two seconds of calibrated stillness and
  reaches an exact hold by 2.75 seconds. Deliberate relative tilt releases it.
  Near-critical settling damping prevents beads leaving the circular display:
  the directional regression's maximum radius fell from 325.40 to 218.59 px.
  No wall time means a fluid toy with “Time not set”, never a fictitious clock.
- Bead hiss uses the sampled `ORB_SOUND_GAME_BEAD` palette cue and RMS flow
  speed, at most 2–5 submissions/second. Slow flow, settled time, calibration
  and pause are quiet. Marble roll is also silent during recalibration.
- Snake requires two consistent engagement steps and 50 ms of neutral before
  another tilt turn. Holding an angle produces one turn. Tilt cadence is
  12→11→10 steps after 0/6/12 foods; touch cadence remains 8→7→6. Changing
  modes preserves fractional travel, clears pending tilt turns and keeps food
  clear of the existing touch arrows.
- Orbit offers 0.8 seconds of recovery after a hit, shown by a steady hollow
  planet. Overlapping comets cost one life, and recycled comets change lanes
  opposite the player. There is no flashing, including under Reduce Motion.
- Pop refills a completely cleared field within 300 ms. Its 45-second expiry
  cannot also cause a missed bubble or spawn a new one on the final step.
- Marble's existing stage geometry, speed limit, rewind, touch controls and
  solvability remain covered by the full behavioural tests; the shared input
  and calibration sound fixes are its tuning changes.
- Large result scores are compacted with a text font that includes their unit
  glyphs; ordinary clock-size scores remain unchanged. Orbit's initial score
  is centred immediately rather than briefly starting outside its clip.

**Rendering and allocations**

Beads now culls particles outside each display band's clip before submitting
draw tasks. LVGL otherwise allocates and dispatches off-band rectangles. The
change adds no buffer or widget and is checked against retained/full redraws
and actual draw-task creation. Frame damage remains the real moving field;
culling reduces CPU/metadata work, not panel bytes.

The deterministic draw-task check allocates 564 particle fill tasks for 519
live particles over a full frame, with zero off-band tasks; a small empty rim
redraw allocates zero particle tasks. All 44 regenerated render pixel hashes
match the captures from before culling exactly.

Exact ESP32-S3 structure sizes were obtained from GCC/DWARF, with temporary
compilation of the original HEAD for comparison:

| Existing allocation / storage | Before | After | Change |
| --- | ---: | ---: | ---: |
| Games runtime, one per active game | 40,840 B | 40,864 B | +24 B |
| Games view, one per active game | 56,976 B | 56,984 B | +8 B |
| Combined existing game-owner allocations | 97,816 B | 97,848 B | +32 B |
| Model state, inside those allocations | 13,500 B | 13,504 B | +4 B |
| Tilt filter, inside runtime | 52 B | 68 B | +16 B |
| App input, transient stack value | 24 B | 32 B | +8 B |

There are **no new heap requests or LVGL widgets**. Beads collision scratch
remains 4,480 static bytes. The devtools override adds 24 static bytes and an
8-byte lock, and fast-poll observation adds one atomic bool. Existing shared
fonts supply the new text; no new font or pixel asset is added. These sizes
are not a substitute for `orb_memory` budget enforcement on the board.

The separate post-culling host render run measured:

| Game | Host p95 | Mean / maximum flushed pixels per frame |
| --- | ---: | ---: |
| Beads flow, two physics steps plus rendering | 6.149 ms | 194,696 / 194,696 |
| Beads held clock, 60 frames | no redraw | 0 total |
| Marble, three stages and both input modes | 2.336–2.998 ms | 4,113–4,683 / 11,476 |
| Orbit | 4.118 ms | 63,112 / 64,896 |
| Ring Snake | 1.496 ms | 9,223 / 48,704 |
| Pop | 1.065 ms | 13,396 / 59,820 |

Log: `/tmp/orb-games-final-render.log`. Before culling the preceding standalone
Beads run measured 122.845 ms p95. Mac load and scheduling vary: these are
captured host observations, not ESP32 timing or a controlled board benchmark.
Active Beads still flushes about 90% of the display square and needs board CPU,
panel cadence, input latency and power measurements.

**Evidence and design gate**

Run `nice -n 10 sh sim/games-tune-shots.sh` to regenerate the principal sheets.
Each design pair uses `python3 sim/png_tools.py concat FRAME RENDER OUT`.
Paths below are relative to the repository root.

| Surface | Evidence |
| --- | --- |
| Marble play | `sim/build/games-tune/sheets/marble-play-{default,ax3-bold-rt-rm}.png` |
| Beads exact clock | `sim/build/games-tune/sheets/beads-play-{default,ax3-bold-rt-rm}.png` |
| Beads flow / settling | `sim/build/games-tune/sheets/beads-{flow,settling-early,settling-late,clock}-modes.png` |
| Orbit play / result | `sim/build/games-tune/sheets/orbit-{play,end}-{default,ax3-bold-rt-rm}.png` |
| Ring Snake play / result | `sim/build/games-tune/sheets/ring-snake-{play,end}-{default,ax3-bold-rt-rm}.png` |
| Pop play / result | `sim/build/games-tune/sheets/pop-{play,end}-{default,ax3-bold-rt-rm}.png` |
| Every game's start and pause | `sim/build/games-tune/sheets/GAME-{start,paused}-modes.png` |
| Calibration, unavailable motion/time, re-zero pause, maximum scores, Orbit recovery | `sim/build/games-tuning-review/` (24 inspected sheets) |
| Supporting Face / list / card | `sim/build/games-tune/gate/{face,sessions,agents,approval}-{default,ax3-bold-rt}-sheet.png` |
| Independent Beads / Snake reference review | `sim/build/logic-review-final/` |

The default and AX3 + Bold + Reduce Transparency + Reduce Motion sheets were
inspected, including intermediate settling and error states. Independent
review also inspected the supplemental and Beads/Snake reference sheets.

| Gate heading | Scoped result |
| --- | --- |
| Two layers | Pass: matte game content; glass only for reading-scene controls, one tinted primary, no stacked glass. |
| Canvas | Pass: true black, no gradient, glow, or new decorative background. |
| Round-first | Pass: bounds, status lines, particles and result scores fit their chords; settling trace stays on panel. |
| Sizes | Pass: shared Inter styles, existing touch targets and accessible pause controls; maximum score units have real glyphs. |
| Colour | Pass: existing game accents; calibration/errors use text and Orbit recovery uses a shape, not colour alone. |
| Approvals / agents | Supporting Face/list/card captures retained; games add no approval or send action. |
| Motion | Pass: essential play remains, recovery does not flash, preferences work and settled Beads has zero redraw. |
| Navigation | Pass: physical BOOT/PWR and existing pause, touch, Resume and exit actions remain reachable. |
| Craft | Host interaction, retained-frame and accessibility checks pass; lead panel review remains required. |

Remaining literal reference differences are recorded rather than claiming an
exact pixel match: Beads' existing sampled digit masks have a coarser lattice
silhouette than the exported SVG-clipped dots; Snake uses the shared 96 px
clock face instead of the reference's unavailable 84 px, and Best uses 21 px
instead of 18 px (the latter is below HIG's minimum). Orbit's 24 px score maps
to the existing 23 px footnote style. Marble's first-play rewind teaching line
is intentional. Generic GameOver comparisons use the reference's Pop layout
with each game's own name, score and accent. HIG takes precedence over the
reference's undersized labels. Lead review must assess these differences.

**Lead tool and board follow-up**

`scripts/orb-dev.sh tilt X Y MS` works over USB or the existing `--wifi`
transport. X/Y are finite specific-force g values within the unit disc;
duration is 1–5000 ms. It wakes first, reconstructs positive z, replaces a prior
override and expires automatically. All transport/firmware allowlists and
`docs/protocol.md` are updated. The override affects only app input, never
physical pickup, face-down, tap, twist or power sensing, and is compiled only
with devtools. The hub CLI preserves float arguments.

For a repeatable trace, start a tilt game, hold `tilt 0 0 5000` while using
Pause → Resume to capture that neutral, allow at least 300 ms, then use
`tilt -0.20 0 1000` (right) or `tilt 0 0.20 1000` (down), followed by
`tilt 0 0 4000` to observe settling. Also test the actual physical held pose:
synthetic overrides cannot establish sensor mounting, comfort or real latency.

The lead still needs to:

1. Install on the dev board and capture `orb-dev.sh shot` for every game,
   calibration, pause, held clock, recovery and result, in both text profiles.
   Review the documented reference differences before any merge/release.
2. Verify desk offset, upright/in-hand poses, left/right/up/down, slow tilt,
   quick flick, shake rejection, wake and Resume re-zero on the enclosure.
   Confirm Snake's 200→167 ms tilt cadence feels fair without a frantic return
   to neutral, and Beads forms the time within 2–3 seconds of real stillness.
3. Capture `orb-dev.sh heap` before/after repeated entry/exit and accessibility
   changes; require no new games/glass/raster/LVGL owner rejections. Check
   internal/PSRAM floors and fragmentation; the simulator has no owner limits.
4. Measure real active-Beads and other-game frame cadence/CPU/I2C latency with
   existing diagnostics (`top`, display logs), including 100 Hz polling and
   sound. Check idle returns to 50 Hz. No board FPS/power numbers are claimed.
5. Listen at normal desk volume to roll, hiss, switch, collect, pop, failed
   and done cues. Verify Silent/face-down policy and that flow hiss stays quiet
   at its maximum density. The simulator does not play the physical speaker.

**Validation**

- Final simulator build and complete CTest under `nice`, four workers:
  **777/777 passed**, 29.04 seconds, including the off-band draw-task regression.
- Final ESP-IDF dev build under `nice`, four workers: **passed**;
  `firmware/build/orb.bin` is 2,686,976 bytes, with 57% of its 6 MiB slot free.
- Render tests: **13,879 checks**, 1,746 moving and 72 transition comparisons
  passed; 30 cold-scene redraw oracles, accessibility/error states, actual
  off-band task counts and held-clock zero-damage checks are included.
- ASan + UBSan game logic: **26,098,794 checks** passed; game runtime:
  **24,564 checks** passed, including sample lifecycle, missing motion,
  rejected impacts, sound density, pause and failed-entry cleanup.
- Tool integration: **141** transport/log daemon Python tests and **90** hub
  devtools tests passed, plus **104** app host checks and devtools sanitizer
  suites. The hub tests use this worktree in `PYTHONPATH`; the installed venv
  otherwise points to integration. Test sockets stayed local/temporary.
- Ruff, shellcheck and `git -c maintenance.auto=false diff --check` passed.
- No commit, signing, flash, OTA, owner settings change or live service restart.

Final logs: `/tmp/orb-games-sim-build.log`, `/tmp/orb-games-ctest.log`,
`/tmp/orb-games-idf-build.log`, `/tmp/orb-games-sanitizers.log`, and
`/tmp/orb-games-final-render.log`.
