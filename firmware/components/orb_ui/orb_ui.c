#include "orb_ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#else
#include <time.h>
#endif

#include "orb_ui_private.h"
#include "src/core/lv_obj_scroll_private.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_obj_raster_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/misc/lv_area_private.h"
#include "src/misc/lv_text_private.h"

static orb_ui_config_t s_cfg;
static bool s_reduce_transparency;
static bool s_scroll_diagnostics_active;
static orb_ui_scroll_diagnostics_t s_scroll_diagnostics;
static lv_display_t *s_scroll_display;
static bool s_scroll_in_refresh;

static void scroll_diagnostics_display(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) s_scroll_in_refresh = true;
    else if (code == LV_EVENT_REFR_READY) s_scroll_in_refresh = false;
    else if (code == LV_EVENT_DELETE) { s_scroll_display = NULL; s_scroll_in_refresh = false; }
}

void orb_ui_scroll_diagnostics_begin(void)
{
    orb_ui_scroll_diagnostics_end();
    memset(&s_scroll_diagnostics, 0, sizeof(s_scroll_diagnostics));
    s_scroll_display = lv_display_get_default();
    s_scroll_diagnostics.refresh_phase_available = s_scroll_display &&
        lv_display_add_event_cb(s_scroll_display, scroll_diagnostics_display, LV_EVENT_ALL, NULL);
    s_scroll_diagnostics_active = true;
}

void orb_ui_scroll_diagnostics_end(void)
{
    if (s_scroll_display)
        lv_display_remove_event_cb_with_user_data(s_scroll_display, scroll_diagnostics_display, NULL);
    s_scroll_display = NULL;
    s_scroll_in_refresh = false;
    s_scroll_diagnostics_active = false;
}
void orb_ui_scroll_diagnostics_snapshot(orb_ui_scroll_diagnostics_t *out) { *out = s_scroll_diagnostics; }

uint64_t orb_ui_scroll_diagnostics_clock(void)
{
    if (!s_scroll_diagnostics_active) return UINT64_MAX;
#ifdef ESP_PLATFORM
    return esp_timer_get_time();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
#endif
}

static void scroll_stage_record(uint64_t started, orb_ui_scroll_stage_t *stage)
{
    if (started == UINT64_MAX || !s_scroll_diagnostics_active) return;
    uint64_t elapsed = orb_ui_scroll_diagnostics_clock() - started;
    stage->calls++;
    stage->us += elapsed;
    if (s_scroll_diagnostics.refresh_phase_available && !s_scroll_in_refresh) stage->outside_us += elapsed;
    if (elapsed > stage->max_us) stage->max_us = LV_MIN(UINT32_MAX, elapsed);
}

void orb_ui_scroll_readable_record(uint64_t started, unsigned rows, unsigned writes, unsigned redundant,
                                   unsigned font_changes, bool layout)
{
    if (started == UINT64_MAX || !s_scroll_diagnostics_active) return;
    uint64_t elapsed = orb_ui_scroll_diagnostics_clock() - started;
    s_scroll_diagnostics.readable_calls++;
    s_scroll_diagnostics.readable_us += elapsed;
    if (elapsed > s_scroll_diagnostics.readable_max_us) s_scroll_diagnostics.readable_max_us = elapsed;
    s_scroll_diagnostics.readable_rows += rows;
    s_scroll_diagnostics.readable_layout_calls += layout;
    s_scroll_diagnostics.style_writes += writes;
    s_scroll_diagnostics.redundant_style_writes += redundant;
    s_scroll_diagnostics.font_changes += font_changes;
}

void orb_ui_init(const orb_ui_config_t *cfg)
{
    lv_obj_raster_rows_set_font_size_cb(orb_fonts_nominal_px);
    if (cfg) {
        s_cfg = *cfg;
    }
}

void *orb_ui_alloc(size_t size)
{
    return s_cfg.alloc ? s_cfg.alloc(size) : lv_malloc(size);
}

void orb_ui_free(void *p)
{
    if (!p) {
        return;
    }
    if (s_cfg.free) {
        s_cfg.free(p);
    } else {
        lv_free(p);
    }
}

