/* Design 1.1 / HIG §§3,7. A row's source tree is immutable during motion.
 * Retain one native source, not one bitmap per width. The compositor moves
 * the trailing rail and clips label runs at premeasured glyph boundaries.
 * Width and text cuts are prepared before scrolling.
 * Motion and draw read this cache only; explicit idle content updates may
 * prepare a changed source. Explicit initial apply prepares a navigation
 * destination; subsequent updates wait until input and motion are idle. */
/* One reusable immutable pose per cached row; never allocate during motion.
 * This is PSRAM through the UI allocator, not internal/DMA heap fallback. */
static size_t s_chord_pose_bytes;

#include "orb_round_source.h"

static void chord_row_event(lv_event_t *e);

static bool round_chord_visible(const round_list_t *list, const lv_area_t *area)
{
    lv_area_t viewport; lv_obj_get_coords(list->obj, &viewport);
    return area->y2 >= viewport.y1 && area->y1 <= viewport.y2;
}

/* Pixels from the viewport (zero while visible). Viewport caches retain and
 * idle-prepare rows within one viewport height (HIG §6: a continuing or
 * reversing throw must not wait for a capture on its first moving frame). */
static int32_t round_chord_distance(const round_list_t *list, const lv_area_t *area)
{
    lv_area_t viewport; lv_obj_get_coords(list->obj, &viewport);
    if (area->y2 < viewport.y1) return viewport.y1 - area->y2;
    if (area->y1 > viewport.y2) return area->y1 - viewport.y2;
    return 0;
}

static bool round_chord_window(const round_list_t *list, const lv_area_t *area)
{
    return round_chord_distance(list, area) <= lv_obj_get_height(list->obj);
}

static void chord_row_release(round_chord_row_t *r)
{
    /* Task cleanup is on the UI thread. The worker only sees immutable pose
     * and source data, so retirement never races a refcount or frees its ink. */
    if (r->draw_leases) { r->retired = true; return; }
    if (r->draw_pose) {
        s_chord_pose_bytes -= r->draw_pose_bytes;
        orb_ui_free(r->draw_pose);
    }
    for (unsigned i = 0; i < r->count; i++) {
        orb_round_text_dots_release(r->children[i].dots);
        orb_ui_free(r->children[i].cut);
    }
    orb_ui_free(r->source_run_index);
    orb_ui_free(r->pixels);
    orb_ui_free(r);
}

/* Worker poses belong to rows on screen; a departed row returns an unleased one. */
static void chord_pose_release(round_chord_row_t *r)
{
    if (!r->draw_pose || r->draw_leases || r->draw_pose_busy) return;
    s_chord_pose_bytes -= r->draw_pose_bytes;
    orb_ui_free(r->draw_pose);
    r->draw_pose = NULL;
    r->draw_pose_bytes = 0;
}

static void chord_guard_row(round_list_t *list, lv_obj_t *row)
{
    for (unsigned i = 0; i < lv_obj_get_event_count(row); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(row, i);
        if (lv_event_dsc_get_cb(event) == chord_row_event &&
            lv_event_dsc_get_user_data(event) == list) return;
    }
    lv_obj_add_event_cb(row, chord_row_event, LV_EVENT_ALL | LV_EVENT_PREPROCESS, list);
}

static bool round_chord_pointer_pressed(void)
{
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input))
        if (lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER &&
            lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}

static bool round_chord_can_prepare(const round_list_t *list)
{
    return (list->preparing || !round_chord_pointer_pressed()) && !list->scrolling_update &&
        !list->scroll_owned && !list->user_scroll && !lv_obj_is_scrolling(list->obj);
}

static uint32_t chord_hash_tree(lv_obj_t *obj, const lv_area_t *origin, uint32_t hash, bool root)
{
    lv_area_t a; lv_obj_get_coords(obj, &a);
    const uintptr_t fields[] = {(uintptr_t)obj, a.x1 - origin->x1, a.y1 - origin->y1,
        lv_area_get_width(&a), lv_area_get_height(&a), lv_obj_is_hidden(obj),
        lv_color_to_u32(lv_obj_get_style_text_color(obj, 0)),
        root ? 0 : lv_color_to_u32(lv_obj_get_style_bg_color(obj, 0)),
        root ? 0 : lv_obj_get_style_bg_opa(obj, 0),
        root ? 0 : lv_obj_get_style_radius(obj, 0),
        lv_obj_get_style_outline_width(obj, 0), lv_obj_get_style_outline_pad(obj, 0),
        lv_obj_get_style_outline_opa(obj, 0), lv_obj_get_style_text_letter_space(obj, 0),
        lv_obj_get_style_text_line_space(obj, 0),
        (uintptr_t)lv_obj_get_style_text_font(obj, 0), orb_fonts_revision()};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        hash = (hash ^ (uint32_t)fields[i]) * 16777619u;
    if (lv_obj_check_type(obj, &lv_label_class))
        for (const unsigned char *s = (const unsigned char *)lv_label_get_text(obj); *s; s++)
            hash = (hash ^ *s) * 16777619u;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        hash = chord_hash_tree(lv_obj_get_child(obj, i), origin, hash, false);
    return hash;
}

static uint32_t chord_hash(lv_obj_t *obj, const lv_area_t *origin, uint32_t hash, bool root)
{
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    uint32_t result = chord_hash_tree(obj, origin, hash, root);
    scroll_stage_record(started, &s_scroll_diagnostics.hash);
    return result;
}

static round_chord_row_t *round_chord_find(round_list_t *list, lv_obj_t *row)
{
    for (round_chord_row_t *r = list->rows; r; r = r->next)
        if (r->obj == row) return r;
    return NULL;
}

static unsigned chord_bucket(int width, int current)
{
    return LV_CLAMP(0, (width - current + ROUND_CHORD_WIDTH_STEP - 1) / ROUND_CHORD_WIDTH_STEP,
                    CHORD_TEXT_BUCKETS - 1);
}

static bool chord_trailing(lv_obj_t *child, int width, int x, int cw)
{
    lv_align_t align = lv_obj_get_style_align(child, 0);
    return align == LV_ALIGN_RIGHT_MID || align == LV_ALIGN_TOP_RIGHT || align == LV_ALIGN_BOTTOM_RIGHT ||
        (x > width / 2 && cw <= 128 && x + cw >= width - 24);
}




typedef struct {
    const uint16_t *next, *end, *pixels;
    int start, finish;
} chord_source_cursor_t;

static chord_source_cursor_t chord_source_line(const round_chord_row_t *r, const round_chord_child_t *child, int y)
{
    chord_source_cursor_t c = {0};
    if (r->sparse) {
        const uint32_t *index = r->pixels;
        const uint16_t *runs = (const uint16_t *)(index + r->height + 1) + r->palette_count;
        c.next = runs + index[y]; c.end = runs + index[y + 1];
        if (child && child->source_runs) {
            int first_y = LV_MAX(0, child->y - child->pad);
            int last_y = LV_MIN(r->height, child->y + child->height + child->pad);
            if (y >= first_y && y < last_y) c.next += child->source_runs[y - first_y];
        }
    } else {
        c.pixels = (const uint16_t *)(r->source.data + y * r->source.header.stride);
        c.finish = r->width;
    }
    return c;
}

