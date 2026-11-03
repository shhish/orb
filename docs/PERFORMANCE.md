# Orb core optimization — measured checkpoint

Tested on the paired development ESP32-S3 AMOLED board on October 26, 2026.
Accepted firmware: **0.3.0-9690379**, build **3c24572**, source branch
`orb/integration`. Main remains 48faeb0; nothing was pushed or merged into main.

## Results on the board

The same bounded 16-second scene ran twice per build, with networking active:
approximately 8 seconds of scrolling followed by 8 seconds of animated glass.
Timing ends at actual final DMA completion. This is a stress scene, not a claim
that every screen runs at these rates. Historical renderer-only 50–58 FPS numbers
do not describe this integrated firmware.

| Metric | Baseline d0956c5 | First pass b54551f | Accepted 9690379 |
| --- | ---: | ---: | ---: |
| Scrolling FPS |14.92 /15.01 |18.92 /19.02 |18.56 /18.57 |
| Scrolling draw, ms/frame |53.77 /53.60 |39.49 /39.46 |40.58 /40.34 |
| Glass FPS |16.38 /17.97 |25.82 /25.30 |39.95 /40.45 |
| Glass draw, ms/frame |48.61 /46.23 |29.17 /29.54 |15.26 /15.24 |
| Internal free after returning home |71,963 B |71,915 B |68,627–68,663 B |
| Largest internal free block |31,744 B |31,744 B |31,744 B |

The accepted build improves scrolling by about **24%** and glass throughput by
about **2.3×** versus the original integrated baseline. Exact caching trades
about 2% of scrolling throughput relative to the first optimization pass for
the much larger glass gain. That tradeoff is visible here rather than hidden
behind one favorable benchmark.

Accepted glass phases verified about 90% exact cache hits. Scrolling hit about
32%, so it remains primarily a drawing bottleneck. The cache settled at
163,124 bytes of retained payload, below its 196,608-byte cap, with no allocation
failure, busy bypass or oversize bypass in these scenes.

Accepted frame p95 fell in the 72 ms bucket for scrolling and 28–32 ms buckets for
glass. The histogram did not exist in the original baseline, so there is no
baseline p95 comparison. Buckets are 4 ms wide; the final bucket is **≥252 ms**
with no finite upper bound. These are render-to-transfer timings, not touch
latency. Phase-boundary in-flight frames can shift a sample slightly.

The original home screen redrew about 1.5–2.5 times/second while physically idle.
The optimized home was quiet for separate 25-second samples apart from genuine
status/clock changes. A first-pass one-second CPU sample changed from 91.5% to
99.2% aggregate idle capacity across both cores. CPU utilization is not a power
measurement.

## What changed

### Rendering, state delivery and memory

- Exact RGB565 blur acceleration shares interpolation work, specializes the
  small kernel and uses exact conversion/tint tables. It reduces three scratch
  allocations to one. Host blur is about 1.7× faster; the complete device scene
  improves less because it includes other work. Typical 250×72 scratch grows
  by1,290 bytes, in PSRAM.
- A four-entry cache stores the unblurred source and its final blurred output.
  Every hit requires the same geometry, clip, format, radii, quality, tint and
  opacity **and a byte-for-byte match of the current input**. Moving content
  therefore cannot receive stale pixels. Entry leases protect concurrent draw
  workers; allocation, comparison and rendering happen outside the metadata
  lock. Retained and reserved payload is bounded to 192 KiB of explicit PSRAM;
  allocator headers and fixed metadata are additional.
- Main callbacks mark status dirty and wake the owner task. The UI consumes
  changed snapshots; fallback clock/RSSI sampling is 1 Hz. Motion/buttons retain
  their 20 ms cadence. Repeated status, small RSSI changes, unchanged progress
  structure and meaningless near-zero light angles no longer rebuild/redraw.
- Font compressed staging is freed before recursively loading fallbacks.
  A constrained real-loader test reduces a synthetic chain's peak from 18,432
  to 14,336 bytes. A real-pack lifetime model suggests up to 73,274 bytes saved on
  a cold chain; this is a model, not a measured board heap saving.