void orb_ui_set_reduce_transparency(bool on)
{
    s_reduce_transparency = on;
}

bool orb_ui_reduce_transparency(void)
{
    return s_reduce_transparency;
}

static int32_t chord_at(int32_t y, float r)
{
    float dy = fabsf(y - (float)ORB_SCREEN_R);
    return dy >= r ? 0 : (int32_t)(2 * sqrtf(r * r - dy * dy));
}

int32_t orb_chord_width(int32_t y1, int32_t y2, int32_t margin)
{
    /* A line box's top edge is row y1's top, its bottom edge row y2's bottom;
     * the chord shrinks away from the middle, so one of them is the narrowest. */
    float r = (float)(ORB_SCREEN_R - margin);
    int32_t top = chord_at(y1, r), bottom = chord_at(y2 + 1, r);
    return top < bottom ? top : bottom;
}


/* A half-pixel center keeps odd/even row sizes symmetric about the panel. */
#define ROUND_CENTERS (ORB_SCREEN_D * 2 + 1)
#define ROUND_CHORD_STEP 4
#define ROUND_CHORD_BUCKETS (ORB_SCREEN_R / ROUND_CHORD_STEP + 2)

typedef struct round_shape {
    struct round_shape *next;
    int32_t width, height;
    bool used;
    uint16_t scale[ROUND_CENTERS];
    uint8_t opa[ROUND_CENTERS];
    uint16_t chord[ROUND_CHORD_BUCKETS];
} round_shape_t;

typedef struct round_chord_row round_chord_row_t;

typedef struct round_scroll_guard {
    struct round_scroll_guard *previous;
    bool deleted;
} round_scroll_guard_t;

typedef struct {
    orb_round_list_config_t config;
    round_shape_t *shapes;
    round_scroll_guard_t *scroll_guard;
    lv_timer_t *prepare_timer;
    lv_obj_t *obj;
    unsigned prepare_next;
    bool updating;
    bool scroll_owned;
    bool user_scroll;
    int32_t scroll_y;
    int scroll_direction;
    round_chord_row_t *rows;
    uint32_t source_builds, lookahead_builds;
    bool scrolling_update;
    bool preparing;
    bool cache_prepare;
    bool lookahead; /* Idle lookahead slices; any restart reruns the full update. */
    size_t row_bytes; /* Largest retained row source seen: the admission estimate. */
} round_list_t;

static void round_list_event(lv_event_t *e);
static bool round_row_hidden(lv_obj_t *row);
static void round_transform(const round_list_t *list, int32_t width, int32_t height,
                            int32_t center2, uint16_t *scale, lv_opa_t *opa);
static round_shape_t *round_shape(round_list_t *list, int32_t width, int32_t height, bool prepare);
#include "orb_round_chord.inc"

static void round_list_prepare(lv_timer_t *timer)
{
    round_list_t *list = lv_timer_get_user_data(timer);
    lv_obj_t *obj = list->obj;
    if (list->config.chord_fit && !list->config.visible_cache && round_chord_pointer_pressed()) return;
    if ((!list->config.visible_cache && (list->scroll_owned || lv_obj_is_scrolling(obj))) ||
        !lv_obj_is_visible(obj)) {
        lv_timer_pause(timer);
        return;
    }
    if (list->config.chord_fit) {
        /* Model changes wait for idle. Viewport-only caches may prepare newly
         * entering rows here during motion, ahead of the display timer; no
         * SCROLL or DRAW callback allocates or captures a source. */
        round_scroll_guard_t guard = {.previous = list->scroll_guard};
        list->scroll_guard = &guard;
        if (!list->lookahead) {
            list->cache_prepare = list->config.visible_cache;
            orb_round_list_update(obj);
            if (guard.deleted) return;
            list->cache_prepare = false;
        }
        /* Then one idle lookahead slice; waiting for idle polls slowly and
         * never repeats the full update or its hashes. */
        bool more = round_chord_lookahead(list);
        if (guard.deleted) return;
        list->scroll_guard = guard.previous;
        list->lookahead = more;
        lv_timer_set_period(timer, more && !orb_round_idle() ? ORB_ROUND_IDLE_MS : 2);
        if (!more) lv_timer_pause(timer);
        return;
    }
    round_scroll_guard_t guard = {.previous = list->scroll_guard};
    list->scroll_guard = &guard;
    lv_obj_update_layout(obj);
    if (guard.deleted) return;
    list->scroll_guard = guard.previous;
    lv_area_t viewport; lv_obj_get_coords(obj, &viewport);
    int height = lv_area_get_height(&viewport);
    viewport.y1 -= height; viewport.y2 += height;
    unsigned count = lv_obj_get_child_count(obj);
    while (list->prepare_next < count) {
        lv_obj_t *row = lv_obj_get_child(obj, list->prepare_next++);
        lv_area_t area; lv_obj_get_coords(row, &area);
        if (!lv_obj_get_raster(row) || lv_obj_is_hidden(row) || lv_obj_is_floating(row) ||
            lv_obj_raster_ready(row) || area.y2 < viewport.y1 || area.y1 > viewport.y2) continue;
        /* Cold glyph-size preparation cost up to 24ms in the board trace.
         * Prepare one nearby source per idle scheduling slice, never alongside
         * an active fling. Admission still pins visible rows and obeys the
         * shared cache/free-heap limits; this never warms the whole long list. */
        lv_obj_raster_invalidate_draw(row);
        lv_obj_raster_prepare(row);
        lv_obj_raster_invalidate_draw(row);
        return;
    }
    lv_timer_pause(timer);
}

