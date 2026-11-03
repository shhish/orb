/*
 * Liquid Glass (docs/hig.md §2.2). Per frame, a glass control is LVGL's
 * backdrop blur of what's beneath, tinted as the blur writes it (an Orb patch
 * to LVGL: no extra pass), plus two 8-bit masks blended over it: a black one
 * holding the soft shadow outside and the darkened inner edge, and the lit
 * rim. The masks are drawn
 * from signed distance fields once per shape size (orb_glass_sdf.c) and kept
 * in slow memory; only the rim is rewritten, and only its own pixels, when the
 * light moves. LVGL's rounded fills, borders, arcs and box shadows cost far
 * more, as they rebuild their masks every frame.
 */
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

#include "orb_glass_sdf.h"
#include "orb_glass_masks.h"
#include "orb_glass_fused.h"
#include "orb_ui.h"
#include "orb_kit_motion.h"
#include "orb_ui_private.h"
#include "src/core/lv_obj_material_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"

#define MAX_GLASS 16
#define MASK_ENTRIES 32
#define MASK_BUDGET (640u * 1024u)
#define BLUR_RADIUS ORB_PT(16)   /* with LV_BLUR_QUALITY_SPEED: every third pixel, blurred */
#define SHADOW_SIGMA ORB_PT(4)    /* blur 8 pt */
#define SHADOW_OFF_Y ORB_PT(2)
#define SHADOW_PAD (5 * SHADOW_SIGMA / 2 + SHADOW_OFF_Y)   /* 2.5 sigma: the rest is under 1 % */
#define SHADOW_OPA 46             /* 18 %: enough over content, without a heavy dark fringe */
#define EDGE_OPA 46               /* 18 % black: the darkened edge of the 27 releases */

/* The masks for one size of one shape: `dark` is padded by SHADOW_PAD all round. */
typedef struct {
    int32_t w, h, rest_w, rest_h;
    unsigned refs;
    uint64_t used;
    size_t bytes;
    lv_draw_buf_t dark, rim;
    orb_rim_px_t *rim_px;
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    uint16_t *dark_spans, *rim_spans;
#endif
    int rim_n;
    float lit_deg, lit_amt;       /* the light the rim was drawn for */
} masks_t;

typedef struct {
    lv_obj_t *obj;
    orb_glass_kind_t kind;
    lv_color_t tint;
    int32_t w, h;                 /* at rest */
    float swell;                  /* 0 at rest, 1 fully pressed */
    float amount, target, start_amount;
    orb_delight_spring_t material;
    lv_obj_t *material_owner;     /* parent that holds the material clock */
    lv_opa_t rest_opa, rest_text_opa, tint_opa;
    bool material_started, material_reduced, kit_press;
    unsigned mask_failed;
    unsigned suspended_steps;
    uint8_t *backdrop;
    size_t backdrop_capacity;
    lv_area_t backdrop_area;
    bool backdrop_ready;
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    orb_glass_fused_t fused;
    bool fused_attempted, fused_ready, fused_drawn;
#endif
    masks_t *sizes[SWELL_STEPS];
} glass_t;

static glass_t *s_glass[MAX_GLASS];
static int s_count;
static orb_glass_preference_t s_preference = ORB_GLASS_PREFERENCE_DEFAULT;
static float s_light_deg = 270;   /* LVGL angles: 0 at 3 o'clock, clockwise; 270 is up */
static float s_light_amt = 0.5f;  /* 0..1, how far the device leans */
static orb_glass_build_stats_t s_build_stats;
static masks_t *s_masks;
static size_t s_mask_bytes;
#ifndef ESP_PLATFORM
static uint64_t s_mask_clock;
#endif
static uint64_t mask_touch(void)
{
#ifdef ESP_PLATFORM
    return orb_memory_cache_touch();
#else
    return ++s_mask_clock;
#endif
}
static lv_timer_t *s_prepare_timer;
static bool s_cache_suspended;

static void set_tint(glass_t *g);

static int32_t preference_blur(void)
{
    if (orb_ui_reduce_transparency()) return BLUR_RADIUS;
    return s_preference == ORB_GLASS_PREFERENCE_CLEAR ? BLUR_RADIUS * 2 / 3 :
        s_preference == ORB_GLASS_PREFERENCE_TINTED ? BLUR_RADIUS * 3 / 2 : BLUR_RADIUS;
}

bool orb_glass_set_preference(orb_glass_preference_t preference)
{
    if ((unsigned)preference > ORB_GLASS_PREFERENCE_TINTED) return false;
    if (preference == s_preference) return true;
    s_preference = preference;
    for (int i = 0; i < s_count; i++) {
        set_tint(s_glass[i]);
        lv_obj_invalidate(s_glass[i]->obj);
    }
    return true;
}

orb_glass_preference_t orb_glass_preference(void) { return s_preference; }

void orb_glass_build_stats(orb_glass_build_stats_t *out)
{
    if (!out) return;
    *out = s_build_stats;
    out->bytes = s_mask_bytes;
    out->entries = 0;
    if (s_masks) for (unsigned i = 0; i < MASK_ENTRIES; i++) out->entries += s_masks[i].bytes != 0;
}

static void masks_free(masks_t *m)
{
    orb_glass_masks_buffer_free(&m->dark);
    orb_glass_masks_buffer_free(&m->rim);
    orb_ui_free(m->rim_px);
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    orb_ui_free(m->dark_spans);
    orb_ui_free(m->rim_spans);
#endif
    memset(m, 0, sizeof(*m));
}

static void relight(masks_t *m)
{
    if (!m->rim.data || (m->lit_deg == s_light_deg && m->lit_amt == s_light_amt)) {
        return;
    }
    float lx, ly;
    orb_glass_masks_light_vector(s_light_deg, &lx, &ly);
    orb_sdf_rim_light(m->rim.data, m->w, m->rim_px, m->rim_n, lx, ly, s_light_amt);
    m->lit_deg = s_light_deg;
    m->lit_amt = s_light_amt;
}

