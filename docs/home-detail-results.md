# Home detail implementation and design review

Device/simulator source: `firmware/components/orb_home/orb_home_detail.c` and
`orb_home_detail.h`. Reusable circular content feedback is in
`orb_home_fill.c` / `orb_home_fill.h`. Changes are uncommitted on `orb/w2-home`.
No board, live hub, credentials or owner settings were used.

## Behaviour

- Light brightness uses a 270° retained rim arc, 10% ticks, a touch-only glass
  knob, end rubber-band and a critically damped .5 s spring. The UI tracks
  pointer samples locally and queues one absolute write on release. Visible
  Minus/Plus controls offer the same adjustment without a drag.
- The light's second page previews its actual hue/saturation or white colour
  temperature. Twelve separated colour samples are a selection control, not
  a continuous gradient. A visible chevron cycles pages. Scenes use readable
  one-per-page controls with Next; each captures a stable scene ID, and a
  reordered snapshot rebuilds the scene page.
- Thermostat current temperature is large at rest, with `Heating to 22.0°`
  (or cooling/target) below. During adjustment the large reading is the target.
  Off / Heat / Cool / Auto are explicit, readable choices on the mode page.
- Fan speed uses the same rim. Its Framework7 glyph makes one finite 600 ms
  acknowledgement turn after an on/speed change, then rests. Reduce Motion
  keeps it static. This resolves the conflict between continual on-state
  rotation and the explicit zero-frames-when-idle requirement.
- Locks require an uninterrupted one-second centre hold. A tap never unlocks;
  release, moving outside, disconnect, unreachability, or hiding any ancestor
  cancels the hold. The progress ring clears both title and instruction at AX3.
  A failed optimistic unlock visibly returns to Locked.
- Switch/outlet disc state changes fill only their disc; sensor, switch and
  lock updates retain the tree and update changed text/glyphs/fills in place.
  Missing readings show an em dash or State unavailable. A humidity accessory
  selects humidity even when the same service also reports temperature.
- Pending requests say Updating; failures retain `Couldn't reach …` beside
  the reverted state. Offline/unreachable controls cannot queue writes.

## HIG decisions

`hig.md` wins where the literal Home sketch conflicts:

- Four separate glass thermostat segments would exceed the glass budget and
  four text buttons in one row violate §3. A single bottom Mode control opens
  two rows of two plain material choices, with the shared glass Back.
- Colour is meaningful accessory content. The selection samples are discrete,
  with no gradient or glow; actual hue/temperature is preserved in the preview.
- A detail has at most three glass objects: shared Back, the primary control,
  and the knob only while touched. Minus/Plus and page alternatives are plain
  controls. The content layer remains true black with flat semantic fills.
- The circular disc helper uses a finite .5 s spring integrated at 240 Hz and
  releases its animation state on completion, retarget, or object deletion.
  Reduce Motion publishes the new state immediately; it creates no idle work.

## PNG review evidence

The complete final matrix is produced by the Home simulator/tests and retained
under `sim/build/home-gate/{default,ax3,ax3-bold-rt}/`. Detail surfaces are
`light.png`, `colour.png`, `white.png`, `scenes.png`, `thermostat.png`,
`modes.png`, `fan.png`, `lock.png`, `unlocked.png`, `switch.png`, `outlet.png`,
`temperature.png`, `humidity.png`, `motion.png`, `contact.png`,
`low-battery.png`, `no-response.png`, and `error.png`.

Real pointer sequences are under `sim/build/home-sequences-default/` and
`sim/build/home-sequences-ax3-bold-rt/`: `rim-*`, `rim-settle-*`, `toggle-*`,
`hold-cancel-*`, `hold-unlock-*`, and `revert-*`.

Critical review checked the Default and AX3/Bold/Reduce Transparency detail
PNGs, including the intermediate rim, hold and error states. It led to a
larger lock title/instruction gap, current-versus-target thermostat hierarchy,
humidity-kind selection, explicit scene paging and in-place updates.

Design gate notes for these surfaces:

- Two layers: flat content; only navigation/action/touched knob are glass.
- Canvas: true black; no gradient, glow, content frost or full-screen tint.
- Round-first: the arc follows the bezel; centred text stays inside measured
  chords, Back is at (101,101), and primary action is at six o'clock.
- Sizes: shared Inter styles, Dynamic Type and Bold Text; 72 px controls,
  46 px minimum rim hit width, no custom small fonts.
- Colour: words/glyphs carry each state as well as colour. Light colour is an
  honest accessory preview; off/unavailable remains distinct and labelled.
- Safety: deliberate unlock hold with cancellation; no list or scene unlock.
- Motion: finite springs/feedback only; Reduce Motion has direct updates,
  Reduce Transparency uses the shared opaque glass treatment.
- Gesture alternatives: Plus/Minus, page chevron, scene Next, and explicit mode
  choices. Hold-to-unlock is itself a visible deliberate action.

## Validation

Commands used during iteration:

```sh
cmake --build sim/build --target orb_home_sim test_home_view -j4
sim/build/test_home_view
sim/build/test_home_view ax3 bold rt
sim/build/orb_home_sim light /tmp/orb-home-detail-review/light-default.png
sim/build/orb_home_sim thermostat /tmp/orb-home-detail-review/thermostat-now.png ax3 bold rt
```

The latest AX3/Bold/Reduce Transparency run in this stream passed 136 checks.
The shared validation stream adds further regression coverage; its final full
CTest receipt supersedes this intermediate count. All measured settled Home
surfaces, including Fan, reported zero frames and zero invalidations. Rim
samples changed only bounded arc/label regions: the AX3 run's maximum
invalidation was 44,756 pixels, maximum transferred host buffer area 66,252
pixels. Host areas are renderer receipts, never claims of physical panel FPS,
SPI timing, audio output quality or heap headroom. Firmware build/full suite
results are recorded by the integrating agent.