- Keep one 78,288-byte internal draw band. A second full internal buffer caused
  the original BLE `ESP_ERR_NO_MEM`. LVGL waits on a semaphore, and the sender
  drains SPI results before allowing reuse. Failed bands are never counted as
  successful frames.
- Fixed benchmark screen/timer cleanup and atomic draw-worker shutdown make
  repeatable measurements and simulator teardown safe. CPU diagnostics use
  copied task identities, avoiding pointers into tasks that can disappear.

### Clock policy

The accepted configuration allows 80–240 MHz CPU operation and disables automatic
light sleep. FreeRTOS requests full speed for runnable work. An additional
balanced CPU_MAX reference covers each actual display band through queueing,
TE waiting and DMA completion; it releases after returning the buffer/waking
the UI. This restored wire time from about 12.1 ms/frame in the initial DFS trial
to about 10.8 ms in scrolling and avoids lock leaks for clipped/empty renders.

The `power` diagnostic confirms 80 MHz minimum, 240 MHz maximum and no light sleep.
The transport lock returned to Active=0 after more than 11,000 transfers in a
60-second soak. Across a separate 25.742-second quiet interval, mode-counter deltas show **73.2% at 80 MHz** and **26.8% at 240 MHz**. The display lock stayed inactive and its
acquisition count did not change.
No current meter was used: **no battery-life or mA saving is claimed**.

### Bounded networking and resource ownership

- Hub requests now reject more than 16 in flight, matching the firmware table.
  One deadline covers send-lock acquisition, transport backpressure and reply.
  Cancellation, disconnect and timeout remove/consume pending futures. A stalled
  transport previously retained 64requests beyond their deadlines; the regression
  now finishes all 64, with 48busy rejections and 16 timeouts, and leaves zero pending.
- Firmware control state lives outside its 32-entry payload queue. Disconnect
  survives queue saturation. Losing a valid application frame or its allocation
  forces a fresh connection instead of leaving a plausible stale UI. A graceful
  close still drains already accepted pairing/final events. Old-connection
  fragments cannot corrupt a new assembler. Protocol-invalid oversized messages
  retain the documented drop behavior.
- Audio construction checks every acquisition, publishes output handles only
  on complete success, and unwinds wrappers, codec/interfaces and both I2S
  channels after failure. Failure tests include partially allocated TX-only
  channels and immediate retry. Audio is not yet running on the board.

## Experiments deliberately not enabled

**Direct PSRAM display DMA:** two fixed snapshots were intended to overlap
rendering with transfer while preserving scarce internal RAM. At both 80 MHz
and 40 MHz external clocks, the real board reported DMA TX underflow during
startup. The 40 MHz failure had queue=ESP_OK, completion=ESP_ERR_INVALID_STATE
and a 60,480byte PSRAM source. The automatic fallback worked and the board
reconnected. This mode remains default-off; accepted builds allocate neither
snapshot, recovering 156,672 bytes PSRAM. Do not infer target support/performance
from host tests alone.

**Adaptive cache admission:** skipping four calls after repeated source changes
reduced copies in host tests but did not improve physical scrolling and slightly
reduced glass throughput. It was removed in 9690379. The accepted cache has no
cooldown heuristic.

No speculative TLS-buffer shrink, task-stack reduction, BLE capacity reduction
or heap-IRAM relocation was enabled. Fonts remain about 1.7 MB of image content;
moving all defaults to the independently updated asset partition would break
the fallback guarantees of app-only OTA. A future compressed built-in/rescue
font architecture must preserve atomic signed application ownership.

## Verification and operational state

- **39/39 simulator CTests**, **120 hub tests**, **254 development-tool tests** pass.
- Exact blur: 800randomized fixtures,6.67 MB of golden output; cache-enabled
  comparison 700,992 assertions. Retained cache passes 98,287 checks under both
  ASan/UBSan and ThreadSanitizer. Existing blur regression adds 65,786 checks.