static int swell_step(const glass_t *g)
{
    return orb_ui_reduce_motion() ? 0 : (int)(g->swell * (SWELL_STEPS - 1) + 0.5f);
}

/* Capsule radius=min(w,h)/2 and the AA/shadow recipe are fixed by HIG §2.2.
 * Rest size belongs in the key: the shadow caster stays still during swell.
 * Tint, object identity, location and IMU light do not change geometry. */
static masks_t *mask_lookup(int32_t w, int32_t h, int step)
{
    if (!s_masks) return NULL;
    int32_t mw, mh;
    orb_glass_masks_size(w, h, step, &mw, &mh);
    for (unsigned i = 0; i < MASK_ENTRIES; i++) {
        masks_t *m = &s_masks[i];
        if (m->rim.data && m->w == mw && m->h == mh && m->rest_w == w && m->rest_h == h) {
            m->used = mask_touch();
            return m;
        }
    }
    return NULL;
}

static void mask_drop(masks_t *m)
{
    s_mask_bytes -= m->bytes;
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_MASKS, m->bytes);
#endif
    masks_free(m);
}

void orb_glass_cache_clear(void)
{
    /* Only between completed refreshes. Live objects lease every referenced
     * image, so no deferred LVGL draw can observe an eviction. */
    if (!s_masks) return;
    for (unsigned i = 0; i < MASK_ENTRIES; i++) {
        if (!s_masks[i].refs) mask_drop(&s_masks[i]);
    }
    if (!s_count) {
#ifdef ESP_PLATFORM
        orb_memory_release(ORB_MEMORY_MASKS, sizeof(*s_masks) * MASK_ENTRIES);
#endif
        orb_ui_free(s_masks);
        s_masks = NULL;
        s_mask_bytes = 0;
    }
}

#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
/* At most two occupied spans per row keep the capsule's large transparent
 * centre and outside padding off the software blend path. Any remaining
 * small gaps stay in the spans and use ordinary zero-alpha handling. */
static void mask_spans(uint16_t *spans, const lv_draw_buf_t *mask)
{
    int32_t w = mask->header.w, h = mask->header.h;
    for (int32_t y = 0; y < h; y++, spans += 4) {
        const uint8_t *row = mask->data + y * mask->header.stride;
        int32_t first = 0, end = w;
        while (first < end && !row[first]) first++;
        while (end > first && !row[end - 1]) end--;
        int32_t gap_start = end, gap_end = end;
        for (int32_t x = first; x < end;) {
            if (row[x]) { x++; continue; }
            int32_t start = x;
            while (x < end && !row[x]) x++;
            if (x - start > gap_end - gap_start) { gap_start = start; gap_end = x; }
        }
        spans[0] = first; spans[1] = gap_start;
        spans[2] = gap_end; spans[3] = end;
    }
}
#define MASK_SPANS(m, kind) ((m)->kind##_spans)
#else
#define MASK_SPANS(m, kind) NULL
#endif

#ifdef ESP_PLATFORM
static masks_t *oldest_mask(void)
{
    masks_t *oldest = NULL;
    if(s_masks) for(unsigned i = 0; i < MASK_ENTRIES; i++) {
        masks_t *m = &s_masks[i];
        if(m->bytes && !m->refs && (!oldest || m->used < oldest->used)) oldest = m;
    }
    return oldest;
}
static uint64_t mask_oldest(void *ctx)
{
    (void)ctx;
    masks_t *m = oldest_mask();
    return m ? m->used : UINT64_MAX;
}
static bool mask_evict(void *ctx)
{
    (void)ctx;
    masks_t *m = oldest_mask();
    if(!m) return false;
    mask_drop(m);
    return true;
}
#endif

