# Marble Conservatory — design round 3

The owner-reported motion and control fixes supersede the interaction details,
screenshots and validation counts below. See [OWNER_FIXES_REVIEW.md](OWNER_FIXES_REVIEW.md)
for the current sign analysis, changed-file ledger, tests and design evidence.
The rest of this document is the historical round-three review.

This supersedes the round 2 visual verdict. The implementation
uses `docs/hig.md`, `docs/delight.md`, research concept 10 and the mandatory
design gate. No Git writes, board access, owner configurations or live services
were used. The pre-existing untracked root `Makefile` remains untouched.

## What changed

- The physical screen is the garden wall: Marble's radius is 223 px (10 px rim
  inset), with no drawn inner outline. Other games retain their geometry.
- The arena now fills the circle: stage one has gate radii 94/188 px, stages
  two and three have 64/126/188 px. The outer stone surface is 39 px inside
  the bezel. A 24 px marble starts at `(-182,103)`, radius 209.1 px, and
  travels inward to a 48 px central flower (goal capture radius 20 px).
- Gravity is 480 px/s²/g (was 360), 60 Hz damping is 0.980 (was 0.984), and
  the speed cap is 210 px/s (was 180). Inner-to-outer gate speeds are
  `+0.40/-0.24`, `+0.46/-0.36/+0.26`, `+0.52/-0.40/+0.29` rad/s; gap half-angles
  are `.70/.36`, `.67/.48/.35`, `.64/.48/.34` radians. Outer gates turn more
  slowly to keep their tangential speed and crossing windows manageable.
- Gates are continuous 12 px stone arcs with round caps and a subtle 1 px upper
  edge. Shared analytic intervals drive rendering and collision, including
  touch-control and fixed rim-control clearances. Reserved control pockets
  cannot be used as extra gate openings; tests sample both ink and navigation.
  There are no invisible dotted-post gaps.
- Three dots on the upper arc fill as stages finish. No timer or invented par
  time. A centred Framework7 pause glyph sits in a 72 px glass circle at
  `(365,101)`; held PWR remains the OS-owned exit.
- A 72 px glass rewind button at `(233,399)` uses paired back chevrons. First
  unearned play teaches “Hold to rewind”, then removes the hint after the first
  hold or four seconds. A garden restored from storage skips that tutorial.
  “Rewinding” appears only while held. Four neutral dots trail departed marble
  positions and fade while gates continue forward. No extra record writes.
- Goal arrival causes a 350 ms blossom spring from 0.6 to 1, followed by a
  bounded curved flight to the lower arc, ending at 850 ms. The blossom briefly
  cross-fades into the earned keepsake. Reduce Motion uses stationary endpoints
  and a 150 ms cross-fade. The receipt remains until Next stage or Finish.
- The garden grows from a central planting along a still spiral to radius
  145 px; outer petals reach about 70% of the screen radius.
  Deterministic size and rotation variation gives each keepsake its own shape;
  stones are filled warm pebbles with a one-pixel upper highlight. The count
  occupies the single secondary line. The garden is still at rest.
- Entry is Marble / Play / Garden. Touch Controls lives in Pause. Primary glass
  is blue, ordinary labels neutral, and green belongs to the goal and success.
  Outgoing and incoming controls sequence their material to avoid overlapping
  labels and keep at most three glass shapes visible.
- Pause has one title, primary blue glass Resume, secondary neutral glass
  End Round with red text, and a plain Touch Controls toggle row. Its thumb
  position makes the setting readable independently of colour.
- Gate redraws compare the union of each ring's visible intervals, retaining
  unchanged stone and damaging only tiled angular changes and cap boundaries.
  Bounds include LVGL's integer-degree angle quantization. Changed sprites
  retain their separate old/new damage; game frame code still allocates nothing.

## Reproduce

```sh
make -C firmware/components/orb_games/tests/host test -j4
cmake --build sim/build -j4
ctest --test-dir sim/build -j4 --timeout 60 --output-on-failure
sim/conservatory-shots.sh
shellcheck sim/conservatory-shots.sh
IDF_PY_BUILD_JOBS=4 scripts/idf.sh build
```

