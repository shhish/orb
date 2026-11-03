#pragma once

/*
 * Orb's design system (docs/hig.md): tokens, type, the Liquid Glass material,
 * and motion (the light on the glass and parallax). Uses LVGL and orb_fonts, so
 * apps and the desktop simulator build against it unchanged.
 *
 * Sizes are given in Apple points and converted to device pixels by physical
 * size, so a 44 pt target is as big under the finger here as on an iPhone.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"
#include "orb_fonts.h"
#include "orb_color_tokens.h"

/* 466 px across 1.75 in is 266 ppi; Apple's point is 1/163 in (1.634 px/pt). */
#define ORB_PT(pt) ((int32_t)((pt) * 266 / 163))

/* The round screen (§3): its radius, and the margins glass and text keep from the bezel. */
#define ORB_SCREEN_D 466
#define ORB_SCREEN_R (ORB_SCREEN_D / 2)
#define ORB_RIM_MARGIN_GLASS ORB_PT(6)
#define ORB_RIM_MARGIN_TEXT ORB_PT(10)
/* HIG §3: round touch targets up; ORB_PT truncates layout distances. */
#define ORB_CONTROL 72              /* default 44 pt target */
#define ORB_CONTROL_MIN 46          /* minimum 28 pt target */

/* Apple dark-mode system colours, as re-tuned for Liquid Glass (§5). */
#define ORB_LABEL_LEGACY_SECONDARY lv_color_hex(ORB_RGB_LEGACY_SECONDARY)
#define ORB_BLUE lv_color_hex(ORB_RGB_BLUE)
#define ORB_GREEN lv_color_hex(ORB_RGB_GREEN)
#define ORB_ORANGE lv_color_hex(ORB_RGB_ORANGE)
#define ORB_RED lv_color_hex(ORB_RGB_RED)
#define ORB_YELLOW lv_color_hex(ORB_RGB_YELLOW)
#define ORB_TEAL lv_color_hex(ORB_RGB_TEAL)
#define ORB_INDIGO lv_color_hex(ORB_RGB_INDIGO)
#define ORB_PURPLE lv_color_hex(ORB_RGB_PURPLE)
#define ORB_PINK lv_color_hex(ORB_RGB_PINK)
#define ORB_GRAY lv_color_hex(ORB_RGB_GRAY)
#define ORB_GRAY4 lv_color_hex(ORB_RGB_GRAY4)
#define ORB_GRAY5 lv_color_hex(ORB_RGB_GRAY5)
#define ORB_GRAY6 lv_color_hex(ORB_RGB_GRAY6)

/* HIG §5: existing power telemetry colours, including increased contrast.
 * Keep the low-battery red distinct from the Design 2.0 action red above. */
#define ORB_POWER_LOW lv_color_hex(ORB_RGB_POWER_LOW)
#define ORB_POWER_LOW_CONTRAST lv_color_hex(ORB_RGB_POWER_LOW_CONTRAST)
#define ORB_GREEN_CONTRAST lv_color_hex(ORB_RGB_GREEN_CONTRAST)
#define ORB_GRAY_CONTRAST lv_color_hex(ORB_RGB_GRAY_CONTRAST)

/* Scalar manifests and reported artwork share the same RGB conversion. */
static inline lv_color_t orb_color_from_rgb(uint32_t rgb) { return lv_color_hex(rgb); }
/* Existing recursive accessibility pass; preserves palette order and fallback. */
lv_color_t orb_color_increased_contrast(lv_color_t color);

/* Labels: primary white; secondary #EBEBF5 at 60 %. */
#define ORB_LABEL lv_color_white()
#define ORB_LABEL_SECONDARY lv_color_hex(ORB_RGB_LABEL_SECONDARY)
#define ORB_LABEL_SECONDARY_OPA 153

/*
 * Width a line box spanning rows [y1, y2] may use inside the circle, `margin`
 * from the bezel: the narrower of the chords at its top and bottom (§3).
 */
int32_t orb_chord_width(int32_t y1, int32_t y2, int32_t margin);

/* Fade content to the black canvas at a viewport's top/bottom edges. Apply to
 * the content viewport, with floating controls as later siblings, never to
 * the screen that owns those controls. A zero extent disables that edge.
 * Idempotent; callbacks and memory are released with the viewport. */
bool orb_scroll_edge_apply(lv_obj_t *viewport, int32_t top_px, int32_t bottom_px);

/* As above, with no dissolve at the settled start/end or elastic overshoot. Tracks
 * the viewport's actual position during drag, fling and settle; no timer.
 * Keep viewport.y >= 140 below the diagonal toolbar's swollen glass controls,
 * so the entire sampled control rectangle remains the true black canvas. */