static void round_list_prepare_restart(round_list_t *list)
{
    if (!list->prepare_timer) return;
    list->prepare_next = 0;
    list->lookahead = false;
    lv_timer_set_period(list->prepare_timer, 2);
    lv_timer_reset(list->prepare_timer);
    lv_timer_resume(list->prepare_timer);
}

static void *event_data(lv_obj_t *obj, lv_event_cb_t callback)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(d) == callback) {
            return lv_event_dsc_get_user_data(d);
        }
    }
    return NULL;
}

static void round_transform(const round_list_t *list, int32_t width, int32_t height,
                            int32_t center2, uint16_t *scale, lv_opa_t *opa)
{
    float r = ORB_SCREEN_R - list->config.margin;
    float dy = fabsf(center2 * 0.5f - ORB_SCREEN_R);
    if (r <= 0 || dy >= r || width <= 0 || height <= 0) {
        *scale = 0;
        *opa = LV_OPA_TRANSP;
        return;
    }
    if (list->config.stepped) {
        /* Frames specify a per-position step, not perspective on every pixel.
         * Both raster variants are reused while finger/rim motion translates. */
        bool rim = center2 < 280 || center2 > 720;
        if (list->config.partial_only) {
            int top = (center2 - height) / 2, bottom = top + height;
            int viewport_top = lv_obj_get_y(list->obj);
            if (list->config.chord_fit) {
                lv_area_t viewport; lv_obj_get_coords(list->obj, &viewport);
                viewport_top = viewport.y1;
            }
            rim = top < viewport_top || bottom > viewport_top + lv_obj_get_height(list->obj) ||
                  (list->config.chord_fit ? dy + height * .5f > r :
                   orb_chord_width(top, bottom - 1, list->config.margin) < width);
        }
        *scale = rim ? 211 : LV_SCALE_NONE;
        *opa = rim ? LV_OPA_50 : LV_OPA_COVER;
        return;
    }
    float hw = width * 0.5f, hh = height * 0.5f;
    float aa = hw * hw + hh * hh;
    /* HIG §§3,7: fit both outer corners inside the safe circle, rather than merely fitting
     * the chord through the row's center. This keeps the entire row off the rim. */
    float s = (-dy * hh + sqrtf(aa * r * r - hw * hw * dy * dy)) / aa;
    *scale = (uint16_t)(LV_CLAMP(0.0f, s, 1.0f) * LV_SCALE_NONE);
    float fade = list->config.fade;
    *opa = fade > 0 ? (lv_opa_t)(LV_MIN(1.0f, (r - dy) / fade) * LV_OPA_COVER) : LV_OPA_COVER;
}

