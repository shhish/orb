# Marble — owner motion and control fixes

Implemented in the existing `orb/w2-marble3` worktree. No git writes or board
access. The owner screenshots in `work/owner-review/marble-{start,play}.png`
were inspected. All build commands use four jobs; no simulator build contents
were deleted. This review supersedes the round-three interaction descriptions.

## Physical sign analysis

All readings below are accelerometer **specific force**, not downhill gravity.
At rest the support force points away from Earth. The board mapping stays
unchanged so the glass still receives its visually tuned up vector.

| Hop | Evidence and convention |
| --- | --- |
| QMI8658 raw | `orb_board/qmi8658.c:68` selects little-endian registers and ±4 g; `:82` reads AX/AY/AZ from `0x35`. |
| Decode | `orb_board/orb_imu.c:3` casts each little-endian pair to signed int16 and divides by positive 8192 LSB/g; no sign inversion. |
| Mounting | `orb_board/orb_imu.c:10` maps `(-sensor_x,-sensor_y,+sensor_z)` to screen axes: +x right, +y up, +z out. Face up reads +z, upright +y. |
| Board wrapper | `orb_board/board_ws175c.c:513` returns that same mapped vector. |
| Main | `firmware/main/main.c:540` reads it; `:560` now preserves all three components through app polling. There is no gameplay gravity filter or negation in main. |
| Registry | `orb_apps/orb_apps.c:135` preserves finite x/y/z, invalidating motion if any component is nonfinite. |
| Game runtime | `orb_games/orb_games.c:349` rotates and shapes the measured vector in a game-only neutral frame; it preserves specific-force signs. |
| Marble | `orb_games/orb_game_logic.c:539` now integrates `vx -= ax*480*dt`, `vy += ay*480*dt`, accounting for LVGL's +y-down pixels. The old `+ax/-ay` rolled uphill on both axes. |
| Ring Snake | `orb_games/orb_game_logic.c:660` now queues Right for negative ax and Left for positive ax. Orbit and Pop use motion only for decorative lighting. |
| Legacy proof | `orb_apps/orb_marble.c:45` also moves `-ax/+ay`; touch nudges keep their existing screen direction. |
| Glass/parallax | Main's `orb_ui_set_gravity` call and `orb_ui/orb_motion.c` / `orb_motion_math.c` are unchanged. Upright +y still lights from 270° (above); parallax already used downhill `-x/+y`. |

Synthetic raw-to-screen tests exercise face up, upright, right-side-down and
bottom-side-down poses through both production decode and mapping. Physics
tests label the four cardinal accelerometer poses explicitly: right-side-down
reads ax < 0 and accelerates right; bottom-side-down reads ay > 0 and accelerates
down. No new board measurements are claimed.

## Neutral and input behavior

- Play, Resume, screen wake and switching back to tilt capture a fresh 300 ms
  time-weighted average. Duplicate timestamps contribute nothing; sampling gaps
  over 100 ms restart an unfinished average. The fixed-step debt is cleared
  while calibrating, so neither marble nor gates advance during capture.
- A Rodrigues rotation maps the averaged 3D neutral to +z. Tests cover viewing
  angles 20°, 35° and 50°, cross-axis coupling, averaging, recentering and reset.
  A radial 2.5° dead zone, gentle response curve and 10 Hz one-pole low-pass use
  actual input sample time; physics remains fixed at 60 Hz.
- Unavailable, failed, nonfinite or near-zero acceleration enables Touch
  Controls automatically, with a three-second hint once per app entry. Touch
  steering ignores IMU input.
- Long contact anywhere on the screen, rim or touch arrows starts continuous
  rewind; release or lost contact stops it. Long-contact release neither pauses
  nor issues an arrow nudge. The bounded ten-second history and forward-moving
  gates remain unchanged.
- A short screen tap or PWR opens Pause. PWR is idempotent while paused.
  Pause exposes Resume, Garden, End Round and Touch Controls. Garden returns
  to Pause with a concise Back action. End Round, BOOT or shell Back exits from
  Pause through the main-owned registry, preserving saves. Held PWR remains
  the OS escape. Game-owned BOOT input does not start voice recording.
- Normal tilt play has no X, pause or rewind control. Only the arena, marble,
  gates, flower and three stage dots remain after the first three seconds of
  the first-stage hint. Rewind feedback is confined to faint departed-position
  dots; Reduce Motion omits those. Optional touch steering displays arrows.
- Start uses a frozen miniature of real stage-one geometry and spawn, followed
  by Marble, small secondary Garden and primary Play. The miniature produces
  no idle redraw after settling. The stray hero flower and subtitle are gone.

## Reproduce and validation

