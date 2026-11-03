# orb_fonts

Orb's type and icons for LVGL 9.6: **Inter** set to Apple's watchOS text styles at every Dynamic Type size Orb offers, and an **SF Symbols-like icon set** based on Framework7 Icons 5.0.5 and offset to the weight of the text beside it. Everything is prerendered at 4 bpp with kerning and generated from pinned sources by `scripts/fonts.sh`. All normal faces live in a signed, versioned pack in `assets`; the signed application keeps rescue 28 px Inter, matching Framework7 icons and compact status markings.

![Sessions screen](../../../docs/screens/fonts/screen-sessions.png) ![Face](../../../docs/screens/fonts/screen-face.png)

![Content sizes](../../../docs/screens/fonts/content-sizes.png)

Previews of every style, the content sizes, Bold Text, the clocks and all icons are in [docs/screens/fonts](../../../docs/screens/fonts). Typography sheets use Pillow from the same instanced fonts at the same pixel sizes, laid out with lv_font_conv's advances and kerning, and checked against LVGL (see Verification). `status.png` and `screen-face.png` are drawn entirely by the real LVGL fork on the Mac, including the live battery and Wi-Fi widgets; Pillow only encodes their RGB framebuffers as PNG.

## Using it

```c
#include "orb_fonts.h"

orb_text_apply(title, ORB_TEXT_LARGE_TITLE, ORB_TEXT_REGULAR, 0);   /* font, tracking, leading */
orb_text_apply(row, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
lv_label_set_text(row, ORB_ICON_TERMINAL " esp32s3-firmware");     /* icons inline, any style */
orb_text_apply(timer, ORB_TEXT_BODY, ORB_TEXT_TABULAR, 0);          /* figures that don't jiggle */
lv_label_set_text(timer, "Elapsed: 01" ORB_TIME_SEP "23");          /* time colon raised, ':' not */
orb_icon_apply(button_glyph, ORB_TEXT_TITLE3, ORB_TEXT_REGULAR, 0); /* icon-only label, centred */
orb_clock_apply(face_time, ORB_CLOCK_LARGE, 0);

orb_fonts_set_content_size(ORB_CONTENT_SIZE_LARGER);               /* Settings > Text Size */
orb_fonts_set_bold_text(true);                                      /* Settings > Bold Text */
```

