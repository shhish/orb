# Offline Play games

Latest player audit: [Games 4 results](../../../sim/GAMES4_REVIEW.md).

## Games 4 current behavior

This section supersedes the historical grid-Snake, bead-density, touch and
30 Hz descriptions below. The long-standing app ID `ring-snake` now displays
**Snake**. It moves continuously in the circular arena. Absolute downhill tilt
sets its target heading, level keeps it straight, and a 3.2 rad/s turn limit
prevents instantaneous reversal. Speed grows gently from 78 to 128 px/s with
length. The centre/up start has one second of collision grace. Food grows the
round chain; rim/self contact loses and 48 segments completes the round.
A quiet rim warns before contact. **Tilt to steer** is the tutorial.

Touch and hold anywhere steers immediately toward the finger. A short tap
opens Pause; releasing a hold never also pauses. No arrows or HUD cover the
body's path. Score and Best remain on results. New Snake best scores use
`high-score-v2` in the existing namespace. The old grid-game record is preserved
and ignored, so incompatible scores cannot become the new best.

Beads uses 256 collision-radius-7 px particles drawn at radius 5 px, with
seeded flat brightness and individual highlights. A 15 px clock lattice keeps
every minute readable without changing particle count. The force is radial:
`1850 * tanh(3.6 * projected_g)` px/s². Near level it gives 116.24 px/s² per
degree; at 10° it gives 1026.18 px/s². Normalized screen-frame gravity reaches
that response without neutral capture or steering-filter attenuation. The
contact solver dissipates energy without stopping valid small-angle force.

**Touch Controls** offers **Hold to pour**: hold toward a side to pour and
release to settle into the time. Sensor failure offers this same alternative.
Tilt mode still requires real gyro-confirmed quiet before forming the clock.
The toy, including all lifecycle and touch interactions, remains silent.

Logic stays fixed at 60 Hz. Presentation follows the panel period (17 ms
fallback), interpolating previous/current positions without changing physics,
scores or RNG. Pauses, results, fresh spawns and mode/stage changes preserve
their exact current state. Host draw timings and the complete per-game play
tests are recorded in Games 4; the lead still owns real-board acceptance.

## Fluid merge presentation

The display gate retains the fluid stream's 16 ms producer, coalescing while a
frame is pending and resampling after admission. Only a dirty refresh's READY
readies the next producer; clean/hidden/held-clock refreshes stay idle. Games 4
previous/current interpolation, fixed 60 Hz rules and all spawn/pause guards
are preserved. The benchmark uses compact immutable positions with equivalent
interpolation guards; a parity oracle compares it with the full Games 4 path.

Continuous Snake has no polar tracks. Orbit retains exact native cached ring
coverage (3,686 bytes with native fallback). Beads retains Games 4's individual
brightness and reflection through native primitives; the former one-tone 7×7
batch cannot be reused for its new 11×11 body without a new pixel oracle.
Sprites/gates are clipped before task allocation. Nearby old/new damage pairs
and bounded particle strips retain the fluid stream's conservative invalidation
behavior, including all actual glass dependencies. Board FPS, memory and cadence
are recorded by build ID in `docs/agent-briefs/wave2/fluid-results.md`.

## Earlier implementation notes

`orb_games` supplies Marble, Beads, Orbit, Snake and Pop through the optional
`orb_apps_register_builtins()` provider. Firmware retains the provider with
`WHOLE_ARCHIVE`; a build without this component retains the original proof
Marble controller. The games are offline. Their Design 1.0 identities are green,
pink, indigo, orange and yellow respectively. Beads is completely silent. Other
games use `orb_sound` only for meaningful events; continuous rolling/flow has no
cue. Missing/busy audio never blocks play and the service respects Silent,
face-down, quiet-hours and capture gates.

Apps → Play hosts `orb_game_play_view`: a centred arena miniature, neighbour
peeks, game name, optional progress, one green tinted Play capsule and five page
dots. Swipe, rim or twist changes the selection; the miniature and Play queue
that game through the shell's main-owned service and start its round immediately.
The picker is the attract screen; games do not add another Play step.
BOOT returns to Apps. Leaving a game preserves the picker selection, including
fully offline navigation.
The view reports launches and leaves lifecycle ownership with the shell/runtime.