- Link: 28,925 host checks plus 7,714actual-callback fault checks under ASan/UBSan.
  Audio: 1,697ownership/failure/retry checks. Display ownership/SPI failure and
  PM balance tests cover partial queue failures, dropped transfers, overlapping
  jobs, empty/clipped endings and failed lock acquisition.
- Final ESP-IDF 6.1 build passes. Signed image is 3,674,112 bytes (`0x381000`),
  leaving 508 KiB in each 4 MiB OTA slot. Secure Boot eFuses were not changed.
- Multiple signed OTA installs completed with accepted hello and healthy boot.
  A 30-second rendering stress scene ran while downloading the 1203872 image.
  Attempt minimum was 36,663 internal bytes and 28,875 internal DMA bytes, with
  2,896 bytes installer-stack margin. These heap regions overlap; do not add them.
  Animation slowed during flash/update work; normal FPS is not promised during OTA.
- Two 16-second benchmarks followed by a 60-second repeated-scene soak on 1203872
  returned to 68,663 internal free bytes each time; PSRAM changed by only 120 bytes
  between warm samples. The accepted cache simplification was then rechecked
  with two 16-second physical runs. Its first 4,079bands completed without a
  transfer error or failed frame. No memory failure, panic or watchdog appeared.
- Scan-crossing estimates occasionally incremented (including baseline and
  accepted scrolling). They are not optical proof of tearing. Physical-panel
  shadow/ghosting review remains in `ux-feedback.md`.
- Two developer-console reads timed out during this long session; retries
  succeeded while the device stayed online. No PM lock inversion or persistent
  runtime fault was found; the timeout cause is not proven. Keep this diagnostic
  transport observation separate from the measured firmware results.

An acknowledged hub-issued reboot of the accepted image was followed by a fresh
accepted hello, with Wi-Fi and pairing preserved. The final screenshot shows the
connected home screen.

The device remains paired on the development channel. Hub requests run from
the integration worktree on port 8444; port 8443 belongs to nginx. The signing
key is still the isolated development test key. Permanent-key bootstrap and
any Keychain operation remain owner tasks. Main is untouched and all worktrees
were clean at the final code inventory.

## Reproduce and extend

Use `scripts/orb-dev.sh bench 16000`, `heap`, `top` and `power`. Capture idle
power snapshots before and after a quiet interval; subtract cumulative mode
times rather than attributing the whole boot to idle. Leave brightness, scene,
radio state and compiler configuration constant for comparisons. Measure
screenshots separately because they change memory demand.

Preserved baseline: branch `orb/perf-baseline`, head d0956c5. Raw private evidence
is in this Codex chat's `work/core/`; sanitizer/audit evidence is in sibling
`work/blur-cache/`, `work/hub-request/`, `work/integration-review/`,
`work/audio-review/` and `work/band-power-review/` folders. Do not publish raw
provisioning images or secrets. The owner-facing report is copied to the chat's
outputs folder.

Next work is bounded session/attention/message/choice cards, then push-to-talk.
Reserve internal audio DMA separately: the current I2S settings need at least
5,760 sample-buffer bytes plus descriptors, task stacks and codec state. Keep
larger stream/jitter buffers in PSRAM. Measure combined voice, large font sets,
reconnect and OTA before claiming those workloads fit. This checkpoint tests
the paired development board, not every future workload or fresh provisioning
configuration.

## Wave 2 round 2 (October 27, 2026)

The lead reproduced accepted control **18.43–18.69 fps scroll / 40.02–40.24 fps
glass**, **40.10–40.61 ms scroll DRAW** after settling. Round 1 A–G timings are
contaminated by automatic OTA: A starts installing the dev feed at 11.553 seconds,
writes flash at 12.553 seconds, and `devices-before/after.json` show download
progress 10%→70% across the benchmark. OTA's 8 KiB stack plus HTTP/TLS working set
plausibly explains most of A's ~11 KiB internal deficit; flash/cache contention
and preemption inflate elapsed DRAW. Exact cost attribution remains unmeasured.
No disabled-option source change explains that magnitude. The custom config and
`-B build` path are valid, but 20-second boot logging did not establish stability.

