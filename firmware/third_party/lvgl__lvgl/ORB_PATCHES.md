# Orb's LVGL fork

LVGL 9.6.0 (from the component registry, `lvgl/lvgl`), trimmed to what the
firmware builds (no demos, examples, docs, tests, dev scripts or CI files),
with these changes. Every change is marked `ORB PATCH` in the source.

## Band split hook

`lv_display_set_band_cb()`: in partial render mode, a callback chooses each
band of an invalidated area: where it ends, instead of always the most rows
that fit in the draw buffer, and which columns it covers. It gets the band's
first row, the area's columns and last row and the buffer's size in pixels; it
sets the last row and may narrow the columns (x1 > x2 skips rows with nothing
to show).

Why: Orb renders into small bands in internal RAM (PSRAM is ~50x slower to
draw into), and Liquid Glass uses backdrop blur, which only sees the band being
drawn. Ending bands just above a glass surface keeps each one whole in a band,
so its blur sees all of it. The panel is round: narrowing each band to the
circle's chord leaves the square's corners undrawn and unsent, and narrower
bands near the top and bottom fit more rows.

Files: `include/lvgl/display/lv_display.h`, `src/display/lv_display_private.h`,
`src/display/lv_display.c`, `src/core/lv_refr.c` (`refr_invalid_areas`).

## Raster cache

`lv_obj_set_raster()`, Core Animation's `shouldRasterize`: a widget and its
children are drawn once into a bitmap, which is then copied to the screen frame
after frame. See `include/lvgl/core/lv_obj_raster.h`.

- **Outdating.** Any invalidation of a widget (`lv_obj_invalidate()`,
  `lv_obj_invalidate_area()`) marks the caches of its rasterized ancestors and
  its own, before the visibility check, so a change off screen counts too.
  `lv_obj_move_to()` marks only the ancestors: moving a cached widget
  (scrolling, layout) keeps its bitmap. Size changes are caught by comparing
  the size the bitmap was drawn at.
- **Drawing the cache.** Once per frame, after the invalidated areas are
  joined and before rendering, outdated caches that this frame shows are
  redrawn (`lv_obj_raster_refresh()` from `lv_display_refr_timer()`); the rest
  wait until they're on screen, or `lv_obj_raster_prepare()` draws them ahead.
  A cache is drawn like `lv_snapshot`, into an ARGB8888 bitmap, then split:
  a dynamic program over the rows picks horizontal bands, each an opaque core
  (stored in the display's color format, so drawing it is a plain copy, which
  the SW renderer does with SIMD) between blended edges (A8 plus a color where
  the edge has one color, as text has; ARGB8888 otherwise). Fully transparent
  rectangles are dropped and the ARGB8888 bitmap freed.
- **Drawing from it.** In `lv_obj_refr()`, a widget with an up-to-date cache
  and no layer (no transformation, no `opa_layered`) is drawn as one image per
  part instead of being drawn with its children. Widgets with a layer are drawn
  as usual.
- **Matte**: `lv_obj_set_raster_matte()` draws the cache over a solid color,
  for a widget that always sits on it (rows on the black canvas): the bitmap is
  then entirely opaque, one copy.
- **Opacity**: `lv_obj_set_raster_opa()` sets the compositing opacity, as a
  Core Animation layer's, without outdating the bitmap.
- Memory comes from `lv_obj_raster_set_allocator()` (Orb: PSRAM); the widget's
  destructor frees it.

Files: `include/lvgl/core/lv_obj_raster.h`, `src/core/lv_obj_raster.c`,
`src/core/lv_obj_raster_private.h` (new); `include/lvgl/lvgl.h`,
`src/core/lv_obj_private.h` (`spec_attr->raster`), `src/core/lv_obj.c`
(destructor), `src/core/lv_obj_pos.c` (invalidation, move),
`src/core/lv_refr.c` (draw, refresh).

## Draw buffers drawn without the image decoder

`lv_draw_image()` and the SW image helper take a draw buffer
(`LV_IMAGE_FLAGS_ALLOCATED`, not compressed, not indexed, not A1/A2/A4) as it
is, instead of opening it through the image decoders.

Why: the decoder only wraps such a buffer, but `lv_image_decoder_open()` takes
a global mutex, so the two draw threads queued on it for every image: with the
raster cache's dozens of parts per frame this cost more than the copies. LVGL's
own layers benefit too.

Files: `src/draw/lv_draw_image.c`.

## Blur: rounded corners, and the tint mixed in

- The backdrop blur of a widget used `draw_dsc.radius` before the descriptor
  was filled in (radius 0), so a round widget's whole bounding box was
  blurred, corners included. It now uses the widget's radius.
- `lv_draw_blur_dsc_t` has a `tint` and `tint_opa`, mixed into each pixel as
  the blur writes it. A backdrop-blurred widget with a plain background passes
  its background color and opacity as the tint and doesn't fill it again: one
  mix during bilinear reconstruction instead of another pass
  over the area. This is Liquid Glass's tint.

Files: `include/lvgl/draw/lv_draw_blur.h`, `src/draw/sw/lv_draw_sw_blur.c`,
`src/core/lv_obj.c` (`LV_EVENT_DRAW_MAIN`).

## Quiet FreeRTOS atomics

`src/osal/lv_freertos.c` includes ESP-IDF's `freertos/atomic.h`, whose helpers
are plain statics on Xtensa (`portFORCE_INLINE` is empty there), so every
unused one warned. Espressif's LVGL adapter, which the Waveshare BSP pulled in,
used to add `-Wno-unused-function` to all of LVGL; the warning is now silenced
around that include only.

## Thread-safe RLE font decoding

The decoder for compressed (RLE) font bitmaps kept its state in one global
(`LV_GLOBAL_DEFAULT()->font_fmt_rle`). With two software draw units
(`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`), both threads decode glyphs at once and
corrupt each other's bitmaps: in a host test, 7,178 and 19,948 of 20,000
concurrent decodes came out wrong. The state is now a local in `decompress()`,
passed to `rle_init()`, `decompress_line()` and `rle_next()`; the global field
stays in `lv_global_t`, unused. `scripts/fonts.sh check` runs the two-thread
test (`tools/fonts/lvgl_check/main.c`), so an update that brings the global back
fails it.

Why: Orb's titles, clocks and every icon font are RLE-compressed
(`components/orb_fonts`), and every text font falls back to an icon font.

Files: `src/font/fmt_txt/lv_font_fmt_txt.c`.

## Updating

Copy the new release over this tree (same exclusions), then re-apply each
`ORB PATCH` hunk and the three new raster files.

## Centered raster shrinking

`lv_obj_set_raster_scale(obj, 0..256)` shrinks a cached visual about its center without rebuilding its source bitmap. The logical hit/layout area stays unchanged; Orb uses it only for non-interactive session rows near the circular rim. Native size retains the copy fast path. `sim/tests/test_scene.c` checks source-cache reuse and output bounds.

## Smooth backdrop blur