`orb_games_progress()` copies a bounded snapshot with no filesystem access on
the UI thread. Registration reads validated storage after mount; leaving a game
refreshes its progress after the save attempt. Marble reports completed stages
and keepsakes, since storage does not contain a resumable current stage. Orbit,
Ring Snake and Pop report a validated saved best. Missing, invalid or unavailable
records and Beads omit the progress line. Failed saves retain the last actual
stored values. Registry-only fixtures omit progress when the runtime is absent.

`orb_game_flow_sim` links the production provider, view/model and shell, with
throwaway in-memory storage. It covers physical swipes, rim turns, twist, both
launch targets, BOOT, app exits, selection retention and offline navigation.
`sim/build/game-flow-{default,ax3-bold-rt}/02-play-marble.png` renders the actual
shell destination with a validated persisted Marble fixture.

## Screens and controls

### Marble Conservatory (Stream G)

Marble now has three short, deterministic tilt puzzles. Roll to the central
flat flower through solid, rotating stone arcs. The 12 px rounded arcs and
their clear gaps share the exact continuous collision geometry. The physical
bezel is the wall, inset 10 px, with no drawn inner circle. The first stage has
rings at radii 94/188 px; the next two have 64/126/188 px. Their outer surface
sits 39 px inside the bezel. A 24 px marble starts in the outer lane and rolls
inward to the 48 px central flower. **Turn the rim to rewind** retraces the marble's own last ten
seconds at 30 samples/second; the gates continue forward. A tangential movement
of 8 px in the outer 40 px starts rewind immediately; lift to stop. Holding
anywhere on the screen remains an alternative. An exhausted history stays still until release. Release zeros
velocity and resolves a moved gate locally, without restoring an old impulse.
Three quiet dots on the top arc fill as stages finish. Tilt play has no floating
controls or time readout. A short screen tap opens Pause; PWR pauses and locks into Always On; a hold's
release never also pauses. First-stage play teaches **Turn rim to rewind** for
three seconds, once per entry, then leaves only the arena and game content.
Faint departed-position dots explain retracing, except under Reduce Motion.
The Play picker presents the still miniature of stage one and **Marble**;
tapping the miniature or **Play** enters the playing arena directly.

The IMU supplies specific force, pointing away from Earth at rest: +x right,
+y up, +z out of the panel. Marble accelerates downhill as `-ax/+ay` in LVGL
pixels. Round entry and explicit restart capture a fresh 300 ms average of the
current viewing pose. A 3D rotation maps that pose to level, with a smooth
small-signal response and low-pass steering. That frame never adapts during play
and survives Pause, Resume and switching input modes. Physics waits only for
the initial calibration.
Missing or failed acceleration switches to touch controls with one brief hint.
Shared glass lighting and parallax retain their existing physical up vector.

Pause presents primary **Resume**, **Garden**, **Done**, and a separate
**Touch Controls** toggle row. BOOT/back or Done exits from Pause;
holding PWR opens the system power sheet. That setting switches to four visible arrows and
disables IMU steering, providing a non-motion alternative. Reduce Motion freezes
decorative light and removes rewind trails; essential puzzle movement remains.
The goal blooms on a 350 ms spring, then its keepsake lifts toward the bottom
arc, finishing at 850 ms. Reduce Motion uses a stationary 150 ms cross-fade.
**Planted** then stays visible until **Next stage** or **Finish**. Each completion
adds an original flat stone or blossom to **Garden**, reachable from Pause and
results. The garden grows along a spiral to radius 145 px (about 70% of the
panel radius including its petals), with deterministic size and rotation
variation, filled warm pebbles and a quiet count. It is still at rest, bounded
to twelve keepsakes; a full garden replaces its oldest keepsake when another stage finishes. Aborting from Pause
leaves the app through **End Round**, preserving the earned garden.