Round 2 keeps all options default-off. Draw scratch now has a dedicated bounded
internal-first allocator with explicit PSRAM fallback, independent image/blur/
mask switches and allocation/fallback counters. General LVGL objects, raster
bitmaps and backdrop-cache payloads retain their policy. Internal scratch is
charged against a shared 16 KiB cap and 24 KiB free / 8 KiB contiguous-DMA admission
margins. Typical two-image payload is ≤7,456 B; image+capsule blur ≤11,504 B.

The infeasible late second 78,288 B buffer is replaced by an early 78,336 B arena
(**+48 B**): two 39,144 B usable half-bands for complete small invalidations, one full
84-row band otherwise. DMA is drained before layouts alias. This can admit
small glass overlap without another large contiguous block; it deliberately
retains full-screen scroll's band geometry to avoid blur seams. Periodic counters
show actual half/full use. Neither admission snapshot guarantees future audio,
TLS reconnect or OTA capacity; audio samples still need at least 5,760 B plus other
resources. Direct PSRAM DMA remains disabled.

The lead-only `scripts/speed_ab.py` uses fresh configs and saves build provenance,
requires exact firmware/ELF identity, 90 seconds continuously observed online,
and no new OTA throughout measurement. USB experiments reuse the current feed
version through IDF's existing version Kconfig, avoiding automatic replacement
without changing the live hub. These reused-version images must not be published.
See the [round 2 speed plan](agent-briefs/wave2/speed-plan.md) for the short control,
image-only, blur-only, combined, then adaptive-buffer sequence and exact commands.

Host validation includes **40/40 simulator CTests**, byte-identical adaptive
rendering, 714 scratch allocation checks, 530 admission/fence checks, 102 timing
checks, and 2,052 Kconfig checks. Full host suite results and screenshots are in
the speed plan. The sandbox did not run ESP-IDF, flash, touch the board or measure
FPS. The ~45/55 fps targets remain unverified; 40 ms scroll DRAW still caps perfect
overlap near 25 fps. Work remains uncommitted for lead review.

## Primary references