The software blur averages source pixels before downsampling, filters at 8 bits per channel, and reconstructs with bilinear interpolation. Rounded coverage blends the result with untouched backdrop pixels. RGB565 quantization uses screen-anchored Bayer dithering. Scratch buffers are per draw task (about 8.3 KiB for a 250x72 SPEED blur). General allocations retain their existing policy; the bounded allocator below independently places scratch. Host validation includes partial clips, padding guards, RGB565 byte order, RGB888, A8 and rounded edges. Device timing must be remeasured: quality adds substantial arithmetic relative to the old nearest-pixel path.

## Bounded draw scratch (Wave 2; internal preference default off)

`lv_malloc_draw(size, kind)` / `lv_free_draw(ptr, kind)` distinguish temporary
software draw buffers from widgets, raster bitmaps, caches and display buffers.
The same kind must be passed when freeing. `lv_free`/`lv_realloc` cannot be used
on this allocation. Hosts keep the original `lv_malloc`/`lv_free` behavior.
ESP firmware always uses capability-explicit allocation: disabled kinds use
PSRAM, including small scanline masks, and never generic malloc's internal
fallback. This closes the unbounded internal-allocation path that previously
remained when all three internal preferences were off. It does not establish
the cause of the lead's historical 8-byte DMA minimum; board measurements must
separate a fresh scroll window from earlier boot/radio lows.

Task/descriptor blocks, heap-owned layer metadata and temporary tile-layer
arrays also use explicit PSRAM on ESP via private
`lv_draw_metadata_alloc/free` helpers. These CPU-only allocations need no DMA
capability. They are zeroed and explicitly aligned to `max_align_t`; host
placement stays unchanged. The existing draw-worker completion fences and
nested layer ownership paths still decide when to free them. No internal
fallback is attempted on PSRAM exhaustion; LVGL's existing allocation-failure
handling still applies. Stack-embedded layers, widget/style metadata, cold
circle-radius cache objects and persistent pixel buffers keep their policies.
`test_draw_metadata.{c,py}` compiles the production helpers and ownership
functions for host and ESP paths under ASan/UBSan, including 128 pending tasks,
nested layer cleanup, descriptor zeroing/alignment and exhaustion.

Three independent Orb options select the call sites:

- `ORB_DISPLAY_INTERNAL_IMAGE_SCRATCH`: transformed image tiles, recolor tiles
  and image clip scanlines. The image file changes only allocation/free calls,
  an allocation-failure return and the patch marker, to isolate the merge from
  the glass stream's pixel-path changes.
- `ORB_DISPLAY_INTERNAL_BLUR_SCRATCH`: the combined downsampled pixels,
  interpolation arrays, scanline mask and RGB565 quantization LUT. Blur cache
  payloads keep their existing explicit PSRAM allocator.
- `ORB_DISPLAY_INTERNAL_DRAW_MASKS`: task-local fill, border, arc, line,
  triangle and rounded-rectangle scanline masks, plus arc-end circle masks.
  Most scanlines are at most 466 bytes. Explicit placement keeps these out of
  the 1,024-byte generic allocator's internal preference when this option is
  disabled. Large arc masks obey the same cap when it is enabled.

Enabled kinds share **16 KiB** of concurrently charged internal scratch across
both draw workers, enforced with atomics. Each charge includes payload, an
aligned allocation header and **64 B** conservative allocator slop. Allocation
also preserves **24 KiB free** in both internal/8-bit and internal/DMA/8-bit heap
views and **8 KiB largest contiguous DMA** after allocation. Heap views overlap
and are never added. A failed preflight, allocation or post-allocation check
releases its budget and uses explicit PSRAM. PSRAM exhaustion returns NULL,
without generic malloc falling back into the protected internal margin. New
null guards avoid dereferencing failed image/mask allocations; blur already
releases its cache lease on failure. This is point-in-time admission, not a
reservation against future Wi-Fi/TLS/audio allocations; device stress remains
required. Nothing retains an internal scratch allocation between draw tasks.

At 466px RGB565, image tiles normally request at most **3,728 B**, so two
workers need at most **7,456 B payload**. Resting demo blur scratch requests
**3,834 B** for a 71x71 control or **7,776 B** for its 195x71 capsule
(`ORB_PT` currently truncates integer conversions); a mixed
image/capsule pair uses **11,504 B payload**. Two capsules use **15,552 B**, plus
headers/slop; larger simultaneous requests spill to PSRAM. The cap includes
that overhead estimate. Atomic diagnostics add approximately **44 B static
internal RAM** on the 32-bit ESP32; the runtime heap delta is transient and
bounded, not a new permanent 16 KiB pool.

`lv_draw_scratch_get_stats()` reports per-kind internal allocations, PSRAM
fallbacks and failures, plus current/peak internal charges. Counters are atomic
and never print in the hot path; concurrent snapshots are observational, not
transactional. Use device counters to verify that an enabled experiment is
actually served internally before interpreting FPS.

Audit exclusions: circle-radius cache objects and their cold construction,
gradient state, persistent draw/layer buffers and blur/raster caches keep
generic/their existing explicit allocators. Orb uses precomputed SDF glass
shadows, so the stock box-shadow scratch is not a hot Orb draw path. Disabled
ARM2D/vector backends are unchanged. No pixel math, quality or object allocation
policy changes.

Files: `include/lvgl/stdlib/lv_mem.h`, new `src/stdlib/lv_mem_draw.c`,
`src/draw/sw/lv_draw_sw_{img,blur,fill,border,arc,line,triangle,mask_rect}.c`,
and the explicit heap dependency in `env_support/cmake/esp.cmake`.
Production allocator host tests live in
`firmware/components/orb_display/tests/host/test_draw_scratch.{c,py}` and cover
all eight option combinations, host fallback, allocation failure, alignment,
overflow and concurrent admission under ASan/UBSan. Device timing is pending.

## Bounded draw attribution (Wave 2 round 3, default off)

`CONFIG_ORB_DISPLAY_DRAW_PROFILE` wraps `execute_drawing()` with two microsecond
wall timestamps. `lv_draw_sw_profile.{h,c}` accumulates native 32-bit atomic
elapsed-us/call counters in 160 fixed slots (1,280 bytes): draw-task type,
widget class (`obj`, `label`, `image`, other, or no owner), and an image/layer
transform flag. It performs no allocation, serial output, or locking around a
primitive; disabled builds contain no counters or timestamp calls. Transform
means non-native X/Y scale or rotation, not a claim about the source format.

The developer benchmark takes modulo differences at the same phase boundaries
as its display counters. Six additional native atomic stage counters (72 bytes)
attribute row atlas preparation/direct drawing and glass preparation, downsample,
filter and fused composition. About 26 KiB of explicit PSRAM holds at most 16
phase snapshots, with an overflow flag. It prints `DRAWPROFILE`, `DRAWTASK`,
`HOTSPOT` (including zero-work stages), `ROWATLAS`, `FUSEDGLASS`, and `RASTER`
records only after the timed
scene ends. `GLASS_PREPARE` includes downsample/filter; hotspot stages are separate
attribution, never additive frame totals. Off builds inline their begin/end calls
away. Allocation failure fails the
benchmark; it never consumes the reserved internal draw/audio heap instead.
Atlas/fused records include work/fallback deltas and resident/peak/budget gauges;
fused payload gauges exclude temporary source/mask/scanline scratch, so heap
minima remain the whole-workload admission evidence.
`RASTER builds` is a separate row-cache rebuild delta; `CACHE hits/misses` in
the existing benchmark describes the exact backdrop-blur cache.

