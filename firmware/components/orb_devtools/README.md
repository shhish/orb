# orb_devtools

Developer access to a running Orb over USB or its paired development-channel hub link: screenshots of what LVGL renders, injected touches, and CPU and memory snapshots. It starts itself at boot (an `ESP_SYSTEM_INIT_FN`), so the app doesn't call anything; turn it off with `CONFIG_ORB_DEVTOOLS=n`.

From the Mac mini (the board is on the MacBook Pro's USB):

```bash
scripts/orb-dev.sh shot                  # → shots/<time>.png (round panel, transparent outside)
scripts/orb-dev.sh tap 233 400
scripts/orb-dev.sh swipe 233 380 233 120 300
scripts/orb-dev.sh top                   # CPU % per task over one second
scripts/orb-dev.sh heap                  # internal / DMA / PSRAM free, largest block, low-water mark
scripts/orb-dev.sh --wifi lightsleep usb on # RAM-only bench exception; Wi-Fi transport while USB sleeps
scripts/orb-dev.sh --wifi power          # eligibility, blocker, actual sleep entries/us and IDF lock residency
scripts/orb-dev.sh --wifi lightsleep usb off # revoke immediately; reboot also clears the exception
scripts/orb-dev.sh buttons               # last 16 button edges, monotonic ms, raw/debounced provenance
scripts/orb-dev.sh press pwr double      # synthetic Control Center navigation
scripts/orb-dev.sh press boot hold       # synthetic listening-feedback preview, released after 250 ms
scripts/orb-dev.sh profile 400           # LVGL ftrace, when the built-in profiler is enabled
scripts/orb-dev.sh bench_open apps       # Face → Apps; run first to retain its cold visit
scripts/orb-dev.sh bench_open play       # Apps → Play (also dot, agents, back)
scripts/orb-dev.sh bench_game marble 16000 # also beads, orbit, ring-snake, pop
scripts/orb-dev.sh bench_nav slide-left 16000 # real cached shell navigation
```

`lightsleep usb on|off` is compiled only with `CONFIG_ORB_DEVTOOLS`. It
changes a RAM-only exception, defaults to off, and resets on reboot. Every
other sleep blocker remains enforced. Use the paired Wi-Fi transport for the
USB bench exception: serial traffic cannot be relied on while the MCU sleeps.
`power` reports the latest eligibility decision and reason, wake readiness,
USB exception, successful sleep-entry count and cumulative `sleep_us`. These
are actual IDF sleep-exit callback durations, including entry/exit overhead;
rejected and too-short attempts do not count. The IDF lock/mode table follows.
Explicit queued wake/screenshot/benchmark work gets one UI handler pass while
dark, then the display resumes its notification-only wait.

`bench_nav slide-left|slide-right|stack-up|shade-down|back|iris|rotary MS`
runs the production shell navigation compositor for 1000–60000 ms. It admits
and caches its source surfaces before the timed sample, then supplies positions
every 8 ms. Input samples are not counted as frames. The `NAV` receipt's `fps`
uses successful final-DMA completions divided by the full timed interval; the
final pending frame is drained before sampling, and restoration is excluded.
The `target_fps` and `meets_target` fields report 58 fps for cached transitions
and 55 fps for rotary, without changing renderer pacing or claiming a simulator
result.

`wake` and `goto` queue the normal power-controller request onto the UI task;
they retain its face-down and other wake guards. The console waits up to five
seconds, cancels a pending callback on timeout, and drains any completed result
before reusing the request. This keeps cold scene construction off the 4 KiB
console stack. A rejected wake must be retried normally, never forced.

Admission failures emit `@@ORBDEV ERROR bench_nav reason=... sequence=...
psram_free=... psram_largest=...`. The retained `NAV_ADMISSION` receipt in `heap`
survives restoration and later successful commands. Reasons distinguish `busy`,
`drain`, `workspace_hold`, `source_capture`, `target_capture` and `compositor_begin`.
Invalid command arguments have their own usage error and do not replace the
last runtime failure. Heap sizes are sampled at the failing stage.

`heap` also emits `HEAP_OWNER` used/peak/budget/rejections per PSRAM owner,
`HEAP_CACHE` for the shared optional-cache ceiling, and `HEAP_PSRAM` physical
heap usage versus tracked payload. The residual includes IDF/TLS allocations,
allocator headers and uninstrumented buffers; XIP is outside that heap total.
`HEAP_CACHE margin` is the actual free-PSRAM cache-admission floor, not reserved
memory: required allocations can consume it after admission. It starts at
640 KiB and protects up to 128 KiB of flash-only bitmap savings, after a 16 KiB
metadata/header allowance. Default mapped faces reach 768 KiB; unmapped faces
retain 640 KiB, and assets OTA removes the credit for migrated PSRAM copies.
`HEAP_UNOWNED sound_assets` names the exact signed sound-palette payload already
included in that residual; it does not account for service stacks or IDF buffers.
`HEAP_FONTS` separates resident PSRAM bytes, mapped flash bitmap bytes and bitmap
copies retained in PSRAM before assets OTA. Copies are included in resident;
mapped flash bytes do not consume heap space.
Owner maxima overlap across working modes; see `docs/memory-budget.md`.

The receipt includes mean render/frame, draw, wire, wait and hold times; bytes,
bands, raster builds during the timed window, capture/setup time, heap margins,
and transfer/failed-frame counts. `minimum_internal` is the historical low;
separate `NAV_MEMORY` lines report invocation-only internal/DMA/PSRAM minima,
free bytes and largest blocks for `window=post_prepare_nav`. These include
concurrent radio/link work and exclude preparation/restoration. DMA and internal
views overlap, so never add them. `p95_frame_upper_ms` comes from 4 ms display
latency bins. `p95_cadence_upper_ms` comes from the optional 1 ms rendering
accounting histogram; `accounting=0` means it is unavailable. A percentile of
`-1` means no samples or an unbounded last histogram bin, never a zero-latency
claim. These bins do not establish sub-millisecond cadence or optical response.
The fixture chooses Face/Apps/Agents/Settings as required, regardless of its
starting workspace place. It restores the prior Back stack, selection, nested
page/detail state and scroll, including nested Home and optional service menus,
on completion or cancellation. Active offline games and mandatory system
setup/install/identify screens reject the command. It changes
navigation/scroll position only and never submits a card, voice or Home action.

Wireless access uses the same handlers and console receipts:

```bash
orb-hub dev orb-020000abcdef shot /tmp/orb-wifi.png
orb-hub dev orb-020000abcdef tap 233 400
orb-hub dev orb-020000abcdef swipe 233 380 233 120 300
orb-hub dev orb-020000abcdef bench_scroll agents 16000
orb-hub dev orb-020000abcdef bench_nav apps
orb-hub dev orb-020000abcdef heap
```

The device must have `CONFIG_ORB_DEVTOOLS=y`, a paired authenticated TLS link,
a negotiated `devtools` capability and an accepted `dev` channel. Moving to
`stable` immediately revokes wireless access; moving back to `dev` needs a fresh
hello. No listener is added to the device. The private hub control socket is the
only local entry point; devtools is not a Dot/MCP capability.

Both USB and Wi-Fi touches are synthetic. They can navigate and scroll, but
cannot answer any card, send a voice reply, control Home, install an update,
forget Wi-Fi or unpair. Those actions keep their current state and briefly show
`Answer on Orb`; use the physical touchscreen to confirm. PWR and BOOT only
navigate, wake, or start/stop push to talk. A channel
that is not yet known is not advertised as development: accepting the first
`dev` assignment reconnects once to negotiate the capability.

`buttons` and `press pwr|boot short|double|hold` work over USB and Wi-Fi. Edge
history includes bounce (raw), accepted (debounced) and synthetic provenance;
timestamps are monotonic milliseconds since boot. Injected presses enter the
main task's bounded mailbox and retain `synthetic=1` through release. They may
preview navigation, listening feedback and the power sheet, but never record
voice, confirm a decision, send, restart or operate Home. A synthetic hold is
released after 250 ms so the feedback can be captured without leaving a stuck
button. Real GPIO debouncing and hold timing are covered by host tests;
injection cannot establish that the physical wiring works.

Physical and injected button events share `orb_shell_button`: PWR short locks
through Always On on every surface, and BOOT short opens Apps on the Face or
goes Back elsewhere. `wake`, `goto` and touch commands renew both the shell's
power idle clock and LVGL's inactivity clock. The power adapter observes every
pointer, including the developer pointer created after the hardware pointer.
`goto` wakes before using ordinary shell navigation, including layer dismissal
and history; a rejected wake rejects the jump too. Synthetic provenance remains
unchanged and cannot authorize a protected action.

Stable releases use `scripts/idf.sh --channel stable -j4 build`, which excludes
the entire devtools implementation. Publish `firmware/build-stable/orb.bin`
with `orb-hub release --channel stable`; the release command does not rebuild.

One executor serializes USB and Wi-Fi. Wireless admission is asynchronous, so
benchmark duration never blocks the link receive loop. Strict positional wire
arguments are whitelisted before constructing a console command; arbitrary
console strings and profiling commands are unavailable over Wi-Fi. The Wi-Fi
`devtools.bench_nav` method retains its one-surface alias for `bench_open`; USB `bench_nav` accepts the
gesture and duration described above.

Screenshots retain the indexed 192-byte base64 chunks and CRC32 used by USB;
the Wi-Fi transport groups adjacent lines into bounded text events. Output is
limited to 2 MiB/8192 events/90 seconds and every event acknowledges the actual
bounded TLS write, including its connection generation and current channel.
A disconnect or channel change stops further injected movement and permits only
bounded benchmark restoration; pending output cannot migrate to a new link.
Diagnostic FILE sinks keep power-lock dumps and memory phase receipts in the
same private stream. Formatted command replies keep the caller's absolute
deadline on either transport, including the single whole-trace emission budget;
queued Wi-Fi events expire with that deadline. USB framing and command spellings
remain unchanged.

Host verification (all transports, heaps, touch and LVGL completion are fakes):

```sh
make -C firmware/components/orb_devtools/tests/host test
make -C firmware/components/orb_link/tests/host test
make -C firmware/components/orb_mem/tests/host test
```

- **Screenshots** render the active screen (plus anything on the top layer) into PSRAM with `lv_snapshot_take` only when asked, so they cost nothing per frame; the UI pauses for the render (~100 ms). They show the pixels LVGL produced, not the panel's scan-out, so tearing or brightness problems need the real panel.
- **Touches** come from a second LVGL pointer device, so they go through the same gesture handling as a finger.
- **Memory phases:** `heap` also emits bounded `@@HEAPPHASE` receipts for link,
  TLS/hello, snapshot/card parsing, real voice startup and OTA, plus
  `@@HEAPFAIL` allocation-failure counts. Historical minima remain preserved.
  See [the memory handoff](../../../docs/memory-online.md) for attribution limits
  and the lead's combined 24-session/card/voice acceptance procedure.
  IDF's historical internal minimum adds each region's own low, reached at
  different times. A build with `CONFIG_HEAP_USE_HOOKS=y` also prints
  `@@ORBDEV HEAPTRACE_META`: the lowest *simultaneous* internal free total after
  any task-context internal allocation, plus task and backtrace for each new low
  below 40 KiB (`HEAPTRACE` lines). Ordinary builds compile no hook.
- **Transport:** base64 lines on the console, each tagged with its index, the image with a CRC32; `scripts/orb_dev_remote.py` reassembles it around any log lines and retries a damaged transfer. Input is read straight from the USB-Serial-JTAG FIFO without the console driver, because with the driver installed, logging blocks whenever nothing on the Mac is reading.
  Open receipts/errors and final command acknowledgements use a bounded reliable
  FIFO writer: normal no-driver VFS logging silently drops bytes after 50 ms of
  USB backpressure. Receipts wait/yield for up to 2 s, serialize with stdout,
  and explicitly terminate exact 64-byte USB packets after they drain. The
  formatted open receipt uses temporary PSRAM after sample collection; short
  acknowledgements/errors need no allocation. No console driver, persistent
  ring buffer or longer host command timeout is introduced. Host FIFO/PTY tests
  cover backpressure, fragmentation, interleaved logs and missing hosts; only
  repeated lead board samples can verify this resolves the observed intermittency.
- Selects `LV_USE_SNAPSHOT` and FreeRTOS run-time stats in Kconfig.
- **Profiling:** enable `CONFIG_LV_USE_PROFILER_BUILTIN=y` and disable
  `CONFIG_LV_PROFILER_BUILTIN_DEFAULT_ENABLE` for an instrumented build. The
  `profile MS` command accepts 1–5000 ms, calls `orb_display_profile`, emits
  `=== PROFILE BEGIN ===` / `=== PROFILE END ===`, then the normal
  `@@ORBDEV OK profile MS` acknowledgement. A build without the profiler emits
  `@@ORBDEV ERROR profile disabled: ...` instead. The scrolling demo also
  captures 400 ms automatically three seconds after boot when profiling is
  enabled; let that capture finish before requesting another. Instrumentation
  and console output affect measured timings, especially if the 192 KiB trace
  buffer fills during capture. See `scripts/orb-profile.sh` for conversion and
  per-frame summaries.

The screenshot design follows Muse Gadgets' bench snapshots (Apache-2.0).

`bench_game marble|beads|orbit|ring-snake|pop MS` runs the production game logic and
renderer in a temporary offline scene for 1000–60000 ms. Exit any real game
first. It restores the previous shell screen, leaves accessibility settings
unchanged, and never enters a game's storage namespace or writes a high score.
An atomic reservation also rejects an app launch already waiting for the UI
lock, before the app registry has published that game as active.
The fixed-step model receives deterministic tilt/taps/turns and restarts after
game over, so a long receipt measures moving content throughout. Networking and
other services remain live.
Beads reports `workload=continuous_pour`: a 2.4-second elliptical force trace
(`gx=.55 cos(phase)`, `gy=.4 sin(phase)`) never dwells at one tilt or accrues
clock quiet. Earlier receipts used multi-second constant tilts that let the
physical pile become still; comparisons across that trace change are not an
isolated renderer A/B. Other games report `workload=repeatable_play`.

The `@@ORBDEV GAME` receipt reports successfully completed DMA frames, observed
`fps`, mean `frame_ms` and `draw_ms`, errors, skipped presentation slots, dropped
simulation steps, and cold construction/initial-transfer time separately.
`dirty_px` is transferred RGB565 bytes divided by two per completed frame;
`dirty_pct` uses the full 466² panel bounding square. These count repeated and
overlapping transfers rather than pretending to be a unique union. Renderer
`view_dirty_px` counts requested old/new sprite bounds and is reported separately
from physical output; `view_update_ms` measures model-to-view CPU work. Internal
minimum is the historical boot low-water mark; largest block is sampled at end.
`view_changed_updates` counts update calls producing sprite damage, alongside
all `view_updates`; unchanged pixels are never forced onto the panel to inflate
fps. Label/style damage can also render independently of sprite damage.

`panel_period_us=0` explicitly marks the absence of a live panel measurement.
Games publish at the live panel period (nominally 59 Hz), or 16667 µs without
a sample, while physics remains a fixed 60 Hz. The benchmark uses the same
previous-pose interpolation and active-union snapshot copy as production.
`logic_mean_us`/`logic_max_us` time the actual fixed-step solver, separately from
view update and draw. Beads uses a core-0 worker (`logic_mode=core0_worker`),
matching production's separation of physics and LVGL. Its solver never takes
LVGL or the publication mutex; only bounded active-state/pose copies use that
mutex. The other games report `logic_mode=ui_serial` for their inexpensive
logic. Both paths cap catch-up at six steps and report drops. A timer arms one
pending refresh, then REFR_START samples the latest pose after TE admission;
waiting for a slot no longer accumulates stale sprite damage.
`te_phase_locked=1` requires every completed frame to be scheduled and complete
on slot, with no late or skipped slots; `0` can mean disabled
pacing or an observed miss, distinguished by `CADENCE`.
The `CADENCE` receipt shared by games, navigation and scrolling reports the
successful final-DMA interval distribution in 128 one-millisecond bins, including
an overflow bin for intervals ≥127 ms. It also reports p50/p95/p99 upper bounds,
TE availability and scheduled/on-slot/late/skipped pacing deltas. A percentile
of `-1` means unavailable or in the unbounded overflow bin. The first interval
can cross the sample's start boundary; scroll distributions include settled
gaps and must not be read as moving-only cadence. `dma_timed_frames` and
`dma_timed_bands` count transfers with verified SPI interrupt timestamps.
`basis=spi_isr_verified_endpoints` means every completed frame in the sample
has a verified final-pixel endpoint; otherwise the successful SPI result fence
is an upper bound (`basis=spi_result_fence_upper_bound`). A successful fence
still counts a frame when optional timestamps are unavailable, but cannot
claim verified on-slot completion. `dma_wire_ms`, `dma_queue_ms` and
`dma_cleanup_ms` are per-frame sums for timestamped bands: actual SPI wire
time, submission-to-SPI-start delay, and interrupt-to-task-cleanup delay.
The existing `wire_ms` includes that queue and cleanup cost. The separate `LATENCY`
receipt reports render-to-final-DMA time in 64 four-millisecond bins (overflow
≥252 ms), with the same percentile conventions. Optional `DRAWTASK` and
`HOTSPOT` receipts now cover games and navigation too; worker wall spans overlap
and cannot be added to infer serial frame time.
Host timings cannot establish ESP32-S3 FPS. Board acceptance needs all five
receipts, small dirty areas, zero transfer failures, and the normal internal-RAM
floor while the lead controls the single development board.

`bench_open play|dot|agents|apps|back` uses the real shell's navigation entry,
with `origin=injected_navigation`; it does not claim physical pointer-sampling
or optical photon latency. The source surface settles and its queued SPI work
drains before input time begins. The first scaffold render tags its changed
heading region, and the token travels with every queued band. Deferred `OPEN`
receipts distinguish successful DMA completion of the first changed band,
the first complete changed frame, and the final settled frame. Old transfers,
failed frames and unrelated regions cannot satisfy the probe. Model-space
`motion_end_us` and source preparation remain separate from those DMA times.

The receipt includes actual first-visit-since-boot `cold`, font count/time,
object construction total and longest slice, raster/material builds, draw/wire,
cache payload, and historical internal-RAM minimum. Source preparation can warm
Apps, so measure `apps` first after boot; the emitted classification is
authoritative. Repeat each command for warm samples. Each sample settles Face,
then Apps and Play as needed to establish a fresh source and Back history. It
restores and settles the prior surface before acknowledging completion, so
immediate repeated commands also work when their destination was already open.
It never answers a card or changes accessibility settings.

The completion test requires this sample's nonzero response token and settled
navigation, as well as its DMA timestamps. A timer running before RENDER_START
cannot consume the previous sample's ended receipt. Failures emit
`ERROR bench_open surface=… reason=…`, with source/restoration stage, live
navigation sequence/readiness, response token/timestamps and transfer counters.
Allocation, busy/absent-shell and async-queue failures also name their reason.
The top-task copies and screenshot line are allocated in PSRAM only for their
command; shared benchmark counter snapshots are bounded PSRAM storage. Shell
status, mask descriptors/pixels, material dependencies/payloads and open-cache
owners/pixels likewise use explicit PSRAM allocation, not small generic malloc.

`raster_us` measures source cache construction with draw profiling enabled;
`scale_raster_us` is the separate exact-scale-cache cost. `raster_timing=0`
marks these zeros as unavailable. `glass_mask_builds`, `glass_mask_failures` and
`glass_mask_us` always count/time actual SDF-mask creation. `material_base_builds`
counts retained-material bases; `material_counters=0` means that instrumentation
is unavailable. `glass_us` is material input preparation (capture/downsample/filter
or retained map copy), excluding reconstruction; `glass_timing=0` marks it
unavailable without render accounting. Optional `preparation_us`,
`scene_prepare_us` and `whole_update_us` have their own `accounting_timing`
availability flag. Worker elapsed sums include preemption and may overlap;
these columns must not be added to infer serial input latency.

The lead can add these captures to an already built/flashed A/B case:

```bash
python3 scripts/speed_ab.py measure /absolute/path/to/case --opens --open-runs 20 --fixtures
```

The harness runs opens before synthetic renderer fixtures and writes
`measurement/open.json`, retaining every receipt and nearest-rank p95 with
sample counts for each observed cold/warm class. One first visit is one cold
sample, not a 20-boot distribution. The first complete changed-frame targets
are 35 ms cold / 20 ms warm; both first-band and full-frame timings remain
visible. Board acceptance also requires transition completion within the HIG
spring duration plus one measured panel interval and internal minimum at least
15 KiB. `motion_duration_us` records 550 ms default / 150 ms Reduce Motion, and
`panel_period_us` records the live TE period. The report evaluates transition
completion against their sum. A missing/stale TE sample is zero and produces
`transition_pass:null`, never a fabricated panel interval. Host tests do not
establish those targets.

For a lead-owned same-source before/after run, set `ORB_BENCH_VERSION` to the
current dev-feed version (these USB experiment images must never be published).
Use fresh directories and leave the device on its normal paired Face before
each measurement:

```bash
ORB_OPEN_AB="/tmp/orb-open-ab-$(date +%Y%m%d-%H%M%S)"
python3 scripts/speed_ab.py build "$ORB_OPEN_AB/control" --bench-version "$ORB_BENCH_VERSION" --open-control
python3 scripts/speed_ab.py flash "$ORB_OPEN_AB/control"
python3 scripts/speed_ab.py measure "$ORB_OPEN_AB/control" --opens --open-runs 20 --fixtures
python3 scripts/speed_ab.py build "$ORB_OPEN_AB/fix" --bench-version "$ORB_BENCH_VERSION" --compare-config "$ORB_OPEN_AB/control/sdkconfig.h"
python3 scripts/speed_ab.py flash "$ORB_OPEN_AB/fix"
python3 scripts/speed_ab.py measure "$ORB_OPEN_AB/fix" --opens --open-runs 20 --fixtures
```

`--open-control` disables only `CONFIG_ORB_OPEN_LATENCY_FIXES`; instrumentation
remains present. The normal build explicitly enables the fix. This switch is
independent of the default-off renderer experiments. To collect the optional
source-raster and material-preparation breakdown, repeat both cases in fresh
directories with identical `--enable ORB_DISPLAY_DRAW_PROFILE --enable
ORB_DISPLAY_RENDER_ACCOUNTING` build flags. Keep ordinary and instrumented
latency results separate; profiling itself can affect timing.

### Real shell scrolling

`bench_scroll agents|rooms|settings|notifications|favourites MS` renders the
production workspace and Home views from private synthetic snapshots. Agents
contains eight focus sessions and the `12 idle` disclosure; Home has twelve
rooms; Notifications has ten receipts. `rooms` and the historical `favourites`
selector both exercise Home's named room list with a vertical centre drag
(`workload=touch_fling_settle`). The list's native scroll offset and viewport
supply the same diagnostics as the other list workloads.
No live model reset, preference, answer,
voice-focus or Home action occurs. Physical taps cannot act on fixture rows.
The original screen and Reduce Motion setting return after the run; accessibility
text size, Bold Text and Reduce Transparency stay as selected.
The command requests an ordinary touch wake before preparing its private scene;
face-down or another rejected wake emits `power_wake_rejected`. It never forces
the power state. A sample with no completed moving frames or no list offset
changes fails explicitly instead of reporting a successful zero-frame run.

A private LVGL pointer follows a 240 ms accelerating finger stroke, reporting
every nominal 16 ms, then releases into production LVGL momentum. This models the touch path's configured active polling
period; the CST9217's actual IRQ/report cadence must be measured on the board.
The 1600 ms cycle leaves time to settle. Lists progress to their end before
reversing. Endpoints stay inside each viewport. Missed reports are skipped, never replayed; no direct
scroll offsets or duplicate timer reads are injected.

`SCROLL fps` is completed frames per sampled moving second; `overall_fps`
includes settling. Movement includes tracking and native momentum/spring;
boundary attribution is approximate by one UI sampling interval. `idle_frames`
starts after 750 ms continuously settled (allowing the real scroll mark to fade).
The workload is `touch_fling_settle`. The reporter still accepts historical
`page_swipe` receipts only with zero animation FPS.
Targets are 50 fps for Agents/Settings/Notifications and 30 for Rooms/favourites.

Enable default-off `ORB_DISPLAY_SCROLL_TRACE` for bounded PSRAM input, scroll,
damage, refresh, completion, loop and wait events. Allocation precedes the
sample; the console task emits immutable receipts after capture stops. The
2,048-event detail buffer is a prefix: inspect `dropped` even for a 1–2 second
run. The full-run completion histogram continues after detail fills.

The v2 wire format packs 13 little-endian bytes per event: a microsecond offset
from `started_us` (`uint32`), signed `a`/`b` (`int32`), then kind (`uint8`).
Numbered base64 chunks contain at most 192 decoded bytes and carry individual
CRC32s; the header and terminal `SCROLLTRACE_END` repeat whole-stream byte/chunk
counts and CRC32. Records may cross chunks. One 10-second deadline bounds the
entire trace emission. Report allocation/encoding/transport failure suppresses
terminal OK. `SCROLL_PROGRESS` marks setup and emitting; the host streams raw
bytes immediately, preserving partial failed captures and `@@` prefixes. Its
command deadline is the requested duration plus 25 seconds, with no benchmark
retry. This budget covers setup and bounded emission; it is not a measured board
USB throughput claim.

`scripts/scroll_trace.py INPUT.txt --output OUTPUT.json` accepts legacy text or
validates v2 chunks, full CRC, capture bounds, histogram and terminal receipt.
It reports 1 ms interval histograms and each traced frame gap's observed
waits/unattributed time. Timing spans overlap, physical sample time is after
I²C, and loop elapsed includes lock wait. Events carry no frame identity, so
render-start/completion is not paired. Trace overhead is diagnostic; accept
throughput from the untraced build.

The `SCROLL` receipt also records draw/wire/whole-update milliseconds per
completed frame, transferred dirty pixels and percent of a full round-screen
bounding box, boot-lifetime minimum internal RAM, failures, and both full-window
and moving-window per-core non-idle residency. CPU snapshots occur at motion
boundaries, and moving CPU percentages are weighted by the observed interval.
`cpu_available`, `moving_cpu_available`, `accounting`, and `glass_timing` explicitly
identify unavailable measurements. Enable `ORB_DISPLAY_RENDER_ACCOUNTING` for
whole-update/cadence receipts and `ORB_DISPLAY_DRAW_PROFILE` for BLUR task wall
time plus glass prepare/compose stage time. Worker wall times can overlap and
include preemption; never add them into an inferred frame latency.

Untraced `SCROLL` receipts also expose bench-only cadence counters without
printing or allocation in the measured callbacks: handler/lock/refresh and
requested/actual wait time, animation timer period/ticks/lateness, input reports
and skipped samples, offset changes, round/readable-row callback time and
redundant style writes, invalidated area, UI-task CPU/priority/core, and total
and moving-only frequency observations. These are source-attribution evidence,
not proof that a particular task or PM policy caused a frame gap. Frequency is
sampled before handler work, not time residency. Invalidated rectangles can
overlap; transferred dirty pixels remain separate. Inclusive callback/wait
spans must not be added into frame latency. See
[`trace-capture-results.md`](../../../docs/agent-briefs/wave2/trace-capture-results.md)
for hypotheses, counter meanings and exact lead-only board commands.

Every ordinary (untraced) `SCROLL` line now includes bench-only cadence
observations. Capture starts after fixture warm-up and freezes before receipt
output/teardown; it allocates nothing per event and leaves UI behavior unchanged:

- `handler_us`/`handler_max_us` are UI timer-handler wall time; `lock_us` is
  time waiting for the LVGL mutex. `handler_outside_refresh_us` subtracts only
  refresh spans completed within each matched handler, revealing work such as
  input and scroll callbacks outside DRAW. Whole-sample `refresh_us` includes
  refreshes crossing capture boundaries; do not subtract it from `handler_us`.
- `timer_wait_us`, `fairness_wait_us`, `wait_requested_us`, `wait_overrun_us`
  and `wait_max_us` compare requested sleeps with observed wake time. Overrun
  includes timer dispatch, scheduling and preemption; it does not identify a
  particular competing task. `ui_cpu_available`/`ui_cpu_pct` use begin/end UI-task
  runtime counters, with wrap handling and boundary-slice uncertainty. Compare
  these with existing per-core residency, `ui_priority`/`ui_core` and lock/wait
  evidence before blaming Wi-Fi. No task list is allocated during motion.
- `anim_period_ms`, `anim_ticks`, `anim_gap_max_us`, `anim_late_max_ms` observe
  the real LVGL animation timer's last-run changes/lateness. A paused timer
  resets the gap origin; this timer serves all animations, not only scroll throw.
  `indev_period_ms` is the configured production LVGL input period; the fixture
  uses manual EVENT-mode reports. It is not a measured controller frequency.
- `round_calls/us/max_us/rows` and `readable_calls/us/max_us/rows` capture the
  actual round-list and readable-row callbacks, including layout-triggered calls
  (`readable_layout_calls`). Their wall spans may nest and must not be added.
  `style_writes` counts opacity/alignment setters, including the too-tall-font
  fallback; `redundant_style_writes` compares values immediately before each
  setter. `font_changes` counts actual font transitions. None of the setters
  are suppressed by this instrumentation.
- `invalidations`/`invalidated_pixels` count requested aligned areas before
  merging; overlapping/duplicate requests count repeatedly. Compare with
  `refreshes`, `render_starts`, completed frames and transferred `dirty_pixels`
  to distinguish redundant invalidation from render cost.
- `input_reports/releases/skipped/gap_max_ms` describe synthetic reports.
  Skipped is nominal 16 ms down slots not delivered as of the latest fixture
  tick, including entirely missed strokes; no catch-up is performed.
  `scroll_events`, `offset_changes`, `offset_gap_max_ms` observe real list scrolls;
  a new gesture resets the gap origin so the deliberate settle is excluded.
  These list-offset counters are zero for the custom Home page spring.
- `freq_samples/below_max/min_mhz/max_mhz` sample CPU frequency before each
  handler. `moving_freq_*` restricts this to the previous sampled movement
  window, with the same boundary uncertainty as moving FPS/CPU. Zero samples
  means unavailable, not a zero-MHz observation. Sampling is opportunistic,
  not continuous residency, and can miss short DFS transitions.

These counters cover the full timed sample (except explicit `moving_freq_*`),
including settling. Wall times include preemption. They are evidence for
ranking causes; they do not by themselves establish scheduler or radio causality.

Lead-owned board commands (no board was flashed by the implementing agent):

```sh
scripts/orb-dev.sh bench_scroll agents 16000
scripts/orb-dev.sh bench_scroll rooms 16000
scripts/orb-dev.sh bench_scroll settings 16000
scripts/orb-dev.sh bench_scroll notifications 16000
scripts/orb-dev.sh bench_scroll favourites 16000
```

`python3 scripts/speed_ab.py measure CASE --surfaces` captures all five surfaces
three times under its existing exact-ELF, online-settle and no-new-OTA guard;
`--surfaces agents rooms` selects a subset. `measurement/surfaces.json` keeps
motion FPS, true idle tails, memory gates and timing availability; per-surface
accounting/cadence remains in `measurement/accounting.json`. The harness rejects
faulted, incomplete, contradictory or unavailable-but-nonzero receipts. A
subtarget result is retained as a failing gate, never described as a pass.

For a same-source control, leave `ORB_UI_SCROLL_EDGE_SOFT` and
`ORB_DISPLAY_GLASS_SOLID_FAST` disabled in `speed_ab.py build`; enable them
explicitly in the candidate. Keep `ORB_RENDER_ROWS_AT_SIZE`, accounting and
profiling identical in both. Add `ORB_DISPLAY_TE_PACING` only in a third case to
isolate its cadence/throughput cost. Keep it off by default until lead measurements
show a win. Always use the current dev-feed version with `--bench-version`.


Pass 4 diagnostics allocate the `heap` command's mask/display counter snapshots
in one bounded PSRAM block on demand. The basic heap, cache, owner, navigation
and phase receipts remain available if allocation fails; optional mask/display
detail is then replaced by `HEAP_DETAIL available=0 reason=psram_scratch_oom`.
There is no internal-heap fallback and no persistent scratch allocation.

`GAME_CPU` reports the benchmark Beads worker's aggregate scheduled task time,
including solver, pose capture/publication, loop overhead and ISR time charged
to that task. `worker_mean_upper_us` divides the complete worker lifetime by
completed solver steps, so it is an upper bound for mean solver CPU, not an
exclusive solver-only measurement. Ordinary `logic_mean_us`/`logic_max_us`
remain solver-call wall spans. Sampling waits at most 5 ms after the worker's
done semaphore for its self-suspension, so the final scheduler slice is committed
before reading its still-owned TCB. A join failure, missing suspension, unsupported
runtime clock or a window crossing the FreeRTOS runtime-clock wrap leaves the
CPU receipt unavailable. The sender's runtime is explicitly unavailable; its
wall-time wire/wait receipts must not be interpreted as sender CPU.
