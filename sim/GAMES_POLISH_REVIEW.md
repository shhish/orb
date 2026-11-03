# Games polish: independent simulator review

Reviewed on 2026-10-27 by the simulator/test agent, who did not author the game
renderer. Standard: `docs/hig.md` and
`docs/agent-briefs/wave2/design-gate.md`. The four games pass the screenshot gate
in the configurations below. Device FPS, physical sound quality, panel response
and internal heap minima remain unmeasured by this stream.

## Reproducible captures

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build -j4
ctest --test-dir sim/build --output-on-failure -j4
sim/games-shots.sh sim/build/games-polish
shellcheck sim/games-shots.sh
```

This fresh worktree initially lacked managed cJSON. The simulator was configured
against the existing, read-only dependency checkout with:

```sh
cmake -S sim -B sim/build -G Ninja \
  -DORB_CJSON_DIR="$PWD/../w2-render/firmware/managed_components/espressif__cjson/cJSON"
```

`ORB_CJSON_DIR` defaults to this worktree's managed component when available.
The final configuration and test run use that canonical local managed component
after the firmware build populated it. No sibling worktree was changed.

The capture script writes **196 PNGs** under `sim/build/games-polish/`, in
`default/`, `ax3/`, `opaque/` and `ax3-bold-rt-rm/`. For each game ID (`marble`,
`orbit`, `ring-snake`, `pop`) every directory contains:

- `<id>-start.png`, `<id>-play.png`, `<id>-paused.png`, `<id>-end.png`.
- `<id>-sequence/00-start.png` through `07-result.png`: attract, actual pointer
  launch, gameplay, collection/switch/pop feedback, and the New Best reward.
- `marble-controls-play.png`: the optional visible four-direction alternative.

For example, Marble's Default start is
`sim/build/games-polish/default/marble-start.png`; Orbit's enlarged play is
`sim/build/games-polish/ax3/orbit-play.png`; Snake's opaque pause is
`sim/build/games-polish/opaque/ring-snake-paused.png`; Pop's enlarged, bold,
opaque, motion-reduced end is
`sim/build/games-polish/ax3-bold-rt-rm/pop-end.png`.

The `--reward` and `--end` options are explicit simulator fixtures. Collection
fixtures place a target on the next legal move and invoke the production rules;
they do not add a second renderer. Scores/best values are synthetic. The full
CTest run also generates the unchanged Face/list/card Default, AX3 and opaque
design fixtures under `sim/build/gate-*`.

Some preview clients omit white text in these PNGs. A separate standard-library
PNG decoder (zlib plus PNG row-filter reconstruction, no image library) verified
Marble's start footer in the actual RGB pixels: Default and opaque each contain
465 white pixels in `(187,287)`–`(277,301)`; AX3 contains 846 and AX3/Bold/opaque/
Reduce Motion contains 949 in `(175,285)`–`(290,304)`. All lie inside the intended
footer region `(70,277)`–`(396,313)`; a sampled text mask reads `Best 120`.
The missing footer in a preview is not missing from the generated image.

## Design gate

Each note applies to that game's start, playing, paused and result captures in
all four directories above; the sequences additionally verify intermediate
frames.

| Heading | Marble | Orbit | Ring Snake | Pop |
| --- | --- | --- | --- | --- |
| Two layers | Pass: plain marble/gems; glass only on Close, arrow setting and the single primary. | Pass: plain tracks/hazards; Close and one primary are the only glass. | Pass: plain polar tracks/snake; Close and one primary are glass. | Pass: plain bubbles; Close and one primary are glass. |
| Canvas | Pass: true black, flat marble facets and moving reflection; no gradient/glow/shadow. | Pass: true black with crisp green lanes and red capsules. | Pass: true black, flat segments, white head/eyes and marked food. | Pass: true black, AA circle rims and inset reflections; no glow. |
| Round-first | Pass: centered 215 px field; diagonal chrome; lower primary; no clipped text. | Pass: concentric full-field lanes; bottom player clear of Switch at y=300. | Pass: concentric paths; two turn targets follow the lower curve. | Pass: radial drift/rim; lower Pop target; no bezel clipping. |
| Sizes/type | Pass: Inter/AX3/Bold; 72 px controls; compact large scores fit. | Pass: Inter/AX3/Bold; score/pause, Switch and result labels fit. | Pass: full Ring Snake title and both turn glyphs fit at AX3/Bold. | Pass: AX3/Bold life/combo, action and result text remain legible. |
| Colour/state | Pass: white marble versus faceted yellow gems; green identifies controls/reward. | Pass: white player versus capsule hazards; lives also written as text. | Pass: head/eyes, body and faceted food remain distinct without hue. | Pass: combo hue is paired with a numeric multiplier; lives remain text. |
| Approvals/agents | N/A: no approvals, risky defaults, agent spinners or ambiguous OK controls. | N/A: same. | N/A: same. | N/A: same. |
| Motion/accessibility | Pass: finite attract/collect/odometer motion; optional effects suppressed under Reduce Motion; opaque glass verified. | Pass: essential movement retained; decorative trail/reward suppressed with Reduce Motion. | Pass: interpolated grid motion with bounded food response; reduced mode retains playability. | Pass: drift retained; decorative droplets/ring/reward suppressed in reduced mode. |
| Gesture alternatives | Pass: controller setting exposes all four arrows; visible score/pause, Resume, End Round and Close. | Pass: Switch duplicates tapping; visible score/pause, Resume, End Round and Close. | Pass: left/right buttons duplicate tap/tilt; visible pause/result actions. | Pass: Pop selects the urgent bubble; visible pause/result actions. |
| Feels Apple-made | Pass: tilt leads, sparse chrome and a readable round field. | Pass: clear two-lane task and restrained green/red identity. | Pass: coherent polar geometry, smooth body and distinct head. | Pass: restrained bubble reflections and readable, quiet combo feedback. |

The brief's shaded bitmap and soft shadow were resolved in favor of the stricter
design gate: flat layered facets and an IMU reflection, without gradients, glow
or shadow on the black canvas. Initial chrome is prepared before screen entry;
later appearances/retractions use the shared glass material animation. Reduce
Motion uses the shared cross-fade. No live audio or panel-quality claim follows
from these screenshots.

## Regression evidence

The final full simulator run passed **563/563 CTests**. The renderer contract
passed **1,373 checks**, including **360 moving** and **72 transition** frozen
partial/full comparisons plus start/pause/result/accessibility fixtures. Pixel
comparisons remain byte-for-byte exact; no colour tolerance or oracle bypass was
added.

The tests exercise real pointer Play, Pause, Resume, End Round, Play Again,
Close, Marble arrows and the other games' visible alternatives. They cover a
raised best-score baseline across multiple rounds, outgoing chrome immediately
stopping input interception, live AX3/Bold/opaque/Reduce Motion changes, score
warnings beside life/combo text, stable widget counts, unchanged-model idle
damage, and inactive-shell screen isolation. Close is the final action before
view destruction, matching the actual runtime lifecycle.

The exact oracle exposed two concrete retained-damage defects: a moved mutable
life label left old text, and partial glass patches could quantize six rim
pixels differently from a complete refresh. The renderer now invalidates the
old label geometry before its text changes. Affected glass damage includes its
sample halo as a full-width **row band**; this is bounded to attract/reward or
material transitions and sprite/glass intersections. It is not a full-frame
redraw and does not add continuing glass work to steady gameplay. The same
strict comparison verifies the fix.

Host/native tests do not establish 30 FPS, TE timing, heap headroom or the absence
of LVGL's own draw-task allocations. Game-owned tick/frame allocations and sound
mute policy are separately covered by the runtime/sound host tests. The lead
must run `bench_game` on the dev board before claiming the device performance
gate has passed.