These are wall times including preemption. Draw tasks can overlap across both
workers, and raster preparation occurs before display RENDER_START, outside
the display DRAW denominator. Task percentages therefore need not sum to 100;
no residual is labelled CPU or scheduler time. Counter update/classification
overhead is outside the measured primitive but inside instrumented refresh
cost. Use the ordinary control firmware for final throughput comparisons.
The 60-second benchmark cap keeps phase elapsed differences far below a
32-bit microsecond wrap, even summed across both workers. No counter reset
races a worker. Snapshot reads are atomic; benchmark callbacks occur between
refreshes after their software workers have finished.

Host tests exercise classification, modulo wrap, 200,000 concurrent updates,
deferred phase reporting, exact denominator/raster deltas, overflow and cleanup,
and absence of symbols in disabled builds. A profiler-enabled full simulator
build passes the normal rendering suite. No ESP build or device timing was
performed by the sandboxed agent.


## Wave 2 round 3 rendering options (unmeasured on ESP32)

Every performance path below is independently Kconfig gated, default off. Host
CMake has corresponding options; disabled configurations retain the old filter.
The correctness-only cover search defaults on. No ESP-IDF build/flash was run.

### Empty-band blur dispatch and moving backdrop quality

`CONFIG_ORB_DISPLAY_GLASS_SINGLE_PASS` rejects nonintersecting controls before
creating BLUR tasks. The existing whole-control guard already executes three
real blur/cache operations per scrolling frame; ~eight profiler dispatches were
padded shadow bounds visiting other bands. This change removes empty tasks, not
five actual kernels. No stale identity-based reuse or future-band reads.

`CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY` snapshots factor4 averaged sampling into
immutable blur descriptors during actual scrolling or parallax. The default
factor3 material returns after120ms of quiet, with a redraw even if content no
longer invalidates. A per-display timer/source-delete lifecycle avoids a leaked
benchmark timer or access to a deleted scene/display. Cache keys include factor;
normal-quality results cannot reuse lower-quality output. Factor4 RGB565 uses
packed exact sums (including swapped format), average-before-filter, existing
8-bit IIR, bilinear reconstruction, full-resolution rounded AA and anchored Bayer
dither. This explicitly experimental mode changes interior pixels; it does not
relax the accepted cache's byte-verification rule. Physical review is pending.

Changed fork files:
- `include/lvgl/draw/lv_draw_blur.h`: factor in descriptor, UI-thread motion APIs.
- `src/draw/lv_draw_blur.c`: early task cull, bounded motion state/timer/lifecycle.
- `src/core/lv_obj_scroll.c`: note real scroll movement.
- `src/core/lv_obj_draw.c`: snapshot motion factor during descriptor creation.
- `src/draw/sw/lv_draw_sw_blur.c`: explicit factor and optimized4×4 RGB565 average.
- `src/draw/sw/lv_draw_sw_blur_cache.c`: factor in exact parameter key.

Outside the fork, `orb_motion.c` notes parallax and host blur/cache tests cover
factor4 versus an independent average reference and parameter-key separation.
Native motion tests cover timer extension, tick wrap, source/display deletion and
repeated init/deinit; lifecycle tests retain zero timers after benchmark teardown.

### Exact scaled raster variants

`CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE` keeps a bounded LRU of exact existing
integer-scale variants for opaque, matte, single-part RGB565/SWAPPED sources.
There is no scale/opacity quantization. An entry retains the generic filter's
exact AA coverage; a verified opaque rectangle is split into display-format
RGB565 so existing `orb_lv_simd` IMAGE copying applies, with RGB565A8 edge strips.
Content/size/format changes and deletion purge variants. Translation and opacity
reuse them. Current-frame sources are pinned before admission; eviction occurs
only before task creation, so queued bands/workers keep valid source pointers.
Unsupported sources or allocation/budget failures fall back to generic drawing.

`CONFIG_ORB_DISPLAY_RASTER_SCALE_CACHE_KIB` bounds retained pixels plus metadata
(512KiB default,64–1024 range); allocation uses the existing raster PSRAM callbacks.
Source rasters remain separate, and combined raster stats include variant bytes.
Temporary construction is also PSRAM (~84KiB max for demo), not internal DMA.
The public stats/runtime-selection API supports exact host A/B and benchmark
`SCALECACHE` records; `build_us` is present with draw profiling. Builds occur
before RENDER_START: lower DRAW alone is not evidence of higher throughput.

Changed fork files: `include/lvgl/core/lv_obj_raster.h` (stats and between-frame
A/B API), `src/core/lv_obj_raster.c` (cache, budget, lifetime, IMAGE dispatch and
reporting). Native tests compare all256 scales, opacity/translation/clips,
source changes/resize, both RGB565 byte orders, OOM retry, over-budget simultaneous
owners, pinned lifetimes and teardown. No redundant source widget cache is built.

### Exact integer RGB565 scale-only interior

`CONFIG_ORB_DISPLAY_RGB565_SCALE_FAST` hoists constant Y row/weight calculation,
uses an integer X accumulator, and fills all-255 coverage in bulk for safe opaque
interiors. It preserves both5-bit bilinear rounding stages and byte swapping;
checked AA edges and generic alpha/rotation/non-AA paths remain unchanged.
No lookup storage or assembly is added. The loop avoids an unused final X
accumulator increment so it does not introduce an overflow after the last pixel. This independently accelerates changing
scales/cold variants where the LRU hit rate is low. It overlaps warm-cache savings.

Changed fork files: `src/draw/sw/lv_draw_sw_transform.c` (specialized interior and
option-off fallback), `src/draw/sw/lv_draw_sw.h` (UI-thread, quiesced host A/B
selection). Raw guarded transform tests compare full/clipped buffers over256
scales, native/swapped565, alpha fallback and nonuniform scale/rotation/non-AA.

### Raster cover-search correctness

`CONFIG_ORB_DISPLAY_RASTER_COVER_CHECK`, default on, rejects faded/scaled cached
roots as full-cover candidates and does not descend into their opaque source
children. They are a single composited layer. The old cover search skipped
background erasure near the bezel, leaving prior-frame pixels. The unchanged
glass-stream partial/full redraw fixture failed65/167 frozen frames with old
search and passes167/167 with this repair.

Changed fork files: `src/core/lv_refr.c` and
`include/lvgl/core/lv_obj_raster.h` / `src/core/lv_obj_raster.c`
(`lv_obj_raster_is_opaque`). No intentional visual style/layout change.

