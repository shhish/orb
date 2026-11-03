# Orb memory budget

The current accessibility switching fix and exact six-set/storage accounting are
in [fontsets-results.md](agent-briefs/wave2/fontsets-results.md). It supersedes the
historical font residency and two-set assumptions below.

The working-set table below is historical planning, not a current board
measurement. The 2026-10-31 candidate's exact font backing, dynamic cache floor,
expected owner deltas and board acceptance checks are in
[mem2-results.md](agent-briefs/wave2/mem2-results.md).

The board has **8,388,608 bytes of PSRAM**, not an 8 MiB graphics heap. XIP
instructions and constants also occupy it. The pre-change map reserved
`0x24ffe0` bytes in `.ext_ram.dummy`; the table rounds that upward to 2,432 KiB
to allow code growth. Replace that number from the exact candidate's map before
acceptance. Internal and DMA-capable heap views overlap; do not add their free
counts or count the output arena twice.

These are engineering allocations, **not new board measurements**. The host
font loader, using the checked-in signed-pack payload and all required variants,
reports 1,673,516 bytes resident at Default and 2,188,240 at AX3 Bold, before
allocator headers and compressed staging. Neither the fonts nor their required
fallback chains have been removed to fund graphics.

## Expected working set

The two columns are complete, alternative steady working sets. Both total
7 MiB, leaving **1 MiB operating margin** for alignment, allocator headers,
fragmentation, cold preparation and managed radio/TLS allocations. They are
planning limits for the lead to validate with the combined workload, not a claim
that radio/TLS is exactly the size in this table.

| Owner | Default, KiB | AX3 Bold, KiB | Accounting / lifetime |
| --- | ---: | ---: | --- |
| PSRAM XIP code, rodata and alignment | 2,432 | 2,432 | Linker map; outside heap owner totals |
| Navigation pool and glyph allowance | 1,280 | 1,280 | Boot-reserved; 3 × 434,312-byte RGB565 planes = 1,302,936 bytes |
| Fonts/assets | 1,664 | 2,176 | Exact allocation charge, includes parsed metadata; published faces immutable |
| **All optional graphics caches together** | **1,024** | **512** | Shared 1,024 KiB maximum; shrink with actual free-heap pressure |
| Generic LVGL objects, styles, draw tasks | 256 | 256 | Explicit PSRAM allocation charge; 512 KiB owner ceiling |
| Application JSON/network payloads | 192 | 192 | Explicit JSON allocation charge; 384 KiB owner ceiling |
| Retained session/Home model stores | 128 | 128 | Exact retained-store getters, including an open idle page |
| Game state and view bookkeeping | 64 | 64 | PSRAM-only owner allocation; LVGL widgets remain in generic LVGL |
| Cold/profiler workspace | 128 | 128 | Transient; 256 KiB envelope, never per animation frame |
| **Expected live total** | **7,168** | **7,168** | **7,340,032 bytes** |
| **Operating margin** | **1,024** | **1,024** | **1,048,576 bytes** |

The owner ceilings are independent rejection limits, **not additional reserved
pools** and not quantities to add on top of this table. In particular the font
allocator has a 5 MiB preparation envelope to retain an existing complete font
set while preparing another. That is not permission to allocate 5 MiB alongside
every graphics maximum. The current complete set remains authoritative if a
setting cannot be prepared; its fallback chain is never partially published.
Heap receipts must check the actual peak during such a change separately.

The network owner includes cJSON, fragmented input, queued voice packets and
private pairing PEM buffers. A negotiated-card input allocation is at most
4,097 bytes; 32 queued entries plus the assembly can reach 135,201 bytes.
Legacy/Home input permits 16,385 bytes, so its theoretical 540,705-byte queue
cannot bypass the combined 384 KiB owner ceiling. Queued audio packets are at
most 1,031 bytes each (32,992 bytes for 32); the queue mixes text and audio, so
these maxima are alternatives. Managed WebSocket RX/TX payloads (8,192 bytes),
TLS state and the public caller-owned HTTPS certificate remain in the explicitly
reported residual. Their allocation/free ABI is preserved.

## Cache admission and eviction

`orb_memory` maintains allocation-free atomic used/peak/budget/rejection counters
per owner. `heap` exposes `HEAP_OWNER` receipts and a separate shared-cache total.
Explicit cache payloads use their cache owner, so they are not charged again to
the generic LVGL allocator. Allocator headers and fragmentation belong to the
operating margin. Existing model/Home stores report their exact payload sizes
without changing their allocation ABI.

The graphics owners below **share** a single 1 MiB aggregate ceiling:

| Optional graphics owner | Individual ceiling | Eviction policy |
| --- | ---: | --- |
| Source rasters | 768 KiB | Least recently used unpinned source |
| Full-page raster | 448 KiB | At most one 466 × 466 RGB565 page; unpinned LRU |
| Material source mips | 192 KiB | Retired with their owning source |
| Native-size row glyph atlases | 512 KiB | Unreferenced LRU face/glyph storage |
| SDF glass masks | 640 KiB | Unreferenced LRU geometry; active controls pinned |
| Exact blur/material retention together | 192 KiB | Local leased-entry LRU / ordinary renderer fallback |