The garden's `marble/keepsakes` record is exactly 84 bytes: `OGK1`, version 1,
count, maximum completed stage, reserved zero, twelve six-byte little-endian
kind/stage/x/y entries, then a four-byte checksum. Decode validates version,
length, checksum, reserved bytes, kinds, stages, coordinates and unused slots.
It is read at entry and atomically saved through `orb_fs` once at stage
completion, outside LVGL/publication locks. Failed writes preserve the previous
file, show **Save unavailable**, and retry on the next completion or exit.
Nothing writes a path sample to flash. Exit/re-entry restores the garden;
current-stage position is intentionally temporary.

Physics remains fixed at 60 Hz, independent of the 30 Hz presentation timer.
The 300 eight-byte path samples occupy 2,400 bytes inside a 2,648-byte model.
Publication copies the small render prefix (extended to cover the other game
unions), never the full history. No game-owned allocations occur in frame or
step code. Every gate arc, marble and trail point has a stable sprite slot and
separate old/new damage; garden/pause/result scenes become still after material
settles. Saturated steering input cannot lose a rewind release; screen-off and
pause discard held input and accumulated time. Held PWR keeps the OS escape.

Run `sim/conservatory-shots.sh` for Default, AX3+Bold+Reduce Transparency and
Reduce Motion PNGs, including hold/release, planting/next-stage and garden
navigation sequences. The current gate and remaining board acceptance steps
are in [CONSERVATORY_REVIEW.md](CONSERVATORY_REVIEW.md).

### Other games and historical score presentation

Orbit, Ring Snake and Pop start playing immediately when launched from the
picker. Their visible score/pause target opens Resume, End Round and Close; the OS-owned
held-PWR exit and screen-off/wake pause remain intact. Results retain
**Play again** to start another round and **Done** to return to the picker.
Error text never replaces Paused or Resume.

The logical arena is centred at (233,233), with no display/input rescaling.
Marble uses radius 223 and circular control clearances; the other games retain
radius 215. Pop reserves its HUD and visible alternate-control regions in the
model. Their targets spawn and move outside those regions. Snake has a continuous
circular arena with no in-play score disc, track or steering arrows to conceal
the moving head, body or food. Orbit retains its own visible orbit tracks.

- Marble's former endless collectible loop is replaced by the Conservatory
  stages above. Its four straight arrows remain the touch alternative.
- Orbit says **Tap to change orbit** and **Change Orbit**, with capsule hazards,
  a white player, explicit lives and one bounded collision response paired with
  the runtime's existing loss/stop cue.
- Snake says **Tilt to steer** for its first second. Absolute downhill tilt
  selects its heading; level preserves its current heading. Its bounded
  3.2 rad/s turn rate and 78→128 px/s earned speed apply equally to tilt and
  touch. Hold anywhere to steer toward the finger; release goes straight in
  Touch Controls. A short stationary field tap opens Pause; a drag never also
  pauses. The centre/up start has 60 fixed steps of grace. White eyed head,
  green round body and orange pellets remain distinct by shape. Pellets grow
  the body and add 10 points; 48 segments/420 points completes the arena.
  Swept visible-disc self collision or the r215 rim ends the round. A quiet
  warning ring appears while the head is within 55 px of the rim. Best score
  uses `ring-snake/high-score-v2`; the old polar-grid score remains untouched
  and is intentionally excluded from the new game's record.
- Pop begins with three separated, slowly drifting bubbles, substantial rims,
  flat translucent interiors and curved highlights. Direct bubble taps remain
  primary; **Pop Nearest** selects the active bubble nearest the field centre.
  **Combo ×N**, rolling score, burst ring and droplets answer each successful
  pop without covering the next target.

All content stays on true black. Marble uses at most three glass controls,
with one blue primary action; green is reserved for the goal and success.
The other games retain Close and the single tinted primary on reading
surfaces. Reduced Motion retains stationary fading feedback rings and immediate score changes; essential playable motion continues. The
principal AX3/Bold/Reduce Transparency evidence keeps motion enabled.

### Beads and event feedback (2026-10-28)

