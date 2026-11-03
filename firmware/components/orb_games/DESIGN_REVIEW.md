# Current UX review

Stream G replaces Marble's collectible loop with Marble Conservatory.
Its current screenshots, design gate, tests and lead-only board checklist are
in [CONSERVATORY_REVIEW.md](CONSERVATORY_REVIEW.md). The older all-game evidence
below remains applicable to Orbit, Ring Snake and Pop.

The final `orb/w2-gameux` review, current screenshots and validation are in
[the game UX evidence ledger](../../../docs/screens/ux-fixes/gameux/README.md)
and [independent design review](../../../docs/screens/ux-fixes/gameux/REVIEW.md).
They supersede the historical polish evidence below. F16 remains partial for
the documented shell-owned offline Apps/Play routing restriction.

---

# Games polish design evidence — 2026-10-27

Branch `orb/w2-gamespolish`; changes are uncommitted. The standard is
`docs/hig.md` and `docs/agent-briefs/wave2/design-gate.md`. The independent
simulator reviewer records the final screenshot inspection and test receipts in
[sim/GAMES_POLISH_REVIEW.md](../../../sim/GAMES_POLISH_REVIEW.md).

## Captures and gate

Run `sim/games-shots.sh sim/build/games-polish`. Every game (`marble`, `orbit`,
`ring-snake`, `pop`) has `start`, `play`, `paused` and `end` PNGs under each of
`default`, `ax3`, `opaque`, and `ax3-bold-rt-rm`. Each game also has an eight-frame
`<id>-sequence/` showing attract, actual play/input, a model-generated reward,
and the result. Each mode has `marble-controls-play.png` for the arrow setting.
The end fixture deliberately marks a result; the interactive Pause → End Round
path is separately driven by real pointer input in the renderer tests.

| Surface | PNGs, relative to `sim/build/games-polish/` | Gate evidence |
| --- | --- | --- |
| Marble | `{default,ax3,opaque,ax3-bold-rt-rm}/marble-{start,play,paused,end}.png` | Full circular field, moving flat marble reflection, diamond gems, bounded eight-particle catches; tilt first with optional arrows. |
| Orbit | `{default,ax3,opaque,ax3-bold-rt-rm}/orbit-{start,play,paused,end}.png` | Crisp green tracks, capsule hazards, white dot and short switch trail; the Switch alternative clears both lanes. |
| Ring Snake | `{default,ax3,opaque,ax3-bold-rt-rm}/ring-snake-{start,play,paused,end}.png` | Interpolated polar segments, eyes and tapered-opacity tail; distinct faceted food and explicit steering alternatives. |
| Pop | `{default,ax3,opaque,ax3-bold-rt-rm}/pop-{start,play,paused,end}.png` | Antialiased outlined bubbles with inset reflections; numeric combo/lives and expanding-ring/six-droplet catches. |

Each surface is checked against every gate heading:

- **Two layers:** content paths and text on black; glass only on Close, Marble's
  control toggle and the one primary action. At most three glass shapes and one
  tint; no glass content or stacked glass.
- **Canvas:** true black, no gradients or glows. The brief's shaded bitmap/soft
  shadow is replaced with a few flat marble facets and a moving inset reflection
  because the design gate bans gradients/glows everywhere.
- **Round-first:** field centre `(233,233)`, radius 215 (18 px bezel margin).
  Diagonal toolbar circles are `(101,101)` and `(365,101)`. The 220×72 primary
  at `(233,385)` fits the circular safe area. Summary lines and the narrow top
  HUD fit their chords. Glass toolbar controls retract for play.
- **Sizes:** normal controls are 72 px; score/pause is 128×72. Inter uses
  `orb_text_apply`, including AX3 and Bold. Large compact scores and combo 99
  have one-line layout assertions.
- **Colour:** system palette only. Hazards are capsules, player a white disc,
  food a diamond and Snake head has eyes. Lives, combo, New Best and score-save
  failure have text, independent of hue. Tinted actions use black emphasized type.
- **Approvals/agents:** not applicable; games contain neither approvals nor
  agent work, no spinners and no timed dismissal of results.
- **Motion/accessibility:** bounded attract/reward animation, critically damped
  score roll, shared material/press springs. Reduce Motion suppresses decorative
  movement and uses the shared 150 ms material cross-fade. Essential game motion
  remains. Reduce Transparency and Bold/AX3 use shared settings. Increase
  Contrast still has no shared getter for games; that existing shell-owned
  integration is not claimed as implemented here.
- **Gesture alternatives:** Marble's controller toggle exposes four arrows;
  Orbit Switch, Snake's two arrows and Pop's urgent-bubble action duplicate
  gesture input. The top pause glyph opens Resume/End Round/Close. Holding PWR
  retains the OS-owned escape. Results remain until Play Again or Close.
- **Feels Apple-made:** sparse chrome, round fields, system typography and
  purposeful small rewards. Independent review is linked above; physical-panel
  optical quality remains the board owner's gate.

## Retained drawing and limits

Sprites retain stable slots. Damage covers old/new antialiased bounds; visual
and logic state use fixed arrays with no game-owned per-frame allocation.
Moving models and normal/reduced/mid-toggle scene transitions are compared to
full redraws byte-for-byte. A sprite under visible glass expands damage to that
control's backdrop band. Only these bands and the brief material transition
widen horizontally, keeping the shared blur/dither origin deterministic; steady
play has no glass and never takes this path. No tolerance masks are used.

The design suite also renders unchanged Face/list/card fixtures at Default,
AX3, Reduce Transparency and combinations under `sim/build/gate-*.png`.

No board was flashed or measured. Simulator time is not device FPS or heap
headroom. The lead must run the README's `bench_game` commands on the dev board
before claiming steady 30 fps, physical dirty area or TE cadence. The second
board is untouched.
