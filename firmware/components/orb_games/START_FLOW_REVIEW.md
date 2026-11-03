# Immediate play entry review

The shell's Play picker is the attract screen. Both its Play capsule and
miniature launch an active round. `enter_game()` publishes `started = true`
before creating/updating the view, resets the initial tilt calibration and emits
one start cue after successful initialization. No launched game visits
`SCENE_START`. Explicit unstarted model/renderer fixtures still cover that
standalone fallback, rather than describing the picker lifecycle.

Marble already exposes Garden on Pause. Returning from Garden preserves Pause;
Resume continues the same round. PWR/BOOT, screen-off/wake and held-PWR routing
are unchanged. Scored game results retain Play again and Done. The real Pop
integration flow exercises both actions and verifies Done restores the selected
Pop picker item.

## Validation

- `nice -n 10 scripts/idf.sh build`: passed; Ninja used `-j 4`.
- `nice -n 10 sh -c 'cmake --build sim/build -j4 && ctest --test-dir sim/build -j4 --output-on-failure'`:
  passed, **717/717**, 22.90 seconds for the final test run.
- Updated `game_flow_default`, `game_flow_ax3_bold_rt`, `emu_flow`,
  `buttons_shell_default`, `buttons_shell_ax3_bold_rt`, `game_runtime` and
  `games_render`. Runtime tests cover all five games at entry and immediate PWR
  pause, with Resume and BOOT exit. Simulator flows cover both picker launch
  targets and offline selection retention.
- The first suite found one render-fixture issue: a formerly inert unstarted
  Snake probe now steered into a boundary. Neutral input verifies immediate
  ticking without changing the existing damage bound. The targeted test and
  subsequent full suite passed.

## Screenshot evidence

Fresh production-controller captures exist in both
`sim/build/game-flow-default/` and `sim/build/game-flow-ax3-bold-rt/`.
The latter uses AX3, Bold Text and Reduce Transparency with motion enabled.

| Surface | PNG in each directory |
| --- | --- |
| Picker / Marble | `02-play-marble.png` |
| Immediate Marble arena | `04-real-marble-playing.png` |
| All five immediate arenas | `15-marble-boot-playing.png`, `15-beads-boot-playing.png`, `15-orbit-boot-playing.png`, `15-ring-snake-boot-playing.png`, `15-pop-boot-playing.png` |
| Marble Pause | `05-real-marble-paused-close.png` |
| Garden reached from Pause | `20-paused-garden.png` |
| Pop result | `24-pop-round-over.png` |
| Play again / active round | `25-pop-play-again-playing.png` |
| Done / selected Pop picker | `26-pop-done-return-picker.png` |

The implementation and an independent agent inspected these captures against
the exported Design 1.0 `Play.png`, `Marble.png` and `GameOver.png` frames.

| Design-gate heading | Picker and return | Playing arenas and replay | Pause and Garden | Scored result |
| --- | --- | --- | --- | --- |
| Two layers | Plain miniature/content, one Play glass action. | Content only; no extra start controls. | Plain reading content, glass only on existing actions. | Plain score, existing replay control. |
| Canvas | True black, no gradient/glow. | True black, flat game artwork. | True black, no added treatment. | True black, no gradient/glow. |
| Round-first | Existing chord-safe labels and rim dots. | Full round arena immediately visible. | Title, Garden and actions fit the circle. | Score and both actions fit the circle. |
| Sizes/type | Inter at Default and AX3/Bold fits. | Existing content geometry retained. | Enlarged labels and Back fit without clipping. | Enlarged Play again and Done remain legible. |
| Colour | Existing meaningful selection/Play colour. | Existing shaped game-state colours; no added accents. | Existing palette/tint retained. | Existing single replay accent retained. |
| Approvals/agents | No approval/agent behavior changed. | No approval/agent behavior changed. | No approval/agent behavior changed. | No automatic action or dismissal. |
| Motion/accessibility | Existing picker motion and opaque mode retained. | Immediate play verified; physics/damage bounds pass. | Pause freezes play; Garden returns paused; opaque variants fit. | Receipt persists; replay requires a tap. |
| Navigation alternatives | Miniature and Play both launch; BOOT returns. | PWR Pause and held-PWR escape verified. | Garden, Back, Resume and BOOT exit verified. | Real pointer replay/Done return verified. |
| Craft | One clear launch decision. | Arena matches the Marble reference without a second title/button. | Garden remains discoverable on Pause. | Result/replay/return match the existing design. |

Required unchanged shell baselines were also inspected:
`sim/build/shell-{face,agents,approval}-{default,ax3,ax3-bold-rt}.png`.
They retain the black canvas, separate content/control layers, round-safe text,
Inter/Framework7, enlarged/opaque modes and neutral explicit Deny/Allow actions.
These baseline captures are not an unconditional design-gate pass: the AX3 Face
capture omits the Apps glyph/button, and AX3/Bold/RT omits the battery outline
while retaining its percentage. No shell layout or rendering code changed here.

The changed game-entry flow passes the applicable simulator design checks.
Host screenshots establish rendered layout and behavior, not panel colour,
physical tilt feel or board frame rate. The lead's real-board screenshot review
remains the release gate. No board was flashed and no commit was made.