```sh
make -C firmware/components/orb_games/tests/host test -j4
cmake --build sim/build -j4
ctest --test-dir sim/build -j4 --timeout 60 --output-on-failure
sim/conservatory-shots.sh sim/build/conservatory-owner
shellcheck sim/conservatory-shots.sh
IDF_PY_BUILD_JOBS=4 scripts/idf.sh build
```

- ASan/UBSan host: **4,383,691 logic checks + 21,161 runtime checks** passed.
  Model size 2,648 bytes; allocation-free frame/step assertions pass.
  Log: `/tmp/orb-marble-owner-host.log`.
- Simulator build and full CTest: **642/642 passed**.
  Logs: `/tmp/orb-marble-owner-sim-build-final.log` and
  `/tmp/orb-marble-owner-ctest-final.log`.
- Renderer: **5,156 checks** passed; retained/full redraw equality covers 1,596 moving frames and 72
  transition frames, plus static hero, actual Play-label framebuffer ink,
  screen/rim/arrow hold semantics and AX3 paused-Garden single-line regression.
- Additional registry host: **91 checks** passed; board physical-axis tests and
  unchanged glass-light/parallax host tests passed. ShellCheck and whitespace
  verification passed.
- The first firmware attempt hit the sandbox's process-inspection restriction
  in ESP-IDF's component manager. The same local build was retried with the
  needed access, still four jobs, without flashing or signing.
  **Firmware build passed**, log `/tmp/orb-marble-owner-idf-final.log` confirms
  `ninja -j 4 all`; unsigned output `firmware/build/orb.bin` is 2,555,904 bytes.
  The only compiler warning is the existing unused `command_reply_printf_until`
  in untouched `orb_devtools.c:336`.

## Screenshot ledger and design gate

There are **150 PNGs**, 50 per mode, under
`sim/build/conservatory-owner/{default,ax3-bold-rt,reduce-motion}/`.
AX3 includes Bold Text and Reduce Transparency; Reduce Motion retains tilt
unless the explicit controls fixture selects arrows. Production view/model
render against fake input, not a physical board. The completion fixture invokes
the actual goal rule from a placed goal position; it is not a human playthrough.

The ledger below applies to all three mode paths. Each item covers the headings
in `docs/agent-briefs/wave2/design-gate.md`.

### Entry: `{mode}/start.png`

- Two layers: the miniature is plain content; only Play uses glass.
- Canvas: true black, flat original arena art, no gradient or glow.
- Round-first: centered circular miniature, chord-safe title, primary at six.
- Sizes: 72 px Play, 46 px secondary target, shared Inter styles scale to AX3.
- Colour: neutral title/Garden, green destination, one blue primary.
- Approvals/agents: no external state, timer or loading metaphor.
- Motion: miniature is frozen and idle-render assertions pass in default/AX3.
- Gesture alternatives: explicit Play and Garden; PWR remains a reliable exit.
- Craft: one integrated hero, one title, no floating decorative flower/subtitle.

### Play: `{mode}/play.png`, `play-glyph.png`, `stage-{2,3}.png`

- Two layers: arena content only; all game chrome is hidden after entry settles.
- Canvas: black to the bezel, flat continuous stone arcs, no glow/gradient.
- Round-first: arena fills the round panel; three dots sit on the top arc.
- Sizes: first-play Inter hint fits its chord at default and AX3.
- Colour: white marble, neutral gates, green goal; stage dots differ by fill.
- Approvals/agents: no invented timer, score pressure or spinner.
- Motion: fixed physics and bounded damage; Reduce Motion freezes decoration.
- Gesture alternatives: owner-directed Pause sheet and Touch Controls provide
  the visible actions; PWR opens Pause. See the explicit design exception below.
- Craft: no X, pause glyph or rewind button obscures the arena.

### Touch alternative: `{mode}/controls.png`

- Two layers: arrows are ordinary controls; arena remains plain content.
- Canvas: true black with no added control platters, gradients or glow.
- Round-first: compass arrow centers match physical hit/collision geometry.
- Sizes: four 72 px targets and readable default/AX3 arrow glyphs.
- Colour: neutral arrows, green goal; input state also has toggle-thumb position.
- Approvals/agents: local play only, no external state.
- Motion: steering ignores IMU; essential movement survives Reduce Motion.
- Gesture alternatives: arrows replace tilt; holding an arrow also rewinds.
- Craft: optional controls are enabled explicitly or by genuine sensor failure.

### Pause/Garden: `{mode}/paused.png`, `paused-toggle.png`, `paused-save-error.png`, `pause/04-garden.png`

- Two layers: plain reading content, at most three glass controls, no panel glass.
- Canvas: black with no gradient/glow artwork.
- Round-first: title/chords, diagonal Close and lower primary remain within rim.
- Sizes: 72 px glass controls; AX3 toggle and errors clear neighboring surfaces.
  Paused-Garden Back is one line inside its capsule.