static uint16_t chord_source_pixel(const round_chord_row_t *r, chord_source_cursor_t *c, int x)
{
    while (x >= c->finish && c->next && c->next < c->end) {
        c->start = c->next[0]; c->finish = c->start + c->next[1];
        c->pixels = c->next + 2;
        c->next = c->pixels + (r->palette_count ? (c->next[1] + 1) / 2 : c->next[1]);
    }
    if (x < c->start || x >= c->finish) return r->source_bg;
    return r->palette_count ? r->palette[((const uint8_t *)c->pixels)[x - c->start]] : c->pixels[x - c->start];
}

static void chord_pose_prepare(round_chord_row_t *r)
{
    size_t bytes = sizeof(*r) + r->count * sizeof(r->children[0]) +
        ((size_t)r->height + ORB_SCREEN_D) * sizeof(uint16_t);
    if (bytes > ORB_ROUND_LIST_DRAW_POSE_BUDGET - s_chord_pose_bytes) return;
    r->draw_pose = orb_ui_alloc(bytes);
    if (!r->draw_pose) return;
    r->draw_pose->draw_map_scale = 0; /* Zero is invalid: only visible, nonzero scales dispatch. */
    r->draw_pose_bytes = bytes;
    s_chord_pose_bytes += bytes;
}

static round_chord_row_t *round_chord_create(round_list_t *list, lv_obj_t *row, const lv_area_t *area,
                                             bool pose)
{
    unsigned count = lv_obj_get_child_count(row);
    size_t bytes = sizeof(round_chord_row_t) + count * sizeof(round_chord_child_t) +
        (size_t)lv_area_get_height(area) * sizeof(uint16_t);
    round_chord_row_t *r = orb_ui_alloc(bytes);
    if (!r) return NULL;
    memset(r, 0, bytes);
    r->insets = (uint16_t *)(r->children + count);
    r->obj = row; r->width = lv_area_get_width(area); r->height = lv_area_get_height(area);
    r->count = count;
    unsigned minimum_text = UINT16_MAX;
    uint32_t stride = lv_draw_buf_width_to_stride(r->width, LV_COLOR_FORMAT_RGB565);
    r->bytes = stride * r->height;
    r->pixels = orb_ui_alloc(r->bytes);
    if (!r->pixels) { orb_ui_free(r); return NULL; }
    lv_draw_buf_init(&r->source, r->width, r->height, LV_COLOR_FORMAT_RGB565,
                     stride, r->pixels, r->bytes);
    for (unsigned i = 0; i < count; i++) {
        round_chord_child_t *c = &r->children[i];
        c->obj = lv_obj_get_child(row, i);
        c->hidden = lv_obj_is_hidden(c->obj);
        lv_area_t a; lv_obj_get_coords(c->obj, &a);
        c->x = a.x1 - area->x1; c->y = a.y1 - area->y1;
        c->width = lv_area_get_width(&a); c->height = lv_area_get_height(&a);
        /* HIG §7: focus is outside the40px disc. Snapshotting the row already
         * captures it; the retained child's copy rectangle must include it. */
        /* LVGL labels add font_height/4 as an empirical italic allowance,
         * even for upright Inter. Copying that region from a whole-row source
         * leaks adjacent title/subtitle ink into each other. Bounded text
         * lanes own their line boxes; only nontext focus gets extra space. */
        c->pad = lv_obj_check_type(c->obj, &lv_label_class) ? 0 : lv_obj_get_ext_draw_size(c->obj);
        c->trailing = chord_trailing(c->obj, r->width, c->x, c->width);
        c->flexible = lv_obj_check_type(c->obj, &lv_label_class);
        if (lv_obj_check_type(c->obj, &lv_label_class)) {
            unsigned size = orb_fonts_nominal_px(lv_obj_get_style_text_font(c->obj, 0));
            if (size) minimum_text = LV_MIN(minimum_text, size);
        }
        c->bounded_ink = c->flexible && list->config.visible_cache &&
            lv_label_get_max_lines(c->obj) >= 1;
        if (c->flexible) {
            if (c->bounded_ink) orb_round_text_ink(c);
            c->dots = orb_round_text_dots(c->obj);
            if (c->dots) {
                uint64_t started = orb_ui_scroll_diagnostics_clock();
                orb_round_source_cuts(c);
                scroll_stage_record(started, &s_scroll_diagnostics.source_cuts);
            }
        }
    }
    /* Sources are captured only during explicit list preparation. The
     * ordinary tree remains semantically visible, with drawing delegated to
     * its list; it must not retain a duplicate legacy raster. */
    lv_obj_set_raster(row, false);
    bool pressed = lv_obj_has_state(row, LV_STATE_PRESSED);
    if (pressed) lv_obj_remove_state(row, LV_STATE_PRESSED);
    r->source_bg = lv_obj_get_style_bg_opa(row, 0) ? lv_color_to_u16(lv_obj_get_style_bg_color(row, 0)) : 0;
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    lv_result_t result = lv_snapshot_take_area_to_draw_buf(row, LV_COLOR_FORMAT_RGB565, area, &r->source);
    scroll_stage_record(started, &s_scroll_diagnostics.source_snapshot);
    if (pressed) lv_obj_add_state(row, LV_STATE_PRESSED);
    if (result != LV_RESULT_OK) { chord_row_release(r); return NULL; }
    started = orb_ui_scroll_diagnostics_clock();
    bool packed = orb_round_source_pack(r);
    scroll_stage_record(started, &s_scroll_diagnostics.source_pack);
    if (!packed) { chord_row_release(r); return NULL; }
    orb_round_source_index(r);
    /* An idle lookahead source must not take a worker pose from rows on screen. */
    if (pose) chord_pose_prepare(r);
    r->minimum_scale = minimum_text == UINT16_MAX ? 0 : (20 * 256 + minimum_text - 1) / minimum_text;
    chord_guard_row(list, row);
    r->next = list->rows; list->rows = r; list->source_builds++;
    r->signature = chord_hash(row, area, 2166136261u, true);
    return r;
}

static void round_chord_free(round_list_t *list)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(list->obj); i++)
        lv_obj_remove_event_cb_with_user_data(lv_obj_get_child(list->obj, i), chord_row_event, list);
    while (list->rows) {
        round_chord_row_t *r = list->rows;
        list->rows = r->next;
        chord_row_release(r);
    }
}