The sum of the individual ceilings is intentionally larger than 1 MiB. This
allows a page slide, a list, and glass to use different distributions without
retaining every screen's maximum. UI cache admission evicts the globally oldest
eligible entry, using a shared monotonic stamp, until both the owner and shared
ceiling fit. Every shared cache allocation also checks an actual free-PSRAM
admission floor after its payload: **640 KiB + min(max(mapped bitmap bytes -
16 KiB, 0), 128 KiB)**. Default's flash bitmaps fund a 768 KiB floor; unmapped
accessibility faces retain 640 KiB. Required allocations can cross this floor;
it is not reserved memory. `HEAP_CACHE margin` reports the same policy getter.
The draw-worker exact-blur cache uses atomic admission without invoking UI
eviction callbacks. It remains bounded and uses the ordinary renderer if no
reservation fits. Pinned/leased entries are never evicted.

The free-heap test applies at cache admission; subsequent required allocations
can consume that margin. They do not invoke UI eviction callbacks from a
network/font worker. The full workload's physical free/low-water receipts,
including managed TLS/radio buffers in the residual, remain the acceptance
check; the table does not turn the free-heap test into a system-wide guarantee.

Retained planes are independent of optional caches. Navigation preallocates its
RGB565 storage in `app_main` immediately after board initialization, before
fonts/assets, radio startup or neighbour preparation, and never asks
the heap for another plane during a gesture. Allocation failure at boot is
reported; a normal admitted surface cannot lose its plane to a font/raster LRU.
Live consequences still invalidate/cancel stale snapshots according to the
interaction contract.

Cold snapshots borrow the drained display's existing full internal arena.
LVGL renders guarded absolute-coordinate regions there and finishes their draw
tasks before each sequential copy into the PSRAM plane. The whole-control band
guard keeps live backdrop blur in one region. Capturing does not send pixels to
the panel or create another DMA arena.

Opaque black-matte rows and whole pages render directly to RGB565: one full
page is 434,312 bytes. The old 868,624-byte full-screen ARGB8888 intermediate
is forbidden by the per-raster workspace bound. Transparent edges keep their
required alpha representation. Negative raster admission is retained through
the scroll epoch, so an oversized/failed allocation is not retried every band.

## Internal and DMA memory

The display arena stays **78,336 bytes internal DMA**, with two 39,144-byte
usable halves 39,168 bytes apart, and the existing 78,288-byte full-band alias.
No second arena, PSRAM DMA source, smaller wire band or RGB888 output is added.
Actual SPI drain still owns when either half becomes writable.

Generic LVGL allocations and JSON nodes now request PSRAM explicitly, including
small allocations. They cannot fall back into internal/DMA memory when PSRAM
is pressured. The ordinary draw-scratch policy uses PSRAM; the optional bounded
internal experiment retains its 16 KiB concurrency ceiling and 24 KiB free
reserve. Wi-Fi/TLS, sender/radio task stacks and display DMA sizes are preserved.

The supplied board receipts put TLS/connect phase minima at 48,767 internal /
40,979 DMA bytes, whereas the lifetime minimum reaches 7,796 / 8 bytes during
the first online redraw. That supports investigating UI allocation pressure;
it does not establish a measured radio root cause. New boot-only `ui-refresh`
and `wifi-start` phase receipts distinguish these paths. The lead must prove
**lifetime internal ≥ 12,288 bytes and DMA ≥ 4,096 bytes**, including the first
20 seconds and the combined workload. Host tests cannot establish those gates.

## Verification

### Performance-round correction to lifetime minima

The supplied later capture reports PSRAM `min-free=7792` while every monitored
phase stays above 1.4 MiB. This is reproducible without any PSRAM allocation:
IDF 6.1's local-minimum monitor saves per-region history by heap-list ordinal;
Bluetooth release prepends new internal regions while `wifi-start` is active.
Stopping that monitor can restore an old internal minimum onto PSRAM. Both
Bluetooth-release paths now stop monitoring under the phase mutex before heap
insertion and restart with the new ordering afterward. Real historical lows
are preserved; no minimum is reset to manufacture headroom.

Six persistent boot phases distinguish board init, reserved nav planes, asset
verification, storage mount, font preload and glass prewarm. Task-only phase
bookkeeping uses PSRAM; the flash-safe allocation-failure callback retains
internal counters. The next real capture must prove PSRAM lifetime >= 256 KiB
and preserve the supplied internal/DMA floors (25,280 / 23,352 B). The regression
test explains the suspect historical value; it does not establish the new
device's true minimum or justify removing required fonts/snapshots.

`firmware/components/orb_memory/tests/host` exercises owner ceilings, overflow,
balanced charge/release, aligned allocation, preserving realloc failure,
global LRU pressure and pinned entries, plus 40,000 concurrent reserve/release
pairs. The simulator registers the same test as `memory_budget`.

The final board run must archive `heap` before and after scroll/open/nav; compare
`HEAP_OWNER` used/peak to this table and compare owner totals with physical PSRAM
used. The difference includes XIP, allocator overhead and uninstrumented IDF
buffers: never label it as a leak or silently report it as tracked network RAM.
Run admission again after the full workload, inspect retained reason receipts,
and check first changed **band** for Back separately from first complete changed
frame. Preserve the candidate ELF for slow-callback attribution.