static round_shape_t *round_shape(round_list_t *list, int32_t width, int32_t height, bool prepare)
{
    for (round_shape_t *shape = list->shapes; shape; shape = shape->next) {
        if (shape->width == width && shape->height == height) {
            shape->used = true;
            return shape;
        }
    }
    if (!prepare) return NULL;
    round_shape_t *shape = orb_ui_alloc(sizeof(*shape));
    if (!shape) {
        return NULL;
    }
    shape->width = width;
    shape->height = height;
    shape->used = true;
    for (int32_t i = 0; i < ROUND_CENTERS; i++) {
        round_transform(list, width, height, i, &shape->scale[i], &shape->opa[i]);
    }
    for (unsigned i = 0; i < ROUND_CHORD_BUCKETS; i++) {
        float radius = LV_MAX(0, ORB_SCREEN_R - list->config.margin - height * .5f);
        float dy = i * ROUND_CHORD_STEP;
        int span = dy < radius ? (int)(2 * sqrtf(radius * radius - dy * dy)) : 0;
        int available = height + span;
        /* Quantise inwards from the native equatorial width. The far edge
         * of each offset bucket guarantees the cap stays in the safe circle. */
        shape->chord[i] = LV_MAX(height, width -
            ((LV_MAX(0, width - available) + ROUND_CHORD_WIDTH_STEP - 1) /
             ROUND_CHORD_WIDTH_STEP) * ROUND_CHORD_WIDTH_STEP);
    }
    shape->next = list->shapes;
    list->shapes = shape;
    return shape;
}

static void round_shapes_free(round_list_t *list)
{
    while (list->shapes) {
        round_shape_t *next = list->shapes->next;
        orb_ui_free(list->shapes);
        list->shapes = next;
    }
}

static bool round_row_hidden(lv_obj_t *row)
{
    /* A readability policy's explicit default hide wins over a held row's
     * pressed-state feedback, for both retained pixels and hit testing. */
    lv_style_value_t opa;
    return lv_obj_get_style_opa(row, 0) <= LV_OPA_MIN ||
        (lv_obj_get_local_style_prop(row, LV_STYLE_OPA, &opa, 0) == LV_STYLE_RES_FOUND && opa.num <= LV_OPA_MIN);
}