static void chord_geometry(round_list_t *list, round_chord_row_t *r, const lv_area_t *area)
{
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    /* Ancestor navigation can translate the whole viewport without a SCROLL
     * event. Always map the immutable source from its current screen centre. */
    int center2 = area->y1 + area->y2 + 1;
    unsigned bucket = (abs(center2 - ORB_SCREEN_D) + 2 * ROUND_CHORD_STEP - 1) / (2 * ROUND_CHORD_STEP);
    bucket = LV_MIN(bucket, ROUND_CHORD_BUCKETS - 1);
    r->current_width = r->shape ? r->shape->chord[bucket] : r->height;
    round_transform(list, r->current_width, r->height, center2, &r->scale, &r->opa);
    float remaining = ORB_SCREEN_R - list->config.margin - fabsf(center2 * .5f - ORB_SCREEN_R);
    int cap_scale = (int)(LV_MAX(0.f, remaining) * 512 / r->height);
    if (r->scale > cap_scale) r->scale = 0;
    /* Once even a circular cap cannot fit with readable text, hide the
     * preview rather than drawing beyond the safe circle or below20px. */
    if (r->scale < r->minimum_scale) r->scale = 0;
    if (center2 < 0 || center2 >= ROUND_CENTERS || round_row_hidden(r->obj)) r->scale = 0;
    int sw = r->current_width * r->scale / 256, sh = r->height * r->scale / 256;
    r->presented = (lv_area_t){area->x1 + (lv_area_get_width(area) - sw) / 2,
                              area->y1 + (lv_area_get_height(area) - sh) / 2, 0, 0};
    r->presented.x2 = r->presented.x1 + sw - 1;
    r->presented.y2 = r->presented.y1 + sh - 1;
    scroll_stage_record(started, &s_scroll_diagnostics.geometry);
}

static void round_chord_update(round_list_t *list, lv_obj_t *row, round_shape_t *shape, const lv_area_t *area)
{
    round_chord_row_t *r = round_chord_find(list, row);
    if (list->config.visible_cache) {
        chord_guard_row(list, row);
        if (!round_chord_visible(list, area)) {
            if (r && (round_chord_can_prepare(list) || list->cache_prepare)) {
                /* A retained departed row is released beyond the lookahead
                 * window, or once idle shows its content changed. */
                if (!round_chord_window(list, area) || (round_chord_can_prepare(list) &&
                    r->signature != chord_hash(row, area, 2166136261u, true))) {
                    round_chord_row_t **slot = &list->rows;
                    while (*slot != r) slot = &(*slot)->next;
                    *slot = r->next;
                    chord_row_release(r);
                } else {
                    r->shape = shape;
                    chord_pose_release(r);
                }
            }
            return;
        }
    }
    lv_area_t old_area = r ? r->presented : (lv_area_t){0};
    uint16_t old_scale = r ? r->scale : 0;
    lv_opa_t old_opa = r ? r->opa : 0;
    int old_width = r ? r->current_width : 0;
    bool source_changed = !r;
    if (r && round_chord_can_prepare(list) &&
        r->signature != chord_hash(row, area, 2166136261u, true)) {
        /* Explicit content/layout changes invalidate this source only.
         * Scroll never hashes, typesets, allocates or captures a row. */
        round_chord_row_t **slot = &list->rows;
        while (*slot != r) slot = &(*slot)->next;
        *slot = r->next;
        lv_obj_remove_event_cb_with_user_data(row, chord_row_event, list);
        chord_row_release(r);
        r = NULL;
        source_changed = true;
    }
    /* New content is prepared by apply/layout, never admitted by SCROLL. */
    if (!r && (round_chord_can_prepare(list) || list->cache_prepare)) {
        /* Guarded semantic rows normally draw through the compositor. Lift
         * suppression only for this explicit off-refresh source capture. */
        lv_obj_remove_event_cb_with_user_data(row, chord_row_event, list);
        uint64_t started = orb_ui_scroll_diagnostics_clock();
        r = round_chord_create(list, row, area, true);
        scroll_stage_record(started, &s_scroll_diagnostics.source_prepare);
        if (r) list->row_bytes = LV_MAX(list->row_bytes, orb_round_source_bytes(r));
        chord_guard_row(list, row);
    }
    if (!r) return;
    /* A retained lookahead row takes its worker pose on entering. SCROLL never
     * allocates; until the next preparation slice it uses the exact serial path. */
    if (list->config.visible_cache && !r->draw_pose &&
        (round_chord_can_prepare(list) || list->cache_prepare)) chord_pose_prepare(r);
    r->shape = shape;
    chord_geometry(list, r, area);
    bool moved = source_changed || old_scale != r->scale || old_opa != r->opa || old_width != r->current_width ||
        old_area.x1 != r->presented.x1 || old_area.y1 != r->presented.y1 ||
        old_area.x2 != r->presented.x2 || old_area.y2 != r->presented.y2;
    if (moved) {
        if (old_scale) lv_obj_invalidate_area(list->obj, &old_area);
        if (r->scale) lv_obj_invalidate_area(list->obj, &r->presented);
    }
}

/* Lookahead priority: distance, doubled behind the last scroll direction, so
 * upcoming rows are captured first and departed ones leave the budget first. */
static int32_t round_chord_priority(const round_list_t *list, const lv_area_t *area)
{
    int32_t distance = round_chord_distance(list, area);
    lv_area_t viewport; lv_obj_get_coords(list->obj, &viewport);
    bool behind = (list->scroll_direction > 0 && area->y2 < viewport.y1) ||
        (list->scroll_direction < 0 && area->y1 > viewport.y2);
    return behind ? distance * 2 : distance;
}

static void round_chord_unlink(round_list_t *list, round_chord_row_t *r)
{
    round_chord_row_t **slot = &list->rows;
    while (*slot != r) slot = &(*slot)->next;
    *slot = r->next;
    chord_row_release(r);
}

/* Lowest-priority retained row outside the viewport and the bytes retained. */
static round_chord_row_t *round_chord_retained(round_list_t *list, size_t *bytes, int32_t *priority)
{
    round_chord_row_t *last = NULL;
    *bytes = 0; *priority = -1;
    for (round_chord_row_t *r = list->rows; r; r = r->next) {
        lv_area_t area; lv_obj_get_coords(r->obj, &area);
        if (!round_chord_distance(list, &area)) continue;
        *bytes += orb_round_source_bytes(r);
        int32_t value = round_chord_priority(list, &area);
        if (value > *priority) { *priority = value; last = r; }
    }
    return last;
}

/* One slice: trim departed sources to budget, then, once the display is idle,
 * capture the most urgent missing row in the window, displacing a less urgent
 * retained one when full. Returns true while further lookahead remains. */
