# Orb simulator

For the interactive **firmware shell, apps and games**, run `make emu` from the
repository root. See [the emulator guide](../docs/emulator.md) for keys, scaling,
diagnostics, fixture data and SDL offscreen execution. The older demo and PNG
test entry points below remain available.

Builds the firmware's LVGL fork, `orb_ui` and `orb_demo` on macOS with SDL2. It keeps RGB565 byte order, round render bands, band guards, and a circular panel mask. It does not model panel transfer timing or physical display response, so host speed is not device FPS.

Install CMake and SDL2 with your normal package manager, then run:

```sh
sim/run.sh
sim/shot.sh
cmake --build sim/build
ctest --test-dir sim/build --output-on-failure
sim/conf-diff.sh
```

Mouse drag is touch. Arrow keys or Shift/Option-drag tilt the fake board; `0` levels it. `m` toggles Reduce Motion; `s` saves a screenshot; `q` exits. Reduce Motion stops automatic scrolling, staged lighting and simulated presses, while retaining touch input.

For deterministic headless captures:

```sh
sim/run.sh --headless --shot 1000=/tmp/orb-scroll.png
sim/run.sh --headless --tilt 1200=25,0 --shot 2000=/tmp/orb-tilt.png
sim/run.sh --headless --reduce-motion --shot 8100=/tmp/orb-still.png
sim/run.sh --headless --text-size ax3 --reduce-transparency --shot 5100=/tmp/orb-ax3-opaque.png
sim/shot.sh -n 60 /tmp/orb-frames
```

`sim/shot.sh` writes `docs/screens/demo-*.png`. `sim/conf-diff.sh` compares effective simulator settings with `firmware/build/config/sdkconfig.h`; its documented exclusions are host-only differences. No board access is needed.

Tests cover fake-board geometry, headless startup, unchanged clock pixels after an uninterrupted scroll cycle, cached row scaling without rebuilding bitmaps, a stationary Reduce Motion scene, and the software blur including clipped buffers and anti-aliased boundaries. All chrome and session-row caches remain enabled.

## Shell and system screens

