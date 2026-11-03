# orb_display

LVGL on the round panel: the render loop, bands, the wire, touch, and the
renderer's stats.

- **Bands in internal RAM.** LVGL draws in partial mode into one 84-row
  buffer (78,288 bytes) in internal DMA RAM. LVGL waits for the completed DMA
  transfer before reusing it. The second buffer exhausted BLE controller RAM
  during provisioning; removing it retains glass-safe bands and frees 76.45 KiB.
  Drawing stays in internal RAM; rendering and transmission no longer overlap.
- **Round bands.** Through the fork's band hook (`ORB_PATCHES.md`) each band
  covers only the columns the circle shows in its rows, so the square's corners
  are never drawn or sent, and narrow bands near the top and bottom take more
  rows. A band guard (`orb_display_set_band_guard`, e.g. `orb_glass_band_guard`)
  keeps anything whose drawing reads all of itself (backdrop blur) in one band.
- **The wire.** Bands go to the panel through `orb_board_panel_write()`: one
  queued chain of column, row and pixel transactions per band.
- **No tearing.** The panel scans its memory every 16.8 ms; each band is held
  until its write won't cross the scan (`te_hold`, the scan model in
  `orb_display_geom.c`, after Muse Gadgets' `muse_lcd_bands.c`).
- **Touch off the UI task.** A small task reads the controller when its
  interrupt fires (and every 16 ms while a finger is down); LVGL reads the
  cached point, so an I2C read never stalls a frame.
- **Raster caches in PSRAM.** `lv_obj_set_raster()` bitmaps are allocated in
  PSRAM (`lv_obj_raster_set_allocator`).
- **Render loop.** LVGL runs on core 1. Dirty refreshes use a 1 ms admission
  timer; clean surfaces keep LVGL's paused refresh timer. After releasing LVGL,
  a one-shot `esp_timer` blocks the task for the next deadline (at most 10 ms),
  or 100 µs when already overdue. This gives console and draw workers a real
  unlocked handoff without adding a mandatory 10 ms tick at 100 Hz. Timer
  allocation/start failure falls back to RTOS ticks and is traced; creation
  failure logs a warning.

## API

`include/orb_display.h`:

- `orb_display_start()`: panel, LVGL, its task, touch. Anything touching LVGL
  from another task takes `lv_lock()`.
- `orb_display_brightness(pct)`: queued behind bands, never cutting one.
- `orb_display_set_brightness(pct)`: the same queue with a drained command result.
- `orb_display_panel_awake(awake)` / `orb_display_panel_ready(awake)`: request
  CO5300 sleep/wake and observe completion. The sender advances the 120 ms
  sleep-command guards without blocking the UI; interrupted transitions retain
  the latest request. Wake stays dark until the power adapter's safe-frame fence.
- `orb_display_set_panel_on(on)`: retain damage while dark without drawing or
  flushing. This hard gate also applies to forced devtools captures. The dark
  UI waits 100 ms; physical touch/button notifications interrupt that wait.
- `orb_display_set_band_guard(guard)`.
- `orb_display_stats(&st)`: averages since the last call: fps, frame time and
  how it splits (LVGL drawing, LVGL waiting for the wire), the sender's hold and
  wire time, bands and KB per frame, torn bands.
- `orb_display_profile(ms)` with `CONFIG_LV_USE_PROFILER_BUILTIN`: traces LVGL
  and prints an ftrace-format trace to the console.

`orb_display_geom.c` (no ESP-IDF or LVGL): the circle's chords, the band
choice and the scan model, tested on the host.

## Required sdkconfig

```
CONFIG_LV_OS_FREERTOS=y
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2
CONFIG_LV_DRAW_THREAD_STACK_SIZE=4096
CONFIG_LV_DEF_REFR_PERIOD=16
CONFIG_LV_USE_CLIB_MALLOC=y
CONFIG_SPIRAM=y
```

The fork (`third_party/lvgl__lvgl`) comes through `idf_component.yml`.

## Integration