The firmware build requires process inspection for ESP-IDF's component manager;
the sandbox blocked `psutil`'s parent-process check. The same requested build
runs successfully with that access. Build output confirms `ninja -j 4 all`.
No signing or flashing is performed.

## Screenshot ledger and design gate

All paths are relative to `sim/build/conservatory/`. Each surface and sequence
exists under `default/`, `ax3-bold-rt/` and `reduce-motion/`: 45 PNGs per mode,
135 total. AX3 includes Bold Text and Reduce Transparency, with essential motion
retained. The separate Reduce Motion set uses touch steering. These are actual
production model/view renders with fake board input. The goal fixture places
the marble at the goal, then invokes the real completion rule; it is not evidence
of a player solving a stage. Host tests independently solve all three stages by
ordinary deterministic tilt and arrow input.

### Entry — `{mode}/start.png`

- Two layers: Close, Garden and the single primary Play use shared glass; flower and text are plain content.
- Canvas: true black, original flat flower, no glow or gradient artwork.
- Round-first: diagonal Close, centred emblem and chord-safe reading lines, lower-arc primary.
- Sizes: 72 px controls and Inter text; AX3/Bold labels remain single-line.
- Colour: green goal emblem, neutral labels, one blue tinted primary.
- Approvals/agents: none; no loading or productivity metaphor.
- Motion: entry rests; shared material supports Reduce Transparency and Reduce Motion.
- Gesture alternatives: Garden and Play are visible; controls are reachable through Pause.
- Craft: one title and one secondary line; removed the prototype's green settings link and garden count clutter.

### Play — `{mode}/play.png`, `play-glyph.png`, `stage-2.png`, `stage-3.png`

- Two layers: two small glass controls float over flat analytic game content.
- Canvas: bezel is the wall; no inner outline, glow or gradient.
- Round-first: two introductory rings then three span the radius; the outer edge is 39 px inside the bezel, progress dots sit on the upper arc, pause on the upper-right arc, rewind at six o'clock.
- Sizes: 72 px controls, default/AX3 hint fits one line; pause glyph stays centred inside its material.
- Colour: neutral stone gates and trail, white marble, green destination; completed dots also differ by fill.
- Approvals/agents: solid arcs read as physical obstacles; no indeterminate spinner or timer text.
- Motion: only essential puzzle motion continues; rewind ghosts and decorative lighting disappear under Reduce Motion.
- Gesture alternatives: visible pause and hold-to-rewind controls; held PWR still exits.
- Craft: full round garden, obvious gate openings and no textual stage/time HUD; experienced play is glyph-only.

### Touch steering — `{mode}/controls.png`

- Two layers: the same two glass controls; arrow glyphs are ordinary controls over plain content.
- Canvas: black with continuous visible/collidable arc intervals; no decorated field border.
- Round-first: four 72 px arrow targets surround the centre and maintain physical input coordinates.
- Sizes: stable arrow targets and AX3/Bold glyphs clear their bounds.
- Colour: neutral arrows and obstacles, green goal; no green labels.
- Approvals/agents: no loading or external task state.
- Motion: touch steering ignores the IMU; Reduce Motion preserves playable movement.
- Gesture alternatives: arrow buttons fully replace tilt; pause and rewind remain visible.
- Craft: arrows remain in familiar compass directions, with gate geometry respecting their actual clearances.

### Pause and interruption — `{mode}/paused.png`, `paused-toggle.png`, `paused-save-error.png`, `end.png`

- Two layers: Pause has Close, primary Resume and secondary End Round glass; Touch Controls is a plain toggle row. End uses Close, Garden and Play again, with incoming material delayed until End Round retracts.
- Canvas: no background panel, gradients or glow.
- Round-first: centred reading hierarchy and bottom primary; Pause's glass End Round and toggle have distinct hit rows with 20 px between their visible control surfaces.
- Sizes: warning clears End Round and Resume at AX3/Bold; controls remain 72 px high.
- Colour: neutral copy with one blue primary, red End Round text; toggle thumb position adds non-colour state and early exit never claims success.
- Approvals/agents: explicit End Round, no timed dismissal or consequential hidden action.
- Motion: the field stops, stale holds cancel, and outgoing glass settles before replacement controls enter.
- Gesture alternatives: visible Resume, End Round, Close and Touch Controls toggle; PWR exit remains. `paused-toggle.png` captures a real pointer tap changing the setting.
- Craft: one title, at most one secondary line, save error replaces that line instead of adding clutter.