## Raster coverage during partial refresh (Wave 2)

`lv_refr_get_top_obj()` must treat a raster root and its descendants as one
composited bitmap. It previously selected an opaque child or an unscaled source
rectangle even when the bitmap was faded or shrunk. Rendering then skipped the
black backdrop, exposing old pixels in the reused band near the bottom bezel.
Raster roots are now atomic during cover checks, and only full-scale,
full-opacity caches can preserve the source widget's opaque coverage claim.
The ordinary opaque-cache cover optimization remains enabled.

Files: `src/core/lv_refr.c`, `src/core/lv_obj_raster.c`,
`src/core/lv_obj_raster_private.h`. `sim/tests/test_scroll_redraw.c` compares
incremental and freshly invalidated pixels with a single 466×84 buffer, including
scrolling, tilt, presses, opaque raster children and zero scale/opacity.

## Neutral black RGB565 masks (Wave 2)

`lv_draw_image_dsc_t.rgb565_neutral_shadow` is an opt-in for Orb's untransformed
black A8 shadow/inner-edge mask. The software image path forwards it to the
common blend dispatcher for native and swapped RGB565. Independent 5-bit R/B
and 6-bit G floor rounding could leave green-only dark pixels. Neutral input
greys now use coupled R/B5 and replicated G6 when darkened. Colored pixels use
their existing blend; transparent mask pixels and true black stay unchanged.
The flag defaults off and other formats/image paths retain their normal blend.
There is no extra image, scratch allocation or retained cache payload.

Files: `include/lvgl/draw/lv_draw_image.h`, `src/draw/sw/lv_draw_sw_img.c`,
`src/draw/sw/blend/lv_draw_sw_blend_private.h`,
`src/draw/sw/blend/lv_draw_sw_blend.c`. `sim/tests/test_glass_shadow.c` exercises
the real A8 path, both byte orders, every RGB565 source color, neutral alpha
levels, object opacity, clipping, padding and flag-off behavior. Both host and
ESP32 use this common masked blend path.

Opaque black RGB565 fills use `lv_memzero()` in the native/swapped C fallback
after the existing SIMD hook and contiguous-row flattening. The stale-band fix
must clear exposed backdrop pixels; this fast path reduces that cost without
weakening cover checks. ESP32's accepted SIMD path still runs first. Guarded
tests cover odd widths, band heights, unaligned destinations, clipping, padding
and non-opaque behavior. Files: `src/draw/sw/blend/lv_draw_sw_blend_to_rgb565.c`,
`src/draw/sw/blend/lv_draw_sw_blend_to_rgb565_swapped.c`.

## Retained exact material presentation (architecture Stage 1, default off)

`CONFIG_ORB_DISPLAY_RETAINED_MATERIAL` admits Orb's three managed glass controls
into a **192 KiB combined cap**, including raw RGB565 patches, untinted RGB888
filtered maps, RGB565 material bases, descriptors and the bounded revision
registry. All storage uses the explicit Orb PSRAM allocator. A control reserves
its maximum actual integer press rectangle once; continuously changing tint
and every intermediate transform rectangle retain the original exact math.
There is no per-frame retained allocation or full-resolution scale atlas.

An ordered list of at most 32 intersecting lower source generations also records
bounds, effective clips, transform sizes and opacity. Raster roots are atomic;
child invalidations advance their source generation. A 256-slot PSRAM registry
assigns creation/invalidation generations without growing `lv_obj_t`; eviction
or generation wrap invalidates the proof. Source motion, clipping, z-order,
layout, font, theme/accessibility and raster-content changes are dependencies.
Unsupported layers/transitions, unmanaged nonraster draw callbacks, incomplete
lists, changed paint contracts, unsupported geometry/format and OOM use the
compatibility renderer. Object IDs, frame numbers and source-byte comparison
are not cache-validity proofs.

The filtered-map key excludes tint/rim/light. The base key adds actual rounded
geometry, tint/opacity, output format and screen coordinates (dither phase).
Light-only updates restore the base then draw current shadow/rim/labels, avoiding
row transforms, source recapture and blur kernels. Tint updates restore original
pixels for AA and reconstruct from a valid untinted map. A geometry miss captures
and filters its actual rectangle; no four-size SDF approximation is substituted.
The whole-control band guard and full-band serial renderer remain unchanged.
Even-aligned retained patch bounds include the display-window outer pixels,
so light invalidation does not leave an extra transformed one-pixel strip.

Base capture drains tasks before shadow/rim/labels. A material-key change resets
per-key row coverage while independently preserving the proven raw background;
a shadow-only first band cannot publish a new tint key before every interior
row is rebuilt. Full-width partial-damage regression cases exercise that split.
Raw/map/base users complete
before per-control retirement; last-owner teardown releases the revision table.
Admission retires leases belonging to another screen. The developer benchmark
preserves the live shell while constructing its fixture, so a global three-slot
pool otherwise remains occupied entirely by invisible shell controls. Orb's
DRAW_MAIN_BEGIN hook retries admission and restores the newly acquired lease;
returning to the preserved screen therefore re-admits its controls. The combined
192 KiB cap still applies, including the revision table; no second screen's
material payload is retained. A regression now constructs the real Apps shell,
loads three complete benchmark fixtures in turn, checks map/base reuse during
their live glass phase, and verifies restoration and the allocation plateau.
Reduced Motion's transient complete-material cross-fade suspends retention until
settled, preserving its real backdrop mixture. Existing exact backdrop caching
is bypassed for admitted material work, avoiding duplicate backend payloads.

`lv_draw_sw_material_get_stats()` reports actual filter kernels/source samples
in Stage 0 too, plus retained map/base hits/builds, budget/peak and material
preparation wall time (downsample/filter or map-copy, excluding reconstruction).
Worker counters use native atomics; UI snapshots extend modulo-32 work counters
to 64 bits with benchmark phase samples at most 60 seconds apart. Counters never
print in draw loops. Source-generation storage adds no bytes per LVGL object.

Files: new `src/core/lv_obj_material.{c,private.h}`, object initialization/raster
invalidation hooks, `lv_refr.c` cover/restore/even-bound seams,
`lv_draw_blur.{h,c}`, `lv_draw_sw_blur.c`, and Orb glass admission/capture hooks.
`sim/tests/test_retained_material.c` compares frozen output against the disabled
compatibility path in native/swapped RGB565, including continuous tint, actual
press spring rectangles, child revisions, clips, hidden controls, unmanaged
painting, destruction, failed allocation/retry, dimension overflow, three-owner
admission and 20 create/delete cycles. It asserts zero warmed light-only row
transforms, source recaptures, kernels and source raster builds, including the
actual three-control demo after its glass phase has warmed. The ordinary
partial/full redraw and materialisation suites remain the reference.

### App-open source-raster accounting