- Espressif [SPI transfers from PSRAM](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#transactions-with-data-on-psram)
  documents shared-bandwidth underflow constraints; installed IDF driver source
  was also inspected for alignment, cache synchronization and completion errors.
- LVGL [display setup](https://lvgl.io/docs/open/main-modules/display/setup)
  describes buffer ownership, flush completion and render/transfer overlap.
- Espressif [power management](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/power_management.html)
  documents CPU/APB locks, runnable-work behavior and interrupt-latency costs.
- Espressif [RAM usage](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/performance/ram-usage.html)
  supports measuring heap fragmentation and stack margins before reducing them.

These references informed experiments; acceptance was based on actual source,
failure tests and the physical board measurements above.


## Wave 2 round 3 implementation (host validated, device A/B pending)

The lead's [board profile](agent-briefs/wave2/speed-round3-results.md) attributes
~21.1 ms/frame of scroll task elapsed time to BLUR and~15.4 ms to transformed
rows, with zero source raster builds. These are overlapping worker wall times,
not an additive critical-path decomposition. The existing ~18.5–18.8 fps scroll
and~40 fps glass measurements remain the only board results for this round.

The apparent eight scroll blurs are **three real material operations**: cache
hits147+misses306=453=3×151frames. Other BLUR tasks return empty when shadow
padding intersects another band. Existing whole-control guards already preserve
full control sampling; blindly caching by object/frame would risk stale or
incomplete input from an unwritten band. Default-off `GLASS_SINGLE_PASS` culls
those empty tasks before dispatch. No multi-kernel saving is assigned to it.

Four independent default-off performance options now let the lead isolate work:

- `ORB_DISPLAY_GLASS_MOTION_QUALITY`: averaged4×4 samples during actual scroll or
  parallax, normal factor3 after120ms quiet with an explicit redraw. Full-size AA,
  bilinear reconstruction,8-bit channels and screen Bayer dither remain. Sampling
  factor belongs to the exact cache key. About68B static S3 metadata plus
  timer/event heap; scratch decreases, no new retained image payload.
- `ORB_DISPLAY_GLASS_SINGLE_PASS`: empty-band task culling, no persistent payload.
- `ORB_DISPLAY_RGB565_SCALE_FAST`: exact scale-only opaque RGB565/SWAPPED
  specialization, hoisted row math and integer X accumulator. Same checked edges
  and two-stage5-bit interpolation rounding; scalar locals only, no SIMD assembly
  or lookup allocation. This is the first scroll candidate because exact variant
  hits are rare while scales continuously change.
- `ORB_DISPLAY_RASTER_SCALE_CACHE`: exact1/256-scale LRU, exact per-frame opacity,
  source-revision/size/format invalidation. Verified opaque RGB565 core uses plain
  IMAGE and existing `orb_lv_simd`; edge strips retain exact RGB565+A8 coverage.
  Current-frame hits are pinned before miss admission. Retained payload+metadata
  is capped512KiB (configurable64–1024KiB), temporary construction~84KiB PSRAM for
  demo; no extra internal DMA. Source rasters~565KiB remain separate; raster stats
  include variants. OOM/oversize/pinned budget pressure safely falls back.

`ORB_DISPLAY_RASTER_COVER_CHECK` defaults **on** as a correctness repair. A faded
or scaled cached root and its opaque source children cannot cover the backdrop.
The imported glass-stream partial/full redraw fixture goes from65failing frozen
frames to0out of167, preventing stale bottom-bezel pixels. The option permits old
behavior reproduction; it is not a performance win.

The scaled cache builds before RENDER_START. Both native timing and new deferred
`SCALECACHE` records retain pre-DRAW creation cost, hit/miss/build counts, band
blits and memory. Source `RASTER builds` and bounded draw profiling are preserved.
A lower DRAW with a cold variant build is not proof of higher FPS. The pure integer
filter and warm cache overlap; estimates are not additive. Planning budgets:
cull0–0.5ms/frame, motion0–3ms scroll, fast filter2–5ms scroll/0.5–1.3ms glass,
variants0–12ms scroll scaled by actual reuse/~2–3ms warm glass. These are
conditional engineering estimates, not ESP measurements or a claim that45/55fps
has been reached. Keep24KiB free/8KiB contiguous internal-DMA and future audio's
5,760sample-buffer bytes plus resources protected during reconnect/OTA.

No accepted exactness/caching rule changes. Integer filtering and variants are
byte-equivalent in native tests;1/32 quantization was rejected because a row edge
could jump~5px. Experimental moving glass intentionally changes interior pixels:
a5100ms host capture differs in912pixels (channel MAE0.0088/255) and looks equivalent
side by side; settled8600ms is identical. This limited evidence does not justify
enabling it by default or infer equivalence on every backdrop/physical panel.

The [implementation/A-B plan](agent-briefs/wave2/speed-round3-plan.md) contains
exact commands, expected memory/ms budgets, full validation counts and screenshot
paths. Face/list/card fixtures at Default/AX3/opaque/AX3+opaque/Bold are identical
between baseline and candidate; independent and root design reviews pass those
fixtures. Existing production demo issues (truncated71px target and settled Kal
text under Reply) remain unchanged, so no blanket production HIG pass is claimed.
Glass golden and redraw tests run here; shadow/round-helper tests requiring
unmerged sibling APIs remain in their stream. Every fork change is recorded in
`firmware/third_party/lvgl__lvgl/ORB_PATCHES.md`.

No ESP-IDF configure/build, SSH, flash, board measurement, live config/hub action,
commit or push was performed by this sandboxed implementation run.