void orb_round_list_update(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    round_list_t *list = event_data(obj, round_list_event);
    if (!list || list->updating) {
        return;
    }
    list->updating = true;
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    uint64_t layout_started = orb_ui_scroll_diagnostics_clock();
    lv_obj_update_layout(obj);
    scroll_stage_record(layout_started, &s_scroll_diagnostics.layout);
    for (round_shape_t *shape = list->shapes; shape; shape = shape->next) {
        shape->used = false;
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(obj, i);
        if (lv_obj_is_floating(row)) {
            if (lv_obj_get_raster(row)) {
                /* A formerly managed row must not keep its bezel transform
                 * when the owner promotes it to fixed floating content. */
                lv_obj_set_raster_scale(row, LV_SCALE_NONE);
                lv_obj_set_raster_opa(row, LV_OPA_COVER);
            }
            continue;
        }
        lv_area_t area;
        lv_obj_get_coords(row, &area);
        int32_t width = lv_area_get_width(&area), height = lv_area_get_height(&area);
        int32_t center2 = area.y1 + area.y2 + 1;
        bool prepare = !list->config.chord_fit || round_chord_can_prepare(list) || list->cache_prepare;
        round_chord_row_t *cached = list->config.chord_fit ? round_chord_find(list, row) : NULL;
        /* Model/layout changes during a drag or throw keep their old source
         * and shape until idle. A new height must not allocate a width table
         * from the SCROLL callback or invalidate the retained shape pointer. */
        int source_width = !prepare && cached ? cached->width : width;
        int source_height = !prepare && cached ? cached->height : height;
        round_shape_t *shape = round_shape(list, source_width, source_height, prepare);
        if (list->config.chord_fit) {
            round_chord_update(list, row, shape, &area);
            continue;
        }
        uint16_t scale = 0;
        lv_opa_t opa = LV_OPA_TRANSP;
        if (center2 >= 0 && center2 < ROUND_CENTERS) {
            if (shape) {
                scale = shape->scale[center2];
                opa = shape->opa[center2];
            } else {
                /* Allocation failure preserves correct geometry without a table. */
                round_transform(list, width, height, center2, &scale, &opa);
            }
        }
        if (!lv_obj_get_raster(row)) {
            lv_obj_set_raster_matte(row, lv_color_black());
            lv_obj_set_raster_rows(row, true);
        }
        lv_obj_set_raster_scale(row, scale);
        /* A readability policy may hide a row without changing its shape.
         * Do not revive that cache on every SCROLL/LAYOUT notification only
         * for the policy callback to hide it again. */
        if (list->config.partial_only)
            opa = (lv_opa_t)((unsigned)opa * lv_obj_get_style_opa(row, 0) / LV_OPA_COVER);
        lv_obj_set_raster_opa(row, round_row_hidden(row) ? LV_OPA_TRANSP : opa);
    }
    /* Layout changes may introduce new row sizes; discard obsolete tables so
     * Dynamic Type and repeated list replacement cannot grow retained memory. */
    round_shape_t **slot = &list->shapes;
    while (*slot) {
        round_shape_t *shape = *slot;
        if (!shape->used) {
            *slot = shape->next;
            orb_ui_free(shape);
        } else {
            slot = &shape->next;
        }
    }
    list->updating = false;
    if (list->config.chord_fit && !list->scrolling_update && !list->scroll_owned &&
        !lv_obj_is_scrolling(obj) && round_chord_pointer_pressed()) round_list_prepare_restart(list);
    if (started != UINT64_MAX && s_scroll_diagnostics_active) {
        uint64_t elapsed = orb_ui_scroll_diagnostics_clock() - started;
        s_scroll_diagnostics.round_calls++;
        s_scroll_diagnostics.round_us += elapsed;
        s_scroll_diagnostics.round_rows += n;
        if (elapsed > s_scroll_diagnostics.round_max_us) s_scroll_diagnostics.round_max_us = elapsed;
        if (s_scroll_diagnostics.refresh_phase_available) {
            if (s_scroll_in_refresh) s_scroll_diagnostics.round_inside_refresh_us += elapsed;
            else s_scroll_diagnostics.round_outside_refresh_us += elapsed;
        }
    }
}

static bool round_pointer_down(lv_obj_t *obj)
{
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input))
        if (lv_indev_get_scroll_obj(input) == obj &&
            lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}

static bool round_pointer_sample(lv_obj_t *obj)
{
    lv_indev_t *input = lv_indev_active();
    return round_pointer_down(obj) || (input && lv_indev_get_scroll_obj(input) == obj);
}

static void round_list_settle(round_list_t *list)
{
    if (!list->user_scroll || list->scroll_owned || round_pointer_down(list->obj)) return;
    list->user_scroll = false;
    if (!list->config.stepped || !list->scroll_direction) return;
    lv_obj_t *obj = list->obj;
    int32_t top = lv_obj_get_scroll_y(obj);
    int32_t limit = LV_MAX(0, top + lv_obj_get_scroll_bottom(obj));
    if (top < 0 || top > limit) return; /* Native elasticity owns the boundary. */
    int32_t target = list->scroll_direction > 0 ? limit : 0;
    int32_t origin = 0;
    bool first = true;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *row = lv_obj_get_child(obj, (int32_t)i);
        if (lv_obj_is_hidden(row) || lv_obj_is_floating(row)) continue;
        if (first) { origin = lv_obj_get_y(row); first = false; }
        int32_t candidate = LV_CLAMP(0, lv_obj_get_y(row) - origin, limit);
        if ((list->scroll_direction > 0 && candidate >= top && candidate < target) ||
            (list->scroll_direction < 0 && candidate <= top && candidate > target)) target = candidate;
    }
    /* HIG §§6/7: keep native finger tracking/throw, then align the nearest
     * row ahead of that throw. An intentional drag must never spring back
     * to the row it just left. Programmatic restoration remains exact. */
    if (target != top) lv_obj_scroll_to_y(obj, target, orb_ui_reduce_motion() ? LV_ANIM_OFF : LV_ANIM_ON);
}