Beads uses normalized true gravity in the already calibrated board axis frame;
it has no captured or adaptive play neutral. Flat face-up produces no force,
and reversing tilt pours the pile across the middle. The sub-stepped spatial
hash solver keeps mild inertia and dissipates collisions without a sound cue.
Only 1.2 s of sensor-time desk quiet, confirmed by valid low-noise gyro samples, can form the time; pause and catch-up cannot invent
quiet samples. Pickup releases the clock. The signed `GAME_BEAD` and `GAME_ROLL`
palette samples remain unchanged, unused by play.

Other games announce entry and pause-sheet opening with Open, Resume and exit
with Close. Collect/pop/switch/hit events remain bounded, with no continuous
movement cues. Losing Snake or Orbit uses Failed; Marble completion and Pop's
timed completion use Done. Explicit End Round uses neutral Close. The sound
service still owns Silent, capture and Always On suppression.

## Timing, damage and memory

`orb_game_logic.c` is clock/render/storage independent. Each step is 1/60 second.
Main accumulates input time with a six-step catch-up cap and publishes a small
immutable model copy through a short critical section. Sleep clears accumulated
time and queued actions, so wake cannot advance a stale round. UI callbacks only
queue bounded actions. Close remains available while paused.

The UI owns its view and timer, targeting 30 Hz (twice the last measured panel
period, rounded to milliseconds; 33 ms fallback). This follows the exposed panel
period, not TE phase; no phase-lock claim is made. Logic is independent of that
presentation cadence. Orbit uses the same conservative 30 Hz target until a
board benchmark justifies 60 Hz.

The largest model state is 15,424 bytes (Beads). Its fluid integration scratch
shares storage with clock targets; the two phases never run together. Runtime,
published copies, action queue and both sprite
arrays are allocated once at entry in PSRAM; no private task, image framebuffer
or internal DMA buffer is added. A bounded 16-slot mailbox holds at most 15
pending actions. Game frame/step code allocates nothing and does no filesystem
I/O; native LVGL still owns its normal draw-task machinery. Exit first waits for
the UI lock, removes the timer and view, then saves and frees the runtime.

The view uses clipped LVGL discs, continuous rounded arcs, pebbles, capsules
and two-triangle diamond facets.
Sprites retain slot identity; pixel-identical slots do not invalidate. Changed
non-gate slots invalidate separate old/new bounds, including an AA allowance.
Rotating gates retain their unchanged interiors and damage only tiled endpoint
sweeps; gate bounds include LVGL's integer-degree angle quantization.
LVGL merges/clips these and repaints exposed black plus any static tray content.
Only score/life/result changes dirty their own labels. A changed sprite under
visible glass widens damage to that control's horizontal backdrop band; scene
materialisation uses the same bounded bands for 650 ms. This keeps blur/dither
sampling identical to a full redraw. Marble
keeps two small glass controls in play; damage expands their backdrop bands
only when a sprite intersects them. Other games use only sprite/HUD damage. Full-screen invalidation
is restricted to entry/exit or external accessibility/scene changes, never a
moving-frame implementation. No raster cache or bitmap payload is needed.

`orb_game_view_stats()` records submitted damage rectangle areas, including
sprite-triggered glass backdrop expansion (overlap can be counted more than
once), changed update counts and microsecond update CPU/wall elapsed.
Scene materialisation repaint bands are separate; none of these sums is the
actual display transfer area. The simulator renderer test separately captures
band-flush pixels and host update/render timings for every Marble stage in
tilt and touch modes. `bench_game` separately reports
completed DMA frames and transferred RGB565 pixels from display counters.

## Scores

Each namespace stores a 12-byte `high-score` record: `OGS1`, little-endian uint32
score, score XOR `0x4f524231`. The existing `orb_fs` API supplies atomic LittleFS
replace/fsync/quota behavior. Read on entry; write only an improved best on exit,
after releasing LVGL. Restart retains the run's best. A missing record means
zero; malformed/truncated/checksum-invalid records show a score warning. A failed
write preserves the previous file, logs the error and exposes it through
`orb_games_last_save_error()` for the shell owner. Benchmarks never write scores.
An interrupted run before exit can lose its unsaved improvement.

