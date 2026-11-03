# Games 3 — physical gravity, fluid motion and all-game audit

2026-10-28. Implementation and simulator validation are complete. **Real-board
frame time, heap receipts and panel screenshots remain the lead's release gate.**
No board was flashed or measured by this stream. No Git writes, palette edits,
owner configuration changes or live-service operations were performed. Changes
are confined to `orb_games`, simulator harnesses/tests and documentation.

This supersedes the adaptive-neutral, neutral damping, continuous audio and
accelerometer-only clock expectations in `GAMES_TUNING_REVIEW.md` and the older
component reviews. HIG §§6–8 and the owner's new silent-Beads requirement govern.

## Verified causes and fixes

All four reported causes were present in production code:

1. `orb_game_tilt_update_motion()` changed the neutral after 0.5 s quiet, with
   a 2 s time constant. A held angle became level. Removed that learning.
   Marble/Snake capture a 300 ms round-start neutral; only explicit restart
   captures another. Pause, waking, sensor recovery and returning from touch
   controls retain an existing frame. Orbit/Pop intentionally use touch input.
2. Beads inherited that neutral, a hard input dead zone, near-level `.90`
   damping and a slow-velocity cutoff. Its new gravity path normalizes the
   accelerometer in the existing board-calibrated screen axes, without a
   captured neutral, response curve or dead zone. Flat face-up means zero
   force; downhill remains `-ax/+ay` in LVGL pixels. Other tilt games retain
   smooth filtering and a continuous small-signal response without a hard
   zero-force plateau.
3. Beads resolved contacts once per 60 Hz frame. Replaced this with two
   120 Hz integration substeps and two spatial-hash constraint passes per
   substep: four passes per frame. Position-based velocity reconstruction uses
   actual constrained travel, capped by post-contact kinetic energy. Inelastic
   normal contacts, mild symmetric viscosity and dissipative rim friction
   remove energy. This matters: the first repeated-projection attempt still
   carried about 58 px/s mean velocity in a packed pile; reconstruction fixed
   the cause rather than masking it with near-level damping or sleeping.
4. Flow RMS queued `GAME_BEAD` repeatedly. Removed that path and suppressed
   every game-owned Beads cue, including lifecycle cues. Flow, contact, settle,
   entry, pause, resume and exit are silent. Signed palette assets are untouched.

The allocation limit remains 640 beads with radius 3; no count/radius reduction
was needed for host performance. A particular clock uses its glyph/pool count
(519 at 10:09); maximum-count solver stability and CPU are measured separately.

## Desk clock, pickup and lifecycle

The existing sensor-time quiet detector remains independent of steering.
Quiet requires low accelerometer pose/rate variation and valid, low-noise,
bias-corrected gyro samples. The prior permissive gyro thresholds admitted hand
jitter; tightened instantaneous rate and variance gates prevent the tested
handheld profiles from accumulating 1.2 s quiet. Missing gyro still permits
physical fluid motion, but cannot newly latch the clock. Any stand angle counts.
The clock starts after 72 sampled quiet steps and finishes exact, zero-velocity
placement within another 45 physics steps. Pickup releases in sensor time,
including an impact whose physics sample is rejected. Fixed-step catch-up,
repeated timestamps, gaps and NaNs cannot fabricate stillness.

Fresh flat/stand traces finish in 1.935–1.940 s. Following an abrupt pickup/play
and set-down at the lead's measured resting pose, observer recovery adds time:
that trace finishes in 2.990 s. An IMU detects motion rather than physical desk
contact; the tests establish rejection of the stated hand-noise profiles, not
an ability to distinguish a perfectly motionless hand from a stand. Actual
sensor noise and enclosure feel require the lead's board check.

Redaction, Always On, action bounds, rewind cancellation and six-step catch-up
remain intact. PWR's existing shell contract pauses then locks; waking returns
to user Pause. BOOT/Back exits through the ordinary lifecycle/save path from
Pause. OS overlays preserve the prior round/pause state. No shell/nav/UI/home
source was changed.

## Per-game findings