static void round_list_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    round_list_t *list = lv_event_get_user_data(e);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_DELETE:
        /* stop_scroll_anim emits END and can synchronously delete/recreate
         * this list. Mark every scoped caller before releasing its state. */
        for (round_scroll_guard_t *guard = list->scroll_guard; guard; guard = guard->previous)
            guard->deleted = true;
        round_shapes_free(list);
        round_chord_free(list);
        if (list->prepare_timer) lv_timer_delete(list->prepare_timer);
        orb_ui_free(list);
        break;
    case LV_EVENT_SCROLL_BEGIN:
        if (list->scroll_owned || round_pointer_sample(obj)) {
            list->user_scroll = true;
            list->scroll_y = lv_obj_get_scroll_y(obj);
            list->scroll_direction = 0;
        }
        lv_obj_raster_cache_retry();
        if (list->prepare_timer) lv_timer_pause(list->prepare_timer);
        break;
    case LV_EVENT_SCROLL_END:
        round_list_prepare_restart(list);
        round_list_settle(list);
        break;
    case LV_EVENT_SCROLL:
        if (list->user_scroll && (list->scroll_owned || round_pointer_sample(obj))) {
            int32_t y = lv_obj_get_scroll_y(obj);
            if (y != list->scroll_y) list->scroll_direction = y > list->scroll_y ? 1 : -1;
            list->scroll_y = y;
        }
        list->scrolling_update = true;
        orb_round_list_update(obj);
        list->scrolling_update = false;
        if (list->config.visible_cache) round_list_prepare_restart(list);
        break;
    case LV_EVENT_LAYOUT_CHANGED:
    case LV_EVENT_SIZE_CHANGED:
        orb_round_list_update(obj);
        if (lv_event_get_code(e) != LV_EVENT_SCROLL) round_list_prepare_restart(list);
        break;
    case LV_EVENT_DRAW_MAIN:
        if (list->config.chord_fit) round_chord_draw(list, lv_event_get_layer(e));
        break;
    default:
        break;
    }
}

bool orb_round_list_scroll_to(lv_obj_t *obj, int32_t y)
{
    if (!obj) return false;
    round_list_t *list = event_data(obj, round_list_event);
    int32_t top = lv_obj_get_scroll_y(obj);
    int32_t limit = LV_MAX(0, top + lv_obj_get_scroll_bottom(obj));
    y = LV_CLAMP(0, y, limit);
    /* The same explicit input marker covers unmanaged fallback readers.
     * Native scroll_to also emits BEGIN during restoration, so BEGIN alone
     * cannot distinguish a crown sample from restoring saved geometry. */
    if (y != top) orb_kit_scroll_arc_input(obj);
    if (!list) return false;
    if (y == top) return true;
    if (!list->scroll_owned) {
        /* HIG §6: the crown takes over momentum once. Native scroll-to emits
         * END for every sample, which otherwise re-snaps a released fling. */
        round_scroll_guard_t guard = {.previous = list->scroll_guard};
        list->scroll_guard = &guard;
        lv_obj_stop_scroll_anim(obj);
        if (guard.deleted) return true;
        list->scroll_guard = guard.previous;
        /* END callbacks may also snap a surviving list. Use that fresh
         * position rather than applying a delta measured before cancellation. */
        top = lv_obj_get_scroll_y(obj);
        limit = LV_MAX(0, top + lv_obj_get_scroll_bottom(obj));
        y = LV_CLAMP(0, y, limit);
        if (y == top) return true;
        list->scroll_owned = true;
        if (lv_obj_send_event(obj, LV_EVENT_SCROLL_BEGIN, NULL) != LV_RESULT_OK) return true;
    }
    /* A callback may delete the owner. The sample was still consumed; the
     * adapter must not fall back to scroll_to on that deleted object. */
    lv_obj_scroll_by_raw(obj, 0, top - y);
    return true;
}

void orb_round_list_scroll_finish(lv_obj_t *obj)
{
    if (!obj) return;
    round_list_t *list = event_data(obj, round_list_event);
    if (!list || !list->scroll_owned) return;
    list->scroll_owned = false;
    lv_obj_send_event(obj, LV_EVENT_SCROLL_END, NULL);
}

bool orb_round_list_scroll_active(lv_obj_t *obj)
{
    round_list_t *list = obj ? event_data(obj, round_list_event) : NULL;
    return list && list->scroll_owned;
}