static bool round_chord_lookahead(round_list_t *list)
{
    if (!list->config.visible_cache) return false;
    size_t bytes;
    int32_t last_priority;
    round_chord_row_t *last;
    while ((last = round_chord_retained(list, &bytes, &last_priority)) &&
           bytes > ORB_ROUND_LIST_LOOKAHEAD_BUDGET) round_chord_unlink(list, last);
    if (!round_chord_can_prepare(list)) return false;
    if (!orb_round_idle()) return true;
    lv_obj_t *best = NULL;
    lv_area_t best_area = {0};
    int32_t urgent = INT32_MAX;
    for (unsigned i = 0; i < lv_obj_get_child_count(list->obj); i++) {
        lv_obj_t *row = lv_obj_get_child(list->obj, i);
        if (lv_obj_is_floating(row) || lv_obj_is_hidden(row) || round_chord_find(list, row)) continue;
        lv_area_t area; lv_obj_get_coords(row, &area);
        if (!round_chord_distance(list, &area) || !round_chord_window(list, &area)) continue;
        int32_t value = round_chord_priority(list, &area);
        if (value < urgent) { urgent = value; best = row; best_area = area; }
    }
    if (!best) return false;
    /* Admit only an expected fit: trims never discard a fresh capture. */
    if (bytes + list->row_bytes > ORB_ROUND_LIST_LOOKAHEAD_BUDGET) {
        if (!last || last_priority <= urgent) return false;
        bytes -= orb_round_source_bytes(last);
        round_chord_unlink(list, last);
        if (bytes + list->row_bytes > ORB_ROUND_LIST_LOOKAHEAD_BUDGET) return true;
    }
    lv_obj_remove_event_cb_with_user_data(best, chord_row_event, list);
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    round_chord_row_t *r = round_chord_create(list, best, &best_area, false);
    scroll_stage_record(started, &s_scroll_diagnostics.source_lookahead);
    chord_guard_row(list, best);
    if (!r) return false; /* OOM: entering rows retain the original timer path. */
    /* round_chord_create counted an admission build; report it separately. */
    list->source_builds--;
    list->lookahead_builds++;
    list->row_bytes = LV_MAX(list->row_bytes, orb_round_source_bytes(r));
    r->shape = round_shape(list, r->width, r->height, true);
    chord_geometry(list, r, &best_area);
    return true;
}

static void chord_row_event(lv_event_t *e)
{
    round_list_t *list = lv_event_get_user_data(e);
    round_chord_row_t *r = round_chord_find(list, lv_event_get_current_target_obj(e));
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    if (r && r->capturing) return;
    if (lv_obj_is_floating(row) && lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_COVER_CHECK) {
        /* The list is the compositor. A logical row can never occlude it. */
        lv_event_set_cover_res(e, LV_COVER_RES_NOT_COVER);
        lv_event_stop_processing(e);
    } else if (code == LV_EVENT_DRAW_MAIN) {
        lv_event_stop_processing(e);
    } else if (code == LV_EVENT_DRAW_MAIN_END) {
        /* lv_obj_redraw restores the caller's clip on return. This prevents
         * a second, native child rendering without hiding semantic content. */
        lv_event_get_layer(e)->_clip_area = (lv_area_t){1, 1, 0, 0};
    } else if (code == LV_EVENT_DELETE && r) {
        round_chord_row_t **slot = &list->rows;
        while (*slot && *slot != r) slot = &(*slot)->next;
        if (*slot) *slot = r->next;
        chord_row_release(r);
    }
}

static uint16_t chord_blend(uint16_t bg, uint16_t fg, unsigned a)
{
    unsigned inv = 255 - a;
    return (uint16_t)(((((bg >> 11) * inv + (fg >> 11) * a + 127) / 255) << 11) |
        (((((bg >> 5) & 63) * inv + ((fg >> 5) & 63) * a + 127) / 255) << 5) |
        (((bg & 31) * inv + (fg & 31) * a + 127) / 255));
}

static int chord_child_width(const round_chord_row_t *r, const round_chord_child_t *c)
{
    if (!c->flexible || !r->scale) return c->width;
    int width = LV_MAX(0, c->width + r->current_width - r->width);
    int source_x = c->x;
    if (c->trailing) {
        /* The title gives up its lane first. Only after it is exhausted may
         * a measured value rail shrink; keep it clear of the leading disc
         * (and its focus outline), with its trailing edge anchored. */
        int leading = 16;
        for (unsigned i = 0; i < r->count; i++) {
            const round_chord_child_t *other = &r->children[i];
            if (!other->hidden && !other->trailing && !other->flexible)
                leading = LV_MAX(leading, other->x + other->width + 14);
        }
        int trailing = r->width - c->x - c->width;
        width = LV_MIN(c->width, LV_MAX(0, r->current_width - leading - trailing));
        source_x += r->current_width - r->width + c->width - width;
    }
    int x = r->presented.x1 + source_x * r->scale / 256;
    int y = r->presented.y1 + c->y * r->scale / 256;
    int height = c->height * r->scale / 256;
    /* Tall AX3 line boxes can be narrower than the capsule. HIG §3 checks
     * both ends of every complete line, even when the cap itself fits. */
    int chord = orb_chord_width(y, y + height - 1, ORB_RIM_MARGIN_TEXT);
    int right = ORB_SCREEN_R + chord / 2;
    width = LV_MIN(width, LV_MAX(0, (right - x) * 256 / r->scale));
    if (c->bounded_ink && c->dots && c->cut && width < (int)c->dots->header.w &&
        c->cut[chord_bucket(c->width, width) * c->lines] != UINT16_MAX) {
        /* An overflowing preview cannot show a chopped ellipsis. A short
         * native value such as4 has no cut and keeps its complete ink. */
        return 0;
    }
    return width;
}

static int chord_inset(const round_chord_row_t *r, int y)
{
    int sh = lv_area_get_height(&r->presented);
    int radius = LV_MIN(lv_obj_get_style_radius(r->obj, 0), r->height / 2) * r->scale / 256;
    float dy = fabsf(y - r->presented.y1 + .5f - sh * .5f) - (sh * .5f - radius);
    return dy > 0 ? (int)ceilf(radius - sqrtf(LV_MAX(0.f, radius * radius - dy * dy))) : 0;
}

/* Source pixels retain their compact RGB565 format in all destinations.
 * Snapshot/transition layers use32bit pixels; dispatching here preserves the
 * same matte presentation without allocating another source or draw task. */
static uint16_t chord_pixel_read(const void *pixels, int x, lv_color_format_t format)
{
    if (format == LV_COLOR_FORMAT_ARGB8888 || format == LV_COLOR_FORMAT_XRGB8888) {
        const lv_color32_t *pixel = (const lv_color32_t *)pixels + x;
        return lv_color_to_u16(lv_color_make(pixel->red, pixel->green, pixel->blue));
    }
    uint16_t pixel = ((const uint16_t *)pixels)[x];
    return format == LV_COLOR_FORMAT_RGB565_SWAPPED ? (uint16_t)((pixel << 8) | (pixel >> 8)) : pixel;
}

static void chord_pixel_write(void *pixels, int x, uint16_t color, lv_color_format_t format)
{
    if (format == LV_COLOR_FORMAT_ARGB8888 || format == LV_COLOR_FORMAT_XRGB8888) {
        ((lv_color32_t *)pixels)[x] = lv_color32_make((color >> 11) * 255 / 31,
            ((color >> 5) & 63) * 255 / 63, (color & 31) * 255 / 31, LV_OPA_COVER);
    } else {
        ((uint16_t *)pixels)[x] = format == LV_COLOR_FORMAT_RGB565_SWAPPED ?
            (uint16_t)((color << 8) | (color >> 8)) : color;
    }
}

