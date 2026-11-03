# Power preview design gate — revised

Reviewed 2026-10-27 against `docs/hig.md` §§1–9, `docs/delight.md`,
`docs/interaction-model.md`, `docs/power-and-calm.md`, and the independent
`work/review-power-agy.md`. The changed view remains a simulator-only public-data
stub outside the shipped shell. It uses production Inter/Framework7 assets,
LVGL and the real deterministic power controller. No board was accessed.

## Reproduce

```sh
make -C firmware/components/orb_power/tests/host test -j4
cmake --build sim/build -j4
ctest --test-dir sim/build -j4 --output-on-failure
sh sim/power-shots.sh
IDF_PY_BUILD_JOBS=4 scripts/idf.sh build
```

Host tests pass **40 cases / 8,316 checks** under ASan/UBSan: 13 controller/adapter
cases (5,094 checks), plus 27 synthetic IMU cases (3,222 checks).

The IDF job environment variable is supported by the installed IDF Ninja
runner and explicitly passes `-j 4`. All build concurrency is bounded at four.
Firmware build **passes**, producing unsigned `firmware/build/orb.bin` with
61% of the app slot free. Full CTest is **615/617 passed**, including all five
power preview tests. `home_board_ax3` (#395) and `home_board_reduce_motion`
(#397) spun in unchanged LVGL label rendering: the original run was stopped
after roughly 315 s per test to free CPU, the complete rerun timed them out at 60 s,
and an isolated retry outside the sandbox timed out at 30 s. Their source and
LVGL were not modified for this task. The sample places the active draw worker
in `lv_draw_label_iterate_characters`, while the main thread waits for flush.
A stale text-length / zero-progress glyph loop is a hypothesis, not an identified
root cause. No general renderer fix was added to this power revision.

Logs: `sim/build/power-ctest.log`, `sim/build/power-home-retry.log`,
`/private/tmp/orb-power-home-board-sample.txt`, and
`/private/tmp/orb-power-idf-build.log`. Render counts do not imply a device pass.

The screenshot command produces **415 power PNGs plus nine shell context PNGs**
in `sim/build/power-gate/`. All five variants pass **11,888 assertions**:
Default 2,404; AX3/Bold/RT 2,406; Reduce Motion 2,335; AX3/Bold/RT/RM 2,337;
Increase Contrast 2,406. Each of `default/`, `ax3-bold-rt/`, `reduce-motion/`,
`ax3-bold-rt-rm/` and `increase-contrast/` contains 83 power frames:

- `aod.png`, `aod-clear.png`, `aod-count-cap.png`, `aod-shift-{1,2,3}.png`;
- `attention-clear-{000,080,250}.png`, `attention-arrive-{000,080,250}.png`;
- `sleep-NNN.png`, `wake-NNN.png`, `wake-aod-NNN.png`,
  `wake-interrupted-NNN.png`, at 0, 1, 40, 80, 120, 149, 150, 200, 249, 250,
  280, 320, 399, 400 and 500 ms;
- `rest-NNN.png`, at 0, 1, 40, 80, 120, 149, 150, 200, 249, 250 and 400 ms.

## Per-surface evidence

The headings below apply independently to the cited Default and AX3/Bold/opaque
surfaces; the reduced-motion variants supply the motion alternative. An
independent agent inspected original-detail PNGs and contact sheets. Its two
findings (overlapping attention glyphs on lit wake, abrupt attention-presence
recentring) were fixed and re-reviewed before this pass.

| Gate heading | Always On / attention reflow | Rest / sleep | Wake |
| --- | --- | --- | --- |
| Two layers | Pass: plain text/glyph content, zero glass or tinted controls. | Pass: retained content only. | Pass: retained public content; no control/material animation. |
| Canvas | Pass: true black, sparse ink, no gradient/glow, ring or battery. | Pass: black throughout; `sleep-250.png` is entirely black. | Pass: no flash or novelty artwork. |
| Round-first | Pass: clear clock ink is centred at the equator plus burn-in shift; paired ink union is optically centred. `aod-count-cap.png` fits at AX3/Bold. | Pass: no content leaves the text-safe circle. | Pass: 2 px time lift, with previous burn-in offset interpolated on lit wake. |
| Sizes | Pass: semantic Large Title/tabular time and Headline attention; ≥16 px hand/text clearance; AX3/Bold never shrinks to fit. | Pass: semantic sizes retained, no interactive micro-targets. | Pass: arriving mark scales .9→1; a mark already present retains its current size to avoid a discontinuity. |
| Colour | Pass: white time, HIG orange hand and count, increased-contrast orange variant. Symbol and words carry state. | Pass: only existing semantic ink fades. | Pass: no reward/failure colour attached to wake. |
| Approvals & agents | Pass: numeric time/count API admits no private strings or actions. | Pass: no answer, navigation, send or model mutation. | Pass: automatic wake stays redacted; first intentional contact is consumed. |
| Motion | Pass: settled 1 Hz, zero unchanged writes. Presence changes have a bounded 250 ms reflow; minute offsets stay discrete for every element. | Pass: 250 ms settle/fade; immediate dark for face-down/critical. | Pass: critically damped ≤400 ms, no glyph ghosting or endpoint pop; sleep interruption preserves its actual frame. |
| Accessibility | Pass: AX3/Bold/RT and Increase Contrast; no glass needs an opaque substitute. | Pass: Reduce Motion uses a 150 ms fixed-frame cross-fade. | Pass: reduced motion has no positional/scale animation; snapshot source preserves partial fades. |
| Gesture alternatives | Pass: whole-face touch/PWR wake; no tiny required target. | Pass: no decision or forced wait. | Pass: input remains usable during the transition. Visible Quiet/Active controls remain shell integration work. |
| Craft | Pass: sparse, balanced and still after settling. | Pass: purposeful retreat to calm content, then black. | Pass: information itself answers pickup. |

Representative surfaces:
[Default Always On](build/power-gate/default/aod.png),
[AX3/Bold/RT](build/power-gate/ax3-bold-rt/aod.png),
[empty equator](build/power-gate/default/aod-clear.png),
[largest count](build/power-gate/ax3-bold-rt/aod-count-cap.png),
[recentring](build/power-gate/default/attention-clear-080.png),
[arrival](build/power-gate/ax3-bold-rt/attention-arrive-080.png),
[rest](build/power-gate/default/rest-080.png),
[sleep fade](build/power-gate/default/sleep-080.png),
[dark](build/power-gate/default/sleep-250.png),
[dark wake](build/power-gate/default/wake-080.png),
[lit wake](build/power-gate/default/wake-aod-080.png),
[interrupted wake](build/power-gate/default/wake-interrupted-080.png),
[RM interrupted wake](build/power-gate/ax3-bold-rt-rm/wake-interrupted-080.png).

`default/`, `ax3-bold-rt/` and `ax3-bold-rt-rm/` also contain unchanged
`face-context.png`, `agents-context.png` and `approval-context.png`.
All gate headings pass for these context captures: black canvas, round-safe
semantic type, separate control layer, proper targets, meaningful accents,
opaque RT controls, neutral Allow/Deny and no automatic decision. These
captures establish context, not a change to shell behaviour.

## Pixel and timing checks

The executable verifies round-safe label bounds, count cap, ink budget,
Headline clearance, redaction, settled cadence, zero unchanged writes,
complete burn-in offsets, compositor reflow cadence, and exact 2 px dark wake.
Transition starts compare every pixel to the preceding frame, including an
interrupted partial fade. Endpoints guard against one-frame content pops;
settled endpoints produce no further redraw. Every saved PNG is read back,
decoded and compared byte-for-byte to the full panel RGB buffer.

Attention-presence reflow is compositor-owned: consume
`power_view_frame_interval_ms()` alongside the controller cadence. It requests
16 ms frames only during the admitted 250 ms reflow (150ms under Reduce Motion),
then returns 0. Public-data changes are coalesced before admission. The minute
burn-in shift remains discrete even across a reflow.

## Limits and review decisions

The richer Face in delight.md includes ring/battery content; the HIG minimal
Always On contract takes precedence here. See power-and-calm.md for the
accepted changes and rejected proposals: any-pose rest replaces a partial
z gate; the independent 9% setting replaces proportional brightness; ordinary
sleep is 250 ms; USB retains finite 30-minute idle sleep. The already-visible
attention mark interpolates from its current scale/opacity instead of replaying
an entrance; newly arriving ink uses the specified .9→1 fade/spring.

The shipped shell still needs safe-frame acknowledgement/fences and power
preferences. Simulator evidence does not establish physical brightness,
panel-off, current, wake reliability, false-wake rate or board frame rate.
Independent owner/lead review remains required before merge.