bool orb_round_list_apply(lv_obj_t *obj, const orb_round_list_config_t *config)
{
    if (!obj) {
        return false;
    }
    round_list_t *list = event_data(obj, round_list_event);
    if (!list) {
        list = orb_ui_alloc(sizeof(*list));
        if (!list) {
            return false;
        }
        memset(list, 0, sizeof(*list));
        list->obj = obj;
        if (!lv_obj_add_event_cb(obj, round_list_event, LV_EVENT_ALL, list)) {
            orb_ui_free(list);
            return false;
        }
        list->prepare_timer = lv_timer_create(round_list_prepare, 2, list);
        if (list->prepare_timer) lv_timer_pause(list->prepare_timer);
    } else {
        round_shapes_free(list);
        round_chord_free(list);
        list->row_bytes = 0;
    }
    list->config = config ? *config : (orb_round_list_config_t){.margin = ORB_RIM_MARGIN_TEXT, .fade = ORB_PT(32)};
    list->config.margin = LV_CLAMP(0, list->config.margin, ORB_SCREEN_R);
    list->config.fade = LV_MAX(0, list->config.fade);
    if (list->config.chord_fit && list->config.visible_cache) orb_round_idle_watch(obj);
    if (list->config.chord_fit) {
        /* The cached content plane is explicitly black (HIG §§1/3). Besides
         * its matte contract, this prepares LVGL's transient opacity layer
         * before the direct compositor writes a band during navigation. */
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    /* HIG §§2.2/3: the round list's black canvas is static. Keep damage on
     * moving cached chords; do not transmit their untouched black gaps. */
    lv_obj_set_scroll_damage_children(obj, true);
    /* Explicit apply prepares a newly constructed navigation destination,
     * even if the pointer is still held on its source page. Unlike an update
     * driven by SCROLL/DRAW, this caller-owned phase may allocate its cache. */
    list->preparing = true;
    lv_obj_update_layout(obj);
    orb_round_list_update(obj);
    list->preparing = false;
    bool prepared = true;
    if (list->config.chord_fit) {
        for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
            lv_obj_t *row = lv_obj_get_child(obj, i);
            lv_area_t area; lv_obj_get_coords(row, &area);
            if (!lv_obj_is_floating(row) &&
                (!list->config.visible_cache || round_chord_visible(list, &area)) &&
                !round_chord_find(list, row)) prepared = false;
        }
        if (!prepared) {
            /* A partially prepared compositor must not leave uncached native
             * rows outside the circle. Fall back as a whole to the original
             * corner-fitted presentation and report the preparation failure. */
            round_chord_free(list);
            round_shapes_free(list);
            list->config.chord_fit = false;
            list->config.stepped = false;
            list->config.partial_only = false;
            orb_round_list_update(obj);
        }
    }
    round_list_prepare_restart(list);
    return prepared;
}

static bool round_row_presentation(lv_obj_t *row, lv_area_t *logical, lv_area_t *presented,
                                  uint16_t *scale, lv_opa_t *opacity)
{
    if (!row || lv_obj_is_hidden(row) || round_row_hidden(row)) return false;
    lv_obj_t *parent = lv_obj_get_parent(row);
    round_list_t *list = parent ? event_data(parent, round_list_event) : NULL;
    if (!list || lv_obj_is_floating(row)) return false;
    if (list->config.chord_fit) return round_chord_presentation(list, row, logical, presented, scale, opacity);
    if (!lv_obj_raster_presentation_visible(row)) return false;
    lv_obj_get_coords(row, logical);
    int32_t width = lv_area_get_width(logical), height = lv_area_get_height(logical);
    lv_opa_t opa;
    round_transform(list, width, height, logical->y1 + logical->y2 + 1, scale, &opa);
    if (!*scale || opa <= LV_OPA_MIN) return false;
    if (opacity) *opacity = opa;
    int32_t sw = width * *scale / LV_SCALE_NONE, sh = height * *scale / LV_SCALE_NONE;
    *presented = (lv_area_t){.x1 = logical->x1 + (width - sw) / 2,
                             .y1 = logical->y1 + (height - sh) / 2};
    presented->x2 = presented->x1 + sw - 1;
    presented->y2 = presented->y1 + sh - 1;
    return sw > 0 && sh > 0;
}