static void chord_blit(const round_chord_row_t *r, const round_chord_child_t *c, lv_layer_t *layer,
                        const lv_area_t *clip, int width, unsigned bucket)
{
    if (width <= 0) return;
    int x = c->x - c->pad + (c->trailing ? r->current_width - r->width : 0);
    if (c->trailing && c->flexible) x += c->width - width;
    lv_area_t dst = {r->presented.x1 + x * r->scale / 256,
                     r->presented.y1 + (c->y - c->pad) * r->scale / 256, 0, 0};
    dst.x2 = dst.x1 + (width + 2 * c->pad) * r->scale / 256 - 1;
    dst.y2 = dst.y1 + (c->height + 2 * c->pad) * r->scale / 256 - 1;
    lv_area_t a;
    if (!lv_area_intersect(&a, &dst, clip)) return;
    unsigned opacity = (unsigned)r->opa * lv_obj_get_style_opa(r->obj, 0) / 255;
    opacity = opacity * layer->opa / 255;
    uint16_t live_bg = lv_obj_get_style_bg_opa(r->obj, 0) ?
        lv_color_to_u16(lv_obj_get_style_bg_color(r->obj, 0)) : 0;
    for (int py = a.y1; py <= a.y2; py++) {
        void *out = lv_draw_layer_go_to_xy(layer, a.x1 - layer->buf_area.x1, py - layer->buf_area.y1);
        int sy = c->y - c->pad + (py - dst.y1) * 256 / r->scale;
        if (sy < 0 || sy >= r->height) continue;
        /* Disjoint ink bands must not restore a neighboring lane's fragments
         * after its cut, even when their leading rectangles overlap. */
        if (c->bounded_ink && (sy < c->y + c->ink_top || sy > c->y + c->ink_bottom)) continue;
        chord_source_cursor_t source = chord_source_line(r, NULL, sy);
        int inset = chord_inset(r, py);
        unsigned line = chord_text_line(c, sy);
        int cut = c->flexible && c->dots && c->cut && line < c->lines ? c->cut[bucket * c->lines + line] : -1;
        if (cut == ORB_ROUND_CUT_HIDDEN) continue;
        if (cut == UINT16_MAX) cut = -1;
        int dot_y = sy - c->y - (int)line * c->line_height;
        for (int px = a.x1; px <= a.x2; px++) {
            if (px < r->presented.x1 + inset || px > r->presented.x2 - inset) continue;
            int sx = (px - dst.x1) * 256 / r->scale;
            uint16_t color;
            if (cut >= 0 && sx >= cut) {
                int dx = sx - cut;
                if (!c->dots || dx < 0 || dx >= (int)c->dots->header.w ||
                    dot_y < 0 || dot_y >= (int)c->dots->header.h) continue;
                const lv_color32_t *dot = (const lv_color32_t *)((const uint8_t *)c->dots->data + dot_y * c->dots->header.stride) + dx;
                uint16_t old = chord_pixel_read(out, px - a.x1, layer->color_format);
                color = chord_blend(old, lv_color_to_u16(lv_color_make(dot->red, dot->green, dot->blue)), dot->alpha * opacity / 255);
            } else {
                int sx0 = c->x - c->pad + sx;
                if (sx0 < 0 || sx0 >= r->width || sy < 0 || sy >= r->height) continue;
                /* The source includes its matte background. Composite it
                 * once over black, rather than dimming it over an already
                 * dimmed capsule (which produces visible label rectangles). */
                uint16_t pixel = chord_source_pixel(r, &source, sx0);
                if (pixel == r->source_bg) pixel = live_bg;
                color = chord_blend(0, pixel, opacity);
            }
            chord_pixel_write(out, px - a.x1, color, layer->color_format);
        }
    }
}

static uint16_t chord_565_order(uint16_t color, bool swapped)
{
    return swapped ? (uint16_t)((color << 8) | (color >> 8)) : color;
}

static uint16_t chord_565_color(const chord_565_t *paint, uint16_t color)
{
    return paint->red[color >> 11] | paint->green[(color >> 5) & 63] | paint->blue[color & 31];
}

static void chord_565_prepare(const round_chord_row_t *r, lv_layer_t *layer, chord_565_t *paint)
{
    unsigned opacity = (unsigned)r->opa * lv_obj_get_style_opa(r->obj, 0) / 255;
    opacity = opacity * layer->opa / 255;
    bool swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint16_t live_bg = lv_obj_get_style_bg_opa(r->obj, 0) ?
        lv_color_to_u16(lv_obj_get_style_bg_color(r->obj, 0)) : 0;
    if (paint->valid && paint->opacity == opacity && paint->swapped == swapped &&
        paint->live_bg == live_bg) return;
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    paint->valid = true;
    paint->opacity = opacity;
    paint->swapped = swapped;
    paint->live_bg = live_bg;
    for (unsigned i = 0; i < 64; ++i) {
        unsigned channel = (i * paint->opacity + 127) / 255;
        paint->green[i] = chord_565_order((uint16_t)(channel << 5), paint->swapped);
        if (i < 32) {
            paint->red[i] = chord_565_order((uint16_t)(channel << 11), paint->swapped);
            paint->blue[i] = chord_565_order((uint16_t)channel, paint->swapped);
        }
    }
    paint->background = chord_565_color(paint, live_bg);
    for (unsigned i = 0; i < r->palette_count; ++i)
        paint->palette[i] = chord_565_color(paint, r->palette[i] == r->source_bg ? live_bg : r->palette[i]);
    scroll_stage_record(started, &s_scroll_diagnostics.palette_prepare);
}

static void chord_565_fill(uint16_t *out, unsigned count, uint16_t color)
{
    /* The band may start at an odd pixel. Only aligned pairs use wide stores. */
    if (count && ((uintptr_t)out & 3)) { *out++ = color; --count; }
    uint32_t pair = (uint32_t)color | ((uint32_t)color << 16);
    uint32_t *wide = (uint32_t *)out;
    while (count >= 2) { *wide++ = pair; count -= 2; }
    if (count) *(uint16_t *)wide = color;
}

/* A capsule's X clip depends only on its displayed height, radius and
 * scale, not its Y translation or band. Prepare the complete bounded table
 * once at a geometry boundary; children and later bands only read it. */
static void chord_565_prepare_insets(round_chord_row_t *r)
{
    int radius = lv_obj_get_style_radius(r->obj, 0);
    if (r->inset_scale == r->scale && r->inset_radius == radius) return;
    int height = lv_area_get_height(&r->presented);
    for (int y = 0; y < height; ++y) r->insets[y] = chord_inset(r, r->presented.y1 + y);
    r->inset_scale = r->scale;
    r->inset_radius = radius;
}