| Game | Findings and resulting behavior |
| --- | --- |
| **Beads** | Fixed all four causes above. Gravity stays physical while held, reversal pours across the middle, inertia survives crossing level, a packed pile becomes still, gyro-confirmed desk quiet forms the time, pickup releases it, and every game cue is suppressed. |
| **Marble** | Removed shared neutral learning and resume recapture. Uniform `.980` damping and 210 px/s speed cap already avoid near-level freezing/tunnelling. Existing four-pass analytic rounded-arc collisions and deterministic solutions of all three increasingly difficult stages pass. Removed repetitive rolling audio. Stage/final completion remains meaningful Done. Added actual tangential rim rewind after 8 px travel in the outer 40 px, with immediate activation and release/lost-contact stop; hold-anywhere remains available. Tutorial says “Turn rim to rewind.” Gates keep moving; release resolves the current gate locally with zero restored impulse. |
| **Orbit** | It is tap-to-change-orbit, so no tilt steering/neutral was invented. Fixed switch trail discs drawn on the positive y axis instead of the planet's actual polar angle. Existing earned-score speed ramps from 1.05 to a capped 1.85 rad/s; lane recycling away from the player and 0.8 s hit recovery prevent overlapping comets costing several lives. Collect, switch and hit cues remain events; losing now uses Failed rather than Done. |
| **Ring Snake** | Removed shared neutral learning; held tilt remains latched until a real neutral return. Its two-step engagement and 50 ms neutral re-arm are intentional discrete gestures, not velocity freezing. Movement interpolates across grid/angle wrap; vacating-tail collision is legal unless eating. Existing earned-score ramp remains 200→167 ms for tilt, 133→100 ms for touch. Fixed 72 px arrows covering the inner r140 track: centres move to (187,293)/(279,293); Best moves above the score only in touch mode. Added 1,080 full-track head-clearance samples. Pause truthfully says tilt keeps its centre. Losing uses Failed. |
| **Pop** | Existing separated spawns, bounded attempts, HUD exclusion, capped 15→31 px/s drift, ≤300 ms empty-field refill and 45 s deadline pass. Timeout cannot also charge a last-frame miss. Direct taps and nearest-target alternative remain. Pop/collect cues are event based. Successful timed completion uses Done; a loss uses Failed; explicitly ending a round uses neutral Close. |

Pause-sheet entrance uses Open, Resume/exit Close in the other games. Repeated
pause requests do not replay cues. Explicit End Round never celebrates a loss.
Results persist until Play Again/Done. Replay retains the run's best. Validated
OGS1 per-game scores and OGK1 Marble keepsakes still save atomically outside
LVGL/publication locks; malformed records, failed saves, retries and namespace
isolation pass. As before, an interrupted process before lifecycle save may lose
an unsaved score improvement. No persistence format changed.

## Deterministic traces

`games_rest_traces` runs production gravity/quiet detection/physics with sensor
cadence independent of 60 Hz integration. Numeric evidence:
`sim/build/games3-beads.csv` and `sim/build/games3/beads-traces.csv`.

| Trace | Captured result |
| --- | --- |
| Left 35°, hold 3 s with hand gyro jitter | Centroid **−172.21 px**, **95.0%** at low rim. True projected force stays `+sin(35°)` g. |
| Reverse right 35°, hold 3 s | Centroid **+172.26 px**, **93.6%** at opposite rim; **519/519** particles cross from x<−120 to x>120. |
| Slow −0.5°→+0.5° sweep | Minimum speed within ±0.1° of level **7.1704 px/s**; no zero-force plateau or velocity cutoff. |
| Fresh flat, 35°, 85°, 120°; 5/10/20 ms samples | Exact clock **1935–1940 ms**, stable for another second; gyro-only yaw releases it. |
| Pickup/play/new measured stand pose | Exact clock **2990 ms** after set-down, without Resume/recalibration. |
| 30 s ±0.35° random tilts and ±0.8°/s gyro jitter | Never settles or accrues sufficient quiet. |
| No gyro, gaps, duplicate duration, NaNs, impact/free fall | No fabricated quiet; missing gyro preserves steering; impact releases before invalid physics. |
| Maximum 640-bead pile, held 35° for 30 s, then observe 5 s | Initial mean speed **0.00112 px/s**; peak mean **0.00130**, peak p95 **0.00300**, worst bead **0.00777 px/s**; maximum displacement **0.00034 px**. Speed magnitudes are averaged, not cancelling vectors. |
| Marble/Orbit/Snake, 10 s held roll, 3 starting poses × 3 cadences | **27 traces** retain neutral byte-for-byte and force **−0.274507 g** after filtering; Marble accelerates, Orbit continues tap-controlled motion, Snake does not synthesize a second turn. |
| Beads runtime flow/reversal/rest/pause/resume/exit | Every sound queue counter remains **zero**. |