bool orb_round_list_hit_test(lv_obj_t *row, const lv_point_t *point)
{
    lv_area_t logical, area;
    uint16_t scale;
    if (!point || !round_row_presentation(row, &logical, &area, &scale, NULL)) return false;
    if (point->x < area.x1 || point->x > area.x2 || point->y < area.y1 || point->y > area.y2) {
        return false;
    }
    round_list_t *list = event_data(lv_obj_get_parent(row), round_list_event);
    if (list && list->config.chord_fit) {
        int radius = LV_MIN(lv_obj_get_style_radius(row, 0), lv_area_get_height(&logical) / 2) * scale / 256;
        int cx = LV_CLAMP(area.x1 + radius, point->x, area.x2 - radius);
        int cy2 = area.y1 + area.y2;
        int dx2 = 2 * (point->x - cx), dy2 = 2 * point->y - cy2;
        if (dx2 * dx2 + dy2 * dy2 > 4 * radius * radius) return false;
    }
    /* A row may be partly visible while the tap falls beyond a clipping
     * viewport. Check this exact point through the complete ancestor chain. */
    lv_area_t point_area = {point->x, point->y, point->x, point->y};
    return lv_obj_area_is_visible(row, &point_area);
}

bool orb_round_list_child_bounds(lv_obj_t *row, lv_obj_t *child, lv_area_t *area)
{
    if (!child || !area) return false;
    lv_obj_t *ancestor = child;
    while (ancestor && ancestor != row) {
        if (lv_obj_is_hidden(ancestor) || lv_obj_get_style_opa(ancestor, 0) <= LV_OPA_MIN) return false;
        ancestor = lv_obj_get_parent(ancestor);
    }
    if (ancestor != row) return false;
    lv_area_t logical, presented, bounds;
    uint16_t scale;
    if (!round_row_presentation(row, &logical, &presented, &scale, NULL)) return false;
    lv_obj_get_coords(child, &bounds);
    lv_obj_t *parent = lv_obj_get_parent(row);
    round_list_t *list = event_data(parent, round_list_event);
    if (list->config.chord_fit) round_chord_child(list, row, child, &bounds);
    /* HIG §§3/6: launch from the visible token, preserving the same centered
     * chord scale as row hit testing instead of sampling its adjacent title. */
    lv_area_t mapped = {
        .x1 = presented.x1 + (bounds.x1 - logical.x1) * scale / LV_SCALE_NONE,
        .y1 = presented.y1 + (bounds.y1 - logical.y1) * scale / LV_SCALE_NONE,
    };
    mapped.x2 = mapped.x1 + LV_MAX(1, lv_area_get_width(&bounds) * scale / LV_SCALE_NONE) - 1;
    mapped.y2 = mapped.y1 + LV_MAX(1, lv_area_get_height(&bounds) * scale / LV_SCALE_NONE) - 1;
    lv_area_t visible = mapped;
    if (!lv_obj_area_is_visible(row, &visible)) return false;
    *area = mapped;
    return true;
}

bool orb_round_list_row_bounds(lv_obj_t *row, lv_area_t *area)
{
    lv_area_t logical;
    uint16_t scale;
    return area && round_row_presentation(row, &logical, area, &scale, NULL);
}

lv_opa_t orb_round_list_row_opacity(lv_obj_t *row)
{
    lv_area_t logical, area;
    uint16_t scale;
    lv_opa_t opacity;
    if (!round_row_presentation(row, &logical, &area, &scale, &opacity)) return LV_OPA_TRANSP;
    return (unsigned)opacity * lv_obj_get_style_opa(row, 0) / 255;
}

bool orb_round_list_cache_stats(lv_obj_t *obj, orb_round_list_cache_stats_t *out)
{
    round_list_t *list = obj ? event_data(obj, round_list_event) : NULL;
    if (!list || !out) return false;
    memset(out, 0, sizeof(*out));
    if (list->config.chord_fit) round_chord_stats(list, out);
    return true;
}