static masks_t *mask_build(int32_t w, int32_t h, int step)
{
    if (s_cache_suspended) return NULL;
    masks_t *m = mask_lookup(w, h, step);
    if (m) return m;
    if (!s_masks) {
#ifdef ESP_PLATFORM
        orb_memory_cache_register(ORB_MEMORY_MASKS, mask_oldest, mask_evict, NULL);
        if(!orb_memory_cache_admit(ORB_MEMORY_MASKS, sizeof(*s_masks) * MASK_ENTRIES)) return NULL;
#endif
        s_masks = orb_ui_alloc(sizeof(*s_masks) * MASK_ENTRIES);
        if (!s_masks) {
#ifdef ESP_PLATFORM
            orb_memory_release(ORB_MEMORY_MASKS, sizeof(*s_masks) * MASK_ENTRIES);
#endif
            s_build_stats.failures++; return NULL;
        }
        memset(s_masks, 0, sizeof(*s_masks) * MASK_ENTRIES);
        s_mask_bytes = sizeof(*s_masks) * MASK_ENTRIES;
    }
    int32_t mw, mh;
    orb_glass_masks_size(w, h, step, &mw, &mh);
    int32_t dw = mw + 2 * SHADOW_PAD, dh = mh + 2 * SHADOW_PAD;
    /* A capsule's <=2.5px AA rim is bounded by six times its box perimeter
     * half-sum, including tiny/odd shapes. No full SDF scan just to count it. */
    int capacity = 6 * (mw + mh);
    size_t bytes = (size_t)dw * dh + (size_t)mw * mh + sizeof(orb_rim_px_t) * capacity;
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    bytes += (size_t)(dh + mh) * 4 * sizeof(uint16_t);
#endif
    if (w <= 0 || h <= 0 || w > ORB_SCREEN_D || h > ORB_SCREEN_D || bytes > MASK_BUDGET) return NULL;
    while (s_mask_bytes + bytes > MASK_BUDGET) {
        masks_t *oldest = NULL;
        for (unsigned i = 0; i < MASK_ENTRIES; i++) {
            masks_t *candidate = &s_masks[i];
            if (candidate->bytes && !candidate->refs && (!oldest || candidate->used < oldest->used)) oldest = candidate;
        }
        if (!oldest) return NULL;
        mask_drop(oldest);
    }
    for (unsigned i = 0; i < MASK_ENTRIES; i++) {
        if (!s_masks[i].bytes) { m = &s_masks[i]; break; }
    }
    if (!m) {
        for (unsigned i = 0; i < MASK_ENTRIES; i++) {
            if (!s_masks[i].refs && (!m || s_masks[i].used < m->used)) m = &s_masks[i];
        }
        if (!m) return NULL;
        mask_drop(m);
    }
#ifdef ESP_PLATFORM
    if(!orb_memory_cache_admit(ORB_MEMORY_MASKS, bytes)) return NULL;
#endif
    uint64_t started = orb_glass_masks_clock_us();
    m->w = mw;
    m->h = mh;
    m->rest_w = w;
    m->rest_h = h;
    m->rim_px = orb_ui_alloc(sizeof(orb_rim_px_t) * capacity);
    if (!orb_glass_masks_buffer_alloc(&m->dark, dw, dh) || !orb_glass_masks_buffer_alloc(&m->rim, mw, mh) || !m->rim_px) goto fail;
    orb_capsule_t glass = {SHADOW_PAD, SHADOW_PAD, mw, mh};
    orb_capsule_t rest = {SHADOW_PAD + (mw - w) / 2, SHADOW_PAD + (mh - h) / 2, w, h};
    m->rim_n = orb_sdf_masks(m->dark.data, dw, dw, dh, m->rim_px,
                            capacity, rest, glass, SHADOW_OFF_Y, SHADOW_SIGMA, SHADOW_OPA, EDGE_OPA);
    if (m->rim_n < 0) goto fail;
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    m->dark_spans = orb_ui_alloc((size_t)dh * 4 * sizeof(uint16_t));
    m->rim_spans = orb_ui_alloc((size_t)mh * 4 * sizeof(uint16_t));
    if (!m->dark_spans || !m->rim_spans) goto fail;
    mask_spans(m->dark_spans, &m->dark);
    /* Light can change an AA rim pixel between zero and nonzero. Build from
     * every possible geometry pixel, never from this frame's lit alpha. */
    for (int i = 0; i < m->rim_n; i++) m->rim.data[m->rim_px[i].y * mw + m->rim_px[i].x] = 255;
    mask_spans(m->rim_spans, &m->rim);
#endif
    m->lit_deg = -1;
    relight(m);
    m->used = mask_touch();
    m->bytes = bytes;
    s_mask_bytes += bytes;
    s_build_stats.builds++;
    uint64_t elapsed = orb_glass_masks_clock_us() - started;
    s_build_stats.elapsed_us += elapsed;
    if (elapsed > s_build_stats.max_build_us) s_build_stats.max_build_us = elapsed;
    if (w == h && w >= 72 && w <= 100 && elapsed > s_build_stats.circle_max_us) s_build_stats.circle_max_us = elapsed;
    if (s_mask_bytes > s_build_stats.peak_bytes) s_build_stats.peak_bytes = s_mask_bytes;
    return m;
fail:
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_MASKS, bytes);
#endif
    masks_free(m);
    s_build_stats.failures++;
    s_build_stats.elapsed_us += orb_glass_masks_clock_us() - started;
    return NULL;
}

void orb_glass_cache_prewarm(void)
{
    /* Boot only, after the PSRAM allocator is installed and before UI/Wi-Fi.
     * Standard toolbar/app circles and common action capsules, HIG §§2/7. */
    static const int32_t shapes[][2] = {
        {ORB_CONTROL,ORB_CONTROL}, {ORB_PT(120),ORB_CONTROL},
        {72,72}, {88,88}, {96,96}, {100,100}, {140,72}, {188,72},
        {230,72}, {250,72},
    };
    for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) mask_build(shapes[i][0], shapes[i][1], 0);
    for (int step = 1; step < SWELL_STEPS; step++) {
        mask_build(72, 72, step);
        mask_build(ORB_CONTROL, ORB_CONTROL, step);
    }
}

static masks_t *masks_for(glass_t *g)
{
    masks_t *m = g->sizes[swell_step(g)];
    if (m) relight(m);
    return m;
}

/* A cache miss never builds inside construction or DRAW_MAIN. Use the HIG §6
 * opaque accessibility material until a bounded preparation job completes.
 * One shape per timer invocation prevents a new view's controls from turning
 * into a long synchronous group. Standard boot shapes avoid even this path. */
static void prepare_masks(lv_timer_t *timer)
{
    bool pending = false;
    for (int i = 0; i < s_count; i++) {
        glass_t *g = s_glass[i];
        int step = swell_step(g);
        if (g->sizes[step] || (g->mask_failed & (1u << step)) || !lv_obj_is_visible(g->obj)) continue;
        masks_t *m = mask_build(g->w, g->h, g->kit_press ? -step : step);
        if (m) {
            for (int j = 0; j < s_count; j++) {
                glass_t *other = s_glass[j];
                if (other->w == g->w && other->h == g->h && !other->sizes[step] &&
                    (!step || other->kit_press == g->kit_press)) {
                    other->sizes[step] = m;
                    m->refs++;
                    set_tint(other);
                    lv_obj_invalidate(other->obj);
                }
            }
        }
        if (!m) g->mask_failed |= 1u << step;
        pending = true;
        break;
    }
    if (!pending) lv_timer_pause(timer);
}

static void request_masks(glass_t *g)
{
    if (s_cache_suspended) return;
    int step = swell_step(g);
    if (!g->sizes[step]) {
        masks_t *m = mask_lookup(g->w, g->h, g->kit_press ? -step : step);
        if (m) { g->sizes[step] = m; m->refs++; }
        else if (!(g->mask_failed & (1u << step)) && lv_obj_is_visible(g->obj)) {
            if (s_prepare_timer) lv_timer_resume(s_prepare_timer);
        }
    }
}

void orb_glass_cache_suspend(void)
{
    if (s_cache_suspended) return;
    s_cache_suspended = true;
    if (s_prepare_timer) lv_timer_pause(s_prepare_timer);
    for (int i = 0; i < s_count; i++) {
        glass_t *g = s_glass[i];
        lv_obj_material_unregister(g->obj);
        g->suspended_steps = 0;
        for (int step = 0; step < SWELL_STEPS; step++) {
            if (!g->sizes[step]) continue;
            g->suspended_steps |= 1u << step;
            g->sizes[step]->refs--;
            g->sizes[step] = NULL;
        }
        g->mask_failed = 0;
    }
    orb_glass_cache_clear();
}