Call `orb_board_init()`, then `orb_display_start()`; build the UI with
`lv_lock()` held or from the LVGL task (`lv_async_call`), which has the larger
stack that drawing the first raster caches needs. The integrated system uses
one buffer and a 1 KiB generic-malloc internal threshold. In the initial
60-second provisioning smoke test, internal free settled at 45 KiB, largest
block 31 KiB, and minimum 11 KiB. These are setup-screen measurements, not
networked rendering throughput; see `docs/HANDOFF.md` for final validation.

## Tests

- Host: `make -C tests/host test` (macOS clang).
- Build: `test_app/` (`idf.py -C test_app set-target esp32s3 build`), which
  brings the panel up with one raster-cached card and logs stats.

## Real-surface attribution and solid backdrops

Real-surface benchmark receipts sample each core's pinned idle task around the
measurement window with `orb_display_cpu_snapshot()`. `orb_display_cpu_busy()`
uses idle-runtime deltas divided by one core's FreeRTOS runtime-clock delta,
with native 32/64-bit wrap handling. It reports non-idle task residency for each
core independently; it does not assign migrating draw tasks to their affinity,
halve a core's percentage by the number of cores, or imply frequency residency.
Runtime updates at task switches, so windows should be at least 500 ms; the
boundary can carry an idle tick. Missing runtime-stat support is unavailable,
never an invented zero. This sampling is independent of DRAW_PROFILE.

The exact uniform-backdrop skip is enabled by default. It proves every pixel
of each actual clipped RGB565 control rectangle, including partial bands, with
an alignment-safe first-row proof and bulk comparisons of the remaining rows.
Stride padding and neighbouring content are excluded. One changed pixel falls
back to the ordinary blur. Tint, rounded AA and screen dither stay identical;
`sim/tests/test_solid_blur.c` compares the complete guarded output to that oracle.

`ORB_DISPLAY_SCROLL_TRACE` is a default-off diagnostic for `bench_scroll`.
It records physical touch sample/read, injected pointer sample/release, scroll
offset, dirty y range, refresh begin/end, render start, successful final DMA,
handler begin/end and unlocked wait begin/end timestamps. The fixed 2,048-event
PSRAM arena is admitted before the measurement and reused, never allocated per
frame. It retains a prefix; even 1–2 second samples can fill it. After it fills,
`dropped` grows while the 1 ms complete-frame interval histogram and handler/wait
totals keep counting. The last histogram bin includes ≥127 ms; the histogram
contains `completions - 1` intervals, including settled gaps.

Capture stops before report/teardown. Events timestamped before its start,
including an in-flight cross-core sample, are excluded. The console task emits
v2 records after the benchmark restores the original screen: explicit packed
little-endian `uint32` relative microseconds, signed `int32` a/b and `uint8`
kind (13 bytes). Numbered base64 chunks carry at most 192 decoded bytes and a
CRC32; header and end repeat the complete stream's byte/chunk counts and CRC32.
The emitter has one 10-second deadline and suppresses OK after report failures.
The host streams raw progress/receipts with a duration-plus-25-second timeout.
`scripts/scroll_trace.py` validates v2 receipts and also accepts legacy text.
Two cores can append slightly out of timestamp order; analysis sorts records.
WAIT reason 1 is a pending timer, reason 2 is the 100 µs overdue fairness handoff.
These observed spans overlap rendering/DMA and do not claim precise CPU ownership.
Capture with tracing for attribution, then disable it for throughput gates.

The independent `orb_display_cadence_*` counters run only during `bench_scroll`,
including untraced builds. `SCROLL` reports UI lock/handler/refresh/wait timings,
requested waits and overruns, invalidation count/area, animation timer service,
UI-task CPU/priority/core, and sampled CPU frequency before handler work. The
receipt combines those with row callback/style-write and fixture input/offset
counts. Sampling frequency does not establish residency; animation ticks
observe the timer's `last_run`, not individual throw callbacks. Inclusive wall
spans contain preemption and can overlap. See
[`trace-capture-results.md`](../../../docs/agent-briefs/wave2/trace-capture-results.md)
for the ranked source hypotheses and lead-only board commands.