static void chord_565_sparse_native(const round_chord_row_t *r, chord_source_cursor_t source,
                                     int source_x, uint16_t *out, int start, int end,
                                     const chord_565_t *paint)
{
    /* At native scale every destination pixel advances one source pixel.
     * Intersect exact integer run endpoints instead of walking an identity
     * map twice before visiting the same ink pixels a third time to write. */
    while (source.next < source.end && start < end) {
        int run_start = source.next[0], length = source.next[1];
        const uint16_t *pixels = source.next + 2;
        source.next = pixels + (r->palette_count ? (length + 1) / 2 : length);
        start = LV_MAX(start, run_start - source_x);
        int finish = LV_MIN(end, run_start + length - source_x);
        if (start >= finish) continue;
        int offset = source_x + start - run_start;
        if (r->palette_count) {
            const uint8_t *indices = (const uint8_t *)pixels + offset;
            while (start < finish) out[start++] = paint->palette[*indices++];
        } else if (paint->opacity == 255 && !paint->swapped) {
            memcpy(out + start, pixels + offset, (size_t)(finish - start) * sizeof(*out));
            start = finish;
        } else {
            pixels += offset;
            while (start < finish) out[start++] = chord_565_color(paint, *pixels++);
        }
    }
}

static void chord_565_sparse_scaled(const round_chord_row_t *r, chord_source_cursor_t source,
                                     int source_x, const uint16_t *map, uint16_t *out,
                                     int start, int end, const chord_565_t *paint)
{
    while (source.next < source.end && start < end) {
        unsigned run_start = source.next[0], length = source.next[1];
        const uint16_t *pixels = source.next + 2;
        source.next = pixels + (r->palette_count ? (length + 1) / 2 : length);
        while (start < end && source_x + map[start] < (int)run_start) ++start;
        int finish = start;
        while (finish < end && source_x + map[finish] < (int)(run_start + length)) ++finish;
        if (r->palette_count) {
            const uint8_t *indices = (const uint8_t *)pixels;
            for (; start < finish; ++start) out[start] = paint->palette[indices[source_x + map[start] - run_start]];
        } else {
            for (; start < finish; ++start) out[start] = chord_565_color(paint, pixels[source_x + map[start] - run_start]);
        }
    }
}

static void chord_blit565(const round_chord_row_t *r, const round_chord_child_t *c, lv_layer_t *layer,
                          const lv_area_t *clip, int width, unsigned bucket, const chord_565_t *paint)
{
    if (width <= 0) return;
    int x = c->x - c->pad + (c->trailing ? r->current_width - r->width : 0);
    if (c->trailing && c->flexible) x += c->width - width;
    lv_area_t dst = {r->presented.x1 + x * r->scale / 256,
                     r->presented.y1 + (c->y - c->pad) * r->scale / 256, 0, 0};
    dst.x2 = dst.x1 + (width + 2 * c->pad) * r->scale / 256 - 1;
    dst.y2 = dst.y1 + (c->height + 2 * c->pad) * r->scale / 256 - 1;
    lv_area_t a;
    if (!lv_area_intersect(&a, &dst, clip)) return;
    int count = lv_area_get_width(&a);
    if (count > ORB_SCREEN_D) {
        chord_blit(r, c, layer, clip, width, bucket);
        return;
    }
    uint16_t fallback_map[ORB_SCREEN_D];
    const uint16_t *map;
    int offset = a.x1 - dst.x1;
    if (r->draw_map && offset >= 0 && offset + count <= ORB_SCREEN_D) {
        map = r->draw_map + offset;
    } else {
        /* Out-of-table clipped children use this bounded compatibility map. */
        if (r->scale == 256) {
            for (int i = 0; i < count; ++i) fallback_map[i] = a.x1 + i - dst.x1;
        } else {
            for (int i = 0; i < count; ++i) fallback_map[i] = (a.x1 + i - dst.x1) * 256 / r->scale;
        }
        map = fallback_map;
    }
    int source_x = c->x - c->pad;
    for (int py = a.y1; py <= a.y2; ++py) {
        int sy = c->y - c->pad + (py - dst.y1) * 256 / r->scale;
        if (sy < 0 || sy >= r->height) continue;
        if (c->bounded_ink && (sy < c->y + c->ink_top || sy > c->y + c->ink_bottom)) continue;
        int inset = r->insets[py - r->presented.y1];
        int first = LV_MAX(0, r->presented.x1 + inset - a.x1);
        int end = LV_MIN(count, r->presented.x2 - inset - a.x1 + 1);
        if (first >= end) continue;
        uint16_t *out = lv_draw_layer_go_to_xy(layer, a.x1 - layer->buf_area.x1, py - layer->buf_area.y1);
        unsigned line = chord_text_line(c, sy);
        int cut = c->flexible && c->dots && c->cut && line < c->lines ? c->cut[bucket * c->lines + line] : -1;
        if (cut == ORB_ROUND_CUT_HIDDEN) continue;
        if (cut == UINT16_MAX) cut = -1;
        int ink_end = end;
        if (cut >= 0) {
            ink_end = first;
            while (ink_end < end && map[ink_end] < cut) ++ink_end;
        }
        int start = first;
        while (start < ink_end && source_x + map[start] < 0) ++start;
        while (ink_end > start && source_x + map[ink_end - 1] >= r->width) --ink_end;
        if (start < ink_end) {
            chord_source_cursor_t source = chord_source_line(r, c, sy);
            if (!r->sparse) {
                if (r->scale == 256 && paint->opacity == 255 && !paint->swapped &&
                    paint->background == r->source_bg) {
                    memcpy(out + start, source.pixels + source_x + map[start],
                           (size_t)(ink_end - start) * sizeof(*out));
                } else {
                    for (int i = start; i < ink_end; ++i) {
                        uint16_t color = source.pixels[source_x + map[i]];
                        out[i] = color == r->source_bg ? paint->background : chord_565_color(paint, color);
                    }
                }
            } else {
                /* Paint sparse gaps in one contiguous pass, then only visit
                 * occupied source spans. The monotone X map is shared by all
                 * scanlines of this child, never divided per pixel. */
                chord_565_fill(out + start, ink_end - start, paint->background);
                if (r->scale == 256)
                    chord_565_sparse_native(r, source, source_x + map[0], out, start, ink_end, paint);
                else chord_565_sparse_scaled(r, source, source_x, map, out, start, ink_end, paint);
            }
        }
        /* Ellipses retain their original ARGB alpha and blend with the exact
         * current matte/ink. This tiny terminal span is the only read/modify
         * part of the565 painter; no backdrop assumption is needed. */
        if (cut >= 0) {
            int dot_y = sy - c->y - (int)line * c->line_height;
            if (dot_y < 0 || dot_y >= (int)c->dots->header.h) continue;
            const lv_color32_t *dots = (const lv_color32_t *)((const uint8_t *)c->dots->data +
                dot_y * c->dots->header.stride);
            int i = first;
            while (i < end && map[i] < cut) ++i;
            for (; i < end; ++i) {
                int dx = map[i] - cut;
                if (dx >= (int)c->dots->header.w) break;
                const lv_color32_t *dot = dots + dx;
                uint16_t old = chord_565_order(out[i], paint->swapped);
                uint16_t color = chord_blend(old, lv_color_to_u16(lv_color_make(dot->red, dot->green, dot->blue)),
                    dot->alpha * paint->opacity / 255);
                out[i] = chord_565_order(color, paint->swapped);
            }
        }
    }
}