The simulator links the same Inter/Framework7 fonts, font pack, live status,
`orb_model` and `orb_shell` used by firmware. The fixture store is loaded only
by the simulator's explicit `orb_model_load_demo()` call. Preview identifiers,
setup codes and agent sessions are synthetic.

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build
sim/build/orb_shell_sim face /tmp/face.png
sim/build/orb_shell_sim approval /tmp/approval.png ax3 bold rt rm
sim/build/orb_shell_sim --flow /tmp
sim/build/orb_shell_sim --delight /tmp
sim/shell-shots.sh
sim/delight-shots.sh /tmp/orb-delight-motion
ctest --test-dir sim/build --output-on-failure
```

Shell scenes: `face` (`home` alias), `demo` (explicit offline preview), `apps`, `agents`, `session`, `stack`,
`session-summary` (the Info disclosure), `approval`, `approval-actions`, `choice`, `choice-options`, `message`, `attention`, `notifications`, `control-center`,
`settings`, `display`, `accessibility`, `about`, `dot`, `voice`, `play`, `updates`.
Additional deterministic page previews are `apps-next`, `agents-next`,
`choice-options-next`, `control-silent`, `control-motion`, `settings-next`,
`access-bold`, `access-motion`, `access-transparency`, `access-contrast`, and `about-licenses`.
`access-contrast` turns the Contrast control On for the capture.
The Apps alias scrolls the accessible app list at AX3. The Control Center
aliases page at AX3; Default shows brightness and both control toggles.
`approval-actions` and `choice-options` are compatibility aliases for their
complete review surfaces; they no longer traverse hidden interstitials.
Round 3 adds `voice-starting`, `voice-recording`, `voice-transcribing`,
`voice-confirming`, `voice-sending`, and `voice-error`. The confirmation and
error fixtures contain a complete long transcript for scroll/readability review.
`answer-queued`, `answer-delivered`, `answer-undelivered`, `answer-other`, and
`answer-rejected` drive a real pointer choice before simulating the hub's
card removal and result. `answer-other` records the other controller's winning
option. `card-cancelled` removes an unanswered card. `message-delivered` and
`message-timeout` distinguish transport receipt from unknown delivery.
System scenes: `setup`, `setup-retry`, `pairing`, `connecting`, `offline`,
`identify`, `service-error`, `update`, `downloading`, `verifying`, `ready`,
`update-error`.

Optional flags may appear in any order after the output: `default`, `larger`,
`ax3`, `bold`, `rt` or `opaque` (Reduce Transparency), `rm` (Reduce Motion),
and `ic` (Increase Contrast). `ic` exercises the real Accessibility toggle
before navigating to the requested scene, and stays enabled throughout flows.
Captures advance
virtual time past the navigation spring and save a round 466×466 PNG. The host
font port reads the repository pack with zlib; firmware inflates fonts from its
assets partition into PSRAM.

`--flow` drives actual LVGL pointer input through Face → Apps → Agents → session
→ approval → Queued. Six numbered PNGs capture every resulting state, including
the exact approval context with both decisions visible. A second two-frame
sequence exercises the Face attention complication's direct review shortcut.
The command fails if a control cannot be found, a destination is wrong, either
decision is missing, or the model fails to queue the answer. It never sends an
answer to a live hub; Queued describes only local acceptance.

`--delight EXISTING_DIRECTORY [flags]` records short deterministic motion
sequences from real pointer input and synthetic model changes. Unlike settled
scene previews, it saves intermediate frames at 0, 16, 48, 96, 176, 320, 600 and
800 ms. It covers Apps/glass and icon presses, app launch/reverse, list drag and
release, Smart Stack paging, changing counts, tilt and Always On, agent
completion, approval arrival, Allow/Deny, live voice levels and explicit Send,
and the final answered card. Approval and voice success frames follow a
synthetic authoritative hub acknowledgement; the recorder checks the exact
queued choice and voice confirmation target. Play captures its empty state,
then starts the real app registry, launches its registered Marble controller
through the shell queue, and exits with Close. `play-registered` is also a
settled scene for the installed-app list. No live service is contacted.
`sim/delight-shots.sh [DIRECTORY]` captures both Default and the
AX3/Bold/Reduce Transparency/Reduce Motion accessibility combination, plus
Face, Agents and Approval with Increase Contrast at Default and AX3/Bold/RT.

`sim/shell-shots.sh [OUTPUT_DIRECTORY]` creates every scene at Default, AX3 and
AX3/Bold/Reduce Transparency, plus representative Reduce Transparency and
AX3/Bold/Reduce Transparency/Reduce Motion frames for Face, Agents and Approval.
It then captures both input flows. Its default output is
`docs/screens/shell/`; set `ORB_SHELL_SIM` to select another built executable.

The shell contract test drives real pointer taps and swipes and checks service
precedence, setup/pairing text safety, destructive-action confirmation, exact
queued card answers, hub cancellation, live accessibility changes, and unchanged
status/motion updates that must not repaint. Round 2 regressions cover direct
approval access, source/project/command context, persistent equal decisions,
exact permission scope labels, Queued feedback, long command readability,
continuous list scrolling, 20 px choice gaps, persistent Largest cancellation,
full enlarged headings, resting Back clearance, and rejection of unreadable
peripheral row hit targets. Round 3 regressions cover maximum-length card IDs,
answer/cancel/result ordering, recorded winners, failed delivery without replay,
reconnect decision gating, session/card voice focus, mic press/release commands,
explicit transcript confirmation without a timeout, immediate Send disabling,
error text retention, and ambiguous message delivery. Synthetic demo fixtures
explicitly mark the model connected/synced; tests separately remove each gate.
CTest also renders every shell and system scene at
Default, AX3 and AX3/Bold/Reduce Transparency. The simulator does not establish physical
panel performance, optical shadow quality, or firmware heap headroom.

`open_latency` drives real pointer opens and checks a changed destination frame
before content construction, live heading/Back during motion, interruption,
stale-snapshot rejection and 20 stable warm cycles. It captures first/moving/final
frames in the build directory. `async_fonts`, `async_fonts_partial` and
`async_fonts_control` exercise firmware worker publication, partial boot failure
and the lazy A/B control with the real pack and a deterministic fake worker.
Use `-DORB_OPEN_LATENCY_FIXES=OFF` in a separate build directory for the
instrumented full-construction control; its staged-open contract is omitted.
Board commands and the independent design gate are in
`docs/agent-briefs/wave2/open-latency-results.md`.
## Speed A/B and regression coverage

`async_fonts` also retains every font selector in detached screens, raw local
styles, rim/live rows and cold rasters through all 36 directed size/Bold pairs.
It holds resolved glyph descriptors and queues a WAITING label task before each
switch. The font port overwrites and frees retired heap records and mapped bitmap
buffers, so AddressSanitizer sees real retirement. `font_lifetime_*` reuses 88
permanent parity fixtures, keeps their actual presenters alive through all 36
pairs, and includes a navigation fixture visiting every shell surface before and
after retirement. These fixtures never select approval, Send or Unlock actions.

Instrument the entire LVGL/Orb build, including both draw workers, with:

```sh
cmake -S sim -B /tmp/orb-font-lifetime-asan -G Ninja -DORB_SANITIZE_ADDRESS=ON
nice cmake --build /tmp/orb-font-lifetime-asan --target test_async_fonts test_font_surfaces -j4
nice ctest --test-dir /tmp/orb-font-lifetime-asan -j4 -R '^(async_fonts|font_lifetime_)' --output-on-failure
```

The bounded fixture's printed resident/peak/free values model the board ABI and
allocation headers against the recorded PSRAM pool; they are not live board heap
measurements. The actual-presenter tests exercise lifetime independently of that
fonts-only accounting fixture.


Native CMake options mirror the default-off performance Kconfig switches:
`ORB_DISPLAY_GLASS_SINGLE_PASS`, `ORB_DISPLAY_GLASS_MOTION_QUALITY`,
`ORB_DISPLAY_RGB565_SCALE_FAST` and `ORB_DISPLAY_RASTER_SCALE_CACHE`.
`ORB_DISPLAY_RASTER_COVER_CHECK` defaults on to prevent stale pixels from faded
or scaled cached rows. `ORB_DISPLAY_DRAW_PROFILE` enables the same draw counters
used on the board. Pass each as `-DOPTION=ON` in a fresh build directory.

`glass_timing` reports DRAW and whole-refresh wall/CPU cost, including pre-DRAW
scaled-variant creation. It checks sample counts, source raster stability,
retained budgets and blur task counts, without an absolute host timing gate.
`scroll_redraw` imports the glass stream's frozen partial/full ghosting fixture;
`blur_exact` retains its identical golden corpus. New cache/filter tests compare
exact pixels, both RGB565 byte orders, clips, opacity, source changes, allocation
failure, simultaneous over-budget owners and teardown. `speed_motion` covers
settle timing, tick wrap and scene/display deletion. Design fixtures generate
Face/list/card Default/AX3/opaque/AX3+opaque/Bold PNGs.

See `docs/agent-briefs/wave2/speed-round3-plan.md` for lead-only board A/B commands.
Native timings cannot establish ESP32 throughput, SPI timing or physical quality.

## Retained rendering stages 0 and 1

`ORB_DISPLAY_RENDER_ACCOUNTING` and `ORB_DISPLAY_RETAINED_MATERIAL` are separate,
default-off CMake/Kconfig options. Stage 0 firmware receipts include whole-update,
scene/cold preparation, material work, 1 ms cadence buckets and a bounded band
trace; simulator timing still excludes SPI. Stage 1 retains exact per-control
raw patches, untinted filtered maps and material bases with ordered revision,
geometry, opacity and clip dependencies. Unsupported scenes or failed admission
use the compatibility renderer. Visible row scaling, material pixels and the
84-row output path remain unchanged.

```sh
cmake -S sim -B sim/build-stage01 -G Ninja \
  -DORB_DISPLAY_RENDER_ACCOUNTING=ON \
  -DORB_DISPLAY_RETAINED_MATERIAL=ON \
  -DORB_DISPLAY_DRAW_PROFILE=ON