- `orb_text_style_t`: `ORB_TEXT_LARGE_TITLE` … `ORB_TEXT_FOOTNOTE2`. Variant flags: `ORB_TEXT_EMPHASIZED` (Apple's bold variant) and `ORB_TEXT_TABULAR` (tabular figures).
- `orb_text_apply()` / `orb_icon_apply()` / `orb_clock_apply()` give an object one of orb_fonts' **shared styles** (`orb_text_style()`, `orb_icon_style()`, `orb_clock_style()`: `text_font`, `text_letter_space`, `text_line_space`), replacing any orb_fonts style it had for that selector. Shared styles follow the content size and Bold Text: when either changes, they switch fonts and LVGL re-lays out every object using them.
- `orb_font(style)`, `orb_font_variant(style, flags)`, `orb_icon_font(style)`, `orb_icon_font_variant(style, flags)`, `orb_clock_font(clock)`: lifetime-stable `lv_font_t *` handles for the current semantic role. Retained pointers and `orb_text_style_init()` snapshots remain valid after switches; only backing records are freed. Observe `orb_fonts_subject()` to re-measure geometry and spacing. Pointer-keyed caches also use `orb_fonts_revision()` because a retained handle follows the new typography.
- `orb_text_style_info(style)` / `orb_clock_info(clock)`: point size, px, Apple leading, tracking, letter space and weights at the current setting.
- `orb_text_line_space(style, flags)`: the `text_line_space` that makes LVGL's line pitch Apple's leading.
- `orb_status_font(style, flags)`: compact tabular Inter figures plus Framework7 `bolt_fill` for the live battery. Sizes follow the text style (44% of its pixel size, minimum 10 px: Body 12/14/17 px at Default/Larger/AX3), using Semibold for regular labels and Bold for emphasized labels. All 34 tiny subsets are in the app image. Widgets use `orb_status_font_for_cap()` with their measured cap height so a redraw never reads the pack and digits still fit if loading a larger text font failed. These are internal status markings, not a replacement for readable text labels.
- `ORB_TIME_SEP` is U+2236, a colon raised to the figures' centre, for times. A plain `:` sits on the baseline in text and tabular fonts, so "Elapsed:" looks right; clock fonts raise both.
- Icons are UTF-8 string macros in `orb_icons.h` (`ORB_ICON_MIC`, `ORB_ICON_WIFI_HIGH`, `ORB_ICON_BELL_FILL`, …) in the private use area U+E000–U+E1FF. `ORB_ICONS_FOREACH(X)` lists them all.
- `orb_fonts_notices()`: Inter's, Framework7's and Phosphor's licences, for Settings > About > Legal.
- Call orb_fonts with the LVGL lock held, like any LVGL function.

## Firmware preload and setting changes

After LVGL starts and storage is mounted, `app_main` calls
`orb_fonts_preload_saved()` before glass prewarm, shell construction or Wi-Fi.
Only the saved size/Bold pair is loaded; a Bold user never also pays for Default.
Missing or incompatible assets keep signed-app rescue typography and report a
failure. UI lookups never read flash or inflate a font.

The pack stores 123 selected records raw, distributed across all six sets. Their
bitmaps map below physical flash address 16 MiB; other records are zlib streams.
Owned wire glyph arrays are converted in place before publication, avoiding a
second native glyph array. Streaming inflation retains a 1 KiB input window and
10,992-byte inflater instead of the whole compressed stream. All font allocations
use PSRAM and preserve a **384 KiB physical free-heap floor**.

Firmware setters accept a deferred request; `orb_fonts_pending()` distinguishes
acceptance from application and `orb_fonts_last_result()` reports completion.
Main services the latest pair on its existing internal stack under LVGL. Drawing
is drained, glass leases are reversibly released, and optional caches are trimmed.
The panel keeps its last complete frame while the immutable set loads. Every
retained handle first parks on app-owned rescue faces; old records not shared by
the requested set are freed **before** loading replacements. All six generated
sets declare their complete fallback closure, asserted by the saved-boot tests;
loaded roots also retain any non-top-level fallback chain. Shared styles and
observers re-measure only after the transaction resolves, and glass leases rebuild
before LVGL unlocks. Public glyph descriptors retain Unicode and stable fallback
handles, never record pointers; obsolete bitmap geometry is refused safely.

The first failed load trims every optional owner again and retries once. A second
failure discards requested-only records and reloads the previous set. Freeing the
partial set restores capacity for the old records with fixed external allocations;
180 first-fit fragmentation histories (six predecessors, all 30 distinct directed
failures) restore the exact previous pair and residency without allocation refusal.
Networking can allocate concurrently, so recovered holes are not a reservation.
If reload also fails, Default is attempted; if that fails, every pack record is
released and immutable app fonts keep every retained pointer usable. The original
failure remains observable, applied settings describe the recovered pair, and a
later retry can succeed. Only one completion is published, including coalescing
back to the already applied pair.

A separate assets transaction mutex keeps single-slot assets OTA from copying or
erasing between retirement and rollback. An OTA already in progress rejects the
font request before retirement; an OTA arriving during a switch waits before its
first bitmap copy. Per-read/decode locking remains independent. App OTA and TLS
continue while LVGL is paused. This is a rare settings transaction, with the panel
retaining its last complete frame throughout.

Only a committed pair is saved, outside LVGL, through atomic `orb_fs_write` to
`system/text` (schema byte, size byte, Bold byte). Invalid or missing saved
records use Default. A saved set that cannot load is retained on disk for a later
healthy boot while the current boot reports its readable fallback honestly.
Host tools retain synchronous setters for deterministic fixtures.

`orb_fonts_load_stats()` reports cold loads, failures, top-level load time,
record/parsed-metadata residency, mapped bitmap bytes and migrated bitmap copies.
It excludes lifetime handles, the pointer table, lease objects and allocator headers.
`python3 tools/fonts/budget.py --json` reports record/table costs for all 36
transitions; add **18,160 bytes** for the fixed six banks of 378 lifetime handles
(48 bytes each plus one owner header). The extended `async_fonts` fixture includes
this charge and retained UI cache metadata. The build checks the 48-byte handle, 108-byte parsed record,
24-byte native cmap, 8-byte glyph and 10,992-byte inflater ABI. These are calculated
budgets; real heap minima, fragmentation and latency remain board release gates.

Calculated peak bytes for all 36 transitions, including the pointer table,
record metadata, bitmap leases, known owner headers, bounded inflater/input and
18,160-byte lifetime handles; fixed status wrappers and backend/MMU overhead are
excluded. Rows are the old set, columns the requested set. `B` means Bold Text.
The largest single allocation is **127,960 bytes**, below the reported 737 KB
largest board block; the host fixture additionally caps contiguous holes at
256 KiB while leaving substantially more total free memory.

| From / to | Default | Default B | Larger | Larger B | AX3 | AX3 B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Default | 1,177,620 | 1,264,136 | 1,198,960 | 1,260,416 | 1,198,276 | 1,252,580 |
| Default B | 1,253,820 | 1,253,820 | 1,253,820 | 1,262,000 | 1,253,820 | 1,260,968 |
| Larger | 1,190,572 | 1,264,136 | 1,190,572 | 1,260,416 | 1,190,572 | 1,252,580 |
| Larger B | 1,253,612 | 1,265,720 | 1,253,612 | 1,253,612 | 1,253,612 | 1,253,612 |
| AX3 | 1,189,888 | 1,264,136 | 1,202,472 | 1,260,416 | 1,189,888 | 1,252,580 |
| AX3 B | 1,252,580 | 1,265,720 | 1,252,580 | 1,262,000 | 1,252,580 | 1,252,580 |

The worst calculated peak is **1,265,720 bytes**, down from **2,435,592 bytes**
with the old overlap order, with no pack change. Using the rounded-down supplied
receipt (1,360,000 bytes free plus 800,000 bytes reclaimable caches), the model's
minimum switch free heap is **2,073,484 bytes**. After charging every target
flash-bitmap migration copy plus a concurrent **128 KiB TLS allocation**, the
minimum is **686,320 bytes** across all 36 directions. Thus the supplied receipt
covers app OTA/TLS and the larger assets-migration case in this model. The older,
more conservative host receipt is retained for switch tests and is not claimed
to guarantee the full assets-migration charge. The port still enforces a physical
**384 KiB admission floor** on each font allocation (128 KiB TLS + 256 KiB margin),
not a reserved heap region. Arbitrary concurrent growth, allocator/MMU overhead,
real latency and exact heap minima require lead board validation; these are
calculated/modelled values, not new board measurements.


## Text styles and Dynamic Type

From [docs/hig.md](../../../docs/hig.md) section 4 and Apple's watchOS Dynamic Type tables, at 1 pt = 1.634 px: **Default** is xLarge (the default on the larger watches), **Larger** is xxxLarge, **AX3** is watchOS's largest accessibility size (Body 135% of Default). Bold Text moves every weight one step up Apple's ladder: Regular to Medium, Semibold to Bold, Bold to Heavy.

<!-- styles:begin -->
| Style | Weight | Emphasized | Default (xLarge) | Larger (xxxLarge) | AX3 |
| --- | --- | --- | ---: | ---: | ---: |
| Large Title | Regular (Medium) | Bold (Heavy) | 65 / 69 px · −20 | 69 / 73 px · −20 | 75 / 79 px · −20 |
| Title 1 | Regular (Medium) | Semibold (Bold) | 62 / 66 px · −20 | 65 / 69 px · −20 | 72 / 77 px · −20 |
| Title 2 | Regular (Medium) | Semibold (Bold) | 49 / 53 px · −14 | 52 / 56 px · −16 | 59 / 63 px · −20 |
| Title 3 | Regular (Medium) | Semibold (Bold) | 33 / 37 px · 0 | 36 / 40 px · −4 | 42 / 47 px · −11 |
| Headline | Semibold (Bold) | Semibold (Bold) | 28 / 32 px · −4 | 31 / 35 px · −12 | 38 / 42 px · −6 |
| Body | Regular (Medium) | Semibold (Bold) | 28 / 32 px · −4 | 31 / 35 px · −12 | 38 / 42 px · −6 |
| Caption 1 | Regular (Medium) | Semibold (Bold) | 26 / 30 px · 0 | 29 / 33 px · −8 | 33 / 37 px · 0 |
| Caption 2 | Regular (Medium) | Semibold (Bold) | 25 / 29 px · +4 | 28 / 32 px · −4 | 31 / 35 px · −12 |
| Footnote 1 | Regular (Medium) | Semibold (Bold) | 23 / 27 px · +14 | 26 / 30 px · 0 | 29 / 33 px · −8 |
| Footnote 2 | Regular (Medium) | Semibold (Bold) | 21 / 25 px · +16 | 25 / 29 px · +4 | 28 / 32 px · −4 |

Each size cell is px / leading, then Apple's tracking (1/1000 em) for that point size. Weights in brackets are with Bold Text.
<!-- styles:end -->

- **Optical sizes.** Inter's `opsz` axis is continuous from 14 to 32, like SF's dynamic optical sizes: every size uses the optical size of its point size (Body 17, Title 3 20, Title 2 30, Large Title 32). Each size is its own instance anyway, so this costs nothing.
- **Tracking.** Apple tracks by point size, so every size takes SF Compact's value for its point size from Apple's table. LVGL's `letter_space` is whole pixels and every glyph advance is rounded to a pixel, so the whole-pixel part goes in `letter_space` (set by the shared styles) and the rest is built into each glyph's advance in the font (split evenly on both sides). Labels that only set the font still get the fractional part. The values are SF Compact's; re-tune them on the device if Inter wants different ones (hig.md says so too).
- **Leading.** LVGL's line box comes from the tallest and lowest glyphs in the font (accented capitals), so it is taller than Apple's leading. The shared styles set a negative `text_line_space` so lines sit `leading` px apart, baseline to baseline.
- **Sharing.** A font is one size, weight and optical size, so styles and sizes that land on the same one share it: Body emphasized is the Headline font, Larger's Caption 2 is Default's Body, AX3's Title 3 is Default's Title 3, and so on. Icons are made at every text size (no borrowing), so they are always centred on their own text's cap height.
- **Tabular figures.** Every style and weight has a tabular companion font holding only 0–9, `. , + −`, `:`, `ORB_TIME_SEP` and the figure space, with Inter's `tnum` forms frozen in: `+ −` and `ORB_TIME_SEP` as Inter's `calt` shapes them between figures, `:` as after a word (on the baseline). `-` isn't in it, so "Wi-Fi" keeps the text hyphen. Its fallback is the normal font, so with `ORB_TEXT_TABULAR` letters and icons still work and only the figures change. It carries its text font's line box.
- **Clocks.** `ORB_CLOCK_LARGE` (140 px) and `ORB_CLOCK_MEDIUM` (96 px): Inter Semibold (Bold with Bold Text) at opsz 32 with tabular figures, the raised colon, space, A, M and P, tracked −10/1000. They don't follow the content size.
- **Always On.** `ORB_CLOCK_ALWAYS_ON` uses the frame's needed 128 px size and approximately −4 px tracking in Inter Light (300), moving to Regular (400) with Bold Text. This is the owner's narrow low-ink exception to HIG §4's general weight rule and IMPLEMENT's existing-size rule, reducing lit pixels on the dim retained clock; ordinary text and active clocks keep their existing weights. The generated pack identity changes, so the lead must re-release the owner-signed fonts + sounds assets together with this application before board review. No signed assets are produced by font generation.
- **Characters.** Printable ASCII; Latin-1 (except the soft hyphen, which Inter lacks); the Windows-1252 extras (€ ‚ ƒ „ … † ‡ ˆ ‰ Š ‹ Œ Ž ‘ ’ “ ” • – — ˜ ™ š › œ ž Ÿ); ‐ ‑ ‒ ′ ″ ⁄ ₹ № − ≈ ≠ ≤ ≥ ∞ ∶; arrows ← ↑ → ↓ ↔; keys ⇧ ⌘ ⌥ ⌃ ⌫ ⏎; ✓ ✗ ● ○ ★ ☆; figure, thin, hair and narrow no-break spaces. 254 glyphs per text font.

## Icons

The primary source is [Framework7 Icons 5.0.5](https://github.com/framework7io/framework7-icons) (MIT), with SF-style names and silhouettes. Orb retains its original public macro names and private-use codepoints: `CHECK` uses `checkmark`, `GEAR` uses `gear_alt`, `LIGHTNING` uses `bolt`, `SPARKLE` uses `sparkles`, and so on. Every mapping is explicit in `tools/fonts/spec.py` and emitted beside the macro in `include/orb_icons.h`.

- **Weight follows Inter.** Framework7 ships closed filled paths at one weight on a 56-unit grid. The generator measures Inter's stem at each size, weight and optical size, targets 0.9 of that width, and offsets the source outline with a round-joined skia-pathops Minkowski sum/difference. Its reference stem is 4 units (Framework7's circle border). Body targets 4.22 units Regular and 6.12 Semibold; the offset on each edge is `(target - 4) / 2`. This derives Medium, Semibold, Bold and Heavy without pixel dilation or substituting another icon family.
- **Native fills.** Framework7's `*_fill` artwork is used where available, including `bolt_fill`. Its bounds are registered to the matching outline before and after the weight offset, preserving native interior detail and swap-stable extents. The already-solid `sparkles` supplies both sparkle macros; `smallcircle_fill_circle` is the filled record form. Phosphor fallback fills retain the previous region-based weight matching.
- **Cap-height alignment.** Framework7's 47.8126-unit circle spans 1.22 text cap heights. Its source centre maps to half Inter's cap height. Icon-only fonts keep a symmetric line box; inline text fallback stays unchanged.
- **Stable swaps.** Every outline/fill pair and existing state group (Wi-Fi, battery, speaker, mic, bell, lock, play/pause/stop) shares an advance and source centre. Switching states does not shift the surrounding text.
- **Legacy battery and Wi-Fi macros remain available.** Wi-Fi levels select Framework7 fan contours (empty reception outlines the bottom wedge); battery levels derive 80/50/20% fills from Framework7's body, with Framework7 bolt/exclamation cut-outs for compatibility states. Body and fill are offset separately with a minimum separator (about 0.75 px at Body); charging/warning knockouts are applied after weighting so they remain open in Semibold and Bold. **Use [orb_status](../orb_status/README.md) for live indicators**, with proportional battery fill, colour, percentage and RSSI strength instead of font-state swaps.
- **ABI preserved and extended:** 109 base icons, 27 filled forms, 136 glyphs per full icon font. New icons are appended; existing codepoints and artwork stay unchanged. The shell additions are `PROMPT`, `UPLOAD`, `FLASHLIGHT`, `ACCESSIBILITY`, `SEEK_BACK`, `SEEK_FORWARD` and `MUSIC_NOTE`.

Only these icons retain Phosphor 2.1.1, because Framework7 5.0.5 has no reasonable equivalent:

| Orb macro (without `ORB_ICON_`) | Phosphor asset | Reason |
| --- | --- | --- |
| `BLUETOOTH` | `bluetooth` | No Bluetooth symbol |
| `BLUETOOTH_SLASH` | `bluetooth-slash` | No Bluetooth disabled state |
| `BLUETOOTH_CONNECTED` | `bluetooth-connected` | No Bluetooth connected state |
| `WATCH` | `watch` | No watch/device-watch glyph |
| `TERMINAL`, `TERMINAL_FILL` | `terminal-window` | No terminal prompt glyph |
| `ROBOT` | `robot` | No robot glyph |
| `GIT_PULL_REQUEST` | `git-pull-request` | No pull-request graph; `arrow_merge` conveys a different operation |
| `PROMPT` | `terminal` | No plain prompt without a window |
| `FLASHLIGHT` | `flashlight` | No torch glyph |
| `ACCESSIBILITY` | `person-arms-spread` | No full accessibility figure |

These ten base icons (eleven glyphs including terminal's fill) use the existing Phosphor stroke matching, normalized to Framework7's circle size. `GIT_BRANCH` uses Framework7 `arrow_branch`.

## Storage and budget

Six sets of fonts: three content sizes, each with and without Bold Text. Of 271 distinct fonts, **36 built-in fonts use 112,913 bytes**: 28 px Inter Regular, its Framework7 icon fallback, and 34 compact battery subsets (43,282 bytes). Keeping status subsets avoids allocations or flash reads while drawing battery indicators. The other **235 fonts use 8,031,836 bytes** in `pack/orb_fonts.pack`. The seven shell glyphs add 1,020 bytes to the rescue icon font; both new Always On weights live only in the pack.

The v2 flashable image contains the 64-byte `ORBASSET` header, 32-byte section
directory, 8,031,836-byte font pack, 344,008-byte sound bank, erased-byte alignment
and a 4096-byte signature sector: **8,380,416 bytes**, leaving **8,192 bytes** in
the 8 MiB assets partition. The last raw record ends at physical `0xFFD354`, below
the 16 MiB cache aperture. A newly signed assets version must ship with this app;
no partition, bootloader or flash addressing changes are required.

- **Signing.** `ORB_SIGNING_KEY=/private/key.pem scripts/fonts.sh assets --version 1 -o /private/orb-assets.signed.bin` wraps/signs the pack. Use the same key as the running app. Initial installation uses `scripts/migrate-partitions.sh`; later updates use `orb-hub release --assets`. The old `orb-fonts-flash` target is intentionally removed.
- **Loading.** Raw records retain metadata in PSRAM and lease bitmap-only flash
  mappings. Mapping failure uses a full PSRAM copy if the physical floor permits;
  otherwise the switch fails safely. Compressed records stream through bounded
  PSRAM workspace. Every selected style, emphasis, tabular face, icon and clock is
  prepared before publication. No glyph range, metric or logical font ID changed.
- **Compatibility.** The generated table checks the pack's exact id and length before interpreting record offsets. A signed pack for another app stays unavailable. Missing/corrupt/incompatible assets or allocation failures yield the built-in 28 px rescue face; every text, clock and icon API remains non-NULL. Accessibility settings requiring a missing pack return an error instead of reporting a false setting.
- **Updates.** A mutex serializes bitmap decoding and record reads with single-slot assets updates. Before allowing the first erase, every flash lease is copied into PSRAM under that mutex. If any copy fails, all provisional copies are freed and the update returns out-of-memory with the old assets and fonts intact. A successful migration preserves all active font pointers through successful or failed updates; glyph callbacks decode into caller-owned A8 buffers and never return flash pointers. The installer verifies the new image on its own task stack, and the next boot activates it. App-only OTA retains the signed rescue face. **This layout requires a newly signed assets pack shipped with its matching application**; the new pack is 8,031,836 bytes before the sound bundle and signing envelope.
- **Boot RAM.** Verification uses a transient 8 KiB task stack (plus task/crypto allocations), avoiding the 3,584-byte main stack. It exits after verification; three static semaphore buffers, cached metadata and the read/switch mutexes persist. Font reads and link callbacks use cached trust and never run RSA.

**Compression.** Text below 33 px is stored plain: lists and body text draw many small glyphs per frame and plain 4 bpp decodes several times faster than LVGL's bit-by-bit RLE. Titles (33 px and up), clocks and all icon fonts (a handful per screen) are RLE-compressed with lv_font_conv's line prefilter. orb_fonts' Kconfig selects `CONFIG_LV_USE_FONT_COMPRESSED`; `orb_fonts.c` refuses to build without it. With two draw units, both threads decode RLE glyphs at once; LVGL kept the decoder's state in one global, so Orb's LVGL fork keeps it per call (see its `ORB_PATCHES.md`). When flashed, `test_app` prints how long LVGL takes to render a full screen of each style in each set.

<!-- sizes:begin -->
**In the app image** (28 px rescue text/icons plus compact battery markings):

| Font | Used by | px | opsz | Glyphs | Bitmaps (KiB) | Total (KiB) | Stored |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| `orb_icons_28_regular` | Body, Caption 2 (Larger), Footnote 2 (AX3) | 28 | 17 | 180 | 30.7 | 32.3 | RLE |
| `orb_status_33_bold` | Live battery digits + bolt | 33 | 14 | 12 | 1.6 | 1.8 | RLE |
| `orb_status_33_semibold` | Live battery digits + bolt | 33 | 14 | 12 | 1.7 | 1.8 | RLE |
| `orb_status_32_bold` | Live battery digits + bolt | 32 | 14 | 12 | 2.4 | 2.6 | plain |
| `orb_status_32_semibold` | Live battery digits + bolt | 32 | 14 | 12 | 2.3 | 2.5 | plain |
| `orb_status_30_bold` | Live battery digits + bolt | 30 | 14 | 12 | 2.2 | 2.3 | plain |
| `orb_status_30_semibold` | Live battery digits + bolt | 30 | 14 | 12 | 2.1 | 2.2 | plain |
| `orb_status_29_bold` | Live battery digits + bolt | 29 | 14 | 12 | 2.0 | 2.2 | plain |
| `orb_status_29_semibold` | Live battery digits + bolt | 29 | 14 | 12 | 2.0 | 2.1 | plain |
| `orb_status_27_bold` | Live battery digits + bolt | 27 | 14 | 12 | 1.7 | 1.9 | plain |
| `orb_status_27_semibold` | Live battery digits + bolt | 27 | 14 | 12 | 1.7 | 1.8 | plain |
| `orb_status_26_bold` | Live battery digits + bolt | 26 | 14 | 12 | 1.6 | 1.8 | plain |
| `orb_status_26_semibold` | Live battery digits + bolt | 26 | 14 | 12 | 1.6 | 1.7 | plain |
| `orb_status_23_bold` | Live battery digits + bolt | 23 | 14 | 12 | 1.2 | 1.4 | plain |
| `orb_status_23_semibold` | Live battery digits + bolt | 23 | 14 | 12 | 1.2 | 1.4 | plain |
| `orb_status_22_bold` | Live battery digits + bolt | 22 | 14 | 12 | 1.2 | 1.3 | plain |
| `orb_status_22_semibold` | Live battery digits + bolt | 22 | 14 | 12 | 1.2 | 1.3 | plain |
| `orb_status_18_bold` | Live battery digits + bolt | 18 | 14 | 12 | 0.8 | 1.0 | plain |
| `orb_status_18_semibold` | Live battery digits + bolt | 18 | 14 | 12 | 0.8 | 1.0 | plain |
| `orb_status_17_bold` | Live battery digits + bolt | 17 | 14 | 12 | 0.7 | 0.9 | plain |
| `orb_status_17_semibold` | Live battery digits + bolt | 17 | 14 | 12 | 0.7 | 0.9 | plain |
| `orb_status_16_bold` | Live battery digits + bolt | 16 | 14 | 12 | 0.7 | 0.8 | plain |
| `orb_status_16_semibold` | Live battery digits + bolt | 16 | 14 | 12 | 0.7 | 0.8 | plain |
| `orb_status_15_bold` | Live battery digits + bolt | 15 | 14 | 12 | 0.6 | 0.7 | plain |
| `orb_status_15_semibold` | Live battery digits + bolt | 15 | 14 | 12 | 0.6 | 0.7 | plain |
| `orb_status_14_bold` | Live battery digits + bolt | 14 | 14 | 12 | 0.6 | 0.7 | plain |
| `orb_status_14_semibold` | Live battery digits + bolt | 14 | 14 | 12 | 0.5 | 0.7 | plain |
| `orb_status_13_bold` | Live battery digits + bolt | 13 | 14 | 12 | 0.4 | 0.6 | plain |
| `orb_status_13_semibold` | Live battery digits + bolt | 13 | 14 | 12 | 0.4 | 0.6 | plain |
| `orb_status_12_bold` | Live battery digits + bolt | 12 | 14 | 12 | 0.4 | 0.5 | plain |
| `orb_status_12_semibold` | Live battery digits + bolt | 12 | 14 | 12 | 0.4 | 0.5 | plain |
| `orb_status_11_bold` | Live battery digits + bolt | 11 | 14 | 12 | 0.3 | 0.5 | plain |
| `orb_status_11_semibold` | Live battery digits + bolt | 11 | 14 | 12 | 0.3 | 0.5 | plain |
| `orb_status_10_bold` | Live battery digits + bolt | 10 | 14 | 12 | 0.3 | 0.4 | plain |
| `orb_status_10_semibold` | Live battery digits + bolt | 10 | 14 | 12 | 0.3 | 0.4 | plain |
| `orb_inter_28_regular` | Body, Caption 2 (Larger), Footnote 2 (AX3) | 28 | 17 | 254 | 34.4 | 44.2 | plain |

**36 fonts, 118.8 KiB** of font data (Text 44.2 KiB, Battery figures and bolt 42.3 KiB, Icons 32.3 KiB).

**In the font pack** (`pack/orb_fonts.pack`): 235 fonts, 7843.6 KiB in flash (9642.8 KiB of font data, raw mapped records plus zlib streams). A set's fonts come from the app where they are the same font (same size, weight and optical size) and from the pack otherwise. The generated font data below excludes record layout and converted LVGL metadata (see Loading above for actual buffer bytes):

| Set | Body | Fonts | From the app | From the pack | Generated font data (KiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Default | 28 px | 57 | 2 | 55 | 1796.9 |
| Default, Bold Text | 28 px | 57 | 0 | 57 | 1919.2 |
| Larger | 31 px | 57 | 2 | 55 | 2006.8 |
| Larger, Bold Text | 31 px | 57 | 0 | 57 | 2135.9 |
| AX3 | 38 px | 57 | 2 | 55 | 2257.4 |
| AX3, Bold Text | 38 px | 57 | 0 | 57 | 2389.9 |

<details><summary>Every font in the pack</summary>

| Font | Used by | px | opsz | Glyphs | Bitmaps (KiB) | Total (KiB) | Stored | In pack (KiB) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| `orb_clock_140_bold` | Clock large (Bold Text) | 140 | 32 | 16 | 13.2 | 13.4 | RLE | 13.5 |
| `orb_clock_140_semibold` | Clock large | 140 | 32 | 16 | 13.4 | 13.6 | RLE | 13.7 |
| `orb_clock_128_light` | Clock always_on | 128 | 32 | 16 | 11.1 | 11.3 | RLE | 11.4 |
| `orb_clock_128_regular` | Clock always_on (Bold Text) | 128 | 32 | 16 | 11.5 | 11.7 | RLE | 11.8 |
| `orb_clock_96_bold` | Clock medium (Bold Text) | 96 | 32 | 16 | 8.7 | 8.9 | RLE | 9.0 |
| `orb_clock_96_semibold` | Clock medium | 96 | 32 | 16 | 8.4 | 8.6 | RLE | 8.8 |
| `orb_icons_75_bold` | Large Title emphasized (AX3) | 75 | 32 | 180 | 121.3 | 122.9 | RLE | 89.6 |
| `orb_icons_75_extrabold` | Large Title emphasized (AX3, Bold Text) | 75 | 32 | 180 | 122.5 | 124.2 | RLE | 88.9 |
| `orb_icons_75_medium` | Large Title (AX3, Bold Text) | 75 | 32 | 180 | 115.3 | 116.9 | RLE | 117.7 |
| `orb_icons_75_regular` | Large Title (AX3) | 75 | 32 | 180 | 111.4 | 113.0 | RLE | 113.8 |
| `orb_icons_72_bold` | Title 1 emphasized (AX3, Bold Text) | 72 | 32 | 180 | 115.5 | 117.1 | RLE | 117.9 |
| `orb_icons_72_medium` | Title 1 (AX3, Bold Text) | 72 | 32 | 180 | 110.6 | 112.2 | RLE | 113.0 |
| `orb_icons_72_regular` | Title 1 (AX3) | 72 | 32 | 180 | 106.5 | 108.1 | RLE | 81.0 |
| `orb_icons_72_semibold` | Title 1 emphasized (AX3) | 72 | 32 | 180 | 113.8 | 115.5 | RLE | 116.3 |
| `orb_icons_69_bold` | Large Title emphasized (Larger) | 69 | 32 | 180 | 109.3 | 110.9 | RLE | 82.1 |
| `orb_icons_69_extrabold` | Large Title emphasized (Larger, Bold Text) | 69 | 32 | 180 | 110.7 | 112.4 | RLE | 80.9 |
| `orb_icons_69_medium` | Large Title (Larger, Bold Text) | 69 | 32 | 180 | 104.6 | 106.2 | RLE | 79.8 |
| `orb_icons_69_regular` | Large Title (Larger) | 69 | 32 | 180 | 101.0 | 102.6 | RLE | 103.4 |
| `orb_icons_65_bold` | Large Title emphasized, Title 1 emphasized (Larger, Bold Text) | 65 | 32 | 180 | 101.1 | 102.8 | RLE | 103.6 |
| `orb_icons_65_extrabold` | Large Title emphasized (Default, Bold Text) | 65 | 32 | 180 | 102.1 | 103.8 | RLE | 75.9 |
| `orb_icons_65_medium` | Large Title (Default, Bold Text), Title 1 (Larger, Bold Text) | 65 | 32 | 180 | 97.0 | 98.6 | RLE | 99.4 |
| `orb_icons_65_regular` | Large Title, Title 1 (Larger) | 65 | 32 | 180 | 93.7 | 95.4 | RLE | 96.1 |
| `orb_icons_65_semibold` | Title 1 emphasized (Larger) | 65 | 32 | 180 | 99.6 | 101.3 | RLE | 76.0 |
| `orb_icons_62_bold` | Title 1 emphasized (Default, Bold Text) | 62 | 32 | 180 | 95.7 | 97.4 | RLE | 72.1 |
| `orb_icons_62_medium` | Title 1 (Default, Bold Text) | 62 | 32 | 180 | 91.8 | 93.5 | RLE | 94.3 |
| `orb_icons_62_regular` | Title 1 | 62 | 32 | 180 | 89.0 | 90.6 | RLE | 68.3 |
| `orb_icons_62_semibold` | Title 1 emphasized | 62 | 32 | 180 | 94.6 | 96.3 | RLE | 72.2 |
| `orb_icons_59_bold` | Title 2 emphasized (AX3, Bold Text) | 59 | 32 | 180 | 90.0 | 91.7 | RLE | 68.9 |
| `orb_icons_59_medium` | Title 2 (AX3, Bold Text) | 59 | 32 | 180 | 86.6 | 88.2 | RLE | 89.0 |
| `orb_icons_59_regular` | Title 2 (AX3) | 59 | 32 | 180 | 83.7 | 85.3 | RLE | 86.1 |
| `orb_icons_59_semibold` | Title 2 emphasized (AX3) | 59 | 32 | 180 | 88.7 | 90.4 | RLE | 91.2 |
| `orb_icons_52_bold` | Title 2 emphasized (Larger, Bold Text) | 52 | 32 | 180 | 76.4 | 78.0 | RLE | 59.0 |
| `orb_icons_52_medium` | Title 2 (Larger, Bold Text) | 52 | 32 | 180 | 73.4 | 75.1 | RLE | 75.8 |
| `orb_icons_52_regular` | Title 2 (Larger) | 52 | 32 | 180 | 71.1 | 72.8 | RLE | 55.3 |
| `orb_icons_52_semibold` | Title 2 emphasized (Larger) | 52 | 32 | 180 | 75.3 | 77.0 | RLE | 77.8 |
| `orb_icons_49_bold` | Title 2 emphasized (Default, Bold Text) | 49 | 30 | 180 | 71.4 | 73.1 | RLE | 54.9 |
| `orb_icons_49_medium` | Title 2 (Default, Bold Text) | 49 | 30 | 180 | 68.6 | 70.3 | RLE | 53.7 |
| `orb_icons_49_regular` | Title 2 | 49 | 30 | 180 | 66.3 | 68.0 | RLE | 51.3 |
| `orb_icons_49_semibold` | Title 2 emphasized | 49 | 30 | 180 | 70.4 | 72.1 | RLE | 54.9 |
| `orb_icons_42_bold` | Title 3 emphasized (AX3, Bold Text) | 42 | 26 | 180 | 58.3 | 60.0 | RLE | 60.8 |
| `orb_icons_42_medium` | Title 3 (AX3, Bold Text) | 42 | 26 | 180 | 56.5 | 58.2 | RLE | 59.0 |
| `orb_icons_42_regular` | Title 3 (AX3) | 42 | 26 | 180 | 54.6 | 56.3 | RLE | 57.1 |
| `orb_icons_42_semibold` | Title 3 emphasized (AX3) | 42 | 26 | 180 | 57.8 | 59.5 | RLE | 60.3 |
| `orb_icons_38_bold` | Headline (AX3, Bold Text), Headline emphasized (AX3, Bold Text), Body emphasized (AX3, Bold Text) | 38 | 23 | 180 | 51.1 | 52.8 | RLE | 53.6 |
| `orb_icons_38_medium` | Body (AX3, Bold Text) | 38 | 23 | 180 | 49.7 | 51.3 | RLE | 52.1 |
| `orb_icons_38_regular` | Body (AX3) | 38 | 23 | 180 | 47.4 | 49.1 | RLE | 49.9 |
| `orb_icons_38_semibold` | Headline (AX3), Headline emphasized (AX3), Body emphasized (AX3) | 38 | 23 | 180 | 50.6 | 52.2 | RLE | 53.0 |
| `orb_icons_36_bold` | Title 3 emphasized (Larger, Bold Text) | 36 | 22 | 180 | 48.2 | 49.8 | RLE | 38.6 |
| `orb_icons_36_medium` | Title 3 (Larger, Bold Text) | 36 | 22 | 180 | 46.6 | 48.2 | RLE | 37.4 |
| `orb_icons_36_regular` | Title 3 (Larger) | 36 | 22 | 180 | 44.4 | 46.0 | RLE | 35.8 |
| `orb_icons_36_semibold` | Title 3 emphasized (Larger) | 36 | 22 | 180 | 47.7 | 49.4 | RLE | 50.2 |
| `orb_icons_33_bold` | Title 3 emphasized (Default, Bold Text), Caption 1 emphasized (AX3, Bold Text) | 33 | 20 | 180 | 42.8 | 44.4 | RLE | 45.2 |
| `orb_icons_33_medium` | Title 3 (Default, Bold Text), Caption 1 (AX3, Bold Text) | 33 | 20 | 180 | 41.2 | 42.8 | RLE | 43.6 |
| `orb_icons_33_regular` | Title 3, Caption 1 (AX3) | 33 | 20 | 180 | 39.0 | 40.7 | RLE | 41.5 |
| `orb_icons_33_semibold` | Title 3 emphasized, Caption 1 emphasized (AX3) | 33 | 20 | 180 | 42.6 | 44.2 | RLE | 45.0 |
| `orb_icons_31_bold` | Headline (Larger, Bold Text), Headline emphasized (Larger, Bold Text), Body emphasized (Larger, Bold Text) and 1 more | 31 | 19 | 180 | 39.4 | 41.1 | RLE | 41.9 |
| `orb_icons_31_medium` | Body (Larger, Bold Text), Caption 2 (AX3, Bold Text) | 31 | 19 | 180 | 37.8 | 39.4 | RLE | 40.2 |
| `orb_icons_31_regular` | Body (Larger), Caption 2 (AX3) | 31 | 19 | 180 | 35.8 | 37.4 | RLE | 38.2 |
| `orb_icons_31_semibold` | Headline (Larger), Headline emphasized (Larger), Body emphasized (Larger) and 1 more | 31 | 19 | 180 | 39.1 | 40.8 | RLE | 41.6 |
| `orb_icons_29_bold` | Caption 1 emphasized (Larger, Bold Text), Footnote 1 emphasized (AX3, Bold Text) | 29 | 18 | 180 | 36.2 | 37.9 | RLE | 38.7 |
| `orb_icons_29_medium` | Caption 1 (Larger, Bold Text), Footnote 1 (AX3, Bold Text) | 29 | 18 | 180 | 34.2 | 35.8 | RLE | 36.6 |
| `orb_icons_29_regular` | Caption 1 (Larger), Footnote 1 (AX3) | 29 | 18 | 180 | 32.5 | 34.1 | RLE | 34.9 |
| `orb_icons_29_semibold` | Caption 1 emphasized (Larger), Footnote 1 emphasized (AX3) | 29 | 18 | 180 | 35.5 | 37.2 | RLE | 38.0 |
| `orb_icons_28_bold` | Headline (Default, Bold Text), Headline emphasized (Default, Bold Text), Body emphasized (Default, Bold Text) and 2 more | 28 | 17 | 180 | 34.4 | 36.0 | RLE | 36.8 |
| `orb_icons_28_medium` | Body (Default, Bold Text), Caption 2 (Larger, Bold Text), Footnote 2 (AX3, Bold Text) | 28 | 17 | 180 | 32.4 | 34.1 | RLE | 34.9 |
| `orb_icons_28_semibold` | Headline, Headline emphasized, Body emphasized and 2 more | 28 | 17 | 180 | 33.9 | 35.5 | RLE | 36.3 |
| `orb_icons_26_bold` | Caption 1 emphasized (Default, Bold Text), Footnote 1 emphasized (Larger, Bold Text) | 26 | 16 | 180 | 30.9 | 32.6 | RLE | 33.3 |
| `orb_icons_26_medium` | Caption 1 (Default, Bold Text), Footnote 1 (Larger, Bold Text) | 26 | 16 | 180 | 29.0 | 30.6 | RLE | 31.4 |
| `orb_icons_26_regular` | Caption 1, Footnote 1 (Larger) | 26 | 16 | 180 | 27.4 | 29.1 | RLE | 29.9 |
| `orb_icons_26_semibold` | Caption 1 emphasized, Footnote 1 emphasized (Larger) | 26 | 16 | 180 | 30.5 | 32.1 | RLE | 32.9 |
| `orb_icons_25_bold` | Caption 2 emphasized (Default, Bold Text), Footnote 2 emphasized (Larger, Bold Text) | 25 | 15 | 180 | 29.2 | 30.9 | RLE | 31.7 |
| `orb_icons_25_medium` | Caption 2 (Default, Bold Text), Footnote 2 (Larger, Bold Text) | 25 | 15 | 180 | 27.1 | 28.7 | RLE | 29.5 |
| `orb_icons_25_regular` | Caption 2, Footnote 2 (Larger) | 25 | 15 | 180 | 25.8 | 27.4 | RLE | 28.2 |
| `orb_icons_25_semibold` | Caption 2 emphasized, Footnote 2 emphasized (Larger) | 25 | 15 | 180 | 28.4 | 30.1 | RLE | 30.9 |
| `orb_icons_23_bold` | Footnote 1 emphasized (Default, Bold Text) | 23 | 14 | 180 | 26.6 | 28.2 | RLE | 22.6 |
| `orb_icons_23_medium` | Footnote 1 (Default, Bold Text) | 23 | 14 | 180 | 24.5 | 26.2 | RLE | 27.0 |
| `orb_icons_23_regular` | Footnote 1 | 23 | 14 | 180 | 23.1 | 24.8 | RLE | 20.8 |
| `orb_icons_23_semibold` | Footnote 1 emphasized | 23 | 14 | 180 | 25.9 | 27.5 | RLE | 22.2 |
| `orb_icons_21_bold` | Footnote 2 emphasized (Default, Bold Text) | 21 | 14 | 180 | 23.2 | 24.9 | RLE | 20.0 |
| `orb_icons_21_medium` | Footnote 2 (Default, Bold Text) | 21 | 14 | 180 | 21.2 | 22.9 | RLE | 23.7 |
| `orb_icons_21_regular` | Footnote 2 | 21 | 14 | 180 | 20.2 | 21.8 | RLE | 18.4 |
| `orb_icons_21_semibold` | Footnote 2 emphasized | 21 | 14 | 180 | 22.5 | 24.2 | RLE | 19.7 |
| `orb_inter_75_bold` | Large Title emphasized (AX3) | 75 | 32 | 254 | 88.1 | 98.5 | RLE | 67.4 |
| `orb_inter_75_extrabold` | Large Title emphasized (AX3, Bold Text) | 75 | 32 | 254 | 90.9 | 101.4 | RLE | 66.9 |
| `orb_inter_75_medium` | Large Title (AX3, Bold Text) | 75 | 32 | 254 | 85.5 | 95.9 | RLE | 65.5 |
| `orb_inter_75_regular` | Large Title (AX3) | 75 | 32 | 254 | 84.3 | 93.9 | RLE | 65.7 |
| `orb_inter_72_bold` | Title 1 emphasized (AX3, Bold Text) | 72 | 32 | 254 | 83.3 | 93.8 | RLE | 63.9 |
| `orb_inter_72_medium` | Title 1 (AX3, Bold Text) | 72 | 32 | 254 | 81.4 | 91.8 | RLE | 62.3 |
| `orb_inter_72_regular` | Title 1 (AX3) | 72 | 32 | 254 | 79.9 | 89.6 | RLE | 63.1 |
| `orb_inter_72_semibold` | Title 1 emphasized (AX3) | 72 | 32 | 254 | 82.3 | 92.7 | RLE | 63.7 |
| `orb_inter_69_bold` | Large Title emphasized (Larger) | 69 | 32 | 254 | 79.2 | 89.6 | RLE | 61.7 |
| `orb_inter_69_extrabold` | Large Title emphasized (Larger, Bold Text) | 69 | 32 | 254 | 80.7 | 91.1 | RLE | 61.7 |
| `orb_inter_69_medium` | Large Title (Larger, Bold Text) | 69 | 32 | 254 | 77.1 | 87.5 | RLE | 59.9 |
| `orb_inter_69_regular` | Large Title (Larger) | 69 | 32 | 254 | 75.9 | 85.5 | RLE | 60.7 |
| `orb_inter_65_bold` | Large Title emphasized, Title 1 emphasized (Larger, Bold Text) | 65 | 32 | 254 | 73.4 | 83.9 | RLE | 84.9 |
| `orb_inter_65_extrabold` | Large Title emphasized (Default, Bold Text) | 65 | 32 | 254 | 74.0 | 84.4 | RLE | 55.9 |
| `orb_inter_65_medium` | Large Title (Default, Bold Text), Title 1 (Larger, Bold Text) | 65 | 32 | 254 | 71.4 | 81.8 | RLE | 82.9 |
| `orb_inter_65_regular` | Large Title, Title 1 (Larger) | 65 | 32 | 254 | 70.5 | 80.1 | RLE | 81.2 |
| `orb_inter_65_semibold` | Title 1 emphasized (Larger) | 65 | 32 | 254 | 72.2 | 82.7 | RLE | 58.2 |
| `orb_inter_62_bold` | Title 1 emphasized (Default, Bold Text) | 62 | 32 | 254 | 68.6 | 79.0 | RLE | 54.2 |
| `orb_inter_62_medium` | Title 1 (Default, Bold Text) | 62 | 32 | 254 | 67.2 | 77.6 | RLE | 53.8 |
| `orb_inter_62_regular` | Title 1 | 62 | 32 | 254 | 66.2 | 75.9 | RLE | 53.1 |
| `orb_inter_62_semibold` | Title 1 emphasized | 62 | 32 | 254 | 68.0 | 78.4 | RLE | 54.9 |
| `orb_inter_59_bold` | Title 2 emphasized (AX3, Bold Text) | 59 | 32 | 254 | 64.5 | 74.9 | RLE | 50.8 |
| `orb_inter_59_medium` | Title 2 (AX3, Bold Text) | 59 | 32 | 254 | 63.0 | 73.4 | RLE | 50.9 |
| `orb_inter_59_regular` | Title 2 (AX3) | 59 | 32 | 254 | 62.2 | 71.9 | RLE | 50.4 |
| `orb_inter_59_semibold` | Title 2 emphasized (AX3) | 59 | 32 | 254 | 63.8 | 74.2 | RLE | 51.5 |
| `orb_inter_52_bold` | Title 2 emphasized (Larger, Bold Text) | 52 | 32 | 254 | 54.9 | 65.3 | RLE | 44.3 |
| `orb_inter_52_medium` | Title 2 (Larger, Bold Text) | 52 | 32 | 254 | 53.6 | 64.0 | RLE | 43.9 |
| `orb_inter_52_regular` | Title 2 (Larger) | 52 | 32 | 254 | 53.0 | 62.7 | RLE | 43.9 |
| `orb_inter_52_semibold` | Title 2 emphasized (Larger) | 52 | 32 | 254 | 54.4 | 64.8 | RLE | 45.0 |
| `orb_inter_49_bold` | Title 2 emphasized (Default, Bold Text) | 49 | 30 | 254 | 50.9 | 61.7 | RLE | 42.8 |
| `orb_inter_49_medium` | Title 2 (Default, Bold Text) | 49 | 30 | 254 | 50.0 | 60.7 | RLE | 42.9 |
| `orb_inter_49_regular` | Title 2 | 49 | 30 | 254 | 49.4 | 59.2 | RLE | 41.8 |
| `orb_inter_49_semibold` | Title 2 emphasized | 49 | 30 | 254 | 50.5 | 61.3 | RLE | 42.8 |
| `orb_inter_42_bold` | Title 3 emphasized (AX3, Bold Text) | 42 | 26 | 254 | 41.8 | 52.5 | RLE | 36.4 |
| `orb_inter_42_medium` | Title 3 (AX3, Bold Text) | 42 | 26 | 254 | 41.2 | 51.9 | RLE | 36.0 |
| `orb_inter_42_regular` | Title 3 (AX3) | 42 | 26 | 254 | 40.8 | 50.6 | RLE | 35.2 |
| `orb_inter_42_semibold` | Title 3 emphasized (AX3) | 42 | 26 | 254 | 41.4 | 52.1 | RLE | 36.0 |
| `orb_inter_38_bold` | Headline (AX3, Bold Text), Headline emphasized (AX3, Bold Text), Body emphasized (AX3, Bold Text) | 38 | 23 | 254 | 36.8 | 47.6 | RLE | 33.0 |
| `orb_inter_38_medium` | Body (AX3, Bold Text) | 38 | 23 | 254 | 36.3 | 47.1 | RLE | 32.6 |
| `orb_inter_38_regular` | Body (AX3) | 38 | 23 | 254 | 36.0 | 45.8 | RLE | 31.7 |
| `orb_inter_38_semibold` | Headline (AX3), Headline emphasized (AX3), Body emphasized (AX3) | 38 | 23 | 254 | 36.5 | 47.3 | RLE | 33.2 |
| `orb_inter_36_bold` | Title 3 emphasized (Larger, Bold Text) | 36 | 22 | 254 | 34.5 | 45.3 | RLE | 31.1 |
| `orb_inter_36_medium` | Title 3 (Larger, Bold Text) | 36 | 22 | 254 | 33.9 | 44.7 | RLE | 30.9 |
| `orb_inter_36_regular` | Title 3 (Larger) | 36 | 22 | 254 | 33.5 | 43.3 | RLE | 29.7 |
| `orb_inter_36_semibold` | Title 3 emphasized (Larger) | 36 | 22 | 254 | 34.2 | 44.9 | RLE | 30.8 |
| `orb_inter_33_bold` | Title 3 emphasized (Default, Bold Text), Caption 1 emphasized (AX3, Bold Text) | 33 | 20 | 254 | 30.4 | 41.1 | RLE | 42.2 |
| `orb_inter_33_medium` | Title 3 (Default, Bold Text), Caption 1 (AX3, Bold Text) | 33 | 20 | 254 | 30.0 | 40.8 | RLE | 41.9 |
| `orb_inter_33_regular` | Title 3, Caption 1 (AX3) | 33 | 20 | 254 | 29.3 | 39.1 | RLE | 40.2 |
| `orb_inter_33_semibold` | Title 3 emphasized, Caption 1 emphasized (AX3) | 33 | 20 | 254 | 30.3 | 41.1 | RLE | 42.2 |
| `orb_inter_31_bold` | Headline (Larger, Bold Text), Headline emphasized (Larger, Bold Text), Body emphasized (Larger, Bold Text) and 1 more | 31 | 19 | 254 | 45.8 | 56.6 | plain | 57.7 |
| `orb_inter_31_medium` | Body (Larger, Bold Text), Caption 2 (AX3, Bold Text) | 31 | 19 | 254 | 42.8 | 53.3 | plain | 54.4 |
| `orb_inter_31_regular` | Body (Larger), Caption 2 (AX3) | 31 | 19 | 254 | 41.5 | 51.3 | plain | 52.4 |
| `orb_inter_31_semibold` | Headline (Larger), Headline emphasized (Larger), Body emphasized (Larger) and 1 more | 31 | 19 | 254 | 44.6 | 55.3 | plain | 56.4 |
| `orb_inter_29_bold` | Caption 1 emphasized (Larger, Bold Text), Footnote 1 emphasized (AX3, Bold Text) | 29 | 18 | 254 | 40.7 | 51.5 | plain | 18.8 |
| `orb_inter_29_medium` | Caption 1 (Larger, Bold Text), Footnote 1 (AX3, Bold Text) | 29 | 18 | 254 | 38.0 | 48.6 | plain | 17.6 |
| `orb_inter_29_regular` | Caption 1 (Larger), Footnote 1 (AX3) | 29 | 18 | 254 | 36.8 | 46.6 | plain | 47.6 |
| `orb_inter_29_semibold` | Caption 1 emphasized (Larger), Footnote 1 emphasized (AX3) | 29 | 18 | 254 | 39.4 | 50.1 | plain | 18.1 |
| `orb_inter_28_bold` | Headline (Default, Bold Text), Headline emphasized (Default, Bold Text), Body emphasized (Default, Bold Text) and 2 more | 28 | 17 | 254 | 38.0 | 48.7 | plain | 49.8 |
| `orb_inter_28_medium` | Body (Default, Bold Text), Caption 2 (Larger, Bold Text), Footnote 2 (AX3, Bold Text) | 28 | 17 | 254 | 35.5 | 46.1 | plain | 47.2 |
| `orb_inter_28_semibold` | Headline, Headline emphasized, Body emphasized and 2 more | 28 | 17 | 254 | 37.2 | 47.9 | plain | 49.0 |
| `orb_inter_26_bold` | Caption 1 emphasized (Default, Bold Text), Footnote 1 emphasized (Larger, Bold Text) | 26 | 16 | 254 | 32.7 | 43.5 | plain | 16.4 |
| `orb_inter_26_medium` | Caption 1 (Default, Bold Text), Footnote 1 (Larger, Bold Text) | 26 | 16 | 254 | 30.9 | 41.4 | plain | 16.0 |
| `orb_inter_26_regular` | Caption 1, Footnote 1 (Larger) | 26 | 16 | 254 | 30.1 | 39.9 | plain | 15.5 |
| `orb_inter_26_semibold` | Caption 1 emphasized, Footnote 1 emphasized (Larger) | 26 | 16 | 254 | 32.1 | 42.6 | plain | 16.6 |
| `orb_inter_25_bold` | Caption 2 emphasized (Default, Bold Text), Footnote 2 emphasized (Larger, Bold Text) | 25 | 15 | 254 | 30.8 | 41.4 | plain | 16.3 |
| `orb_inter_25_medium` | Caption 2 (Default, Bold Text), Footnote 2 (Larger, Bold Text) | 25 | 15 | 254 | 28.9 | 39.5 | plain | 15.8 |
| `orb_inter_25_regular` | Caption 2, Footnote 2 (Larger) | 25 | 15 | 254 | 28.0 | 37.8 | plain | 15.3 |
| `orb_inter_25_semibold` | Caption 2 emphasized, Footnote 2 emphasized (Larger) | 25 | 15 | 254 | 29.9 | 40.4 | plain | 16.3 |
| `orb_inter_23_bold` | Footnote 1 emphasized (Default, Bold Text) | 23 | 14 | 254 | 25.9 | 36.2 | plain | 14.9 |
| `orb_inter_23_medium` | Footnote 1 (Default, Bold Text) | 23 | 14 | 254 | 24.4 | 34.7 | plain | 14.5 |
| `orb_inter_23_regular` | Footnote 1 | 23 | 14 | 254 | 23.8 | 33.3 | plain | 14.0 |
| `orb_inter_23_semibold` | Footnote 1 emphasized | 23 | 14 | 254 | 25.2 | 35.5 | plain | 14.9 |
| `orb_inter_21_bold` | Footnote 2 emphasized (Default, Bold Text) | 21 | 14 | 254 | 22.3 | 32.6 | plain | 13.6 |
| `orb_inter_21_medium` | Footnote 2 (Default, Bold Text) | 21 | 14 | 254 | 21.0 | 31.3 | plain | 13.2 |
| `orb_inter_21_regular` | Footnote 2 | 21 | 14 | 254 | 20.1 | 29.5 | plain | 12.6 |
| `orb_inter_21_semibold` | Footnote 2 emphasized | 21 | 14 | 254 | 21.6 | 31.9 | plain | 13.6 |
| `orb_inter_75_bold_tnum` | Large Title emphasized (AX3) | 75 | 32 | 17 | 5.0 | 5.2 | RLE | 5.3 |
| `orb_inter_75_extrabold_tnum` | Large Title emphasized (AX3, Bold Text) | 75 | 32 | 17 | 5.2 | 5.4 | RLE | 4.1 |
| `orb_inter_75_medium_tnum` | Large Title (AX3, Bold Text) | 75 | 32 | 17 | 4.8 | 5.0 | RLE | 5.1 |
| `orb_inter_75_regular_tnum` | Large Title (AX3) | 75 | 32 | 17 | 4.8 | 5.0 | RLE | 5.1 |
| `orb_inter_72_bold_tnum` | Title 1 emphasized (AX3, Bold Text) | 72 | 32 | 17 | 4.8 | 5.0 | RLE | 4.0 |
| `orb_inter_72_medium_tnum` | Title 1 (AX3, Bold Text) | 72 | 32 | 17 | 4.6 | 4.8 | RLE | 5.0 |
| `orb_inter_72_regular_tnum` | Title 1 (AX3) | 72 | 32 | 17 | 4.5 | 4.7 | RLE | 4.9 |
| `orb_inter_72_semibold_tnum` | Title 1 emphasized (AX3) | 72 | 32 | 17 | 4.7 | 4.9 | RLE | 5.0 |
| `orb_inter_69_bold_tnum` | Large Title emphasized (Larger) | 69 | 32 | 17 | 4.5 | 4.7 | RLE | 4.8 |
| `orb_inter_69_extrabold_tnum` | Large Title emphasized (Larger, Bold Text) | 69 | 32 | 17 | 4.6 | 4.8 | RLE | 3.8 |
| `orb_inter_69_medium_tnum` | Large Title (Larger, Bold Text) | 69 | 32 | 17 | 4.3 | 4.5 | RLE | 4.7 |
| `orb_inter_69_regular_tnum` | Large Title (Larger) | 69 | 32 | 17 | 4.3 | 4.5 | RLE | 4.6 |
| `orb_inter_65_bold_tnum` | Large Title emphasized, Title 1 emphasized (Larger, Bold Text) | 65 | 32 | 17 | 4.2 | 4.3 | RLE | 4.5 |
| `orb_inter_65_extrabold_tnum` | Large Title emphasized (Default, Bold Text) | 65 | 32 | 17 | 4.2 | 4.4 | RLE | 4.5 |
| `orb_inter_65_medium_tnum` | Large Title (Default, Bold Text), Title 1 (Larger, Bold Text) | 65 | 32 | 17 | 4.0 | 4.2 | RLE | 4.3 |
| `orb_inter_65_regular_tnum` | Large Title, Title 1 (Larger) | 65 | 32 | 17 | 4.0 | 4.2 | RLE | 4.3 |
| `orb_inter_65_semibold_tnum` | Title 1 emphasized (Larger) | 65 | 32 | 17 | 4.1 | 4.3 | RLE | 4.4 |
| `orb_inter_62_bold_tnum` | Title 1 emphasized (Default, Bold Text) | 62 | 32 | 17 | 3.9 | 4.1 | RLE | 3.4 |
| `orb_inter_62_medium_tnum` | Title 1 (Default, Bold Text) | 62 | 32 | 17 | 3.8 | 4.0 | RLE | 3.4 |
| `orb_inter_62_regular_tnum` | Title 1 | 62 | 32 | 17 | 3.8 | 4.0 | RLE | 3.4 |
| `orb_inter_62_semibold_tnum` | Title 1 emphasized | 62 | 32 | 17 | 3.9 | 4.1 | RLE | 3.4 |
| `orb_inter_59_bold_tnum` | Title 2 emphasized (AX3, Bold Text) | 59 | 32 | 17 | 3.6 | 3.8 | RLE | 4.0 |
| `orb_inter_59_medium_tnum` | Title 2 (AX3, Bold Text) | 59 | 32 | 17 | 3.6 | 3.8 | RLE | 3.9 |
| `orb_inter_59_regular_tnum` | Title 2 (AX3) | 59 | 32 | 17 | 3.5 | 3.7 | RLE | 3.1 |
| `orb_inter_59_semibold_tnum` | Title 2 emphasized (AX3) | 59 | 32 | 17 | 3.6 | 3.8 | RLE | 3.9 |
| `orb_inter_52_bold_tnum` | Title 2 emphasized (Larger, Bold Text) | 52 | 32 | 17 | 3.1 | 3.3 | RLE | 3.4 |
| `orb_inter_52_medium_tnum` | Title 2 (Larger, Bold Text) | 52 | 32 | 17 | 3.1 | 3.3 | RLE | 3.4 |
| `orb_inter_52_regular_tnum` | Title 2 (Larger) | 52 | 32 | 17 | 3.0 | 3.2 | RLE | 3.4 |
| `orb_inter_52_semibold_tnum` | Title 2 emphasized (Larger) | 52 | 32 | 17 | 3.1 | 3.3 | RLE | 3.4 |
| `orb_inter_49_bold_tnum` | Title 2 emphasized (Default, Bold Text) | 49 | 30 | 17 | 2.9 | 3.1 | RLE | 2.6 |
| `orb_inter_49_medium_tnum` | Title 2 (Default, Bold Text) | 49 | 30 | 17 | 2.9 | 3.1 | RLE | 3.2 |
| `orb_inter_49_regular_tnum` | Title 2 | 49 | 30 | 17 | 2.8 | 3.0 | RLE | 2.6 |
| `orb_inter_49_semibold_tnum` | Title 2 emphasized | 49 | 30 | 17 | 2.9 | 3.1 | RLE | 2.7 |
| `orb_inter_42_bold_tnum` | Title 3 emphasized (AX3, Bold Text) | 42 | 26 | 17 | 2.4 | 2.5 | RLE | 2.7 |
| `orb_inter_42_medium_tnum` | Title 3 (AX3, Bold Text) | 42 | 26 | 17 | 2.3 | 2.5 | RLE | 2.7 |
| `orb_inter_42_regular_tnum` | Title 3 (AX3) | 42 | 26 | 17 | 2.3 | 2.5 | RLE | 2.2 |
| `orb_inter_42_semibold_tnum` | Title 3 emphasized (AX3) | 42 | 26 | 17 | 2.3 | 2.5 | RLE | 2.6 |
| `orb_inter_38_bold_tnum` | Headline (AX3, Bold Text), Headline emphasized (AX3, Bold Text), Body emphasized (AX3, Bold Text) | 38 | 23 | 17 | 2.1 | 2.3 | RLE | 2.4 |
| `orb_inter_38_medium_tnum` | Body (AX3, Bold Text) | 38 | 23 | 17 | 2.0 | 2.2 | RLE | 2.0 |
| `orb_inter_38_regular_tnum` | Body (AX3) | 38 | 23 | 17 | 2.0 | 2.2 | RLE | 2.4 |
| `orb_inter_38_semibold_tnum` | Headline (AX3), Headline emphasized (AX3), Body emphasized (AX3) | 38 | 23 | 17 | 2.0 | 2.2 | RLE | 2.4 |
| `orb_inter_36_bold_tnum` | Title 3 emphasized (Larger, Bold Text) | 36 | 22 | 17 | 1.9 | 2.1 | RLE | 1.9 |
| `orb_inter_36_medium_tnum` | Title 3 (Larger, Bold Text) | 36 | 22 | 17 | 1.9 | 2.1 | RLE | 2.2 |
| `orb_inter_36_regular_tnum` | Title 3 (Larger) | 36 | 22 | 17 | 1.9 | 2.1 | RLE | 2.2 |
| `orb_inter_36_semibold_tnum` | Title 3 emphasized (Larger) | 36 | 22 | 17 | 1.9 | 2.1 | RLE | 2.3 |
| `orb_inter_33_bold_tnum` | Title 3 emphasized (Default, Bold Text), Caption 1 emphasized (AX3, Bold Text) | 33 | 20 | 17 | 1.7 | 1.9 | RLE | 2.0 |
| `orb_inter_33_medium_tnum` | Title 3 (Default, Bold Text), Caption 1 (AX3, Bold Text) | 33 | 20 | 17 | 1.7 | 1.9 | RLE | 2.0 |
| `orb_inter_33_regular_tnum` | Title 3, Caption 1 (AX3) | 33 | 20 | 17 | 1.7 | 1.8 | RLE | 2.0 |
| `orb_inter_33_semibold_tnum` | Title 3 emphasized, Caption 1 emphasized (AX3) | 33 | 20 | 17 | 1.7 | 1.9 | RLE | 2.0 |
| `orb_inter_31_bold_tnum` | Headline (Larger, Bold Text), Headline emphasized (Larger, Bold Text), Body emphasized (Larger, Bold Text) and 1 more | 31 | 19 | 17 | 2.4 | 2.5 | plain | 2.7 |
| `orb_inter_31_medium_tnum` | Body (Larger, Bold Text), Caption 2 (AX3, Bold Text) | 31 | 19 | 17 | 2.2 | 2.4 | plain | 2.5 |
| `orb_inter_31_regular_tnum` | Body (Larger), Caption 2 (AX3) | 31 | 19 | 17 | 2.1 | 2.3 | plain | 2.4 |
| `orb_inter_31_semibold_tnum` | Headline (Larger), Headline emphasized (Larger), Body emphasized (Larger) and 1 more | 31 | 19 | 17 | 2.3 | 2.5 | plain | 2.6 |
| `orb_inter_29_bold_tnum` | Caption 1 emphasized (Larger, Bold Text), Footnote 1 emphasized (AX3, Bold Text) | 29 | 18 | 17 | 2.1 | 2.3 | plain | 2.4 |
| `orb_inter_29_medium_tnum` | Caption 1 (Larger, Bold Text), Footnote 1 (AX3, Bold Text) | 29 | 18 | 17 | 2.0 | 2.2 | plain | 2.3 |
| `orb_inter_29_regular_tnum` | Caption 1 (Larger), Footnote 1 (AX3) | 29 | 18 | 17 | 1.9 | 2.1 | plain | 2.2 |
| `orb_inter_29_semibold_tnum` | Caption 1 emphasized (Larger), Footnote 1 emphasized (AX3) | 29 | 18 | 17 | 2.1 | 2.3 | plain | 2.4 |
| `orb_inter_28_bold_tnum` | Headline (Default, Bold Text), Headline emphasized (Default, Bold Text), Body emphasized (Default, Bold Text) and 2 more | 28 | 17 | 17 | 2.0 | 2.2 | plain | 2.3 |
| `orb_inter_28_medium_tnum` | Body (Default, Bold Text), Caption 2 (Larger, Bold Text), Footnote 2 (AX3, Bold Text) | 28 | 17 | 17 | 1.8 | 2.0 | plain | 2.2 |
| `orb_inter_28_regular_tnum` | Body, Caption 2 (Larger), Footnote 2 (AX3) | 28 | 17 | 17 | 1.8 | 2.0 | plain | 2.1 |
| `orb_inter_28_semibold_tnum` | Headline, Headline emphasized, Body emphasized and 2 more | 28 | 17 | 17 | 1.9 | 2.1 | plain | 2.2 |
| `orb_inter_26_bold_tnum` | Caption 1 emphasized (Default, Bold Text), Footnote 1 emphasized (Larger, Bold Text) | 26 | 16 | 17 | 1.7 | 1.9 | plain | 2.0 |
| `orb_inter_26_medium_tnum` | Caption 1 (Default, Bold Text), Footnote 1 (Larger, Bold Text) | 26 | 16 | 17 | 1.6 | 1.8 | plain | 1.9 |
| `orb_inter_26_regular_tnum` | Caption 1, Footnote 1 (Larger) | 26 | 16 | 17 | 1.6 | 1.8 | plain | 1.9 |
| `orb_inter_26_semibold_tnum` | Caption 1 emphasized, Footnote 1 emphasized (Larger) | 26 | 16 | 17 | 1.7 | 1.8 | plain | 2.0 |
| `orb_inter_25_bold_tnum` | Caption 2 emphasized (Default, Bold Text), Footnote 2 emphasized (Larger, Bold Text) | 25 | 15 | 17 | 1.6 | 1.8 | plain | 1.9 |
| `orb_inter_25_medium_tnum` | Caption 2 (Default, Bold Text), Footnote 2 (Larger, Bold Text) | 25 | 15 | 17 | 1.5 | 1.7 | plain | 1.8 |
| `orb_inter_25_regular_tnum` | Caption 2, Footnote 2 (Larger) | 25 | 15 | 17 | 1.5 | 1.7 | plain | 1.0 |
| `orb_inter_25_semibold_tnum` | Caption 2 emphasized, Footnote 2 emphasized (Larger) | 25 | 15 | 17 | 1.6 | 1.7 | plain | 1.1 |
| `orb_inter_23_bold_tnum` | Footnote 1 emphasized (Default, Bold Text) | 23 | 14 | 17 | 1.4 | 1.6 | plain | 1.0 |
| `orb_inter_23_medium_tnum` | Footnote 1 (Default, Bold Text) | 23 | 14 | 17 | 1.3 | 1.4 | plain | 1.0 |
| `orb_inter_23_regular_tnum` | Footnote 1 | 23 | 14 | 17 | 1.2 | 1.4 | plain | 0.9 |
| `orb_inter_23_semibold_tnum` | Footnote 1 emphasized | 23 | 14 | 17 | 1.3 | 1.5 | plain | 1.0 |
| `orb_inter_21_bold_tnum` | Footnote 2 emphasized (Default, Bold Text) | 21 | 14 | 17 | 1.1 | 1.3 | plain | 0.9 |
| `orb_inter_21_medium_tnum` | Footnote 2 (Default, Bold Text) | 21 | 14 | 17 | 1.1 | 1.3 | plain | 0.9 |
| `orb_inter_21_regular_tnum` | Footnote 2 | 21 | 14 | 17 | 1.0 | 1.2 | plain | 0.9 |
| `orb_inter_21_semibold_tnum` | Footnote 2 emphasized | 21 | 14 | 17 | 1.1 | 1.3 | plain | 0.9 |

</details>
<!-- sizes:end -->

## How the fonts are made

`scripts/fonts.sh` (Python in `tools/fonts`, Node for lv_font_conv):

1. **Sources.** [Inter 4.1](https://github.com/rsms/inter/releases/tag/v4.1) (`Inter-4.1.zip`, SHA-256 `9883fdd4…6b11e`), `framework7-icons` 5.0.5 from npm (SHA-256 `601d32f5d75595025c1a9bdf433cb616c3c20f6026683dea3cdd425b9efefa05`) and `@phosphor-icons/core` 2.1.1 for the listed fallbacks (SHA-256 `313332be…1baadd`) are downloaded once into `tools/fonts/.cache/downloads`. Every run checks all three hashes and unpacks only what it uses (`InterVariable.ttf`, Inter's licence, the SVGs of Orb's icons and both MIT licences) afresh from the verified archives. Python packages are installed with `--require-hashes` from `requirements.txt` (every file PyPI publishes for each pinned version); lv_font_conv 1.5.3 with `npm ci` from `package-lock.json` (its tarball's sha512; it bundles its dependencies).
2. **Inter instances** (fontTools): `InterVariable` pinned at the weight and optical size; for tabular and clock fonts, the glyphs HarfBuzz picks with `tnum` on in each character's context are frozen into the cmap (lv_font_conv applies no OpenType features); subset to the character set with pair kerning kept; overlapping contours merged; composite components placed exactly (Inter asks for them to be rounded to the pixel grid, which lv_font_conv's FreeType honours and which moved `, ; „ Ç ¼` by up to half a pixel); the fractional tracking added to every advance.
3. **Icon fonts** (fontTools, skia-pathops): Framework7 SVG paths offset to the measured stem width, native filled variants registered to outline bounds, fallback SVGs weighted as before, all scaled and centred on Inter's cap height, shared metrics per group, written as TrueType.
4. **Conversion**: `lv_font_conv --bpp 4 --autohint-off` (unhinted, like Apple's rendering: shapes stay as drawn at 266 ppi), kerning on, `--lv-fallback` for the fallback chains. The generator then patches icon line boxes and tabular metrics and writes the app's `fonts/*.c`, every font as a pack record (`tools/fonts/pack.py` has the layout) into `pack/orb_fonts.pack`, `src/orb_fonts_table.c`, `src/orb_fonts_notices.c`, `include/orb_icons.h`, the tables above, `test_app/main/orb_fonts_coverage.h` and the previews.

Generated files say so at the top; don't edit them. A second run on the same inputs leaves git clean.

```bash
scripts/fonts.sh                 # regenerate everything (about 2.5 minutes)
scripts/fonts.sh --no-previews
scripts/fonts.sh check           # LVGL on this Mac: records, two threads, self-test, previews
scripts/fonts.sh status          # fast redraw of status.png and screen-face.png with real LVGL
scripts/fonts.sh test-app build  # ESP-IDF build of test_app (also: size, size-components)
scripts/fonts.sh assets --version 1 -o /private/orb-assets.signed.bin  # uses the release key
```

Python style: black, isort and ruff at 100 columns and flake8 at 120 (`tools/fonts/pyproject.toml`, `tools/fonts/.flake8`). After installing the pinned development requirements into `tools/fonts/.venv`, run:

```bash
tools/fonts/.venv/bin/pytest -q tools/fonts/tests
tools/fonts/.venv/bin/ruff check --no-cache tools/fonts
tools/fonts/.venv/bin/ruff format --no-cache --check tools/fonts
tools/fonts/.venv/bin/black --check tools/fonts
tools/fonts/.venv/bin/isort --check-only tools/fonts
tools/fonts/.venv/bin/flake8 --config tools/fonts/.flake8 tools/fonts
shellcheck scripts/fonts.sh
```

## Verification

- **Self-test** (`test_app/main/orb_fonts_selftest.c`, plain C + LVGL), for each of the six sets: every style and variant resolves all 254 text characters and all 136 icons through LVGL's own lookup and fallback chain without placeholders; tabular and clock figures are all the same width; the tabular `:` sits where the text `:` does, `ORB_TIME_SEP` is raised, `-` comes from the text font; tabular fonts share their text font's line box; `line_height + orb_text_line_space()` equals Apple's leading; icon line boxes are centred on the text's cap height within 1 px; every filled icon's ink box is its outline's within 1 px; then it renders a screen of every style and variant. It also checks that labels styled through the API follow content size and Bold Text changes, that `orb_fonts_subject()` notifies, and that the licence notices are linked. It runs in `test_app` on the device (sets that need the pack are skipped if it isn't flashed) and on the Mac.
- **Host check** (`scripts/fonts.sh check`): builds Orb's LVGL fork and this component for macOS with the system compiler, then compares each of the 36 built-in fonts against its generated record (metrics, glyph descriptors, kerning and decoded bitmaps: identical); decodes RLE glyphs on two threads at once (0 of 40,000 wrong); checks all six sets (0 failures); renders 738 preview lines and compares with Pillow: 95.3% of inked pixels identical, 99.93% within one 4 bpp step, worst 3 steps. Missing/corrupt-pack rescue checks exercise all styles, variants and clocks with real LVGL: 165 checks for each case.
- **Live status**: 2,136 assertions against the real LVGL fork cover state clamping, RSSI thresholds, colour precedence, proportional fill, charging knock-outs, inherited opacity, antialiasing, redraw bounds, all 120 style/settings/variant combinations, tabular figures, font-pack allocation failure/recovery and observer cleanup. The component compiles as C99 with `-Wall -Wextra -Werror`.
- **Pytest**: 68 passing unit/integration tests cover the frozen 129-glyph ABI plus seven appended shell glyphs, upstream SVG transforms, upload arrow/bar orientation, fill/weight geometry, the Always On low-ink weight contract, archive SHA-256 rejection with fake downloads, compact digits, native status checks, deterministic framebuffer output, bounded assets envelopes, real RSA signature/tamper/wrong-key checks and missing/corrupt-pack rescue.
- **Previews** (`docs/screens/fonts`): Pillow always hints, so it renders each glyph at 16x and box-filters down to get FreeType's unhinted coverage (99.87% of glyph pixels within one 4 bpp step of lv_font_conv's), and lays lines out with lv_font_conv's stored advances and kerning.
- The integrated wave 2 firmware builds with ESP-IDF 6.1. No board was touched by this stream; the lead runs migration and captures actual boot/update heap measurements.

## Licences

- **Inter** © The Inter Project Authors, SIL Open Font License 1.1: [licenses/Inter-OFL.txt](licenses/Inter-OFL.txt). Inter declares no Reserved Font Name; the generated instances are named "Orb Inter" anyway.
- **Framework7 Icons** © Vladimir Kharlampidi, MIT: [licenses/Framework7-MIT.txt](licenses/Framework7-MIT.txt).
- **Phosphor Icons** (listed fallbacks) © Phosphor Icons, MIT: [licenses/Phosphor-MIT.txt](licenses/Phosphor-MIT.txt).
- All three licences, with the copyright lines, are compiled into every image that uses these fonts (`src/orb_fonts_notices.c`, referenced by the font table, so whatever links a font links them; `orb_fonts_notices()` returns them) and stored in the font pack's header, as OFL section 2 and the MIT licence require of copies.
- [rsms/esp32-dot](https://github.com/rsms/esp32-dot) (no licence) was read for ideas on driving lv_font_conv; none of its code or assets is used.
- Orb's code here (generator, API, tests) is the repo's own.

## Not yet

- Rendering timings on the device, with two draw units: flash `test_app` (and the pack) and read its log.