### Completion — `{mode}/cleared.png`, `cleared-save-error.png`, `finished.png`

- Two layers: keepsake is content; Close and the one Next stage/Finish/Play again action use glass.
- Canvas: flat original artwork on true black; no confetti, gradient or glow.
- Round-first: reward settles at `(233,333)`, above the lower primary with safe clearance.
- Sizes: enlarged receipt text and action fit; no extra stage-count or time line.
- Colour: green Planted indicates success, quiet keepsake colour carries identity, one blue action.
- Approvals/agents: earned only by the local goal rule; no agent approval or productivity reward.
- Motion: reward is brief and caused; the completed receipt remains until explicit action.
- Gesture alternatives: visible Next stage/Finish/Play again and Close.
- Craft: settled captures include fully materialised controls, rather than an almost-invisible incoming button.

### Garden — `{mode}/garden-empty.png`, `garden.png`, `garden-full.png`, `garden-save-error.png`

- Two layers: plants and count are plain content; Close and Back to play are glass.
- Canvas: true black, flat petals and filled warm stones, no glass content or glow.
- Round-first: a growing spiral reaches radius 145 px; its outer petals reach about 70% of the screen radius. Title/count sit above it and art clears Close and the lower action.
- Sizes: deterministic 13–16 px keepsake variation is art, not a touch target; all actual controls remain 72 px. The empty helper moves below Close; concise save-error copy clears it at AX3.
- Colour: neutral pebble tones and two quiet flower inks; count is secondary, one blue primary.
- Approvals/agents: optional play memory, no score pressure, streak or work-related reward.
- Motion: static at rest, verified to produce no idle frames after material settles.
- Gesture alternatives: explicit Back to play and Close preserve navigation without tilt.
- Craft: stone highlights follow upper edges instead of appearing as minus badges; a full plot still has breathing room.

### Rewind — `{mode}/rewind/00-forward.png` through `07-forward.png`

- Two layers: held rewind uses the existing glass target, no new overlay surface.
- Canvas: only faint separate path dots on black; no luminous trail.
- Round-first: the gesture remains anchored to the lower arc while game content retraces inside the bezel.
- Sizes: Rewinding uses the same chord-safe Inter secondary line as the first-play hint.
- Colour: neutral path and a white marble, with goal green unchanged.
- Approvals/agents: no fake progress; gates demonstrably move forward across the captured sequence.
- Motion: real press/hold/release, reverse path, fading departed positions, deliberate velocity-zero release; reduced mode omits ghosts.
- Gesture alternatives: visible hold target; release or loss of contact stops rewind deterministically.
- Craft: the input trace moves through an open outer quadrant so screenshots show an actual trail instead of a stalled marble.

### Bloom and keepsake — `{mode}/bloom/00-before.png` through `08-rest.png`; `{mode}/stages/*`

- Two layers: moving token is bounded content; it never becomes a glass card.
- Canvas: flat petals/stone and ordinary material controls, no glow or particles.
- Round-first: central goal opens, then reward lifts along a small curve to the lower garden arc.
- Sizes: all art stays inside its recorded damage bounds; final control and token remain separated.
- Colour: green blossom makes the cause clear, then the earned item's persistent colour takes over.
- Approvals/agents: one real goal produces one keepsake; Next stage requires explicit input.
- Motion: 350 ms analytic spring plus flight, cross-faded handoff; Reduce Motion uses stationary 150 ms endpoints.
- Gesture alternatives: receipt controls become available after the reward, and Close remains visible throughout.
- Craft: no ambient garden animation; fully settled final frame and explicit next-stage transition are captured.