/* All fields consumed here belong to the pose or its leased source. In
 * particular, this callback never asks LVGL for an object, style or geometry. */
static void chord_pose_draw(lv_layer_t *layer, const lv_area_t *clip, const void *payload)
{
    const round_chord_row_t *pose = payload;
    int sw = lv_area_get_width(&pose->presented);
    for (int y = clip->y1; y <= clip->y2; ++y) {
        int inset = pose->insets[y - pose->presented.y1];
        int left = LV_MAX(clip->x1, pose->presented.x1 + inset);
        int right = LV_MIN(clip->x2, pose->presented.x1 + sw - inset - 1);
        if (left > right) continue;
        uint16_t *out = lv_draw_layer_go_to_xy(layer, left - layer->buf_area.x1, y - layer->buf_area.y1);
        chord_565_fill(out, right - left + 1, pose->draw_fill);
    }
    for (unsigned i = 0; i < pose->count; ++i) {
        const round_chord_child_t *c = &pose->children[i];
        if (!c->hidden) chord_blit565(pose, c, layer, clip, c->draw_width, c->draw_bucket, &pose->paint);
    }
}

static void chord_pose_retire(void *payload)
{
    round_chord_row_t *pose = payload;
    if (--pose->draw_leases) return;
    round_chord_row_t *source = pose->lease_source;
    source->draw_pose_busy = false;
    --source->draw_leases;
    if (source->retired) chord_row_release(source);
}

static void chord_draw_drain(lv_layer_t *layer)
{
    while (layer->draw_task_head) {
        lv_draw_dispatch_layer(layer->display, layer);
        lv_draw_dispatch(); /* Service child layers before retrying a blocked task. */
        lv_draw_wait_for_finish();
    }
}

static void chord_pose_geometry(round_chord_row_t *pose, const round_chord_row_t *r, bool first)
{
    if (!first && pose->current_width == r->current_width && pose->scale == r->scale &&
        lv_area_is_equal(&pose->presented, &r->presented)) return;
    uint64_t started = orb_ui_scroll_diagnostics_clock();
    pose->current_width = r->current_width;
    pose->scale = r->scale;
    pose->presented = r->presented;
    for (unsigned i = 0; i < pose->count; ++i) {
        round_chord_child_t *c = &pose->children[i];
        c->draw_width = chord_child_width(r, &r->children[i]);
        c->draw_bucket = chord_bucket(c->width, c->draw_width);
    }
    scroll_stage_record(started, &s_scroll_diagnostics.child_prepare);
}

static void chord_pose_tables(round_chord_row_t *pose, const round_chord_row_t *r, bool first)
{
    /* The source and child lanes are immutable. A retired pose reuses prepared
     * widths/cuts and colour tables across bands; actual screen coordinates
     * remain part of the width key because AX3 line boxes fit their own chord.
     * Merely keying by capsule width would change ink near the upper/lower rim. */
    if (first || pose->paint.opacity != r->paint.opacity || pose->paint.swapped != r->paint.swapped ||
        pose->paint.live_bg != r->paint.live_bg) pose->paint = r->paint;
    if (first || pose->inset_scale != r->inset_scale || pose->inset_radius != r->inset_radius) {
        memcpy(pose->insets, r->insets, (size_t)lv_area_get_height(&r->presented) * sizeof(*r->insets));
        pose->inset_scale = r->inset_scale;
        pose->inset_radius = r->inset_radius;
    }
    uint16_t *map = pose->insets + r->height;
    if (pose->draw_map_scale != r->scale) {
        if (r->scale == 256) {
            for (unsigned i = 0; i < ORB_SCREEN_D; ++i) map[i] = i;
        } else {
            for (unsigned i = 0; i < ORB_SCREEN_D; ++i) map[i] = i * 256 / r->scale;
        }
    }
    pose->draw_map = map;
    pose->draw_map_scale = r->scale;
}

static bool chord_pose_dispatch(round_chord_row_t *r, lv_layer_t *layer, const lv_area_t *clip, uint16_t fill)
{
    if (!r->draw_pose || lv_area_get_width(clip) > ORB_SCREEN_D) return false;
    /* Nested snapshots can revisit a row before an older layer retires. The
     * slot is bounded; fence all draw work before overwriting a leased pose. */
    if (r->draw_pose_busy) {
        lv_draw_wait_for_finish();
        lv_draw_dispatch();
        if (r->draw_pose_busy) return false;
    }
    round_chord_row_t *pose = r->draw_pose;
    bool first = !pose->draw_map_scale;
    if (first) {
        *pose = *r;
        pose->next = NULL;
        pose->obj = NULL;
        pose->shape = NULL;
        pose->draw_pose = NULL;
        pose->lease_source = r;
        pose->insets = (uint16_t *)(pose->children + r->count);
        memcpy(pose->children, r->children, r->count * sizeof(r->children[0]));
        for (unsigned i = 0; i < pose->count; ++i) pose->children[i].obj = NULL;
    }
    pose->draw_leases = 1; /* Creator lease protects partial enqueue/fallback. */
    chord_pose_geometry(pose, r, first);
    chord_pose_tables(pose, r, first);
    pose->draw_fill = fill;
    r->draw_pose_busy = true;
    ++r->draw_leases;
    lv_area_t slices[2] = {*clip, *clip};
    slices[0].y2 = (clip->y1 + clip->y2) / 2;
    slices[1].y1 = slices[0].y2 + 1;
    for (unsigned i = 0; i < 2; ++i) {
        if (slices[i].y1 > slices[i].y2) continue;
        ++pose->draw_leases;
        if (!lv_draw_custom(layer, &slices[i], chord_pose_draw, chord_pose_retire, pose)) {
            chord_pose_retire(pose); /* No ownership was transferred. */
            chord_draw_drain(layer);
            chord_pose_draw(layer, &slices[i], pose);
        }
    }
    chord_pose_retire(pose);
    return true;
}