Touch interrupts still wake the controller task immediately. Its fallback poll
uses a separate one-shot timer: nominally 16 ms after the previous read while
pressed, 250 ms otherwise. This avoids `pdMS_TO_TICKS(16)` truncating to 10 ms
on a 100 Hz scheduler. Reads, I²C work and timer dispatch can shift those times;
they are not a measurement of the CST9217's actual interrupt/report frequency.

## TE pacing

`ORB_DISPLAY_TE_PACING` is disabled by the firmware defaults after the Pass 3
board A/B: live lists and games regress under admission. Per-band scan avoidance
remains active, and games retain absolute panel-period producer deadlines.
The option remains available for measured experiments. When enabled, dirty timer refreshes
are admitted before layout/raster preparation on the measured awake-panel TE
grid. Both scrolling and games can use T. An immediate feedback frame measures
work; that cold probe is excluded from the warm work window. Eight initial warm
presentations, then 16-sample blocks, admit a stable integer TE divisor using
the second-largest observation. One isolated interference sample stays visible
as a miss without lowering the whole animation rate. Deliberate scan/deadline
waits are excluded from capacity prediction while real HOLD/late/cadence remain
unchanged. Missing a start window admits work for the next feasible deadline
immediately, avoiding starvation by input/model timers. Millisecond gate retries
round down; the final <=2 ms wait uses the microsecond clock.
Input and spring timers keep their monotonic clock and run during gate retries.
Expired slots are skipped before rendering; no stale frame queue is created.
Only the last band is held for its completion deadline, retaining the existing
scan-avoidance rule, internal buffer ownership, actual SPI fence and PM lease.
Sleep/missing TE and synchronous captures remain available as explicit unpaced
frames. No damage means no TE work or redraw.

With render accounting enabled, deferred `PACING` receipts report phase deltas
for scheduled/on-slot/late/skipped/boundary/unpaced/30-Hz-admission counts and
the last period/divisor. `max_error_us` is the lifetime high-water error, not a
phase delta. On-slot requires successful final SPI completion within 1 ms of
the target. Immediate feedback and forced frames break completion-interval pairs.
The normal DRAW/WAIT/HOLD/WIRE and causal open measurements remain separate.
The scheduler does not make slow content fast: missed slots, late completions
and actual divisors remain visible. `orb_display_counters` always includes a
1 ms final-DMA spacing histogram and the existing 4 ms render-latency histogram,
independent of profiling/accounting. Games coalesce pending damage and sample
their newest interpolated pose at REFR_START, after admission. See
`docs/agent-briefs/wave2/fluid-results.md` for board evidence and limitations.


### Pass 2 producer/service admission

The deadline planner retains separate measured **lead** (gate to final DMA,
excluding intentional holds) and **capacity** (the larger of UI preparation +
drawing and accumulated sender setup/wire/result-drain service). Overlapped
latency can exceed one panel period while both lanes sustain one frame per
period; only capacity selects the integer divisor. Lead still determines when
to begin. Both windows retain the existing eight/16-sample admission policy,
second-largest warm observation and500µs margin. Invalid/failed work does not
become a successful capacity observation. CADENCE reports both estimates.

Certified immutable snapshot navigation bypasses the live producer gate and
keeps ordinary per-band scan avoidance. It does not claim whole-frame phase
lock. Live scenes still use admission; no unchanged scene is redrawn to raise
FPS. Two half-bands still alias only the original internal arena, and all buffer
switches fence queued DMA before reuse.

The sender uses a private microsecond notification timer for scan/deadline
holds, spinning only for the final <=200µs. The actual firmware uses a1kHz
RTOS; tests also exercise100Hz fallback. The sender runs on core0 so its SPI
setup/result cleanup can overlap UI work on core1. The SPI IRQ remains on
core1; result queues fence cross-core descriptor and timing ownership. A
nonzero-tick fallback rechecks time if timer setup fails. Board results, including
contention with the core0 Beads fixture, are recorded in fluid-results.md.

Render accounting, draw profiling and scroll tracing remain default-off
production diagnostics. Existing successful-final-DMA counters and bounded
interval buckets are independent of those optional diagnostics.