void orb_glass_cache_resume(void)
{
    if (!s_cache_suspended) return;
    s_cache_suspended = false;
    for (int i = 0; i < s_count; i++) {
        glass_t *g = s_glass[i];
        unsigned steps = g->suspended_steps;
        if (lv_obj_is_visible(g->obj)) steps |= 1u << swell_step(g);
        g->suspended_steps = 0;
        for (int step = 0; step < SWELL_STEPS; step++) {
            if (!(steps & (1u << step))) continue;
            g->sizes[step] = mask_build(g->w, g->h, g->kit_press ? -step : step);
            if (g->sizes[step]) g->sizes[step]->refs++;
        }
        set_tint(g);
        if (lv_obj_is_visible(g->obj)) {
            lv_obj_material_register(g->obj, g->w + 2 * (int32_t)(g->w * SWELL / 2),
                g->h + 2 * (int32_t)(g->h * SWELL / 2), SHADOW_PAD, orb_ui_alloc, orb_ui_free);
            lv_obj_material_set_active(g->obj, !(g->material_reduced && g->amount < 1));
        }
    }
}

static glass_t *find(lv_obj_t *obj)
{
    for (int i = 0; i < s_count; i++) {
        if (s_glass[i]->obj == obj) {
            return s_glass[i];
        }
    }
    return NULL;
}

/* The rectangle the glass is drawn in: its box, grown while pressed. */
static void glass_area(lv_obj_t *obj, lv_area_t *a)
{
    lv_obj_get_coords(obj, a);
    lv_area_increase(a, lv_obj_get_style_transform_width(obj, LV_PART_MAIN),
                     lv_obj_get_style_transform_height(obj, LV_PART_MAIN));
}

#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
static void fused_frame(lv_event_t *e)
{
    glass_t *g = lv_event_get_user_data(e);
    g->fused_attempted = g->fused_ready = g->fused_drawn = false;
    /* Bench/navigation preserves inactive objects. Their reduced maps must
     * not consume the active screen's internal material admission budget. */
    if (lv_obj_get_screen(g->obj) != lv_display_get_screen_active(lv_obj_get_display(g->obj)))
        orb_glass_fused_release(&g->fused);
}

static bool fused_draw(lv_draw_task_t *task, const lv_draw_blur_dsc_t *dsc, const lv_area_t *coords)
{
    glass_t *g = find(dsc->base.obj);
    if (!g || !g->fused_ready) return false;
    masks_t *m = g->sizes[swell_step(g)];
    if (!m) return false;
    bool drawn = orb_glass_fused_draw(&g->fused, task, dsc, coords, &m->dark, &m->rim,
                                      SHADOW_PAD, (lv_opa_t)(g->amount * LV_OPA_COVER));
    g->fused_drawn = drawn;
    return drawn;
}

static void prepare_fused(glass_t *g, lv_layer_t *layer)
{
    g->fused_drawn = false;
    if (g->fused_attempted) return;
    lv_area_t shape, visible;
    glass_area(g->obj, &shape);
    if (!lv_area_intersect(&visible, &shape, &layer->_clip_area) ||
        !lv_area_intersect(&visible, &visible, &layer->buf_area)) return;
    g->fused_attempted = true;
    if (orb_ui_reduce_transparency() || g->material_reduced || !masks_for(g) ||
        (layer->color_format != LV_COLOR_FORMAT_RGB565 && layer->color_format != LV_COLOR_FORMAT_RGB565_SWAPPED)) {
        orb_glass_fused_note_fallback();
        return;
    }
    masks_t *m = masks_for(g);
    /* A press's independently rounded transform can lie between shared SDF
     * sizes. Preserve the reference material for those transient geometries. */
    if (lv_area_get_width(&shape) != m->w || lv_area_get_height(&shape) != m->h) {
        orb_glass_fused_note_fallback();
        return;
    }
    g->fused_ready = orb_glass_fused_prepare(&g->fused, g->obj, &shape, (int32_t)(preference_blur() * g->amount));
    if (g->fused_ready) lv_obj_material_unregister(g->obj);
}
#endif

static void draw_mask(lv_layer_t *layer, lv_obj_t *obj, const lv_draw_buf_t *mask, const lv_area_t *a,
                      lv_color_t color, lv_opa_t opa, const uint16_t *spans)
{
    if (!mask->data || opa <= LV_OPA_MIN) {
        return;
    }
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = mask;
#if defined(CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS) && CONFIG_ORB_DISPLAY_GLASS_SPARSE_MASKS
    d.a8_spans = spans;
#else
    LV_UNUSED(spans);
#endif
    d.recolor = color;
    /* RGB565 has twice as many green levels: independent rounding of a black
     *mask can leave a green-only pixel. Keep just neutral shadow inputs on
     *neutral levels; the white rim and colored backdrop retain normal blending. */
    d.rgb565_neutral_shadow = lv_color_to_u16(color) == 0;
    d.opa = opa;
    d.base.obj = obj;
    lv_draw_image(layer, &d, a);
}

/* HIG §6: Reduce Motion cross-fades a fixed material. LVGL's ordinary
 * opacity does not attenuate backdrop blur, so preserve only this control's
 * actual backdrop, then mix its complete rendered pixels back over it. The
 * small buffer lives in the UI allocator (PSRAM) for the transition only. */
static void finish_layer(lv_layer_t *layer)
{
    /* wait_for_finish alone only joins work already dispatched; pending
     * content and blur tasks also have to be drained before touching pixels. */
    while (layer->draw_task_head) {
        lv_draw_dispatch();
        lv_draw_wait_for_finish();
    }
}