- Colour: one blue Resume, neutral Garden/toggle, red End Round text.
- Approvals/agents: explicit End Round and errors describe real local state.
- Motion: play stops; stale holds cancel; shared material honors RM/RT.
- Gesture alternatives: visible Resume, Garden, End Round and Touch Controls;
  BOOT/Back exit and held PWR remain reliable.
- Craft: concise labels and a toggle whose thumb expresses state without colour.

### Rewind: `{mode}/rewind/00-forward.png` through `07-forward.png`

- Two layers: no new surface; faint departed points are content feedback.
- Canvas: black, discrete flat dots, no luminous trail.
- Round-first: arbitrary rim contact leaves the whole arena visible.
- Sizes: only the temporary shared Inter hint adds text.
- Colour: neutral trail and white marble preserve destination meaning.
- Approvals/agents: marble retraces actual history while gates continue forward.
- Motion: hold/retrace/release captured; RM removes ghosts; release stops rewind.
- Gesture alternatives: hold is taught once; Pause offers accessible touch input.
- Craft: no Rewinding label or floating button; release never opens Pause.

### Pause flow: `{mode}/pause/00-play.png` through `04-garden.png`

- Two layers: play content transitions to the same shared Pause controls.
- Canvas: true black, no transient decorative background.
- Round-first: all intermediate labels and controls clear the circular bezel.
- Sizes: Default/AX3/RM controls retain physical target dimensions.
- Colour: the primary tint stays unique throughout material transitions.
- Approvals/agents: Pause, Resume and Garden reflect actual input actions.
- Motion: real short taps pause and resume, with fresh neutral on resume.
- Gesture alternatives: the sheet visibly exposes the actions in every mode.
- Craft: Garden returns to the paused state; no accidental resume or text wrap.

Existing receipts, garden/full/empty/error, bloom, next-stage and garden-entry
sequences are also regenerated by the script. Unchanged Face/list/card gate
fixtures are regenerated by CTest under `sim/build/gate-{face,list,card}-*.png`.
Independent review caught the AX3 Back-to-pause overflow; the shortened Back
label and an explicit single-line regression cleared it. Final independent
rereview found no remaining material visual issue in the sampled sets.

The owner explicitly requested chrome-free play and named the Pause sheet as
the visible alternative. That supersedes the gate's literal demand for a
separate visible button for each gesture; there is intentionally no rewind
button. HIG §7 permits games to hide bars while retaining a reliable way back.
No other HIG exception was needed.

The image-preview client occasionally omits regions despite valid PNG pixels.
Direct RGB decoding and framebuffer assertions confirm the Play label and AX3
miniature; original-size rereview confirms the surfaces. No production changes
were made for that preview artifact. These results do not establish physical
tilt comfort, panel FPS, touch comfort or on-panel contrast; board access was
excluded from this task.

## Changed-file ledger

Paths and first changed line are repository-relative; later lines in the same
files contain the implementation/tests described above.

<!-- changed-files -->
- `firmware/components/orb_apps/README.md:23`
- `firmware/components/orb_apps/include/orb_apps.h:20`
- `firmware/components/orb_apps/orb_apps.c:114`
- `firmware/components/orb_apps/orb_marble.c:45`
- `firmware/components/orb_apps/tests/host/test_apps.c:68`
- `firmware/components/orb_board/README.md:35`
- `firmware/components/orb_board/include/orb_board.h:95`
- `firmware/components/orb_board/orb_imu.c:12`
- `firmware/components/orb_board/orb_imu.h:18`
- `firmware/components/orb_board/tests/host/test_imu.c:45`
- `firmware/components/orb_games/CONSERVATORY_REVIEW.md:3`
- `firmware/components/orb_games/README.md:40`
- `firmware/components/orb_games/include/orb_game_logic.h:56`
- `firmware/components/orb_games/include/orb_games.h:14`
- `firmware/components/orb_games/orb_game_logic.c:26`
- `firmware/components/orb_games/orb_game_view.c:37`
- `firmware/components/orb_games/orb_games.c:43`
- `firmware/components/orb_games/tests/host/test_game_logic.c:67`
- `firmware/components/orb_games/tests/host/test_game_runtime.c:188`
- `firmware/components/orb_shell/orb_shell.c:18`
- `firmware/components/orb_shell/tests/test_services.c:68`
- `firmware/components/orb_time_app/tests/host/test_time.c:236`
- `firmware/main/main.c:560`
- `sim/conservatory-shots.sh:11`
- `sim/game_flow_main.c:97`
- `sim/games_main.c:2`
- `sim/shell_main.c:502`
- `sim/tests/test_games_render.c:455`
- `sim/time_shell_main.c:53`
- `firmware/components/orb_games/OWNER_FIXES_REVIEW.md:1` (current report)