## Verification and board commands

```sh
nice make -C firmware/components/orb_games/tests/host -j4 test
nice make -C firmware/components/orb_apps/tests/host -j4 test
cmake -S sim -B sim/build -G Ninja
nice cmake --build sim/build -j4
nice ctest --test-dir sim/build --output-on-failure -j4
nice scripts/idf.sh build
~/.venvs/orb-hub/bin/python -m pytest -q tests/test_benchmark_commands.py tests/test_logd.py tests/test_transport_review.py
```

The sanitized host tests cover deterministic game rules, finite motion and
bounds, collisions, scoring, capped catch-up, PWR while paused, action saturation,
allocation-free runtime ticks, namespace isolation, corrupt records, save errors,
partial entry failures and benchmark ownership. Renderer tests compare 360 moving frames, 72 transition frames and additional
lifecycle fixtures byte for byte, assert bounded damage and unchanged idle
pixels, exercise real pointer controls through repeated rounds and check large
AX3/Bold scores. Transitions include Reduce Motion changes mid-materialisation;
new best baselines, outgoing hit targets and Pause → End Round are covered.

Interactive and headless previews use the same production logic and renderer:

```sh
sim/build/orb_games_sim --game marble
sim/build/orb_games_sim --game orbit --headless --scene play --tap 500=233,300 --shot 1500=/tmp/orbit.png
sim/build/orb_games_sim --game ring-snake --scene play --headless --tilt 200=15,0 --shot 1500=/tmp/snake.png
sim/build/orb_games_sim --game pop --scene play --headless --text-size ax3 --bold --reduce-transparency --reduce-motion --shot 1500=/tmp/pop-ax3.png
```

`--scene start|play|paused|end` selects a deterministic fixture; `--reward MS`
triggers a real model reward, and `--pause MS` / `--end MS` support sequences.
`--best` and `--score` seed isolated counters. `sim/games-shots.sh` generates the
196-PNG start/play/pause/end, reward and controls matrix at Default, AX3, opaque
and AX3/Bold/opaque/Reduce Motion under `sim/build/games-polish/`.

Mouse is touch; arrow keys tilt the fake board. See [the UX-fix evidence](../../../docs/screens/ux-fixes/gameux/README.md)
for current screenshots, reproduction commands and the independent design gate. The simulator cannot
establish ESP32 frame time, live internal heap headroom, SPI timing or panel quality.

Only the lead/board owner should install the build and run these, from the shell
with no active game. Do not use the second board:

```sh
scripts/orb-dev.sh heap
scripts/orb-dev.sh bench_game beads 16000
scripts/orb-dev.sh bench_game marble 16000
scripts/orb-dev.sh bench_game orbit 16000
scripts/orb-dev.sh bench_game ring-snake 16000
scripts/orb-dev.sh bench_game pop 16000
scripts/orb-dev.sh heap
```

Bench duration is 1000–60000 ms. The temporary scene uses the real model/view,
deterministic inputs and bounded restarts, preserves the screen/settings, and
uses an atomic reservation against concurrent app entry. Its receipt reports
actual completed-display FPS, frame/draw ms, physical dirty pixels/percentage,
logic steps/drops, skipped slots, errors, cold cost and internal heap minima.
`te_phase_locked=0` distinguishes sampled pacing from measured TE phase.
No board was flashed or measured by this stream; steady 30 fps and real heap
headroom require these board receipts before acceptance.

For Conservatory, prefer `scripts/orb-dev.sh bench_game marble 20000` so the
fixture visits all three stages. Its additional counters are
`marble_gate_steps`, `marble_rewind_steps`, `marble_stages` and
`marble_history_peak`. The fixture uses the real goal rules to advance stages;
it never writes a keepsake or score. All four counters must be nonzero, in
addition to steady completed-panel 30 fps, bounded damage, no dropped logic
steps and no transfer errors. Build locally with
`IDF_PY_BUILD_JOBS=4 scripts/idf.sh build`; use `-j4` for CMake/CTest/host Make.