static void capture_backdrop(lv_event_t *e)
{
    glass_t *g = find(lv_event_get_target_obj(e));
    if (!g) return;
    /* Hidden system/setup objects do not consume preparation jobs. Their
     * first visible refresh schedules an unfamiliar shape for the next tick. */
    request_masks(g);
    lv_layer_t *layer = lv_event_get_layer(e);
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    prepare_fused(g, layer);
    if (g->fused_ready) return;
#endif
    /* Inactive screens release their bounded material leases. Re-admit the
     * preserved shell on its first draw after a benchmark/navigation return. */
    if (lv_obj_material_register(g->obj, g->w + 2 * (int32_t)(g->w * SWELL / 2),
                                 g->h + 2 * (int32_t)(g->h * SWELL / 2),
                                 SHADOW_PAD, orb_ui_alloc, orb_ui_free)) {
        lv_obj_material_set_active(g->obj, !(g->material_reduced && g->amount < 1));
        lv_obj_material_restore(layer, g->obj);
    }
    if (!g->material_reduced || !g->backdrop || g->amount >= 1) return;
    lv_area_t area;
    glass_area(g->obj, &area);
    g->backdrop_ready = false;
    if (!lv_area_intersect(&area, &area, &layer->_clip_area) ||
        !lv_area_intersect(&area, &area, &layer->buf_area)) return;
    unsigned bytes = lv_color_format_get_size(layer->color_format);
    size_t row_bytes = (size_t)lv_area_get_width(&area) * bytes;
    if (!bytes || row_bytes * lv_area_get_height(&area) > g->backdrop_capacity) return;
    finish_layer(layer);
    for (int32_t y = area.y1; y <= area.y2; y++) {
        const void *source = lv_draw_layer_go_to_xy(layer, area.x1 - layer->buf_area.x1, y - layer->buf_area.y1);
        memcpy(g->backdrop + (size_t)(y - area.y1) * row_bytes, source, row_bytes);
    }
    g->backdrop_area = area;
    g->backdrop_ready = true;
}

static void crossfade_backdrop(glass_t *g, lv_layer_t *layer)
{
    if (!g->backdrop_ready) return;
    finish_layer(layer);
    const lv_area_t *area = &g->backdrop_area;
    unsigned bytes = lv_color_format_get_size(layer->color_format);
    size_t row_bytes = (size_t)lv_area_get_width(area) * bytes;
    unsigned alpha = (unsigned)(g->amount * 255);
    for (int32_t y = area->y1; y <= area->y2; y++) {
        uint8_t *result = lv_draw_layer_go_to_xy(layer, area->x1 - layer->buf_area.x1, y - layer->buf_area.y1);
        const uint8_t *backdrop = g->backdrop + (size_t)(y - area->y1) * row_bytes;
        if (layer->color_format == LV_COLOR_FORMAT_RGB565 ||
            layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED) {
            bool swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
            for (size_t x = 0; x < row_bytes; x += 2) {
                uint16_t src, dst;
                memcpy(&src, backdrop + x, 2);
                memcpy(&dst, result + x, 2);
                if (swapped) {
                    src = (src >> 8) | (src << 8);
                    dst = (dst >> 8) | (dst << 8);
                }
                unsigned r = (((dst >> 11) * alpha + (src >> 11) * (255 - alpha)) + 127) / 255;
                unsigned green = ((((dst >> 5) & 63) * alpha + ((src >> 5) & 63) * (255 - alpha)) + 127) / 255;
                unsigned b = (((dst & 31) * alpha + (src & 31) * (255 - alpha)) + 127) / 255;
                dst = (r << 11) | (green << 5) | b;
                if (swapped) dst = (dst >> 8) | (dst << 8);
                memcpy(result + x, &dst, 2);
            }
        } else {
            for (size_t x = 0; x < row_bytes; x++) {
                result[x] = (result[x] * alpha + backdrop[x] * (255 - alpha) + 127) / 255;
            }
        }
    }
    g->backdrop_ready = false;
}

static void on_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    glass_t *g = find(obj);
    if (!g) {
        return;
    }
    /* Retain the exact material before shadow/rim/labels; HIG §2.2 still draws
     * real backdrop-dependent glass and screen-anchored dither unchanged. */
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    /* Join the blur before deciding whether its edge/rim were fused. */
    if (g->fused_ready) finish_layer(lv_event_get_layer(e));
    if (!g->fused_drawn)
#endif
        lv_obj_material_capture_base(lv_event_get_layer(e), obj);
    /* After LVGL's tinted blur (DRAW_MAIN): shadow and darkened edge, then the lit rim. */
    masks_t *m = masks_for(g);
    lv_layer_t *layer = lv_event_get_layer(e);
    crossfade_backdrop(g, layer);
    if (!m) {
        s_build_stats.fallback_draws++;
        return;
    }
    lv_area_t a, ma, da;
    glass_area(obj, &a);
    lv_area_set(&ma, 0, 0, m->w - 1, m->h - 1);
    lv_area_align(&a, &ma, LV_ALIGN_CENTER, 0, 0);
    da = ma;
    lv_area_increase(&da, SHADOW_PAD, SHADOW_PAD);
    /* HIG §2.2: the edge, shadow and highlight develop with the material.
     * Reduced Motion cross-fades its complete rendered pixels over the backdrop. */
    lv_opa_t amount = (lv_opa_t)(g->amount * LV_OPA_COVER);
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    if (g->fused_drawn) {
        /* The inner dark edge and relit rim were already composed. Only the
         * external shadow remains; clip it around the material rectangle. */
        lv_area_t saved = layer->_clip_area;
        const lv_area_t outside[] = {
            {da.x1, da.y1, da.x2, ma.y1 - 1}, {da.x1, ma.y2 + 1, da.x2, da.y2},
            {da.x1, ma.y1, ma.x1 - 1, ma.y2}, {ma.x2 + 1, ma.y1, da.x2, ma.y2},
        };
        for (unsigned i = 0; i < 4; i++) {
            if (lv_area_intersect(&layer->_clip_area, &saved, &outside[i]))
                draw_mask(layer, obj, &m->dark, &da, lv_color_black(), amount, MASK_SPANS(m, dark));
        }
        layer->_clip_area = saved;
        return;
    }