cmake --build sim/build-stage01
ctest --test-dir sim/build-stage01 --output-on-failure
```

`benchmark_fixtures` tests four synthetic 25-second quiet windows, a live two-page
transition baseline and owner deletion. `retained_material` (enabled with Stage 1)
compares frozen frames against the compatibility renderer, checks warmed light
updates with the profiler, continuous tint/press geometry, invalidation, bounded
allocation failure and repeated teardown. Reduced Motion materialization uses
the existing cross-fade path. The new stages do not implement reduced sampling,
half-band overlap, TE cadence or cached-screen transitions.

Lead-only board commands for control, Stage 0 and stages 0+1, ordinary and profiled,
are in `docs/architecture-rendering.md`. The runners' optional `--fixtures` adds
`bench_transition 16000` and `bench_quiet 25000` without replacing the original
three scroll/glass runs; raw output and parsed accounting stay in each receipt.

## Session focus and source preferences

See [focus-device evidence](FOCUS_DEVICE_RESULTS.md) for the new idle paging,
Sessions preferences, no-speech retry, scenario matrix and measured test results.
`sim/shell-shots.sh sim/build/focus-device` regenerates every requested state
at Default, AX3 and AX3/Bold/Reduce Transparency.

## Offline games

`orb_game_flow_sim EXISTING_DIRECTORY [ax3] [bold] [rt] [rm]` captures the real
Apps → Play → playing round controller/view handoff (no second Play), pointer
Close, screen-off/wake, held-PWR exit and return to the same Play item. It uses the production registry
provider with throwaway in-memory scores and no network capability. The
`game_flow_default` and `game_flow_ax3_bold_rt` CTests assert this lifecycle.
The final offline frames explicitly show the current shell restriction:
games continue offline, but the connection screen blocks offline Apps/Play
entry. See [F16 evidence](../docs/screens/ux-fixes/gameux/flow/README.md).

The `orb_games_sim` target runs the same fixed-step game model and sprite view as
firmware. Mouse input reaches the real LVGL controls; arrow keys tilt the fake
board, `0` levels it, `m` toggles Reduce Motion, and `s` saves a frame.

```sh
cmake -S sim -B sim/build -G Ninja
cmake --build sim/build --target orb_games_sim test_games_render
sim/build/orb_games_sim --game marble
sim/build/orb_games_sim --game orbit --headless --tap 500=233,405 --shot 1500=/tmp/orbit.png
sim/build/orb_games_sim --game ring-snake --headless --tilt 200=15,0 --shot 1500=/tmp/snake.png
sim/build/orb_games_sim --game pop --headless --text-size ax3 --bold \
  --reduce-motion --reduce-transparency --shot 1500=/tmp/pop-ax3.png