`lv_obj_raster_stats_t.build_us` is a cumulative source-construction elapsed
sum available only with `CONFIG_ORB_DISPLAY_DRAW_PROFILE`. It includes failed
attempts and cleanup, while `builds` continues to count successful nonempty
rasters. Disabled builds return zero and perform no clock reads. Scaled-variant
construction retains its separate existing `build_us` counter; neither sum is
presented as independent serial frame time. `glass_timing` verifies measured
cold preparation, unavailable unprofiled timing, and unchanged warm counters.

### Settled-only retention and exact uniform glass

`CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY` keeps Stage 1 leases but bypasses
retained proof, revision-registry maintenance, raw/map/base copies and synchronous
material drains while scroll/parallax is moving. The descriptor snapshots that
state so workers also bypass the compatibility byte-comparison backdrop cache.
The existing bounded 120 ms motion lifecycle now serves this independent option
and TE pacing without enabling the lower-resolution motion-quality experiment.
Motion invalidates all display material proofs before any animated source is
changed; a settle invalidation rebuilds the current exact material. Content
changes during the moving interval therefore cannot revive a stale base.

`CONFIG_ORB_DISPLAY_GLASS_SOLID_FAST` verifies every source pixel in the clipped
RGB565 sample rectangle. Uniform black or solid backdrops bypass downsampling,
IIR filtering, interpolation maps and large scratch storage. They still use the
same rounded coverage, exact tint arithmetic and screen-anchored Bayer writer.
A sparse sample is deliberately insufficient: a single glyph pixel must reject
the path. Admitted settled controls can publish the identical constant untinted
map and complete base, so later light changes preserve normal base reuse.
Failed tiny mask allocation falls back to the existing renderer. Other formats
and nonuniform content retain their full existing blur. Neither option changes
sampling radius, material colour, blur quality, or output-band ownership.

`solid_fills` and `moving_bypasses` add bounded cumulative evidence to material
statistics. `sim/tests/test_solid_blur.c` compares the enabled fast path with the
unaltered full blur at runtime: native/swapped RGB565, black/solid colours,
tint, all Bayer phases, varying radii, expanded shapes, partial task clips and
single-pixel rejection. `test_retained_material` additionally proves zero retained
map/base builds throughout moving real demo scroll, exact pixels during source
and content changes, and recovered light-only base reuse after settle.

### Sparse zero-alpha glass mask spans

`CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS` adds an opt-in A8 image-descriptor
pointer to two half-open occupied X spans per source row. Orb computes them
once from each immutable SDF dark mask and from every possible rim geometry
pixel, before lighting. Light-dependent zero/nonzero AA pixels remain admitted.
The tables share the mask's leases and existing 640 KiB PSRAM charge and unwind
on partial allocation failure. No table is allocated on a draw event.

The software image path forwards this metadata only for untransformed,
unrounded A8 masks. Native and swapped RGB565 blending visits the spans in
one worker task, with the exact existing neutral-shadow and normal color-mix
formulas. Other image paths/output formats retain the dense fallback. Source
and destination stride/clip offsets remain part of ordinary blend setup; no
scratch or additional task is introduced. Only guaranteed zero-alpha coverage
is omitted, so AA, material pixels, dithering and lighting remain unchanged.
`glass_shadow` compares dense and sparse full output including guards at opacity
boundaries, arbitrary RGB565 colors, neutral shadows, both byte orders and clips.
The final image evidence is in `render-round-design.md`.

## Nonblocking presentation admission (render pacing round)

`lv_display_set_refresh_gate_cb()` installs an optional driver callback before
layout, blur expansion and source raster preparation. It runs only for dirty
timer-driven refreshes. A nonzero retry delay preserves the dirty areas and
returns to LVGL's timer handler, so input and time-based springs keep running.
Admission retries use their own delay; successful admissions restore a 1 ms
refresh opportunity rather than adding the independent 16 ms timer to TE.
The usual no-damage pause remains, so TE never starts an idle redraw loop.

`lv_refr_now()` bypasses this gate for synchronous screenshots, restoration and
drain fixtures. The optional callback is null in the compatibility renderer.
The firmware owns measured TE deadlines, admission probes, expired-slot skipping
and actual final-SPI completion; LVGL does not estimate panel timing. Pending
admissions and frames with no final flush must be retired at REFR_READY.
`sim/tests/test_pacing.c` exercises the real gate/timers, deferred deletion,
forced refresh and a virtual TE grid. The display host suite extracts the real
driver and tests failure, scan-delay and buffer/PM ownership paths.

## Native-size row spans and fused material callback (hotspots round)

The scroll round enables `ORB_DISPLAY_GLASS_SOLID_FAST` by default after exact
oracle comparisons. Its proof covers only the actual clipped band region:
alignment-safe RGB565 pairs prove the first row, then full row byte comparisons
prove the rest without inspecting padding or adjacent content. No sparse probe,
screen-sized cache or changed material formula is introduced. Uniform inputs
retain the reference tint, rounded antialiasing and screen-anchored dithering;
one differing source pixel takes the ordinary blur. The simulator regression
includes every pixel/byte of an odd-X, odd-stride region in a partial band.

`CONFIG_ORB_RENDER_ROWS_AT_SIZE` independently opts selected matte raster roots
into `lv_obj_rows.c`: LVGL's label iterator supplies UTF-8/fallback/kerning
placement; sixteen cold-built A4 glyph sizes feed direct RGB565 band spans.
Native-scale rows and unsupported drawing keep the original raster path.
Pinned row references, content-proven glyph identity, a 1 MiB bounded unpinned
LRU and explicit purge protect font/allocator lifetime. A supplied read-only
font-size callback enforces the real 20 px em floor, with adjusted advances and
ellipsis. No full-row image is resampled in the admitted moving path.

`CONFIG_ORB_RENDER_FUSED_GLASS` admits cold 4× area-averaged raster mips under a
192 KiB cap. `lv_obj_raster_sample_ready()` proves a complete current sample
source before `sample_area()` is used; row glyph overlaps compose in a bounded
internal cell. The blur descriptor has an optional Orb material callback with
the normal software blur as its explicit fallback. Orb owns the scene/halo,
internal reduced map, shared SDF masks and fused tint/AA/rim/dither kernel;
LVGL still owns layout, task ordering, object semantics and output bands.
Unknown, dirty or unadmitted painting cannot silently become transparent input.

Profiler stages expose cold preparation, row spans, reduced material sampling,
filtering and composition separately. Deferred atlas/fused-map/mip counters
include actual admission failures, current memory and warm build deltas.
`rows_at_size`, `fused_glass` and `scroll_redraw` have ASan/UBSan coverage;
the first two also verify byte order, clipped writes, OOM/unwind and lifetimes.
This is a default-off material/typography experiment, not an exact-pixel patch
or a board speed claim. See `docs/agent-briefs/wave2/hotspots-results.md` for
visual differences, source scope and lead-only measurement commands.

## Scoped scroll damage publication (scroll attribution round)

`lv_obj_scroll_by_raw()` batches display damage through its child movement and
SCROLL callbacks. A bounded eight-rectangle set retains disjoint damage, merging
only when a bounding rectangle costs no more pixels than its separate inputs;
overflow conservatively collapses the set, never drops damage. Nested scroll
callbacks share the scope and outlying sibling changes remain included. Source
dirty marking remains immediate; only completed damage reaches the driver
INVALIDATE_AREA rounding/admission callbacks. No frame delay or timer is added.