#endif
    draw_mask(layer, obj, &m->dark, &da, lv_color_black(), amount, MASK_SPANS(m, dark));
    draw_mask(layer, obj, &m->rim, &ma, lv_color_white(), amount, MASK_SPANS(m, rim));
}

/* The tint, which LVGL's blur mixes in: brighter while pressed. With Reduce
 * Transparency it is nearly opaque, as Apple's frostier material. */
static void set_tint(glass_t *g)
{
    request_masks(g);
    bool missing = !g->sizes[swell_step(g)];
    bool opaque = orb_ui_reduce_transparency() || missing;
    lv_opa_t opa;
    if (g->kind == ORB_GLASS_TINTED) {
        /* Low tints over black use accent ink (Design 1.0). Preserve the
         * established 70..90% recipe, and brighten lighter tints only within
         * the same bounded material instead of jumping to a solid accent. */
        float strength = fmaxf(0, (float)(g->tint_opa - LV_OPA_70) / (LV_OPA_90 - LV_OPA_70));
        float pressed = g->tint_opa < LV_OPA_70 ? g->tint_opa + LV_OPA_20 :
            LV_OPA_90 + (LV_OPA_COVER * 95 / 100 - LV_OPA_90) * strength;
        opa = (lv_opa_t)(opaque ? LV_OPA_COVER : g->tint_opa + (pressed - g->tint_opa) * g->swell);
    } else {
        opa = (lv_opa_t)(opaque ? LV_OPA_90 : LV_OPA_20 + (LV_OPA_40 - LV_OPA_20) * g->swell);
    }
    /* HIG §6: clarity changes the existing material, never adds another tint
     * or removes adaptive contrast. Opaque accessibility remains identical. */
    if (!opaque && g->kind == ORB_GLASS_REGULAR) {
        if (s_preference == ORB_GLASS_PREFERENCE_CLEAR) opa = opa * 2 / 3;
        else if (s_preference == ORB_GLASS_PREFERENCE_TINTED)
            opa = LV_MIN(LV_OPA_90, opa + LV_OPA_20);
    }
    /* HIG §6: Reduce Transparency replaces glass with an opaque dark
     * surface; it must not turn a low-alpha tint into a bright solid fill. */
    bool dark_surface = opaque && (g->kind == ORB_GLASS_REGULAR || g->tint_opa < LV_OPA_70);
    lv_color_t colour = dark_surface ? ORB_GRAY6 : g->tint;
    if (dark_surface && g->kit_press) {
        /* Reduce Transparency retains a solid surface, with the same bounded
         * press brightening instead of introducing translucent pixels. */
        colour = lv_color_mix(ORB_GRAY5, ORB_GRAY6, (lv_opa_t)(g->swell * LV_OPA_COVER));
    }
    lv_obj_set_style_bg_color(g->obj, colour, 0);
    float amount = g->material_reduced ? 1.0f : g->amount;
    lv_obj_set_style_bg_opa(g->obj, (lv_opa_t)(opa * amount), 0);
    lv_obj_set_style_blur_radius(g->obj, missing ? 0 : (int32_t)(preference_blur() * amount), 0);
    /* A cold/memory-denied SDF uses an ordinary opaque fill. Backdrop blur
     * consumes that fill as tint, but a zero-radius blur emits no pixels;
     * keeping it enabled would leave only the control’s label visible. */
    lv_obj_set_style_blur_backdrop(g->obj, !missing, 0);
}

bool orb_glass_set_tint_opacity(lv_obj_t *obj, lv_opa_t resting_opa)
{
    glass_t *g = find(obj);
    if (!g || g->kind != ORB_GLASS_TINTED || resting_opa < 61 || resting_opa > LV_OPA_90)
        return false;
    if (g->tint_opa == resting_opa) return true;
    /* HIG §2.2: tune the existing backdrop tint; retain blur, lensing,
     * relit rim and press geometry instead of painting an opaque fill. */
    g->tint_opa = resting_opa;
    set_tint(g);
    return true;
}

static void apply_amount(glass_t *g)
{
    /* Reduced Motion mixes a complete material with its real backdrop after
     * DRAW_MAIN. Do not admit that transient external mix into the base key. */
    lv_obj_material_set_active(g->obj, !(g->material_reduced && g->amount < 1));
    set_tint(g);
    /* Only labels follow amount in the normal path; never alpha-fade the
     * complete glass. HIG §6 explicitly permits a 150 ms cross-fade for RM. */
    lv_obj_set_style_opa(g->obj, g->rest_opa, 0);
    lv_obj_set_style_text_opa(g->obj, (lv_opa_t)(g->rest_text_opa * g->amount), 0);
    lv_obj_set_hidden(g->obj, g->amount <= 0);
    lv_obj_invalidate(g->obj);
}

static bool material_frame(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    glass_t *g = context;
    if (finish || elapsed >= (g->material_reduced ? 150 : 550)) {
        g->amount = g->target;
        orb_delight_spring_reset(&g->material, g->amount);
        orb_ui_free(g->backdrop);
        g->backdrop = NULL;
        g->backdrop_capacity = 0;
        g->backdrop_ready = false;
        apply_amount(g);
        return false;
    }
    if (g->material_reduced) {
        float progress = LV_CLAMP(0.0f, elapsed / 150.0f, 1.0f);
        g->amount = g->start_amount + (g->target - g->start_amount) * progress;
    } else {
        orb_delight_spring_step(&g->material, dt / 1000.f, .55f, 0);
        g->amount = LV_CLAMP(0.0f, g->material.value, 1.0f);
    }
    apply_amount(g);
    return true;
}

/* The clock slot lives on the parent at start time; cancel there even if
 * the glass has since been reparented, so the slot never outlives g. */
static void material_cancel(glass_t *g)
{
    if (!g->material_owner) return;
    orb_kit_motion_cancel(g->material_owner, g, false);
    g->material_owner = NULL;
}