bool orb_scroll_edge_follow(lv_obj_t *viewport, int32_t top_px, int32_t bottom_px);

/* Host and firmware use the same default-on soft-edge policy. OFF retains
 * the previous surface-specific extents for same-source visual/perf A/B. */
#ifdef ESP_PLATFORM
/* Kconfig leaves a disabled bool undefined, so test it rather than expand it. */
#if defined(CONFIG_ORB_UI_SCROLL_EDGE_SOFT) && CONFIG_ORB_UI_SCROLL_EDGE_SOFT
#define ORB_UI_SOFT_EDGES 1
#else
#define ORB_UI_SOFT_EDGES 0
#endif
#else
#define ORB_UI_SOFT_EDGES ORB_UI_SCROLL_EDGE_SOFT
#endif

typedef struct {
    /* Safe-circle inset, including any reserved parallax travel. */
    int32_t margin;
    /* Distance from the safe circle's top/bottom over which rows fade. */
    int32_t fade;
    /* Design 1.0: native middle rows and one cached 82% rim presentation.
     * No font changes or continuous scale work during a scroll. */
    bool stepped;
    bool partial_only; /* Design 1.0 Sessions: fade only rows crossing the viewport/rim. */
    /* Design 1.1, opt in: immutable native rows at 2*(233-margin) pixels wide.
     * The cached compositor fits capsule end circles to the CURRENT chord;
     * direct children aligned RIGHT_* follow its trailing edge. Wide labels
     * use the available width, with cached ellipsis cuts. Keep >=16px text
     * insets in the native row. Zero/NULL preserves the original raster API. */
    bool chord_fit;
    /* Kit lists retain intersecting row sources plus a bounded window of
     * nearby ones. A short preparation timer admits entering rows before
     * refresh, outside scroll/draw callbacks; nearby rows wait for idle. */
    bool visible_cache;
} orb_round_list_config_t;

/* Cache direct child rows on black and scale/fade their bitmaps as they scroll
 * toward the bezel. Rows must be horizontally centered on the 466 px panel;
 * their logical layout stays unchanged. NULL uses the text margin and 32 pt
 * fade. Per-size transform tables are reused; no scroll/draw-time source allocation.
 * The helper owns child raster scale/opacity; floating children are restored
 * to native scale/full opacity on update.
 * Call update after adding/changing rows or moving a containing viewport.
 * Scroll, layout and list-size changes update automatically. Chord-fit callers
 * construct/insert rows and call apply/update while idle, before starting a
 * drag or rim owner; do not insert unprepared rows during motion. Changes to
 * retained row content/size defer their source rebuild until motion settles.
 * Sources retain lossless sparse RGB565 ink with an exact palette when useful.
 * visible_cache opts into viewport source retention: entering rows are
 * prepared by a short timer outside scroll/draw callbacks, including during
 * motion. Rows within one viewport height stay retained, and missing ones are
 * prepared one per slice once the display has been idle, within
 * ORB_ROUND_LIST_LOOKAHEAD_BUDGET. Existing dirty sources still wait until
 * idle. Only one temporary native-row snapshot is needed during preparation.
 * If chord source preparation fails, returns false and retains the original corner-fitted
 * raster presentation for the complete list, rather than a partial cache. */
bool orb_round_list_apply(lv_obj_t *list, const orb_round_list_config_t *config);
void orb_round_list_update(lv_obj_t *list);

/* A continuous non-pointer scroll owner (the rim crown) uses the same cached
 * child damage and SCROLL callbacks as a finger. BEGIN/END bracket the entire
 * drag and fling, rather than snapping after each programmatic sample.
 * scroll_to returns false only when this is not a managed round list. */
bool orb_round_list_scroll_to(lv_obj_t *list, int32_t y);
void orb_round_list_scroll_finish(lv_obj_t *list);
bool orb_round_list_scroll_active(lv_obj_t *list);

/* Raster scaling keeps LVGL's logical hit area. Use this check when dispatching
 * a row tap: it accepts only scaled bounds visible through ancestor clipping.
 * Screen coordinates. */
bool orb_round_list_hit_test(lv_obj_t *row, const lv_point_t *point);

/* Map a descendant's logical rectangle through its row's round presentation.
 * Use the returned screen bounds for an iris token or other spatial anchor;
 * a row's unchanged LVGL hit rectangle is not its rendered icon position.
 * False for unrelated, floating, hidden or entirely clipped content. Partly
 * visible descendants retain their complete transformed rectangle. */
bool orb_round_list_child_bounds(lv_obj_t *row, lv_obj_t *child, lv_area_t *area);

