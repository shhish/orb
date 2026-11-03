# orb_ui

Orb's design system (`docs/hig.md`): tokens, type, Liquid Glass and motion.
Depends on LVGL and `orb_fonts`; apps and the desktop simulator use it unchanged.

## Design 2.0 kit

Include `orb_kit.h`. The [2.0 implementation contract](../../../docs/design/2.0/IMPLEMENT.md)
and system sheet define the device-pixel geometry. Factories return ordinary
LVGL objects owned by their parent. They preserve object `user_data` for the
caller; add activation callbacks after creating a control. Use the LVGL lock.
This library introduces shared controls; existing screens migrate separately.

| Role | Factory / tokens |
|---|---|
| Disc | `orb_kit_disc_create`: standard 80, mini 56, row 40, art 180; accent fill, neutral glass, tinted glass, track |
| Focus | `orb_kit_disc_set_focus`: white outline 3 px wide, 4 px clear gap; layout and hit area unchanged |
| Text actions | `orb_kit_button_primary_create`: 64 tall, Headline semibold, 26% accent material; `orb_kit_button_secondary_create`: 52 tall, Caption 2 semibold, neutral glass |
| Glyph actions | `orb_kit_glyph_button_create`: 80 px, tinted primary or neutral secondary |
| Placement | `orb_kit_place_bottom_arc`: center (233,380); `orb_kit_place_arc_pair`: centers (154,356), (312,356) |
| Rim text | `orb_kit_rim_title_create`, `_two_colour_create`, `_date_create`, `_caption_create`; baselines 200/212; `_rim_title_inset_create` or `_rim_title_set_inset` uses 176 inside a full rim ring |
| Position | `orb_kit_page_dots_create`, `_set_active`; 8 px dots, 14 px pitch on radius 186 inside rim rings; `orb_kit_scroll_arc_attach` for lists, `_scroll_arc_create`/`_set_position` for paged stacks: right rim radius 224, width 4 |
| Gauge | `orb_kit_ring_create`, `_set_value`: centerline radius/width supplied, accent track 18%, percentage 0–100 |
| Tick ring | `orb_kit_tick_ring_create`, `_set_position`: 12 minute or 60 second ticks; radius names the outer tips, marker position is elapsed/period |
| Arc input | `orb_kit_rim_slider_create`, `_set_angles`, `_set_value`, `_get_value`: same gauge plus 30 px white knob, touched glass, detents |
| Row | `orb_kit_row_create`, `_set_value`, `_switch`, `orb_kit_list_apply`: 72 px source row with 40 px leading disc and optional subtitle/value/switch |
| Hero fit | `orb_kit_hero_fit`, `_hero_fit_info`: full semantic text, given width and 2–3 line budget; Title 3 → Headline → Caption 2 |
| Toggle | `orb_kit_switch_create`, `_set_checked`, `_get_checked`: one 80×46 control, 38 px knob, shared velocity-preserving travel spring |
| Live value | `orb_kit_value_set`, `_hero_set_value`: existing label, tabular figures and a caller-supplied reservation such as `100%` or `88:88`; `orb_kit_value_info` diagnoses stable geometry and preparation counts |
| Live control text | `orb_kit_button_set_value`, `_chip_set_value`, `_ring_set_text`, `_row_set_live_value`, `_row_set_live_text`: semantic setters, no screen-owned child pointers |
| Level | `orb_kit_level_create`: the Control Center bottom arc, radius 213, width 14, start 145°, sweep −110°, knob 30; optional inset title/icon, accent live value; existing rim-slider setters also update the value |

`ORB_KIT_*` colours name Claude, Codex, Dot, Home, Time, Play, Sound and date
accents from Main. `ORB_GRAY` is exactly #8E8E93. Secondary text is #EBEBF5
at `ORB_KIT_SECONDARY_OPA` (153), including over nonblack content. The disc
ink rule uses linear-light sRGB luminance ≥0.3 for black, white otherwise.
This is the 3:1 white-ink contrast boundary required by HIG §5: orange,
green, yellow, teal and mint choose black, including the Apps catalogue.
It supersedes the frame's white orange/green glyphs, which fall below 3:1.
Framework7 glyphs use shared emphasized
font styles, including the current content size and Bold Text.

### Composition and lifetime