The default path still includes the entire old viewport. Plain, static Orb round
lists explicitly opt into `lv_obj_set_scroll_damage_children()`: old/new cached
child bounds replace viewport damage, retaining untouched gaps. Every moving
child must have a cache and no transformed/layered rendering; blur, transformed
viewports and scrollbars fall back to full viewport damage. Callback content,
style, resize, insertion and deletion invalidations remain ordinary damage.
Cache scale/opacity changes publish old/new composite bounds without dirtying
their own captured pixels; affected ancestor caches and material revisions stay
dirty. Non-matte/unsupported layer states retain conservative source bounds.

The private per-display transaction is unwound even if SCROLL deletes its target.
A synchronous refresh inside a callback publishes current pending damage first;
later callback mutations still publish at scope close. Clearing invalid areas
also clears pending damage, and disabled invalidation remains disabled.
`sim/tests/test_scroll_damage.c` compares retained and fresh framebuffers for
ordinary mutations, sibling moves outside the viewport, nested scroll, deletion,
explicit refresh, disabled invalidation and pending-damage clearing. This patch
reduces repeated publication; raw request counts and final coalesced frame damage
remain different metrics, and neither establishes device timing by itself.

The row-span admission also accepts exact `lv_button_class` content over an
explicit black raster matte. Fully transparent roots use the matte rather than
their unused background colour; partial fills, nonblack mattes and unsupported
painting keep the reference bitmap. This makes transparent workspace rows use
the existing opt-in `ORB_RENDER_ROWS_AT_SIZE` path. Synthetic ellipses require a
real dot glyph, so Framework7 symbols cannot reject an otherwise valid row.
The adapter retains its existing 20 px font floor and clipped glyph spans;
enabling that path can change glyph pixels compared with bitmap scaling.
`test_rows_at_size` verifies the transparent/opaque-black oracle, supported
buttons and fallback cases; the production surface fixture verifies admission.

## Cold preparation reductions (scroll round three)

Opaque matte rasters skip their redundant alpha scan/split pass and directly
convert the one proven-opaque rectangle. Fully suppressed compositor rows defer
cold source/scale work until visible; their draw path suppresses stale/missing
sources too, so deferral cannot expose uncached content. Native-size glyph rows
share immutable A4 payloads when the 20px floor or pixel rounding yields the same
adjacent bucket dimensions. One glyph allocation owns all payloads, preserving
existing single-free lifetime/OOM handling. `size_builds` and `size_reuses` expose
actual resampling versus reuse; warm frames create neither. These changes make
no board timing claim. Production fixture partial/full oracles, hidden-cold
reveal, sparse damage/fallback/overflow and row lifetime/OOM tests cover them.

## Native-width cold glyph integration (navigation board round one)

`src/core/lv_obj_rows_average.h` retains the exact Q8 area-average filter while
using a 32-bit accumulator and division. The atlas scale is at least 136/256,
so a source footprint is bounded by 512 on either axis and its maximum sum is
`255*512*512 < 2^26`. This removes software 64-bit division on ESP32-S3 without
changing coverage, rounding or glyph pixels. `test_rows_at_size` compares the
helper against independent 64-bit integration, including maximum font widths.

## Immutable queued label text (navigation board round one)

Dynamic label tasks snapshot their text at enqueue. `GET_SELF_SIZE` can
temporarily restore the full string behind an ellipsis while a previously
queued software worker still reads the label. The worker could calculate a
line end from the restored string, encounter the reapplied earlier NUL and
spin forever because UTF-8 decoding no longer advances. This was reproduced
on the unmodified archived baseline as well as the candidate.

`src/widgets/label/lv_label.c` marks dynamic text for the existing `text_local`
ownership path; static text remains zero-copy. `src/draw/lv_draw_label.c`
allocates owned strings through the explicit PSRAM draw-metadata allocator
before publishing the task. Allocation failure publishes no task and never
borrows the mutable source as a fallback. `src/draw/lv_draw.c` matches that
allocation in normal and abandoned-task cleanup. No decoder guard conceals
the producer/worker ownership race.

`sim/tests/test_label_text_lifetime.c` deterministically triggers the real
ellipsis restore inside `GET_SELF_SIZE` before task dispatch and inspects the
queued string during font sizing; it fails without ownership and passes with
it. It also checks static zero-copy. Display host metadata tests exercise
explicit-length copying, source mutation, cleanup and PSRAM allocation failure
without an internal-memory fallback. Neither test establishes panel FPS.

### Bounded raster admission (wave 2 memory pass)

Opaque matte sources render directly into RGB565 (native or swapped) rather than
allocating an ARGB8888 master and a duplicate retained plane. Source/page caches
are bounded at 768/448 KiB including part descriptors; transparent preparation is
bounded at 256 KiB. Eviction runs before task dispatch, preserving every current
visible source. Failed allocation sizes remain negatively cached through the
scroll workload; `lv_obj_raster_cache_retry()` at SCROLL_BEGIN permits a fresh
workload to retry. Whole-page rasters skip redundant child preparation.

Transparent build scratch is charged to the dedicated, non-cache
`ORB_MEMORY_RASTER_BUILD` (`raster-build`) owner rather than `WORKSPACE`. Its
256 KiB ceiling matches the existing build-size guard: the host raster-budget
test exercises a supported 253 × 252 build with a 261,480-byte scratch charge,
then rejects the adjacent 254 × 252 build before allocation. The 30,808-byte
Home build measured on the board is a smaller instance of this same envelope.
The dedicated owner leaves retained raster/cache admission unchanged. Host
builds track/untrack the identical charge without enforcing ESP admission;
successful builds and allocator-failure cleanup both return live scratch to
zero. Pixel construction, split decisions and retained bytes are unchanged.

On ESP, source/page/mips, glyph-size atlases and SDF masks register their unpinned
LRU candidates with `orb_memory`; all optional graphics share 1 MiB and preserve
1 MiB live PSRAM margin. Referenced masks/glyphs and current frame sources cannot
be evicted. Materials and exact-blur leases share the same aggregate charge.

### Cold snapshot regions (wave 2 memory pass)

`lv_snapshot_take_area_to_draw_buf()` renders one absolute-coordinate region
into caller-owned storage without reshaping it to full-screen capacity. It
retains the existing snapshot task completion, stalled-dispatch cleanup and
display-layer restoration. The full snapshot API delegates to the same path.
Orb drains SPI and borrows the existing full internal band arena, keeps live
glass whole with its band guard, then copies completed RGB565 rows sequentially
into a reserved navigation plane. No additional internal arena, PSRAM painting
target or panel write is introduced. `sim/tests/test_snapshot_bands.c` compares
whole and banded captures byte-for-byte for text, flat content and backdrop
glass, and checks an independently clipped partial-width region.

### Snapshot band composition before the DMA fence (performance round)