/* Current, un-clipped presentation rectangle, including chord width/rim scale.
 * Also useful for semantic hit targets and geometry tests. */
bool orb_round_list_row_bounds(lv_obj_t *row, lv_area_t *area);
/* Current pose opacity, including the row's local opacity but not ancestors.
 * Floating live content inherits those ancestors beside the cached row. */
lv_opa_t orb_round_list_row_opacity(lv_obj_t *row);
/* Optional immutable draw poses share one PSRAM cap across every list. Source
 * metadata retains its original independent budget; count both for total RAM. */
#define ORB_ROUND_LIST_DRAW_POSE_BUDGET (16u * 1024u)
/* Viewport-cache lists may additionally retain/prepare, while the display is
 * idle, sources for rows within one viewport height of it; PSRAM per list. */
#define ORB_ROUND_LIST_LOOKAHEAD_BUDGET (64u * 1024u)
typedef struct {
    size_t width_table_bytes, source_bytes, metadata_bytes;
    size_t draw_pose_bytes;
    uint32_t source_builds;
    /* Idle lookahead captures, not counted in source_builds, and the source
     * plus metadata bytes currently retained for rows outside the viewport. */
    uint32_t lookahead_builds;
    size_t retained_bytes;
} orb_round_list_cache_stats_t;
bool orb_round_list_cache_stats(lv_obj_t *list, orb_round_list_cache_stats_t *out);

/* Every stage is inclusive wall time, including nested stages/preemption.
 * Outside-refresh is classified by the observed display refresh-event scope;
 * it is a subset of us, never another cost to add to it. */
typedef struct {
    uint32_t calls, max_us;
    uint64_t us, outside_us;
} orb_ui_scroll_stage_t;

/* Bench-only UI-thread observations. Round, layout, hash, source preparation
 * and geometry can nest. Never add these stage totals to the round total. */
typedef struct {
    uint64_t round_calls, round_us, round_max_us, round_rows;
    uint64_t readable_calls, readable_us, readable_max_us, readable_rows;
    uint64_t readable_layout_calls, style_writes, redundant_style_writes, font_changes;
    uint64_t round_inside_refresh_us, round_outside_refresh_us;
    orb_ui_scroll_stage_t layout, geometry, hash, source_prepare, child_prepare, palette_prepare;
    orb_ui_scroll_stage_t source_cuts, source_snapshot, source_pack;
    /* Idle lookahead captures; source_prepare counts admissions only. */
    orb_ui_scroll_stage_t source_lookahead;
    bool refresh_phase_available;
} orb_ui_scroll_diagnostics_t;
void orb_ui_scroll_diagnostics_begin(void);
void orb_ui_scroll_diagnostics_end(void);
void orb_ui_scroll_diagnostics_snapshot(orb_ui_scroll_diagnostics_t *out);
/* UINT64_MAX when inactive. Internal clients bracket existing synchronous work. */
uint64_t orb_ui_scroll_diagnostics_clock(void);
void orb_ui_scroll_readable_record(uint64_t started, unsigned rows, unsigned writes, unsigned redundant,
                                   unsigned font_changes, bool layout);

/* Typography and icons come from orb_fonts, including Dynamic Type. */

typedef struct {
    /* For the glass masks (tens of KB each, written rarely, read once a frame):
     * slow, plentiful memory such as PSRAM. NULL: LVGL's allocator. */
    void *(*alloc)(size_t size);
    void (*free)(void *p);
} orb_ui_config_t;

/* Call once, after lv_init() and before any glass. */
void orb_ui_init(const orb_ui_config_t *cfg);

/* Accessibility (§6). Reduce Motion stops parallax and freezes the light;
 * Reduce Transparency makes glass nearly opaque. */
void orb_ui_set_reduce_motion(bool on);
bool orb_ui_reduce_motion(void);
void orb_ui_set_reduce_transparency(bool on);
bool orb_ui_reduce_transparency(void);

typedef enum {
    ORB_GLASS_REGULAR,   /* toolbars and controls over any content */
    ORB_GLASS_TINTED,    /* the screen's one primary action */
} orb_glass_kind_t;

/* HIG §6: a global clarity preference, retaining regular glass's adaptive
 * tone over black. Reduce Transparency always takes precedence. */
typedef enum {
    ORB_GLASS_PREFERENCE_CLEAR, ORB_GLASS_PREFERENCE_DEFAULT,
    ORB_GLASS_PREFERENCE_TINTED,
} orb_glass_preference_t;
bool orb_glass_set_preference(orb_glass_preference_t preference);
orb_glass_preference_t orb_glass_preference(void);