static void round_chord_draw(round_list_t *list, lv_layer_t *layer)
{
    if (layer->opa <= LV_OPA_MIN) return;
    if (layer->color_format != LV_COLOR_FORMAT_RGB565 && layer->color_format != LV_COLOR_FORMAT_RGB565_SWAPPED &&
        layer->color_format != LV_COLOR_FORMAT_ARGB8888 && layer->color_format != LV_COLOR_FORMAT_XRGB8888) return;
    bool native565 = layer->color_format == LV_COLOR_FORMAT_RGB565 ||
        layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    if (!native565) chord_draw_drain(layer);
    if (!layer->draw_buf && !lv_draw_layer_alloc_buf(layer)) return;
    lv_area_t viewport;
    lv_obj_get_coords(list->obj, &viewport);
    if (!lv_area_intersect(&viewport, &viewport, &layer->_clip_area) ||
        !lv_area_intersect(&viewport, &viewport, &layer->buf_area)) return;
    for (round_chord_row_t *r = list->rows; r; r = r->next) {
        lv_area_t current; lv_obj_get_coords(r->obj, &current);
        /* Presentation is centred inside the logical row and never larger, so
         * a retained row outside this band's viewport has nothing to paint. */
        if (current.y2 < viewport.y1 || current.y1 > viewport.y2) continue;
        chord_geometry(list, r, &current);
        if (!r->scale || !r->opa || lv_obj_is_hidden(r->obj) || lv_obj_is_floating(r->obj)) continue;
        lv_area_t clip;
        if (!lv_area_intersect(&clip, &viewport, &r->presented)) continue;
        chord_565_t *paint = &r->paint;
        if (native565) {
            chord_565_prepare(r, layer, paint);
            chord_565_prepare_insets(r);
        }
        int sw = lv_area_get_width(&r->presented);
        lv_color_t bg = lv_obj_get_style_bg_color(r->obj, 0);
        unsigned opacity = (unsigned)r->opa * lv_obj_get_style_opa(r->obj, 0) / 255;
        opacity = opacity * layer->opa / 255;
        uint16_t fill = chord_blend(0, lv_color_to_u16(bg), opacity * lv_obj_get_style_bg_opa(r->obj, 0) / 255);
        if (native565 && chord_pose_dispatch(r, layer, &clip, chord_565_order(fill, paint->swapped))) continue;
        /* Reference path or bounded-pool/OOM fallback keeps normal ordering. */
        chord_draw_drain(layer);
        for (int y = clip.y1; y <= clip.y2; y++) {
            int inset = native565 ? r->insets[y - r->presented.y1] : chord_inset(r, y);
            int left = LV_MAX(clip.x1, r->presented.x1 + inset), right = LV_MIN(clip.x2, r->presented.x1 + sw - inset - 1);
            if (left > right) continue;
            void *out = lv_draw_layer_go_to_xy(layer, left - layer->buf_area.x1, y - layer->buf_area.y1);
            if (native565) chord_565_fill(out, right - left + 1, chord_565_order(fill, paint->swapped));
            else for (int x = left; x <= right; x++) chord_pixel_write(out, x - left, fill, layer->color_format);
        }
        for (unsigned i = 0; i < r->count; i++) {
            round_chord_child_t *c = &r->children[i];
            if (c->hidden) continue;
            int width = chord_child_width(r, c);
            unsigned bucket = chord_bucket(c->width, width);
            if (native565) chord_blit565(r, c, layer, &clip, width, bucket, paint);
            else chord_blit(r, c, layer, &clip, width, bucket);
        }
    }
}

static bool round_chord_presentation(round_list_t *list, lv_obj_t *row, lv_area_t *logical,
                                     lv_area_t *presented, uint16_t *scale, lv_opa_t *opacity)
{
    round_chord_row_t *r = round_chord_find(list, row);
    if (!r) return false;
    lv_obj_get_coords(row, logical);
    /* A retained lookahead row outside the viewport is not presented or hit. */
    if (list->config.visible_cache && !round_chord_visible(list, logical)) return false;
    chord_geometry(list, r, logical);
    if (!r->scale || r->opa <= LV_OPA_MIN) return false;
    *presented = r->presented; *scale = r->scale;
    if (opacity) *opacity = r->opa;
    /* child_bounds maps relative to the narrower presentation origin. */
    return true;
}

static void round_chord_child(round_list_t *list, lv_obj_t *row, lv_obj_t *child, lv_area_t *bounds)
{
    round_chord_row_t *r = round_chord_find(list, row);
    if (!r) return;
    if (child == row) { bounds->x2 = bounds->x1 + r->current_width - 1; return; }
    lv_obj_t *direct = child;
    while (lv_obj_get_parent(direct) != row) direct = lv_obj_get_parent(direct);
    for (unsigned i = 0; i < r->count; i++) if (r->children[i].obj == direct) {
        round_chord_child_t *c = &r->children[i];
        lv_area_t logical, live;
        lv_obj_get_coords(row, &logical);
        lv_obj_get_coords(direct, &live);
        /* Content changes received during motion retain the previous source.
         * Its cached child geometry, including an aligned child's old y, must
         * remain the spatial anchor until the idle source rebuild. */
        int dx = logical.x1 + c->x - live.x1, dy = logical.y1 + c->y - live.y1;
        bounds->x1 += dx; bounds->x2 += dx; bounds->y1 += dy; bounds->y2 += dy;
        if (child == direct) {
            bounds->x2 = bounds->x1 + c->width - 1;
            bounds->y2 = bounds->y1 + c->height - 1;
        }
        int delta = r->current_width - r->width;
        if (c->trailing) {
            bounds->x1 += delta; bounds->x2 += delta;
            if (child == direct && c->flexible) {
                int width = chord_child_width(r, c);
                bounds->x1 += c->width - width;
                bounds->x2 = LV_MAX(bounds->x1, bounds->x1 + width - 1);
            }
        } else if (child == direct && c->flexible)
            bounds->x2 = LV_MAX(bounds->x1, bounds->x1 + chord_child_width(r, c) - 1);
        return;
    }
}

static void round_chord_stats(round_list_t *list, orb_round_list_cache_stats_t *out)
{
    out->source_builds = list->source_builds;
    out->lookahead_builds = list->lookahead_builds;
    for (round_chord_row_t *r = list->rows; r; r = r->next) {
        lv_area_t area; lv_obj_get_coords(r->obj, &area);
        if (round_chord_distance(list, &area)) out->retained_bytes += orb_round_source_bytes(r);
    }
    for (round_shape_t *s = list->shapes; s; s = s->next) out->width_table_bytes += sizeof(s->chord);
    for (round_chord_row_t *r = list->rows; r; r = r->next) {
        out->source_bytes += r->bytes;
        out->draw_pose_bytes += r->draw_pose_bytes;
        out->metadata_bytes += sizeof(*r) + r->source_run_index_bytes +
            r->count * sizeof(r->children[0]) +
            (size_t)r->height * sizeof(uint16_t);
        for (unsigned i = 0; i < r->count; i++) {
            if (r->children[i].cut)
                out->metadata_bytes += CHORD_TEXT_BUCKETS * r->children[i].lines * sizeof(uint16_t);
            lv_draw_buf_t *dots = r->children[i].dots;
            if (!dots) continue;
            bool counted = false;
            for (round_chord_row_t *previous = list->rows; previous != r; previous = previous->next)
                for (unsigned j = 0; j < previous->count; j++)
                    if (previous->children[j].dots == dots) counted = true;
            for (unsigned j = 0; j < i; j++) if (r->children[j].dots == dots) counted = true;
            if (!counted) out->source_bytes += dots->data_size + sizeof(lv_draw_buf_t);
        }
    }
}