`LV_EVENT_RENDER_BAND` exposes a borrowed `lv_display_render_band_t` for an
unrotated, complete partial-mode band. An opaque immutable compositor fills it
and marks `handled`, bypassing ordinary object drawing. Unhandled events keep
the existing LVGL path. The event follows the single-buffer ownership wait but
precedes the double-buffer flush wait, so the second internal half can compose
while the first is on the wire. Tile fragments do not qualify. The event is
synchronous and must neither retain its pointers nor mutate LVGL state.

Previously Orb composed at `FLUSH_START`, after the double-buffer wait, and
discarded the preceding blank scene draw. That serialized useful work and hid
it outside the DRAW metric. The existing flush callback, SPI result drain,
alias-layout fence, TE scan avoidance, error recovery and PM leases remain
authoritative. No new output allocation or PSRAM DMA source is introduced.
`sim/tests/test_snapshot_overlap.c` checks delayed DMA ownership and exact old
versus new pixels across all transition kinds, directions, reversals, endpoint
overshoot and Reduce Motion.

### Exact warm scroll work (performance round)

`lv_obj_raster_ready()` observes source identity/size/dirty state without
preparing it. Orb's list adapter uses it to prepare at most one nearby row in
an idle timer opportunity, pausing during scrolling. Source admission, visible
pinning, failed-admission epochs and the shared cache budget still apply.
Genuine pressed-state source changes remain invalidated.

The native glyph path uses a bounded exact divide-by-255 fold and skips
framebuffer reads for zero/full coverage. `test_rows_at_size` exhaustively
compares the supported arithmetic domain with the former formula.

The uniform RGB565 blur shortcut now participates in the existing leased
raw-input/output cache. A complete uniformity proof allows trying this cache
even while the backdrop is marked moving; hits still require every input byte
and the full key (including absolute dither coordinates) to match. Cache OOM,
busy leases and changed pixels retain the ordinary renderer. No new cache
budget or different material math is introduced. `test_solid_blur` checks all
16 Bayer phases in both byte orders, repeated moving hits and rejection of a
single changed source pixel against the uncached reference.

### Settled row preparation and exact row arithmetic (performance round)

`lv_obj_raster_ready()` exposes source validity without allocating or changing
presentation. Orb's round-list helper uses it to prepare one nearby row per idle
scheduling slice, pauses during touch/crown momentum, and restarts after settle
or layout changes. Old/new draw damage is published when preparation replaces a
cold fallback; the source and shared-cache budgets remain authoritative.

Native A4 row blending replaces division by 255 with an exact bounded fold for
0..65152 and avoids framebuffer reads for transparent/opaque glyph samples.
`test_rows_at_size` exhaustively compares the fold, opacity products and all
RGB565 channel/alpha combinations with the original division formulas. The
scroll-fixture warm-source check now relies on the production idle preparation,
with no test-only manual source priming.

Uniform RGB565 controls now also use the existing byte-verified raw/output blur
cache after their complete solidity proof. Moving content does not disqualify a
fixed control whose sampled pixels are still identical black. This skips warm
AA/tint/dither reconstruction; absolute shape/clip coordinates retain the
screen-dither phase in the key. Changed pixels use live blur and failed cache
admission uses the existing solid kernel. `test_solid_blur` checks both byte
orders, all Bayer phases, rounded partial support, moving hits and a single
new glyph pixel against the ordinary filter/reconstruction reference.

### Synchronous custom drawing attribution (fluidity round)

Default-off `ORB_DISPLAY_DRAW_PROFILE` now includes `DRAW_EVENTS`: inclusive
wall time in `lv_obj_send_event()` for DRAW_MAIN and DRAW_POST. Custom painters
such as Orb's chord-fitted cached rows write directly into the internal band,
so software-task profiling alone omitted their cost. This counter also contains
callback task drains and nested draw-event delivery; it overlaps software-task
and other stage timings and must never be summed as exclusive CPU time. Other
events and ordinary builds add no clocks or counter work. Pixels, callbacks,
render order and task scheduling are unchanged.

### Bounded damage overflow (2026-10-28)

`lv_inv_area` keeps all requested pixels when its fixed invalidation array fills,
merging the new area into the saved rectangle with the least bounding-box growth.
The upstream full-screen fallback amplified deferred game poses into 18–60%
panel writes on the board. This path allocates no memory, preserves all existing
rounding/blur expansion, and retains full-screen requests. The simulator
`damage_overflow` test checks every requested region and partial/full pixel
equality across repeated 64-region updates, while requiring sparse transfer area.

## Immutable custom software tasks for chord rows (2026-10-28)

`LV_DRAW_TASK_TYPE_CUSTOM` adds an immutable callback payload to the ordinary
software draw workers. `lv_draw_custom` transfers one payload lease only after
successful metadata allocation and intersection with the current layer clip.
Workers receive the saved task clip, intersected with their target buffer; they
must not inspect mutable object/style/layout state or `layer->_clip_area`.

Cleanup retires that lease exactly once on completion, allocation-related
failure or layer abort, after any active worker is fenced. Retirement uses the
existing UI dispatch/cleanup owner. The application captures row pose, palette,
insets, one shared 466-entry X map and child widths separately from the immutable
retained source and splits
disjoint Y areas so the two workers can execute concurrently. One optional pose
slot per prepared row is allocated through the PSRAM UI allocator, with a global
16 KiB cap and exact synchronous fallback. Cache statistics report optional
`draw_pose_bytes` separately from source/index/cut metadata, preserving the
independent source-metadata ceiling; total RAM includes both. The immutable map
adds 932 bytes per pose and shares that same 16 KiB pool, with an exact local-map
fallback for clips outside the table. Only a fully retired slot may regenerate
its map; unchanged scale reuses the immutable table across bands and frames.
Sources, text-cut tables and shared
ellipsis buffers remain leased until the last task retires; motion allocates no
UI cache storage.

`chord_lifetime` and its ASan/UBSan counterpart exercise actual row deletion
before queued completion, queued abort, failed tasks, pose allocation failure,
the pose budget cap and a rendezvous proving two workers can run independent
slices. `chord_list_asan` instruments the complete independent ARGB/native and
swapped565 pixel oracle. The isolated second-descriptor OOM proof is reproducible
with `work/fluid/pass3/chord-dispatch/validate_overlay.py`; its allocator failure
injection is confined to that test copy, not shipped firmware.

## Partial arc task area

`lv_draw_arc()` queues a partial (not full-ring, no image) arc with the task
area clipped to `lv_draw_arc_get_area()` instead of the full circle's square,
and drops an arc with no such area. `lv_draw_sw_arc()` rebuilds the ring
geometry from the descriptor's centre and radius rather than the task area, and
already clipped its pixels to that same bounding box, so every pixel is
identical. A short rim indicator, such as the kit's list scroll arc, no longer
enters, and serialises behind, every band of its full-screen square (Pass 5:
lists +2 to +3 moving fps on the board).