```c
lv_obj_t *title = orb_kit_rim_title_two_colour_create(
    screen, "GUEST SUITE BATH", ORB_KIT_HOME, " · 3 ON");
lv_obj_t *action = orb_kit_button_primary_create(screen, "Play", ORB_KIT_PLAY);
orb_kit_place_bottom_arc(action);
lv_obj_add_event_cb(action, activated, LV_EVENT_CLICKED, context);
```

Rim titles uppercase ASCII while preserving UTF-8. They measure actual glyph
advances and tracking at their selected baseline radius. Above 120°, Footnote 1 steps to
Footnote 2, then removes complete UTF-8 characters for a terminal ellipsis.
Two-colour titles keep suffix opacity separate; dates use red weekday and white
day (`_date_create(screen,"Wednesday","28")`). Caption text remains upright
at 6 o'clock in Footnote 2, secondary at 60%. Initial construction prepares both
transparent slices before a navigation capture can translate the container.
Later content/font changes update text, geometry and metadata immediately,
then coalesce preparation of the two transparent cached slices into one LVGL
async job after the scene callback
unwinds. Immediate captures use the current native glyphs while the sources
are dirty; ordinary refresh can also prepare them. Deleting the rim cancels
its pending job before its borrowed strings and children are retired.
Two slices also bound the AX3 cold workspace below the renderer's 256 KiB
source limit. The returned rim
object is a wrapper containing real `lv_arclabel` instances and retained
rasters; use `_rim_text_set` and `_rim_text_info`, not label setters on it.
For Apps' focused-name caption, call `orb_kit_rim_caption_set_style(caption,
ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED, ORB_LABEL, LV_OPA_COVER)`; the same
setter supplies FaceSlots' accent caption with Footnote 1. Both retain the
212 px baseline and follow accessibility font changes.
`orb_kit_rim_text_get` returns borrowed semantic text before visual fitting.
Inset titles retain their 176 px baseline across content and font changes;
their rendered ink clears a radius-213, 14 px-wide ring even in AX3 + Bold.

Place gauges before other content. Radius means stroke centerline, so an LVGL
arc's outside diameter is `2*radius+width`. Rings animate in 280 ms, retaining
velocity when retargeted; Reduce Motion applies immediately. Sliders default to
HomeLight's 135° start and 270° sweep. `_set_angles` accepts a signed sweep;
for Control's bottom rim use a 145° start and −110° sweep. Programmatic setters
do not emit detents. User value changes notify once when crossing a configured
detent; callbacks may delete the slider. Detents do not play sounds themselves.

Create all rows while idle, then call `orb_kit_list_apply(list)` once. The
helper enables the `orb_round_list` chord compositor with `visible_cache`:
rows intersecting the list viewport retain prepared sources, and so do rows
within one viewport height of it, up to `ORB_ROUND_LIST_LOOKAHEAD_BUDGET`
(64 KiB of PSRAM per list). A 2 ms preparation timer releases rows beyond that
window and admits entering rows before refresh, including during a fling.
Once the display has rendered nothing for 50 ms, the same timer captures one
missing nearby row per slice, upcoming rows (in the last scroll direction)
first, so a later throw rarely pays for a capture. Retained rows outside the
viewport keep no worker pose and report no presentation or hit bounds. Scroll
and draw callbacks never allocate or typeset a source. Existing sources remain immutable during motion, and
explicit content changes wait until idle. Call `orb_round_list_update(list)`
after other explicit content changes. The optional `visible_cache` config
field defaults to false for legacy round-list users, preserving their fully
prepared source contract. Ellipsis rasters are shared by actual font, tracking
and ink; each flexible label retains cuts for its actual line count rather
than six unused lines. Focus capture includes the leading disc's extended
draw bounds, so the 3 px ring and 4 px clear gap survive composition without
changing its 40 px semantic bounds. The optional
switch is a floating sibling of its row with a transparent anchor in the row's
cache. Its bounds follow `orb_round_list_child_bounds` without rebuilding row
sources during travel or scroll. `orb_kit_row_switch(row)` returns that live
control. Delete the row or list to delete its switch; do not reparent it.
The scroll arc is owned by the list and is a sibling; either deletion order is
safe. Delete and reattach explicitly if removing just the indicator.
Every row label has an explicit one-line box and dots overflow; content-height
labels can otherwise wrap into each other. Two-line rows position their actual
glyph ink during creation/font changes, preserving the AX3 type size and a
minimum 2 px gap. Scroll and drawing only use prepared rows; entering sources are admitted by
the separate timer before the display timer refreshes.

Row title/subtitle/value lanes step through the published smaller text styles
before using single-line ellipsis. A measured value rail can use up to half the
row width, with the title yielding first; narrow scrolling previews clip the
prepared lanes at glyph boundaries. `orb_kit_row_set_text_role(row, role,
ORB_TEXT_TABULAR)` selects tabular duration/date/count figures independently
for title, subtitle or value. Default roles retain existing typography.

Row fitting stops at Caption2 (25 px at Default). Footnote2's21px source would
become17px at the82% shoulder presentation, below the compositor's20px minimum,
and could suppress the entire visible row while leaving a native tap target.
Use ellipsis at Caption2 to retain visible and interactive row geometry.

`orb_kit_hero_fit(label, text, width, max_lines, &info)` follows Dynamic Type
and Bold Text, preserves the whole string and never ellipsises. A false return
means the caller must expand its layout budget. “Reading Room & Family Library”
fits 220 px × 3 lines in all three review profiles. The measured widest repeated
32-byte printable ASCII fixture (32 W's) needs 286 px at Default and 352 px at
AX3/Bold; a 220 px × 3-line centre cannot contain that name at the allowed styles.

Tick rings are plain content, with no full-circle raster or independent clock.
Radius 219 and 12 divisions reproduce Design 1.0's stopwatch geometry. Pass
elapsed and period in the same units (for example 82400 and 60000 milliseconds)
to move the marker clockwise from 12 o'clock. Sixty divisions add shorter ticks
between the major five-second ticks. A paused screen simply stops setting time.

All tappables install the same idempotent `orb_kit_press_apply`: 94% in 80 ms,
bounded glass brightening, critically damped 160 ms return. Reduce Motion
retains only the material response. Glass contracts through four prepared
native SDF sizes and scales its retained ink, so its blur continues sampling
the actual backdrop rather than a black temporary transform layer. Native
integer geometry stays within 1 px of the nominal 94% size. `PRESS_LOST`,
10 px pointer travel, scroll ownership, and
`orb_kit_press_cancel` immediately reset visuals and retire the contact until
fresh `PRESSED`; retired clicks never reach application callbacks. Cancellation
also emits `LV_EVENT_CANCEL` so a touched switch knob retires in the same frame.
No shell contact-retirement API is exposed, so the kit implements this rule
locally using LVGL contact/scroll events. Keep switches and buttons on the
scrollable content tree so native pointer arbitration can deliver `PRESS_LOST`.
Release bounds use the original press-time hit rectangle, so a stationary
contact remains valid while an incoming app continues its navigation slide.

### Migration from the consistency and behaviour audits

| Audited variant(s) | Replacement |
|---|---|
| Face 60 px Apps; close/more utilities | Mini disc (56), neutral glass |
| Compass 72, Honeycomb 80, Apps AX3 72 | Standard disc (80), accent fill; focus outline instead of enlargement |
| Home rooms 80/90; scene 64; accessories 72/124/132 | Standard disc (80), glyph/initials content as appropriate; focus outline |
| Agents/Dot talk 64; spoken-stop 72; Time/Games controls 72; Now Playing 64/88 | Glyph button (80), neutral or tinted primary |
| About/Accessibility missing leading disc; Notifications 26; Stack/Sessions 32; Home/design_row 42 | Row factory with shared 40 px leading disc |
| Shell 72; power/alarm 62; Agents/Reminder 60; Notifications 58; HIG 56; low-battery/Play 54; Stack/Onboarding 52; voice/game 48; Scenes/Garden 46; attention 36 | Primary 64 for one action, secondary 52 for other text actions; attention/status content is not a button |
| Thermostat solid capsules; text-only Done | Secondary capsule, or one tinted primary when semantically primary |
| `header`, `design_list`, Sound/Battery/Time and setup/OTA top headings | Rim title, with two-colour/date factories where applicable |
| Apps focused name, Agents hint, scene hint | Upright rim caption |
| Page dots 7/8 px at y420/422/442; approval page text | 8 px rim dots; choose active accent explicitly |
| Stack position radius225; other ad-hoc scroll arcs | Shared radius224, 4 px scroll arc attached to content or set to a stack page |
| Volume/brightness/Home/timer knobs 24/26/40; independently styled slider tracks | Shared rim slider and 30 px touched-glass knob |
| Timer/Usage/gauge variants radius194–225, widths6/10/12/14 | Ring factory with the frame's concentric radius and width |
| #8D8D93, #A0A0A8, #929298, #737376 used as semantic tokens | `ORB_GRAY`, or #EBEBF5 plus 60% opacity for secondary text; preserve intentional Always On dimming |
| Behaviour audit rank15: glass swell, launcher spring, row opacity | Shared press response/contact retirement |
| Behaviour audit rank16: Sessions, Alarm, Sound/Battery/Control switch variants | Unified switch; track ordinary content, knob glass only while touched |

The exported Timer coordinates override Main's approximate “±42°” pair prose.
The old exported ring/title geometry overlapped the ink with the ring. Use the
Design 2.0 inset baseline (176) whenever a full ring is present; the HIG's
nonoverlap requirement governs composition.
This kit does not migrate owner streams or alter their local colour literals.

### Live data and private children

The kit owns every child of a returned button, disc, row, rim title and level.
Screen code must use `orb_kit_button_set_text`, `orb_kit_disc_set_glyph`,
`_set_ink`, `_set_ink_opa`, `_set_text_style`, `_set_glyph_hidden`,
`orb_kit_row_set_text` and `_set_focus`; indexing children is not an API.
Static text updates remeasure and prepare static presentation once. Unchanged
glyph, ink, focus and text setters are no-ops.

For live content, apply the desired typography, then call
`orb_kit_value_set(label, "9%", "100%")`. The template reserves width once;
subsequent values copy into a kit-owned buffer without allocation. `NULL`
retains the template. A template or font change is an explicit layout change;
ordinary ticks never remeasure. Published text faces gain tabular figures;
clock faces preserve their existing tabular font family. A larger fixed caller
lane remains intact. Align text in the reserved lane rather than repositioning
the label every tick.

Steady updates invalidate native ink using the retained single-line metrics.
The kit encapsulates the fork's private label fields for this operation:
LVGL's public text setter otherwise allocates a one-shot display layout
callback per changed value, even for a fully fixed-size label. Screens never
depend on those fields. Values exceeding the caller's maximum text capacity
can grow the buffer once, so templates must cover the complete bounded domain;
large summary counts should use an honest compact format with a fixed maximum.

Live labels remain native at rest as well as while changing. They have no
cached ink to rebuild on a detent or after a short pause. This also avoids a
timer destroying and readmitting a source at the start of every interaction.
`orb_kit_button_set_value` reserves both label and capsule geometry; later
detents never resize the glass. `orb_kit_ring_set_text` owns a centered native
value. `orb_kit_row_set_live_text` supports title, subtitle and value lanes:
the prepared row contains an immutable transparent anchor, and native live
ink sits in a floating clipping lane that follows the chord compositor's
published bounds. Its scale follows row height, since a flexible text lane
can narrow independently of that scale. Clipping trims leading outside the
capsule without hiding readable AX3 figures. Only template/font changes update
the anchor. Values never rebuild row caches, and list travel never typesets
them. The older semantic `orb_kit_row_set_value` also respects an existing
live registration. Explicit role/font changes synchronize all retained lanes.

`orb_kit_chip_set_value` uses a 34 px total horizontal inset rather than the
ordinary secondary action's 68 px. It retains the shared 52 px height and
17 px side margins, allowing the two Control Center selectors to retain their
20 px focus-ring clearance at AX3. Normal secondary buttons keep their action
padding. A leading Framework7 glyph and space in a chip's template creates a
private fixed icon and a separate right-aligned value rail: `9%`, `10%` and
`100%` keep both the icon and percent unit anchored. `orb_kit_button_get_text`
returns the complete semantic string. Ring/level values center their actual
ink inside the reserved box, as the frames require; same-length tabular
changes are still immovable.

Every brightness, volume and Home level uses `orb_kit_level_create`, including
stepped ranges. Standalone pages may position the value with
`orb_kit_level_set_value_center` and choose a hero style with `_set_text_style`;
the arc, knob, detents and pointer mapping stay identical. The optional title
uses the 176 px inset baseline. Pointer samples are mapped once with `atan2f`
before the native arc handler: LVGL's integer angle approximation otherwise
can emit a higher value and then a correction for the same sample near six
o'clock. Programmatic setters remain silent; keyboard/encoder events retain
the native semantic value path.

### Gallery and measurements

Run `nice sim/kit_gallery` to build with four workers and render Default, AX3,
and AX3 + Bold + Reduce Transparency into `sim/build/kit-gallery/`. Run
`nice ctest --test-dir sim/build -j4 --output-on-failure` for geometry, contact,
cache/lifetime and whole-simulator coverage. The gallery prints per-component
retained LVGL payload and separate shared glass-cache memory, plus cached rim
versus plain-label refresh timing. Host payload is not a device heap measurement;
the lead still verifies board screenshots and owner budgets before release.
Detailed measurements and design-gate evidence are in
`docs/agent-briefs/wave2/kit-results.md`.

No kit draw/scroll callback allocates component data. The LVGL renderer still
allocates generic task/layer metadata, also used by a plain label; the gallery
reports those allocator calls explicitly. A literal zero-allocation renderer
would require changes to the LVGL allocator outside this task's edit scope.

## Tokens and type

- `ORB_PT(pt)`: Apple points to pixels by physical size (1.634 px/pt), so a
  44 pt control (`ORB_CONTROL`, 72 px) is as big under the finger as on an
  iPhone. Screen radius and rim margins (`ORB_RIM_MARGIN_GLASS`, `_TEXT`),
  `orb_chord_width()` for a line box's usable width inside the circle.
- Colours: Apple's dark system palette as re-tuned for Liquid Glass, greys,
  label colours.
- `orb_fonts` owns the text API: Inter, Framework7 icons, HIG leading/tracking,
  tabular figures, clock faces, content sizes and Bold Text. Apply a style with
  `orb_text_apply(label, style, ORB_TEXT_REGULAR, 0)`. The UI header includes
  the shared font API; there is no second typography table.

## Liquid Glass

`orb_glass_apply(obj, kind, tint)` on a circle or capsule:

- LVGL's backdrop blur of what's beneath: averaged downsampling, 8-bit IIR filtering,
  bilinear reconstruction and coverage-weighted rounded edges, with tint applied
  during reconstruction and screen-anchored RGB565 dithering. Brighter pressed.
- Over it, two 8-bit masks drawn from signed distance fields once per size
  (`orb_glass_sdf.c`) and kept in PSRAM: a black one with the soft shadow
  outside and the darkened inner edge, and the rim, a hairline with a specular
  highlight where the edge faces the light and a fainter bounce opposite. When
  the light moves only the rim's own pixels are rewritten.
- Pressed, the glass swells 6 % and brightens. Geometry is shared by capsule
  size, rest shadow-caster size and the fixed AA recipe, across objects and
  navigation. A 32-entry LRU holds at most 640 KiB in the UI allocator (PSRAM
  on device); live controls lease entries, so queued image draws cannot see an
  eviction. The rim light is shared too, while tint/backdrop remain per control.
- `orb_glass_cache_prewarm()` prepares the standard circles/capsules at boot,
  after the PSRAM allocator is installed. Navigation and drawing only look up
  shapes. An unfamiliar size gets the opaque accessibility material, then one
  fixed-point preparation job per timer invocation. A failed allocation keeps
  the usable opaque control and suppresses repeated OOM attempts for that key.
  Deleting the last control cancels the preparation timer. Release inactive
  cache payload with `orb_glass_cache_clear()` only between finished refreshes.
- Geometry construction uses integer distance/coverage and a Gaussian lookup
  table instead of padded per-pixel `sqrtf`/`erfcf` scans. Reference comparison
  bounds dark/rim differences at 1/2 A8 levels, except the historical rounded
  alpha-3 shadow cutoff. RGB565 backdrop reconstruction and dither are unchanged.
  `orb-dev heap` includes actual device mask build/max-circle time, payload,
  failures and fallback draws; host build times do not establish device time.
- `orb_glass_band_guard`: for `orb_display_set_band_guard`, keeps each glass
  in one render band, as its blur reads all of it.

LVGL's own rounded fills, borders, arcs and box shadows rebuilt their masks
every frame; this costs a fraction (see `docs/design.md`, Rendering).

## Motion

- `orb_ui_set_gravity(x, y, z, dt)`: the accelerometer at 30-60 Hz, on screen
  axes. Smoothed (150 ms) it moves the light, kept within 30 degrees of 12
  o'clock; against a slow (3 s) estimate of the pose the device rests in, it
  drives parallax, so a steady pose looks centred and movement shows depth.
- `orb_parallax_add(obj, depth, max_px)`: shifts a container with tilt, up to
  `depth * max_px` towards the side that dips (translate style; raster caches
  inside survive a move). Only moves for changes over 0.6 px.
- `orb_glass_set_light(deg, amount)`: to stage the light directly.
- Accessibility: `orb_ui_set_reduce_motion()` (parallax off, light frozen,
  press without swelling), `orb_ui_set_reduce_transparency()` (nearly opaque
  glass). Set them before building the UI.

`orb_glass_materialise(control, true)` develops a newly constructed glass
control from absent to present. Call it after `orb_glass_apply()`; existing
callers of `apply()` remain immediate. The normal transition drives a single
material amount through blur radius, tint strength, shadow, darkened edge and
rim, while the label follows that amount. The object container stays fully
opaque. It uses the HIG §6 .55 s critically damped spring at 240 Hz substeps;
retargeting to `false` preserves current position and velocity. A zero amount
hides the object and its hit target. Deletion cancels both material and press
animations. Call under the LVGL lock.

Reduce Motion replaces this with a 150 ms cross-fade of the complete, fixed
material, as explicitly required by HIG §6. LVGL backdrop blur ignores ordinary
object opacity, so this path saves the control's local backdrop in PSRAM and
mixes the fixed rendered material over it. The bounded scratch buffer is freed
at completion/deletion; allocation failure selects the target immediately.
Reduce Transparency keeps its opaque final material throughout that cross-fade. `orb_glass_set_amount(control, 0..1)` is
available for direct gesture progress and deterministic capture; it clamps the
value and cancels the animation. `orb_glass_get_amount()` reports the current
amount. The current renderer has no independent lens/displacement input, so this
API does not claim to add optical lensing; it animates the implemented blur,
tone and edge material. Adding displacement remains a renderer task.

`orb_glass_sdf.c` and `orb_motion_math.c` are plain C, tested on the host.

## Scroll edges and round lists

Build the content viewport first and floating glass controls as later siblings.
Apply `orb_scroll_edge_apply(viewport, top_px, bottom_px)` to that viewport:
content fades to true black over the given top/bottom distances, while the
controls and their labels stay sharp. Zero disables an edge. Each extent is
clamped to half the viewport height so ramps cannot overlap. This draws two
black alpha ramps after the viewport's children, clipped to the current render
band; it needs no full-screen mask, blur scratch, timer, or extra band buffer.
It is a content-edge effect rather than a canvas background. Choose the extent
to cover the floating bar plus about 8 pt; do not raster-cache the viewport or
place the floating controls inside it.

`orb_scroll_edge_follow(viewport, top_px, bottom_px)` applies the same mask,
with the corresponding dissolve suppressed at the settled start/end or elastic
overshoot. It reads actual scrolling at draw time, so interruption and settle
need no extra timer. Reapplying an unchanged policy creates no invalidation.
The extent grows with the actual clipped distance, preventing a sudden full-line
dimming on the first/last pixel of a drag.
The production lists use 32/48 px top/bottom extents; prose uses 24/24 px.
`CONFIG_ORB_UI_SCROLL_EDGE_SOFT` (host `ORB_UI_SCROLL_EDGE_SOFT`) defaults on;
turn it off to restore each surface's prior extents. List viewports start at
y=140 or lower, beyond the diagonal glass controls even during press swell. Thus
the top navigation samples true black throughout motion, while content below
it dissolves gradually rather than clipping. Face and card controls keep their
existing material and typography. This is the HIG §2.1 content dissolve; it
adds no gradient/glow canvas or decoration.

`orb_round_list_apply(list, config)` makes each non-floating direct child a
cached content row on the black canvas. Center rows horizontally on the panel.
The default config is `{ .margin = ORB_RIM_MARGIN_TEXT, .fade = ORB_PT(32) }`;
reserve additional parallax travel in `margin` when a containing layer moves.

The chord compositor specializes RGB565 and swapped RGB565 output. Each row
retains its adjusted palette/channel tables keyed by effective opacity, live
matte and byte order, and its capsule inset table keyed by scale and radius.
The bounded tables share the row's lifetime and are included in cache metadata;
Y translation and later bands reuse them. Each child prepares one monotone X
map reused by its scanlines. Matte and sparse
gaps use contiguous fills, native direct-colour spans use copies, and indexed
ink uses the adjusted palette without per-pixel division or format dispatch.
The unchanged ARGB/XRGB painter is the exact snapshot reference. Ellipsis alpha
still blends with the current destination. The synchronous callback retains
exclusive band ownership after draining prior tasks; no source, clip or row
pointer escapes to a worker, and no component allocation occurs during paint.
`chord_list` compares native and swapped output pixel-for-pixel against that
reference over palette, sparse direct and dense sources, opacity, shrinking,
ellipses and translated/partial clips. Host results do not establish panel FPS.

Rows uniformly shrink to fit both outer corners inside the safe circle and fade
as their centers approach its top/bottom. All rows of a given size share a
half-pixel-center lookup table (about 2.8 KiB through the `orb_ui` allocator).
Scrolling changes only raster scale and opacity and reuses the source bitmaps.
The helper owns these raster properties on all direct children: floating rows
are restored to native scale and full opacity when the list updates.
Tables are made only for new row sizes, and obsolete sizes are freed after
layout changes. If a table allocation fails, correct geometry is calculated
directly for that row instead.

```c
orb_scroll_edge_apply(viewport, ORB_PT(82), ORB_PT(72));
orb_round_list_apply(list, &(orb_round_list_config_t){
    .margin = ORB_RIM_MARGIN_TEXT + 12,
    .fade = ORB_PT(32),
});
lv_obj_raster_prepare(list);  /* optional: warm caches before the first scroll */
```

Scroll, layout, and list-size events update the row transforms automatically.
Call `orb_round_list_update(list)` after adding/changing rows or moving a
containing viewport. Reapplying either helper updates its configuration without
adding another callback; deleting the object frees its helper state. Neither
helper animates on its own, so Reduce Motion retains normal finger-driven
scrolling and round-safe geometry.

Raster transforms keep LVGL's logical layout and hit rectangles. When dispatching
a tap to a row, call `orb_round_list_hit_test(row, &screen_point)` to reject the
parts outside its visible scaled bounds; a fully faded/hidden row rejects taps.
The check also rejects points clipped by any containing viewport.
The helper is intended for content on black: do not use it for glass controls,
overlapping artwork, or a list that needs a translucent backdrop.

## Integration

`orb_ui_init(&(orb_ui_config_t){ .alloc, .free })` after `lv_init()`: the
allocator is for glass masks (tens of KB each, read every frame); give it
PSRAM. `orb_fonts` selects compressed fonts and observer support; flash the
font pack for Larger, AX3 and Bold Text (enabled in the integrated defaults).

## Tests

`make -C tests/host test` (macOS clang): distance fields, shadow, rim and its
lighting; low-pass, light direction and clamp, parallax direction, limits and
hysteresis.

From the repository root, `ctest --test-dir sim/build -R round_helpers
--output-on-failure` checks the public content-edge/list APIs through the same
band renderer: pixel ramps, partial redraw equality, sharp sibling controls,
round-safe row bounds, tap filtering, cache reuse and allocation cleanup.
`sim/build/test_round_helpers docs/screens/glass/helper` also writes the two
API demonstration frames beside the glass comparison captures.

`ctest --test-dir sim/build -R glass_materialise --output-on-failure` exercises
material development over real text, intermediate blur and full container
opacity, reversible transitions, exact absent/present states, Reduce Motion +
Reduce Transparency, and deletion during animation. `sim/build/test_glass_materialise
PREFIX` optionally captures absent, developing, complete and reduced-crossfade
frames. This test establishes API/render behaviour, not physical panel timing.


`ORB_DISPLAY_GLASS_SPARSE_MASKS` is a separate default-off A/B option. It
prepares two occupied spans per A8 shadow/rim row inside the same mask lease
and 640 KiB PSRAM budget, skipping only transparent padding/interiors during
RGB565 blending. Rim spans include every geometry pixel before relighting,
so moving the light cannot reveal an omitted edge. The dense path remains
the fallback for other formats/transforms. No visible material is simplified.
`ctest --test-dir sim/build-pacing -R 'glass_shadow|glass_materialise|glass_design'
--output-on-failure` verifies dense/sparse pixels, AA/stride/clip/opacity and mask
lifetime. Device cost and heap acceptance still require the lead's A/B run.