Old tests asserting adaptive zero, a half-degree dead zone, resume recentering,
exact instantaneous inertia cutoff, permissive typing vibration, no-gyro clock
and 2–5 flow cues/s were updated because they encoded the reported defects or
contradicted the new behavior. Their useful cadence, invalid-input, collision,
flow, storage and lifecycle coverage is retained or replaced by stronger traces.

## Performance and memory

Captured during the final all-CTest run on this Mac, under `nice -n 10` and four
workers. **Host observations are not ESP32-S3 measurements.**

| Existing/new benchmark | Measured host result |
| --- | --- |
| Maximum 640-particle production solver; 60 warm-up + 1200 timed steps; ±0.5 g x / 0.1 g y | Process CPU **283.993 µs mean / 374 µs p95 / 454 µs max** per 60 Hz step, 2 substeps / 4 constraints. |
| Existing Beads perf test, 519 particles, two logic steps + retained render | **2.554 ms p95 / 6.886 ms max**; **194,696** mean/max flushed pixels. |
| Beads held clock, 60 presentation frames | **0** flushed pixels. |
| Marble stages 1–3, tilt and touch, existing renderer perf | **0.699–1.332 ms p95**; mean **4,113–4,683** flushed pixels; max **11,476**. |
| Orbit / Snake / Pop, existing renderer perf | **1.717 / 0.550 / 0.292 ms p95** respectively. |

The 60 Hz physics budget is 16.667 ms. Presentation retains the existing
30 Hz policy; this work does not establish 60 completed panel frames/s.
`bench_game beads` was **not run on this build**, so board CPU/cadence/headroom
cannot honestly be marked passed from these host numbers.

ESP32 sizes were read from the completed firmware ELF's DWARF (not host ABI):

| Object | Bytes |
| --- | ---: |
| Full model | 15,424 |
| Tilt observation/filter | 140 |
| Existing runtime allocation, including model + published/render copies | 46,696 |
| Existing game view allocation | 58,912 |
| Direct games-owner allocation total | **105,608** of **163,840** budget |

Fluid previous positions overlay clock target storage because those phases
are exclusive; entering clock mode regenerates all sites. This avoids a new
5,120-byte internal-BSS scratch array. The model grows 1,920 bytes, rather than
adding separate target and solver arrays. Hash BSS remains 4,480 bytes. There
are no new heap requests, widgets, frame buffers, per-frame allocations or file
I/O. Actual combined games/LVGL/glass owner peaks still need the board receipt.

The lead must install this build on the **dev board only**, then run the
existing commands (no other agent should flash concurrently):

```sh
scripts/orb-dev.sh heap
scripts/orb-dev.sh bench_game beads 20000
scripts/orb-dev.sh bench_game marble 20000
scripts/orb-dev.sh bench_game orbit 16000
scripts/orb-dev.sh bench_game ring-snake 16000
scripts/orb-dev.sh bench_game pop 16000
scripts/orb-dev.sh heap
scripts/orb-dev.sh shot
```

Record actual completed FPS, frame/draw time, logic/drop/skipped counts, errors,
internal/PSRAM minima and owner rejections. Require no dropped 60 Hz logic steps
and no budget rejections. Recheck real slow pours, sustained left/right tilt,
stand settling/pickup, speaker silence, PWR lock/wake and all accessibility
profiles before release. No stale board receipt is substituted for this build.

## Simulator renders and design gate

Regenerate with `nice -n 10 sh sim/games3-shots.sh`. Both `default` and
`ax3-bold-rt` (AX3 + Bold + Reduce Transparency, motion enabled) contain:

- `sim/build/games3/{mode}/beads-{left,crossing,right,settling,clock}.png`
- `sim/build/games3/{mode}/marble-rim-hint.png`
- `sim/build/games3/{mode}/orbit-switch.png`
- `sim/build/games3/{mode}/snake-controls.png`
- `sim/build/games3/{mode}/{marble,beads,orbit,ring-snake,pop}-paused.png`

These use the production model/view, synthetic accelerometer and gyro inputs.
The pour script deliberately holds hand jitter until 6 s, then makes an abrupt
35° input step; its intermediate/clock frames are visual evidence, not the
settle-timing measurement. The sensor-cadence tests above establish timing.
Fresh unchanged Face/list/card CTest baselines are
`sim/build/shell-{face,agents,approval}-{default,ax3-bold-rt}.png`.

| Gate heading | Changed game surfaces |
| --- | --- |
| Two layers | Plain arena/text content; glass only on existing reading-scene controls; one tinted primary; no stacked glass. |
| Canvas | True black, flat beads/arena content, no new gradient, glow or decorated backdrop. |
| Round-first | Beads stay inside the field/panel; readable time and tutorial fit; Snake controls clear every track. |
| Sizes/type | Shared Inter/Framework7 styles; 72 px steering/Resume targets, 20 px arrow gap; Default and AX3/Bold fit. |
| Colour | Existing game accents, explicit pause/action text, distinct game shapes; no new accent/material system. |
| Approvals/agents | Unchanged; no game auto-action or timed decision dismissal. Supporting card retains explicit choices. |
| Motion | Immediate rim gesture, continuous physical pours, no rest shimmer at trace precision; essential play survives Reduce Motion; existing material/retained/full-redraw checks pass. |
| Navigation | Resume/Done and scored End Round remain visible/reachable; rim release cannot pause/nudge; BOOT/PWR remain physical alternatives. |
| Craft | Readable held clock, truthful fixed-centre copy, clear Snake controls, trail follows Orbit's actual switch path. |

Independent code and visual review **passed the scoped design gate** after
opening the actual Default/AX3 images and six supporting shell renders. The
review checked scratch aliasing, energy dissipation, silent cues, held tilt,
pause, every screenshot phase and all gate headings above. Representative
frames: [left](build/games3/default/beads-left.png),
[right](build/games3/default/beads-right.png),
[clock](build/games3/default/beads-clock.png),
[Snake controls](build/games3/ax3-bold-rt/snake-controls.png).
Real-panel screenshots, physical noise/feel and optical quality remain the
lead's gate.

## Validation

- `nice -n 10 cmake --build sim/build -j4`: passed.
- `nice -n 10 ctest --test-dir sim/build -j4 --output-on-failure`: **907/907
  passed**, **36.23 s**. Includes **26,183,553** game-logic checks,
  **25,929** runtime checks and **1,171,675** motion/rest/perf checks.
- Renderer: **13,922** checks; **1,746** moving and **72** transition
  partial/full frame comparisons plus lifecycle/accessibility fixtures passed.
- Final `nice -n 10 make -C firmware/components/orb_games/tests/host -j4 test`:
  **26,183,553 logic + 25,929 runtime checks passed**, ASan/UBSan.
- `nice -n 10 scripts/idf.sh build`: passed, Ninja **-j 4**. Unsigned
  `firmware/build/orb.bin`: **2,818,048 bytes**, **55%** app-slot free.
  SHA-256: `3ecca99c6f2657032c71138de871e26bad60ff629a3771213503abe427433682`.
  Only compiler warning is pre-existing unused `command_reply_printf_until`
  in untouched `orb_devtools.c`.
- `shellcheck sim/games3-shots.sh` and whitespace check: passed.

Logs: `/tmp/orb-games3-{build-final,ctest,host,idf,idf-final,shots}.log` and
`sim/build/Testing/Temporary/LastTest.log`. Detailed sub-audit notes are in
`GAMES3_OTHER_NOTES.md`, `GAMES3_RUNTIME_NOTES.md` and `GAMES3_TRACES_NOTES.md`.