/*
 * Makes obj a Liquid Glass surface: the content behind it frosted, a tint, a
 * darkened edge, a rim with a specular highlight that follows the light, and
 * a soft shadow. Pressed, it swells slightly and brightens. Circles and
 * capsules, as Apple's controls; give obj its final size first.
 */
void orb_glass_apply(lv_obj_t *obj, orb_glass_kind_t kind, lv_color_t tint);

/* Tune one tinted action over a dark backdrop without changing its material.
 * Resting opacity is bounded to 24..90%; below 70% use accent label ink. A
 * press adds 20 percentage points below 70%, then rises to 90..95% as before.
 * Reduce Transparency uses an opaque gray6 surface for low tints and retains
 * the existing opaque accent material at 70..90%. False for a
 * regular/non-glass object or an out-of-range opacity. */
bool orb_glass_set_tint_opacity(lv_obj_t *obj, lv_opa_t resting_opa);

/* Boot-time PSRAM preparation of standard shape keys; never call during a
 * refresh. Cache is shared, leased by live controls, capped at 640 KiB. */
void orb_glass_cache_prewarm(void);
/* Release unused cache entries between completed refreshes (e.g. teardown). */
void orb_glass_cache_clear(void);
/* Font-setting transaction, with LVGL ownership and all draws drained. No UI
 * refresh may run between these calls. Restore exact leases after old fonts
 * retire, before releasing LVGL; the displayed material stays unchanged. */
void orb_glass_cache_suspend(void);
void orb_glass_cache_resume(void);

/* HIG §§2.2/6: develop/retract the material, not its container opacity.
 * First entrance starts at amount 0; later calls interrupt from the current
 * amount and spring velocity. Normal response .55 s, critical damping;
 * Reduce Motion uses a 150 ms cross-fade of the complete material instead.
 * Amount 0 hides the control (including its hit target); >0 reveals it.
 * Deleting obj cancels its animations. Call under the LVGL lock.
 * False means obj is not a glass surface. */
bool orb_glass_materialise(lv_obj_t *obj, bool visible);

/* Immediate 0..1 material amount for gesture-driven transitions/tests. Values
 * are clamped, NaN/Inf rejected. Cancels an in-flight material animation.
 * A missing glass surface has amount 0. */
bool orb_glass_set_amount(lv_obj_t *obj, float amount);
float orb_glass_get_amount(lv_obj_t *obj);

/* UI-thread cumulative geometry preparation, separate from backdrop filtering.
 * Reading this never resets another benchmark's counters. */
typedef struct {
    uint32_t builds, failures, entries, fallback_draws;
    uint64_t elapsed_us, max_build_us, circle_max_us;
    size_t bytes, peak_bytes;
} orb_glass_build_stats_t;
void orb_glass_build_stats(orb_glass_build_stats_t *out);

/* Deferred diagnostics for the default-off fused material candidate. Payload
 * is internal SRAM; stage elapsed/pixel counters are in the draw profiler.
 * Work counts are native modulo-32 atomics, widened here for diagnostics;
 * compare phase differences modulo32. Sample gauges between refreshes. */
typedef struct {
    uint64_t prepares, draws, fallbacks;
    size_t bytes, peak_bytes, budget_bytes;
    uint32_t maps;
} orb_glass_fused_stats_t;
void orb_glass_fused_get_stats(orb_glass_fused_stats_t *out);

/* For orb_display_set_band_guard: keeps every glass surface, which its blur
 * reads all of, whole in one render band. */
int32_t orb_glass_band_guard(int32_t y, int32_t y_last, int32_t max_rows);

/*
 * The accelerometer, in g on screen axes (+x right, +y up, +z out of the
 * screen), dt_s after the last call. Call at 30-60 Hz with lv_lock() held;
 * it moves the glass highlight and the parallax layers, redrawing only when
 * they visibly move.
 */
void orb_ui_set_gravity(float x, float y, float z, float dt_s);

/*
 * Where the light on the glass comes from: an LVGL angle (0 at 3 o'clock,
 * clockwise; 270 is up) and how hard it hits (0..1). orb_ui_set_gravity sets
 * it from the motion sensor; call it directly only to stage the light, e.g.
 * in a demo or a screenshot. Redraws the glass when the highlight would move.
 */
void orb_glass_set_light(float deg, float amount);

/*
 * Shifts obj with the device's tilt as if it lay `depth` behind the glass
 * (0: on it; 1: furthest back, up to `max_px`). Use a container: moving it
 * keeps the raster caches of what's inside. Uses the translate style.
 */
void orb_parallax_add(lv_obj_t *obj, float depth, int32_t max_px);