Unchanged Face/list/card gate fixtures are regenerated by CTest at
`sim/build/gate-{face,list,card}-{default,ax3,opaque,ax3-opaque-bold}.png`.
Independent round 3 screenshot review passed the arena, pause controls, garden,
receipts and all motion/navigation sequences across the three modes. It caught
the empty-garden helper and long save-error line touching Close at AX3; the
helper now sits below Close and the garden uses the concise "Not saved" line.
Both corrected fixtures were reviewed again. The lead's final visual judgment
remains separate.
The final regenerated `paused-toggle.png`, `garden-empty.png` and
`garden-save-error.png` were independently reviewed in all three modes;
the final 135-PNG set has no remaining visual findings.

## Validation and limits

- Sanitized host suite: **4,507,168 logic checks + 19,898 runtime checks**, all passed;
  log `/tmp/orb-marble3-host.log`. Ordinary tilt and touch input solve every
  stage within 60 seconds; geometry tests reject hidden control-pocket bypasses.
- Simulator build: **passed**, `cmake --build sim/build -j4`.
- Full CTest: **612/612 passed**, 23.47 s; log `/tmp/orb-marble3-ctest-final.log`.
- Focused renderer: **5,139 checks**, including 1,596 moving and 72 transition
  retained/full redraw frames, lifecycle cases and reward-pixel assertions.
  Log `/tmp/orb-marble3-render-final.log`.
- Evidence: **135 PNGs**; screenshot script passes ShellCheck.
- Final firmware: **passed**, `IDF_PY_BUILD_JOBS=4 scripts/idf.sh build`;
  log `/tmp/orb-marble3-idf-final.log`, unsigned `firmware/build/orb.bin`.
- Whitespace check: `git -c maintenance.auto=false diff --check` passed.

An image-preview client intermittently omitted token/title regions even though
the RGB8 PNG contained them. Original-size viewing plus independent RGB decoding
confirmed the pixels; the reward oracle also explicitly asserts token presence
under Reduce Motion and at rest. No production changes were made for that
preview artifact.

The fixed 60 Hz step, bounded ten-second history, fixed arrays, 30 Hz presentation
target, atomic 84-byte OGK1 persistence and PWR lifecycle are preserved. The model
is 2,644 bytes. No game-owned frame allocations or per-frame filesystem I/O.
The renderer compares retained pixels byte-for-byte with a frozen full redraw,
including bloom/flight, rewind, Reduce Motion and material transitions, and
asserts constant widget count, bounded game damage and no garden idle redraw.

The expanded-arena fixture adds 1,080 moving frames, crossing angle wrap and
control-cutout topology changes for every stage in both input modes. Captures
below measure actual simulated band flushes and host update/render time,
excluding the frozen full-redraw oracle. No moving frame invalidates the arena
square; unchanged rings submit zero damage. Submitted area statistics now
include sprite-triggered expanded glass bands, with overlaps counted more than
once; actual flush counts are captured separately.

| Stage / input | Mean flushed pixels | Maximum flushed pixels | Host p95 |
| --- | ---: | ---: | ---: |
| 1 tilt | 34,216 | 111,768 | 1.760 ms |
| 2 tilt | 46,526 | 105,868 | 3.757 ms |
| 3 tilt | 43,762 | 104,976 | 3.135 ms |
| 1 touch | 18,151 | 66,116 | 1.587 ms |
| 2 touch | 41,443 | 105,292 | 2.417 ms |
| 3 touch | 40,560 | 104,400 | 2.266 ms |

The simulator's p95 stays below the 33.333 ms / 30 fps budget in all six
cases. Mean transferred area is 8.4–21.4% of the 466² square, maximum 51.5%;
the largest individual submitted rectangle is 71,764 px including glass's
backdrop. The test guards average flushes below one quarter and maximum flushes
below 60% of the square, and p95 host time below 33.333 ms.

No board access was permitted, so simulator evidence does not establish actual
panel FPS, DMA headroom, physical contrast or touch comfort. Steady completed-panel
30 fps still needs the owner's integrated board measurement. Increase Contrast
also remains the inherited shared-settings handoff gap described in README.

No requested visual direction was rejected. The small visible pause alternative
is retained because the mandatory gate requires a visible gesture alternative.
Blue primary glass replaces the old green chrome so green has goal/success
meaning, as requested. Tutorial dismissal is view-local until the first earned
keepsake, preserving the existing on-disk format and avoiding a tutorial write.