bool orb_glass_set_amount(lv_obj_t *obj, float amount)
{
    glass_t *g = find(obj);
    if (!g || !isfinite(amount)) return false;
    material_cancel(g);
    g->material_started = true;
    orb_ui_free(g->backdrop);
    g->backdrop = NULL;
    g->backdrop_capacity = 0;
    g->backdrop_ready = false;
    g->material_reduced = false;
    g->amount = LV_CLAMP(0.0f, amount, 1.0f);
    orb_delight_spring_reset(&g->material, g->amount);
    apply_amount(g);
    return true;
}

float orb_glass_get_amount(lv_obj_t *obj)
{
    glass_t *g = find(obj);
    return g ? g->amount : 0;
}

bool orb_glass_materialise(lv_obj_t *obj, bool visible)
{
    glass_t *g = find(obj);
    if (!g) return false;
    /* The first entrance starts absent; apply() itself remains immediate for
     * existing callers. Subsequent calls reverse from the current material. */
    if (!g->material_started) {
        g->rest_opa = lv_obj_get_style_opa(obj, 0);
        g->rest_text_opa = lv_obj_get_style_text_opa(obj, 0);
        if (visible) g->amount = 0;
        g->material_started = true;
    }
    material_cancel(g);
    g->material_reduced = orb_ui_reduce_motion();
    g->target = visible ? 1.0f : 0.0f;
    g->start_amount = g->amount;
    g->material.value = g->amount;
    orb_delight_spring_target(&g->material, g->target);
    if (g->amount == g->target && g->material.velocity == 0) {
        orb_ui_free(g->backdrop);
        g->backdrop = NULL;
        g->backdrop_capacity = 0;
        g->backdrop_ready = false;
        apply_amount(g);
        return true;
    }
    if (g->material_reduced) {
        orb_delight_spring_reset(&g->material, g->amount);
        if (!g->backdrop) {
            int32_t w = g->w + 2 * lv_obj_get_style_transform_width(obj, 0);
            int32_t h = g->h + 2 * lv_obj_get_style_transform_height(obj, 0);
            /* A transformed ancestor can introduce an ARGB layer even on
             * the RGB565 panel. Four bytes/pixel bounds both cases. */
            g->backdrop_capacity = (size_t)w * h * sizeof(lv_color32_t);
            g->backdrop = orb_ui_alloc(g->backdrop_capacity);
        }
        /* Low-memory fallback is an immediate fixed material, never an
         * animated blur or an invisible control with an active hit target. */
        if (!g->backdrop) g->amount = g->target;
    } else {
        orb_ui_free(g->backdrop);
        g->backdrop = NULL;
        g->backdrop_capacity = 0;
    }
    apply_amount(g);
    if (g->amount == g->target && g->material.velocity == 0) return true;
    /* The child is intentionally hidden at material amount zero. Its live
     * parent owns the shared clock; deletion cancels this exact property. */
    g->material_owner = lv_obj_get_parent(obj);
    if (!orb_kit_motion_start_fade(g->material_owner, g, material_frame, g)) g->material_owner = NULL;
    return true;
}

static void on_ext_draw_size(lv_event_t *e)
{
    lv_event_set_ext_draw_size(e, SHADOW_PAD);
}

static void swell_exec(void *var, int32_t v)
{
    lv_obj_t *obj = var;
    glass_t *g = find(obj);
    if (!g) {
        return;
    }
    g->swell = v / 1024.0f;
    set_tint(g);
    /* Reduce Motion: the brightening alone, no change of size. */
    float grow = orb_ui_reduce_motion() ? 0 : SWELL * g->swell / 2;
    lv_obj_set_style_transform_width(obj, (int32_t)(g->w * grow), 0);
    lv_obj_set_style_transform_height(obj, (int32_t)(g->h * grow), 0);
}