ctest --test-dir sim/build -R '^(game_|games_render)' --output-on-failure
```

`--shot`, `--tap`, and `--tilt` accept up to 64 timed events. Captures use virtual
time with a deterministic seed. `--game-over` previews the persistent result and
restart surface; `--save-error` exposes the score-save failure state. Each game's CTests write Default, AX3, opaque, AX3/Bold/opaque/
Reduce Motion, and result PNGs in the build directory. `games_render` compares
376 frozen partial redraws with full redraw oracles, bounds moving-frame damage,
checks idle damage and retained widget counts, and exercises the visible Close
button plus enlarged paused/save-error surfaces. No test uses the development
board. These are model/view tests; board FPS, TE cadence, app lifecycle and
LittleFS persistence require their dedicated runtime/device checks.

## Render pacing round

The independent default-off options `ORB_DISPLAY_RETAINED_SETTLED_ONLY`,
`ORB_DISPLAY_GLASS_SOLID_FAST`, `ORB_DISPLAY_GLASS_SPARSE_MASKS` and
`ORB_DISPLAY_TE_PACING` mirror firmware Kconfig. The first depends on retained
material for its board configuration. They preserve visible content/material
pixels. `solid_blur` compares a complete uniform backdrop against the reference
kernel (including single-pixel nonuniform rejections); `glass_shadow` compares
sparse spans with dense masks. `pacing` exercises real LVGL nonblocking refresh
admission and a virtual measured-TE planner, including jitter, missed slots,
input timers, immediate feedback admission and idle/deletion behavior.

The simulator does not install a fake physical cadence in normal scene tests.
Virtual scheduling tests cannot establish board FPS, SPI cadence or heap margins.
Exact build/test commands, screenshot gate and lead-only A/B commands are in
`docs/agent-briefs/wave2/render-round-results.md`.

The scroll-rate round replaces scripted offsets with a private pointer at the
production path's nominal 16 ms polling cadence, a 240 ms finger stroke, native
momentum and an interruptible Home page spring. `scroll_fixtures` checks actual
input-read counts, skipped reports, live-state isolation, settled redraw and
destruction during momentum. `home_view` covers continuous drag, reversal,
repeated-direction retargeting, toolbar drag cancellation, Reduce Motion,
partial/full redraw equivalence and deletion. The lead handoff and exact board
commands are in `docs/agent-briefs/wave2/scroll-rate-results.md`.

## Band-renderer hotspot experiments

`ORB_RENDER_FUSED_GLASS` and `ORB_RENDER_ROWS_AT_SIZE` are independent,
default-off CMake/Kconfig switches. They keep the existing internal 84-row band
and panel transfer path. The former prepares bounded area-averaged backdrop
maps with a 16 px halo, then fuses AA reconstruction/tint/SDF rim/dither into the
band. The latter builds sixteen glyph coverage sizes from the existing font
system before drawing, blends moving rows directly at size, and keeps static
rows and unsupported content on the raster path. Both are visual experiments;
neither promises exact parity or board throughput from a host result.

`rows_at_size` checks cold/warm atlas admission, redraw and lifetime;
`fused_glass` checks halo dependence, split-band invariance, byte order, clips
and unsupported-source fallback. The profiler reports separate `HOTSPOT`
stages and `ROWATLAS`/`FUSEDGLASS` admission and memory counters. Preparation is
reported even when it runs before display DRAW; inclusive stages are labeled.

`scripts/hotspots_compare.py --reference BUILD --candidate BUILD --output DIR`
captures deterministic Default/AX3/Reduce Transparency scenes and Face/list/card
fixtures, then produces PSNR/pixel differences, magnified crops and a design
contact sheet. Full reproduction, known visual changes and lead-only board
commands are in `docs/agent-briefs/wave2/hotspots-results.md`.

## Temperature units

`sh sim/units-shots.sh` captures Fahrenheit Home summary, Rooms, Climate,
thermostat and sensor details, Units/Temperature settings, and the mandatory
Face/list/approval gate at Default, AX3, Reduce Transparency, AX3/Bold/RT and
AX3/Bold/RT/Reduce Motion. Output is `sim/build/units-gate/` by default.
Both preview binaries accept `fahrenheit`; shell scenes add `units` and
`temperature-settings`. All preference data is synthetic. See
[device results](../docs/agent-briefs/wave2/units-device-results.md) for the
protocol boundary, tests, PNG paths and design review.