Files: `src/draw/lv_draw_arc.c`, `src/draw/sw/lv_draw_sw_arc.c`.

## Typographic truncation and command wrapping (polish-text)

Label DOT mode replaces a suffix with one U+2026 on a whole UTF-8 code point
boundary and trims preceding whitespace/separators/periods. It reserves the real
ellipsis advance and verifies the resulting line geometry, backing up whole code
points when kerning or wrapping needs more room. A viewport too small for a whole
ellipsis shows no fragment. The original string remains recoverable after resize,
including one- and two-byte originals whose buffers must grow for the replacement.
The existing four-byte save area is reused; ordinary truncation allocates nothing,
and layout measurements never enter the per-frame draw path.

`lv_label_set_path_breaks` / `LV_TEXT_FLAG_PATH_BREAKS` opt commands and paths into
breaks at spaces or after slashes. Hyphens, periods and overlong command tokens
stay intact; a caller must provide horizontal scrolling for an individual token
wider than its viewport. Default prose wrapping is unchanged. The flag follows
label measurement, hit testing and the existing draw descriptor path.

Files: `include/lvgl/font/lv_text.h`, `include/lvgl/widgets/lv_label.h`,
`src/misc/lv_text.c`, `src/widgets/label/lv_label.c`,
`src/widgets/label/lv_label_private.h`. `sim/tests/test_lvgl_text.c` covers real
Inter faces at all sizes/weights, narrow UTF-8 labels, resize restoration, status
and rescue ellipsis availability, and space/slash command boundaries.

### Final-line widow repair

`lv_label_set_avoid_widow` / `LV_TEXT_FLAG_AVOID_WIDOW` opt balanced headings and
captions into a local final-two-line repair. When greedy wrapping would leave one
word on the final line, move the previous whole word onto that line only if the
pair fits and the preceding line retains content. This handles unequal word
widths such as “Increase label contrast.” without editing semantic text or adding
a line. Explicit newlines, command/path text and recolored text are left alone.
Label size, draw descriptors and letter hit testing carry the same flag; the draw
flag field grows from five to six bits to retain it. The opt-in is restricted to
balanced kit captions/headings, keeping row scroll/source work unchanged. The
common non-final case stops scanning at the following word; only a one-word final
suffix needs extra width measurements. `test_lvgl_text` verifies real AX3 and
Bold Text captions, line count/width, letter positions, forwarded draw flags and
unchanged semantic strings.

### Stable sampling for native live row labels

`lv_label_set_stable_transform` opts the shared row engine's native live labels
into one inverse-transform sampling origin across render bands and damage clips.
The layer descriptor records the cropped source's offset from the label's original
draw bounds; the software sampler translates into that space before its existing
fixed-point calculation and back into the cropped source afterward. Without this,
changing a source crop changes division rounding, producing a few RGB565 edge
pixels that differ between retained and full redraws of the same scaled glyph.

Ordinary labels/images retain a zero offset and their existing pixel path. Source
layer bounds, clipping, live-value buffers, row caches and allocation cadence are
unchanged; no per-pixel division, new raster, font measurement or full-viewport
invalidation is added. `home_busy-room-last_ax3` and
`home_busy-room-last_ax3_bold_rt` exercise the failure through production input,
scrolling and the renderer's strict retained/full RGB oracle.

### Reject bitmap glyph ink outside a draw clip (label cost audit)

`src/draw/lv_draw_label.c` skips decoding a nonrotated bitmap glyph when its
ink bounds miss the task clip. Previously the full line-box background also
had to miss: a band touching only blank ascent/descent rows could decode every
glyph in the line. Selection backgrounds/decorations are emitted independently;
missing-glyph placeholders, rotated/outlined and nonbitmap glyphs retain their
conservative path. No font data, metrics, kerning or blending changes.

`sim/tests/test_label_ink_clip.c` checks six-band plain/selected text against
every reference pixel, plus the conservative cases. `label_work_face` also
bounds actual font/bitmap work in a production Face; Orbit, wake-to-Orbit,
cached iris and list-scroll fixtures guard task amplification and layout work.
Counters live only in test-local copies of the production implementations.

### Keep multiline selection backgrounds across render bands

`src/draw/lv_draw_label.c` finds the first visible line using the greater of
the font line box and the line pitch. Selection backgrounds include positive
line spacing; skipping a line based only on its font height omitted the
remaining background rows when a render band began in that spacing. Negative
line spacing retains the full font-height bound so descender ink still draws.
The glyph-ink decode rejection above is unchanged.

`sim/tests/test_label_ink_clip.c` compares whole labels with 84-row band clips
at every vertical band phase, covering wrapped and explicit multiline text
with and without selection, plus negative spacing and descending glyph ink.
The selected positive-spacing case fails without this change.

### Stable sampling for retained state discs and Pop in

`lv_obj_set_stable_transform` extends the existing native-label sampling opt-in
above to ordinary controls. The kit's flat state discs opt in during construction;
Pop in opts in for its finite lifetime. A held Home lock exposed the same fixed
point crop-phase error in its 94% pressed disc: 16 antialiased pixels at the lower
edge differed between partial and full frozen redraws after an unlock. Reusing
the canonical sampling origin fixes those exact pixels. The original unchanged
`test_home_view` strict held-lock oracle covers Default, Larger and AX3.

The flag defaults off. Other layers keep their existing sampling path. Source
bounds, band sizes, dirty areas and draw allocations remain unchanged; no full
screen damage, extra raster cache, geometry change or input transformation is
introduced by this sampling option. `test_alive_effects` also compares a pressed
flat disc's partial redraw to a full frozen redraw after changing its state.

### Letter task dependencies follow transformed glyph ink

`lv_draw_letter` used its unrotated advance/line-height rectangle as `_real_area`,
although the worker offsets glyph ink around its baseline pivot and rotates it.
The scheduler could admit overlapping curved letters or an earlier background
on separate workers, producing a nondeterministic title-edge pixel in repeated
Back captures. Tasks now advertise transformed bitmap bounds using the same
glyph origin and pivot as the worker. Rendering anchors, glyph pixels, clipping
and draw-unit count are unchanged.

`sim/tests/test_letter_task_bounds.c` uses the real scheduler to require a glyph
to wait for an overlapping worker at four rotations, then checks that completed
overlaps and unrelated regions permit it to run. `parity_smoke` retains exact
repeated-capture checks without a tolerance or ignored pixel.

### Event dispatch releases only a deleted owner's descriptors

`lv_event_send` backed up the list's descriptor array and released it whenever
`e->deleted` was set. `lv_event_mark_deleted` sets that flag when the event's
*original* target is deleted, so a bubbled event whose child was deleted by a
callback released the surviving ancestor's live array: a heap use-after-free.
Each dispatch now records its owner on a small stack; deletion marks only the
dispatch whose owner is the deleted object, saving that list's current array
(callbacks may have added descriptors) for release after the loop. Nested
dispatches on the same owner defer release to the outermost one, as before.
`sim/tests/test_event_owner_lifetime.c` covers the bubbled-delete case.