/* Pressed: the glass swells slightly and brightens, as Apple's controls do. */
static void on_press(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    glass_t *g = find(obj);
    if (!g || g->kit_press) {
        return;
    }
    bool down = lv_event_get_code(e) == LV_EVENT_PRESSED;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, swell_exec);
    lv_anim_set_values(&a, (int32_t)(g->swell * 1024), down ? 1024 : 0);
    lv_anim_set_duration(&a, down ? 120 : 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

bool orb_glass_kit_press(lv_obj_t *obj, int32_t progress)
{
    glass_t *g = find(obj);
    if (!g) return false;
    if (!g->kit_press) {
        lv_anim_delete(obj, swell_exec);
        g->kit_press = true;
        /* Native contraction keeps blur in the real backdrop layer. Scaling
         * a glass subtree through an offscreen LVGL layer loses that backdrop
         * and turns the pressed material black. Prepare the four bounded
         * negative SDF sizes now, never during animation or drawing. */
        for (int k = 0; k < SWELL_STEPS; k++) {
            if (g->sizes[k]) g->sizes[k]->refs--;
            g->sizes[k] = mask_build(g->w, g->h, -k);
            if (g->sizes[k]) g->sizes[k]->refs++;
        }
    }
    g->swell = LV_CLAMP(0, progress, 1024) / 1024.f;
    int32_t w, h;
    orb_glass_masks_size(g->w, g->h, -swell_step(g), &w, &h);
    lv_obj_set_style_transform_width(obj, (w - g->w) / 2, 0);
    lv_obj_set_style_transform_height(obj, (h - g->h) / 2, 0);
    set_tint(g);
    lv_obj_invalidate(obj);
    return true;
}

void orb_glass_kit_resize(lv_obj_t *obj)
{
    glass_t *g = find(obj);
    if (!g) return;
    lv_obj_update_layout(obj);
    int32_t w = lv_obj_get_width(obj), h = lv_obj_get_height(obj);
    if (w == g->w && h == g->h) return;
    for (int k = 0; k < SWELL_STEPS; k++) {
        if (g->sizes[k]) g->sizes[k]->refs--;
        g->sizes[k] = g->kit_press ? mask_build(w, h, -k) : mask_lookup(w, h, k);
        if (g->sizes[k]) g->sizes[k]->refs++;
    }
    g->w = w;
    g->h = h;
    g->mask_failed = 0;
    if (g->kit_press) orb_glass_kit_press(obj, (int32_t)lroundf(g->swell * 1024));
    else request_masks(g);
    if (!s_cache_suspended) lv_obj_material_register(obj, w, h, SHADOW_PAD, orb_ui_alloc, orb_ui_free);
}

static void on_delete(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    for (int i = 0; i < s_count; i++) {
        glass_t *g = s_glass[i];
        if (g->obj == obj) {
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
            lv_display_remove_event_cb_with_user_data(lv_obj_get_display(obj), fused_frame, g);
            orb_glass_fused_release(&g->fused);
#endif
            lv_obj_material_unregister(obj);
            lv_anim_delete(obj, swell_exec);
            material_cancel(g);
            for (int k = 0; k < SWELL_STEPS; k++) {
                if (g->sizes[k]) g->sizes[k]->refs--;
            }
            orb_ui_free(g->backdrop);
            orb_ui_free(g);
            s_glass[i] = s_glass[--s_count];
            if (!s_count && s_prepare_timer) {
                lv_timer_delete(s_prepare_timer);
                s_prepare_timer = NULL;
            }
            return;
        }
    }
}

void orb_glass_apply(lv_obj_t *obj, orb_glass_kind_t kind, lv_color_t tint)
{
    /* Styling a shared control must replace its material, not stack a second
     * draw/press handler over the first (Design 1.0 neutral and low tints). */
    glass_t *g = find(obj);
    if (g) {
        g->kind = kind;
        g->tint = kind == ORB_GLASS_TINTED ? tint : orb_color_from_rgb(ORB_RGB_GLASS_REGULAR_TINT);
        g->tint_opa = LV_OPA_70;
        set_tint(g);
        return;
    }
    g = orb_ui_alloc(sizeof(glass_t));
    if (!g || s_count >= MAX_GLASS) {
        orb_ui_free(g);
        return;
    }
    memset(g, 0, sizeof(*g));
    lv_obj_update_layout(obj);
    g->obj = obj;
    g->kind = kind;
    g->tint = kind == ORB_GLASS_TINTED ? tint : orb_color_from_rgb(ORB_RGB_GLASS_REGULAR_TINT);
    g->tint_opa = LV_OPA_70;
    g->w = lv_obj_get_width(obj);
    g->h = lv_obj_get_height(obj);
    g->amount = 1;
    orb_delight_spring_reset(&g->material, 1);
    orb_kit_motion_prepare(lv_obj_get_parent(obj));
    g->rest_opa = lv_obj_get_style_opa(obj, 0);
    g->rest_text_opa = lv_obj_get_style_text_opa(obj, 0);
    s_glass[s_count++] = g;
    /* Allocate the scheduler before any rendering, even when this control is
     * constructed on an inactive screen. DRAW_MAIN may only resume it. */
    if (!s_prepare_timer) {
        s_prepare_timer = lv_timer_create(prepare_masks, 16, NULL);
        if (s_prepare_timer) lv_timer_pause(s_prepare_timer);
    }
#if defined(CONFIG_ORB_RENDER_FUSED_GLASS) && CONFIG_ORB_RENDER_FUSED_GLASS
    lv_display_add_event_cb(lv_obj_get_display(obj), fused_frame, LV_EVENT_REFR_START, g);
    lv_draw_sw_blur_set_fused_cb(fused_draw);
#endif
    /* Lease boot-prepared/shared sizes without doing geometry on navigation. */
    for (int k = 0; k < SWELL_STEPS; k++) {
        g->sizes[k] = mask_lookup(g->w, g->h, k);
        if (g->sizes[k]) g->sizes[k]->refs++;
    }

    /* LVGL draws the backdrop blur with the tint; everything else comes from the masks. */
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    set_tint(g);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_blur_quality(obj, LV_BLUR_QUALITY_SPEED, 0);

    lv_obj_add_event_cb(obj, capture_backdrop, LV_EVENT_DRAW_MAIN_BEGIN, NULL);
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(obj, on_ext_draw_size, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_add_event_cb(obj, on_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, on_press, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, on_press, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(obj, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_refresh_ext_draw_size(obj);
    /* Admit all independently varying transform rectangles, not just the four
     * SDF sizes. One bounded allocation per control, never per animation tick. */
    if (!s_cache_suspended) lv_obj_material_register(obj, g->w + 2 * (int32_t)(g->w * SWELL / 2),
                             g->h + 2 * (int32_t)(g->h * SWELL / 2),
                             SHADOW_PAD, orb_ui_alloc, orb_ui_free);
}

int32_t orb_glass_band_guard(int32_t y, int32_t y_last, int32_t max_rows)
{
    /* The blur reads only inside the glass, but all of it: keep the glass,
     * grown as far as a press swells it, in one band. */
    int32_t end = y + max_rows - 1;
    for (int i = 0; i < s_count; i++) {
        if (!lv_obj_is_visible(s_glass[i]->obj)) {
            continue;
        }
        lv_area_t c;
        lv_obj_get_coords(s_glass[i]->obj, &c);
        int32_t grow = (int32_t)(s_glass[i]->h * SWELL / 2) + 2;
        int32_t g1 = c.y1 - grow, g2 = c.y2 + grow;
        if (g1 > y && g1 <= end && g2 > end) {
            end = g1 - 1;
        }
    }
    return end - y + 1;
}

void orb_glass_set_light(float deg, float amount)
{
    /* Below these the highlight wouldn't visibly move: don't redraw for noise. */
    float turn = fabsf(fmodf(deg - s_light_deg + 540, 360) - 180);
    /* Lying flat, atan2 of tiny accelerometer noise can swing the angle by
     * sixty degrees. At this strength its direction is not visible; compare
     * angle only once either the old or new highlight has perceptible weight. */
    bool faint = amount < 0.03f && s_light_amt < 0.03f;
    if ((turn < 2 || faint) && fabsf(amount - s_light_amt) < 0.03f) {
        return;
    }
    s_light_deg = deg;
    s_light_amt = amount;
    for (int i = 0; i < s_count; i++) {
        masks_for(s_glass[i]);
        lv_obj_invalidate(s_glass[i]->obj);
    }
}
