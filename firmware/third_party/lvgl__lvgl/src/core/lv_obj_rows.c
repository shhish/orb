/*
 * ORB PATCH: Prototype B's glyph-span content path, adapted to retained LVGL
 * rows rather than a duplicate scene. A cold label walk uses LVGL's own UTF-8,
 * fallback, kerning, bidi and line layout; warm bands blend pre-sized A4 glyphs.
 * No row-sized image is transformed. Static rows keep the existing raster.
 * HIG §§1,2,4: matte content, shared text styles, no decorative material.
 */
#include "lv_obj_rows_private.h"
#include "lv_obj_rows_average.h"
#include "lv_obj_rows_blend.h"
#include "lv_obj_private.h"
#include "lv_obj_draw_private.h"
#include "../draw/lv_draw_private.h"
#include "../draw/lv_draw_label_private.h"
#include "../misc/lv_area_private.h"
#include "../misc/lv_event_private.h"
#include "../../include/lvgl/draw/lv_draw_sw_profile.h"
#include <math.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#endif

#ifndef CONFIG_ORB_RENDER_ROWS_AT_SIZE
#define CONFIG_ORB_RENDER_ROWS_AT_SIZE 0
#endif

#if CONFIG_ORB_RENDER_ROWS_AT_SIZE
#define ROW_SIZES 16
#define ROW_MIN_SCALE 136
#define ROW_MAX_LETTERS 256
#define ROW_BUDGET (512u * 1024u)

typedef struct {
    uint16_t w, h;
    uint8_t * a4;
} row_size_t;

typedef struct row_glyph_t {
    struct row_glyph_t * next;
    lv_draw_buf_free_cb_t release;
    uint32_t hash, refs, bytes;
    uint64_t used;
    uint16_t w, h, line_height, nominal_px;
    uint8_t * original;
    row_size_t sizes[ROW_SIZES];
} row_glyph_t;

typedef struct {
    row_glyph_t * glyph;
    int16_t x, y;
    int16_t run_x, run_y, run_width;
    int16_t run_end, dot_advance;
    uint8_t ellipsis;
    bool focus;
    lv_color_t color;
    uint8_t opa;
} row_letter_t;

struct lv_obj_rows_t {
    lv_draw_buf_malloc_cb_t alloc;
    lv_draw_buf_free_cb_t release;
    uint16_t count, w, h, radius;
    lv_color_t bg;
    bool failed;
    lv_obj_t * focus;
    uint16_t preview_scale;
    lv_area_t focus_area;
    bool published;
    row_letter_t letters[ROW_MAX_LETTERS];
};

/* All mutation is on LVGL's owner thread, before RENDER_START. Glyphs remain
 * pinned by the rows that reference them; no font cache entry is retained. */
static row_glyph_t * s_glyphs;
static uint32_t s_bytes;
#ifndef ESP_PLATFORM
static uint64_t s_use;
#endif
static uint64_t touch(void)
{
#ifdef ESP_PLATFORM
    return orb_memory_cache_touch();
#else
    return ++s_use;
#endif
}
static lv_obj_raster_rows_stats_t s_stats;
static uint16_t (*s_font_size)(const lv_font_t * font);

void lv_obj_raster_rows_set_font_size_cb(uint16_t (*cb)(const lv_font_t * font))
{
    s_font_size = cb;
}

static void remove_glyph(row_glyph_t ** slot)
{
    row_glyph_t * glyph = *slot;
    *slot = glyph->next;
    s_bytes -= glyph->bytes;
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_ROW_FONTS, glyph->bytes);
#endif
    s_stats.glyphs--;
    glyph->release(glyph);
}

#ifdef ESP_PLATFORM
static row_glyph_t ** oldest_glyph(void)
{
    row_glyph_t ** victim = NULL;
    for(row_glyph_t ** slot = &s_glyphs; *slot; slot = &(*slot)->next) {
        if(!(*slot)->refs && (!victim || (*slot)->used < (*victim)->used)) victim = slot;
    }
    return victim;
}
static uint64_t cache_oldest(void * ctx)
{
    LV_UNUSED(ctx);
    row_glyph_t ** slot = oldest_glyph();
    return slot ? (*slot)->used : UINT64_MAX;
}
static bool cache_evict(void * ctx)
{
    LV_UNUSED(ctx);
    row_glyph_t ** slot = oldest_glyph();
    if(!slot) return false;
    remove_glyph(slot); s_stats.evictions++;
    return true;
}
#endif

static bool reserve(unsigned bytes)
{
    if(bytes > ROW_BUDGET) return false;
    while(bytes > ROW_BUDGET - s_bytes) {
        row_glyph_t ** victim = NULL;
        for(row_glyph_t ** slot = &s_glyphs; *slot; slot = &(*slot)->next) {
            if(!(*slot)->refs && (!victim || (*slot)->used < (*victim)->used)) victim = slot;
        }
        if(!victim) return false;
        remove_glyph(victim);
        s_stats.evictions++;
    }
#ifdef ESP_PLATFORM
    orb_memory_cache_register(ORB_MEMORY_ROW_FONTS, cache_oldest, cache_evict, NULL);
    return orb_memory_cache_admit(ORB_MEMORY_ROW_FONTS, bytes);
#else
    return true;
#endif
}

void lv_obj_raster_rows_drop_unused(void)
{
    row_glyph_t ** slot = &s_glyphs;
    while(*slot) {
        if(!(*slot)->refs) remove_glyph(slot);
        else slot = &(*slot)->next;
    }
}

#define mul255 lv_obj_rows_mul255
#define blend565 lv_obj_rows_blend565

static unsigned bucket_scale(unsigned bucket) { return ROW_MIN_SCALE + bucket * 8; }

static unsigned glyph_scale(const row_glyph_t * g, unsigned bucket)
{
    /* HIG §4 has a 20px text floor. Icons use the same baseline policy. This
     * changes small text at the rim; a row clip protects its circle chord. */
    unsigned floor = (20u * 256u + g->nominal_px - 1) / g->nominal_px;
    return LV_MIN(256u, LV_MAX(bucket_scale(bucket), floor));
}

static uint8_t nibble(const row_size_t * size, unsigned x, unsigned y)
{
    unsigned p = y * size->w + x;
    return ((p & 1) ? size->a4[p / 2] & 15 : size->a4[p / 2] >> 4) * 17;
}

static row_glyph_t * admit(lv_obj_rows_t * rows, lv_draw_glyph_dsc_t * dsc)
{
    lv_font_glyph_dsc_t * g = dsc->g;
    if(g->is_placeholder || !g->resolved_font || g->format >= LV_FONT_GLYPH_FORMAT_IMAGE) return NULL;
    unsigned nominal = s_font_size ? s_font_size(g->resolved_font) : 0;
    if(!nominal || nominal < 20) return NULL;
    const lv_draw_buf_t * source = lv_font_get_glyph_bitmap(g, dsc->_draw_buf);
    if(!source || !source->data || source->header.cf != LV_COLOR_FORMAT_A8) return NULL;
    uint32_t hash = 2166136261u;
    for(unsigned y = 0; y < g->box_h; y++) for(unsigned x = 0; x < g->box_w; x++)
        hash = (hash ^ source->data[y * source->header.stride + x]) * 16777619u;
    for(row_glyph_t * glyph = s_glyphs; glyph; glyph = glyph->next) {
        if(glyph->hash == hash && glyph->release == rows->release &&
           glyph->line_height == g->resolved_font->line_height &&
           glyph->nominal_px == nominal &&
           glyph->w == g->box_w && glyph->h == g->box_h) {
            bool same = true;
            for(unsigned y = 0; y < g->box_h && same; y++)
                same = lv_memcmp(glyph->original + y * g->box_w,
                                 source->data + y * source->header.stride, g->box_w) == 0;
            if(!same) continue;
            glyph->refs++;
            glyph->used = touch();
            return glyph;
        }
    }
    row_glyph_t model;
    lv_memzero(&model, sizeof(model));
    model.line_height = g->resolved_font->line_height;
    model.nominal_px = nominal;
    model.release = rows->release;
    model.w = g->box_w;
    model.h = g->box_h;
    uint32_t bytes = sizeof(model) + g->box_w * g->box_h;
    for(unsigned b = 0; b < ROW_SIZES; b++) {
        unsigned scale = glyph_scale(&model, b);
        model.sizes[b].w = LV_MAX(1u, (g->box_w * scale + 128) / 256);
        model.sizes[b].h = LV_MAX(1u, (g->box_h * scale + 128) / 256);
        if(!b || model.sizes[b].w != model.sizes[b - 1].w || model.sizes[b].h != model.sizes[b - 1].h)
            bytes += ((unsigned)model.sizes[b].w * model.sizes[b].h + 1) / 2;
    }
    if(!reserve(bytes)) return NULL;
    row_glyph_t * glyph = rows->alloc(bytes, LV_COLOR_FORMAT_A8);
    if(!glyph) {
#ifdef ESP_PLATFORM
        orb_memory_release(ORB_MEMORY_ROW_FONTS, bytes);
#endif
        return NULL;
    }
    *glyph = model;
    glyph->original = (uint8_t *)(glyph + 1);
    for(unsigned y = 0; y < g->box_h; y++)
        lv_memcpy(glyph->original + y * g->box_w, source->data + y * source->header.stride, g->box_w);
    uint8_t * data = glyph->original + g->box_w * g->box_h;
    for(unsigned b = 0; b < ROW_SIZES; b++) {
        row_size_t * size = &glyph->sizes[b];
        /* HIG's 20px floor and pixel rounding often map adjacent buckets to
         * the same dimensions. Area averaging depends only on those dimensions,
         * so share the immutable payload instead of resampling/copying it. */
        if(b && size->w == glyph->sizes[b - 1].w && size->h == glyph->sizes[b - 1].h) {
            size->a4 = glyph->sizes[b - 1].a4;
            s_stats.size_reuses++;
            continue;
        }
        size->a4 = data;
        s_stats.size_builds++;
        unsigned payload = ((unsigned)size->w * size->h + 1) / 2;
        lv_memzero(data, payload);
        /* Area averaging at cold admission is anti-aliasing, not a warm row
         * transform. Fixed Q8 overlap weights retain thin Inter strokes. */
        for(unsigned y = 0; y < size->h; y++) for(unsigned x = 0; x < size->w; x++) {
            unsigned x0 = x * g->box_w * 256u / size->w;
            unsigned x1 = (x + 1) * g->box_w * 256u / size->w;
            unsigned y0 = y * g->box_h * 256u / size->h;
            unsigned y1 = (y + 1) * g->box_h * 256u / size->h;
            /* >=136/256 buckets, rounded to nearest, have at most a 512-Q8
             * source footprint on either axis, including narrow glyphs. */
            unsigned alpha = lv_obj_rows_average_a8(source->data, source->header.stride, x0, x1, y0, y1);
            unsigned p = y * size->w + x;
            data[p / 2] |= ((alpha + 8) / 17) << ((p & 1) ? 0 : 4);
        }
        data += payload;
    }
    glyph->refs = 1;
    glyph->used = touch();
    glyph->bytes = bytes;
    glyph->hash = hash;
    glyph->next = s_glyphs;
    s_glyphs = glyph;
    s_bytes += bytes;
    s_stats.glyphs++;
    s_stats.peak_bytes = LV_MAX(s_stats.peak_bytes, s_bytes);
    return glyph;
}

typedef struct {
    lv_obj_rows_t * rows;
    lv_area_t origin;
    lv_area_t label;
    bool dots, focus;
} capture_t;

static void capture(lv_draw_task_t * task, lv_draw_glyph_dsc_t * dsc,
                    lv_draw_fill_dsc_t * fill, const lv_area_t * fill_area)
{
    capture_t * ctx = ((lv_draw_dsc_base_t *)task->draw_dsc)->user_data;
    lv_obj_rows_t * rows = ctx->rows;
    if(fill || fill_area || !dsc || rows->failed) { rows->failed = true; return; }
    if(rows->count == ROW_MAX_LETTERS || dsc->rotation || dsc->outline_stroke_width) {
        rows->failed = true;
        return;
    }
    row_glyph_t * glyph = admit(rows, dsc);
    if(!glyph) { rows->failed = true; return; }
    row_letter_t * letter = &rows->letters[rows->count++];
    letter->glyph = glyph;
    letter->x = dsc->letter_coords->x1 - ctx->origin.x1;
    letter->y = dsc->letter_coords->y1 - ctx->origin.y1;
    letter->run_x = ctx->label.x1 - ctx->origin.x1;
    letter->run_y = ctx->label.y1 - ctx->origin.y1;
    letter->run_width = lv_area_get_width(&ctx->label);
    letter->ellipsis = ctx->dots;
    letter->focus = ctx->focus;
    letter->color = dsc->color;
    letter->opa = dsc->opa;
}

static bool collect(lv_obj_rows_t * rows, lv_obj_t * obj, const lv_area_t * origin, bool root)
{
    if(lv_obj_is_hidden(obj)) return true;
    for(uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        uint32_t filter = lv_obj_get_event_dsc(obj, i)->filter & ~LV_EVENT_PREPROCESS;
        if(filter == LV_EVENT_ALL || (filter >= LV_EVENT_COVER_CHECK && filter <= LV_EVENT_DRAW_TASK_ADDED)) return false;
    }
    /* The adapter deliberately admits a small known style vocabulary. Complex
     * widgets/materials/draw callbacks retain the reference raster path. */
    if((lv_obj_get_ext_draw_size(obj) && !lv_obj_check_type(obj, &lv_label_class)) || lv_obj_get_style_border_width(obj, 0) ||
       lv_obj_get_style_outline_width(obj, 0) || lv_obj_get_style_shadow_width(obj, 0) ||
       lv_obj_get_style_transform_rotation(obj, 0) ||
       lv_obj_get_style_transform_scale_x(obj, 0) != 256 ||
       lv_obj_get_style_transform_scale_y(obj, 0) != 256 ||
       ((!root || lv_obj_get_style_bg_opa(obj, 0) != LV_OPA_TRANSP) &&
        lv_obj_get_style_opa(obj, 0) != 255) || lv_obj_get_style_opa_layered(obj, 0) != 255 ||
       lv_obj_get_style_bg_grad_dir(obj, 0) != LV_GRAD_DIR_NONE ||
       lv_obj_get_style_bg_image_src(obj, 0) || lv_obj_get_style_bitmap_mask_src(obj, 0) ||
       lv_obj_get_style_blend_mode(obj, 0) != LV_BLEND_MODE_NORMAL ||
       lv_obj_get_style_clip_corner(obj, 0) ||
       (!root && lv_obj_get_style_bg_opa(obj, 0) != 0)) return false;
    if(lv_obj_check_type(obj, &lv_label_class)) {
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        lv_obj_init_draw_label_dsc(obj, LV_PART_MAIN, &dsc);
        dsc.text = lv_label_get_text(obj);
        if(!dsc.text || dsc.decor || dsc.rotation || dsc.outline_stroke_width) return false;
        lv_area_t coords;
        lv_obj_get_content_coords(obj, &coords);
        capture_t ctx = {.rows = rows, .origin = *origin, .label = coords, .focus = obj == rows->focus};
        if(ctx.focus) { rows->focus_area = coords; lv_area_move(&rows->focus_area, -origin->x1, -origin->y1); }
        dsc.base.user_data = &ctx;
        lv_draw_task_t task;
        lv_memzero(&task, sizeof(task));
        task.draw_dsc = &dsc;
        task.clip_area = coords;
        unsigned first = rows->count;
        lv_draw_label_iterate_characters(&task, &dsc, &coords, capture);
        if(rows->failed) return false;
        unsigned last = rows->count;
        int end = 0;
        for(unsigned i = first; i < last; i++) {
            row_letter_t * letter = &rows->letters[i];
            end = LV_MAX(end, letter->x + letter->glyph->w);
        }
        for(unsigned i = first; i < last; i++) rows->letters[i].run_end = end;
        /* Build the ellipsis from the very same fallback font and atlas. The
         * source label may fit at native size but need truncation at the HIG
         * 20px floor near the round rim. No partial letter is clipped. */
        lv_font_glyph_dsc_t dot;
        bool has_dot = lv_font_get_glyph_dsc(dsc.font, &dot, '.', '.') &&
                       dot.resolved_font && !dot.is_placeholder;
        lv_font_glyph_release_draw_data(&dot);
        /* Framework7 symbol runs have no period. A synthetic missing-glyph
         * ellipsis must not reject their otherwise valid retained row. */
        if(!strchr(dsc.text, '\n') && last > first && has_dot) {
            ctx.dots = true;
            dsc.text = "...";
            lv_draw_label_iterate_characters(&task, &dsc, &coords, capture);
            if(rows->failed || rows->count != last + 3) return false;
            int advance = rows->letters[last + 1].x - rows->letters[last].x;
            for(unsigned i = first; i < rows->count; i++) {
                rows->letters[i].run_end = end;
                rows->letters[i].dot_advance = advance;
            }
        }
    }
    /* The button class adds interaction flags only; all its pixels still use
     * the base object draw handler. Keep other custom widget classes out. */
    else if(!lv_obj_check_type(obj, &lv_obj_class)
#if LV_USE_BUTTON
            && !lv_obj_check_type(obj, &lv_button_class)
#endif
           ) return false;
    uint32_t count = lv_obj_get_child_count(obj);
    for(uint32_t i = 0; i < count; i++) {
        if(!collect(rows, lv_obj_get_child(obj, i), origin, false)) return false;
    }
    return true;
}

lv_obj_rows_t * lv_obj_rows_create(lv_obj_t * obj, lv_color_t matte, lv_draw_buf_malloc_cb_t alloc,
                                  lv_draw_buf_free_cb_t release, lv_obj_t * focus, uint16_t preview_scale)
{
    uint64_t started = lv_draw_sw_profile_stage_begin();
    lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, 0);
    /* The raster owner proves a matte exists. A transparent content button
     * over a black matte has exactly the same background as an opaque black
     * row; its otherwise unused style background must never colour the model.
     * Other mattes/partial fills retain the reference raster path. */
    if(!lv_color_eq(matte, lv_color_black()) ||
       (bg_opa != LV_OPA_COVER && bg_opa != LV_OPA_TRANSP) ||
       lv_obj_get_style_bg_grad_dir(obj, 0) != LV_GRAD_DIR_NONE)
        return NULL;
    if(!reserve(sizeof(lv_obj_rows_t))) return NULL;
    lv_obj_rows_t * rows = alloc(sizeof(*rows), LV_COLOR_FORMAT_A8);
    if(!rows) {
#ifdef ESP_PLATFORM
        orb_memory_release(ORB_MEMORY_ROW_FONTS, sizeof(*rows));
#endif
        return NULL;
    }
    lv_memzero(rows, sizeof(*rows));
    rows->focus = focus; rows->preview_scale = preview_scale;
    rows->alloc = alloc;
    rows->release = release;
    s_bytes += sizeof(*rows);
    s_stats.peak_bytes = LV_MAX(s_stats.peak_bytes, s_bytes);
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    rows->w = lv_area_get_width(&area);
    rows->h = lv_area_get_height(&area);
    rows->radius = LV_CLAMP(0, lv_obj_get_style_radius(obj, 0), LV_MIN(rows->w, rows->h) / 2);
    rows->bg = bg_opa == LV_OPA_TRANSP ? matte : lv_obj_get_style_bg_color(obj, 0);
    if(!collect(rows, obj, &area, true) || !rows->count) {
        s_stats.fallbacks++;
        lv_obj_rows_destroy(rows);
        rows = NULL;
    }
    else { rows->published = true; s_stats.rows++; s_stats.builds++; }
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_ROWS_PREPARE, started, 0);
    return rows;
}

void lv_obj_rows_destroy(lv_obj_rows_t * rows)
{
    if(!rows) return;
    for(unsigned i = 0; i < rows->count; i++) {
        row_glyph_t * glyph = rows->letters[i].glyph;
        glyph->refs--;
    }
    if(rows->published) s_stats.rows--;
    s_bytes -= sizeof(*rows);
#ifdef ESP_PLATFORM
    orb_memory_release(ORB_MEMORY_ROW_FONTS, sizeof(*rows));
#endif
    rows->release(rows);
}

static unsigned shape_coverage(int x, int y, int w, int h, int r)
{
    float dx = fabsf(x + 0.5f - w * 0.5f) - (w * 0.5f - r);
    float dy = fabsf(y + 0.5f - h * 0.5f) - (h * 0.5f - r);
    if(dx <= 0 || dy <= 0) return 255;
    float distance = sqrtf(dx * dx + dy * dy) - r;
    return (unsigned)LV_CLAMP(0.0f, (0.5f - distance) * 255, 255.0f);
}

static void placement(const lv_obj_rows_t * rows, const lv_area_t * area, unsigned scale,
                      lv_area_t * box, unsigned * bucket)
{
    unsigned w = rows->w * scale / 256, h = rows->h * scale / 256;
    box->x1 = area->x1 + (rows->w - (int)w) / 2;
    box->y1 = area->y1 + (rows->h - (int)h) / 2;
    box->x2 = box->x1 + w - 1;
    box->y2 = box->y1 + h - 1;
    *bucket = scale <= ROW_MIN_SCALE ? 0 : LV_MIN(ROW_SIZES - 1, (scale - ROW_MIN_SCALE + 4) / 8);
}

static bool letter_position(const lv_obj_rows_t * rows, row_letter_t * letter, const lv_area_t * box, unsigned scale,
                            unsigned b, int * x, int * y)
{
    unsigned gs = glyph_scale(letter->glyph, b);
    *x = box->x1 + letter->run_x * scale / 256 + (letter->x - letter->run_x) * gs / 256;
    *y = box->y1 + letter->run_y * scale / 256 + (letter->y - letter->run_y) * gs / 256;
    int right = box->x1 + (letter->run_x + letter->run_width) * scale / 256;
    bool preview = rows->focus && scale < rows->preview_scale;
    if(preview) {
        if(!letter->focus) return false;
        int available = (rows->w - 16) * scale / 256;
        int ink = (letter->run_end - letter->run_x) * gs / 256;
        int left = box->x1 + 8 * scale / 256 + LV_MAX(0, (available - ink) / 2);
        *x = left + (letter->x - letter->run_x) * gs / 256;
        int focus_y = rows->focus_area.y1;
        int focus_h = lv_area_get_height(&rows->focus_area) * gs / 256;
        *y = box->y1 + (lv_area_get_height(box) - focus_h) / 2 +
             (letter->y - focus_y) * gs / 256;
        right = box->x1 + (rows->w - 8) * scale / 256;
    }
    bool overflow = letter->dot_advance &&
                    (letter->run_end - letter->run_x) * (int)gs >
                        (preview ? rows->w - 16 : letter->run_width) * (int)scale;
    if(letter->ellipsis) {
        if(!overflow) return false;
        *x = right - 3 * letter->dot_advance * gs / 256 + (letter->x - letter->run_x) * gs / 256;
    }
    else if(overflow) right -= 3 * letter->dot_advance * gs / 256;
    return *x + letter->glyph->sizes[b].w <= right;
}

bool lv_obj_rows_sample_area(lv_obj_rows_t * rows, const lv_area_t * area, uint16_t scale,
                            const lv_area_t * sample, lv_color32_t * out)
{
    lv_area_t box, clip;
    unsigned b;
    placement(rows, area, scale, &box, &b);
    if(!lv_area_intersect(&clip, &box, sample)) return false;
    unsigned cw = lv_area_get_width(&clip), ch = lv_area_get_height(&clip);
    if(cw > 8 || ch > 8) return false;
    uint16_t cell[64];
    int w = lv_area_get_width(&box), h = lv_area_get_height(&box), radius = rows->radius * scale / 256;
    uint16_t bg = lv_color_to_u16(rows->bg);
    for(int y = clip.y1; y <= clip.y2; y++) for(int x = clip.x1; x <= clip.x2; x++) {
        uint16_t color = blend565(0, bg, shape_coverage(x - box.x1, y - box.y1, w, h, radius));
        cell[(y - clip.y1) * cw + x - clip.x1] = color;
    }
    /* Integrate each intersecting atlas span once. The cell never visits every
     * glyph for each backdrop pixel; PSRAM reads are only real glyph coverage. */
    for(unsigned i = 0; i < rows->count; i++) {
        row_letter_t * letter = &rows->letters[i];
        row_size_t * size = &letter->glyph->sizes[b];
        int gx, gy;
        if(!letter_position(rows, letter, &box, scale, b, &gx, &gy)) continue;
        lv_area_t span = {gx, gy, gx + size->w - 1, gy + size->h - 1};
        if(!lv_area_intersect(&span, &span, &clip)) continue;
        uint16_t fg = lv_color_to_u16(letter->color);
        for(int y = span.y1; y <= span.y2; y++) for(int x = span.x1; x <= span.x2; x++) {
            uint16_t * dst = &cell[(y - clip.y1) * cw + x - clip.x1];
            *dst = blend565(*dst, fg, mul255(nibble(size, x - gx, y - gy), letter->opa));
        }
    }
    unsigned red = 0, green = 0, blue = 0;
    unsigned pixels = lv_area_get_size(&clip);
    for(unsigned i = 0; i < pixels; i++) {
        red += cell[i] >> 11; green += (cell[i] >> 5) & 63; blue += cell[i] & 31;
    }
    out->red = (red * 255 + pixels * 15) / (pixels * 31);
    out->green = (green * 255 + pixels * 31) / (pixels * 63);
    out->blue = (blue * 255 + pixels * 15) / (pixels * 31);
    out->alpha = (pixels * 255u + lv_area_get_size(sample) / 2) / lv_area_get_size(sample);
    return true;
}

bool lv_obj_rows_sample(lv_obj_rows_t * rows, const lv_area_t * area, uint16_t scale,
                       int32_t x, int32_t y, lv_color32_t * out)
{
    lv_area_t box;
    unsigned b;
    placement(rows, area, scale, &box, &b);
    if(x < box.x1 || x > box.x2 || y < box.y1 || y > box.y2) return false;
    unsigned a = shape_coverage(x - box.x1, y - box.y1, lv_area_get_width(&box),
                               lv_area_get_height(&box), rows->radius * scale / 256);
    uint16_t color = blend565(0, lv_color_to_u16(rows->bg), a);
    for(unsigned i = 0; i < rows->count; i++) {
        row_letter_t * letter = &rows->letters[i];
        row_size_t * size = &letter->glyph->sizes[b];
        int gx, gy;
        if(!letter_position(rows, letter, &box, scale, b, &gx, &gy)) continue;
        if(x >= gx && y >= gy && x < gx + size->w && y < gy + size->h)
            color = blend565(color, lv_color_to_u16(letter->color), mul255(nibble(size, x - gx, y - gy), letter->opa));
    }
    out->red = ((color >> 11) * 255 + 15) / 31;
    out->green = (((color >> 5) & 63) * 255 + 31) / 63;
    out->blue = ((color & 31) * 255 + 15) / 31;
    out->alpha = 255;
    return true;
}

bool lv_obj_rows_draw(lv_obj_rows_t * rows, lv_layer_t * layer, const lv_area_t * area,
                     uint16_t scale, lv_opa_t opa, lv_color_t matte)
{
    if(!rows || !scale || scale == 256 || lv_color_to_u16(matte) != 0 ||
       (layer->color_format != LV_COLOR_FORMAT_RGB565 && layer->color_format != LV_COLOR_FORMAT_RGB565_SWAPPED)) return false;
    lv_area_t box, clip;
    unsigned b;
    placement(rows, area, scale, &box, &b);
    if(!lv_area_intersect(&clip, &box, &layer->_clip_area) ||
       !lv_area_intersect(&clip, &clip, &layer->buf_area)) return true;
    /* Like Prototype B, join preceding LVGL tasks before writing its band.
     * The band is internal SRAM; no warm row allocation or PSRAM write. */
    while(layer->draw_task_head) { lv_draw_dispatch(); lv_draw_wait_for_finish(); }
    uint64_t started = lv_draw_sw_profile_stage_begin();
    bool swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint16_t bg = lv_color_to_u16(rows->bg);
    uint16_t background = blend565(0, bg, opa);
    if(swapped) background = (uint16_t)((background << 8) | (background >> 8));
    uint32_t pixels = 0;
    int w = lv_area_get_width(&box), h = lv_area_get_height(&box), radius = rows->radius * scale / 256;
    for(int y = clip.y1; y <= clip.y2; y++) {
        uint16_t * dst = lv_draw_layer_go_to_xy(layer, clip.x1 - layer->buf_area.x1, y - layer->buf_area.y1);
        /* Transparent shell rows have a black matte. Their entire rectangle,
         * including the rounded AA edge, is exactly zero; do not solve the
         * rounded shape or branch on every interior pixel again in each band. */
        if(!bg) {
            lv_memzero(dst, (size_t)lv_area_get_width(&clip) * sizeof(*dst));
            pixels += lv_area_get_width(&clip);
            continue;
        }
        /* Derive solid and AA spans once per scanline. Only the narrow AA
         * boundary needs per-pixel distance arithmetic; interiors are fills. */
        float dy = fabsf(y - box.y1 + 0.5f - h * 0.5f) - (h * 0.5f - radius);
        float inner = LV_MAX(0.0f, radius - 0.5f), outer = radius + 0.5f;
        float inset = dy > 0 ? radius - sqrtf(LV_MAX(0.0f, inner * inner - dy * dy)) - 0.5f : 0;
        float aa = dy > 0 ? radius - sqrtf(LV_MAX(0.0f, outer * outer - dy * dy)) - 0.5f : 0;
        int left = box.x1 + (int)ceilf(inset);
        int right = box.x2 - (int)ceilf(inset);
        int aa_left = box.x1 + (int)floorf(aa), aa_right = box.x2 - (int)floorf(aa);
        for(int x = clip.x1; x <= clip.x2; x++) dst[x - clip.x1] = x >= left && x <= right ? background : 0;
        int edge_left = LV_MAX(clip.x1, aa_left), edge_right = LV_MIN(clip.x2, aa_right);
        for(int x = edge_left; x <= edge_right; x++) {
            if(x >= left && x <= right) continue;
            uint16_t c = blend565(0, bg, mul255(shape_coverage(x - box.x1, y - box.y1, w, h, radius), opa));
            dst[x - clip.x1] = swapped ? (uint16_t)((c << 8) | (c >> 8)) : c;
        }
        pixels += lv_area_get_width(&clip);
    }
    uint16_t palette[16];
    uint16_t palette_color = 0;
    unsigned palette_opa = 256;
    for(unsigned i = 0; i < rows->count; i++) {
        row_letter_t * letter = &rows->letters[i];
        row_size_t * size = &letter->glyph->sizes[b];
        int x, y;
        if(!letter_position(rows, letter, &box, scale, b, &x, &y)) continue;
        lv_area_t glyph_area = {x, y, x + size->w - 1, y + size->h - 1};
        if(!lv_area_intersect(&glyph_area, &glyph_area, &clip)) continue;
        uint16_t color = blend565(0, lv_color_to_u16(letter->color), opa);
        /* A4 has only sixteen coverages. Cache their exact RGB565 result over
         * this row's solid background in internal scratch. Reuse it across
         * same-colour letters, with the ordinary painter-order blend retained
         * for overlapping glyphs and rounded edges. No extra retained RAM. */
        if(color != palette_color || letter->opa != palette_opa) {
            uint16_t base = background;
            if(swapped) base = (uint16_t)((base << 8) | (base >> 8));
            for(unsigned a = 0; a < 16; a++) {
                uint16_t result = blend565(base, color, mul255(a * 17, letter->opa));
                palette[a] = swapped ? (uint16_t)((result << 8) | (result >> 8)) : result;
            }
            palette_color = color;
            palette_opa = letter->opa;
        }
        for(int py = glyph_area.y1; py <= glyph_area.y2; py++) {
            uint16_t * dst = lv_draw_layer_go_to_xy(layer, glyph_area.x1 - layer->buf_area.x1, py - layer->buf_area.y1);
            for(int px = glyph_area.x1; px <= glyph_area.x2; px++) {
                unsigned p = (py - y) * size->w + px - x;
                unsigned a = (p & 1) ? size->a4[p / 2] & 15 : size->a4[p / 2] >> 4;
                if(!a) continue;
                uint16_t old = dst[px - glyph_area.x1];
                if(old == background) {
                    dst[px - glyph_area.x1] = palette[a];
                    continue;
                }
                unsigned alpha = mul255(a * 17, letter->opa);
                if(!alpha) continue;
                if(alpha == 255) {
                    dst[px - glyph_area.x1] = swapped ? (uint16_t)((color << 8) | (color >> 8)) : color;
                    continue;
                }
                if(swapped) old = (uint16_t)((old << 8) | (old >> 8));
                uint16_t result = blend565(old, color, alpha);
                dst[px - glyph_area.x1] = swapped ? (uint16_t)((result << 8) | (result >> 8)) : result;
            }
            pixels += lv_area_get_width(&glyph_area);
        }
    }
    s_stats.draws++;
    s_stats.pixels += pixels;
    lv_draw_sw_profile_stage_end(LV_DRAW_SW_PROFILE_ROWS_DRAW, started, pixels);
    return true;
}

void lv_obj_raster_rows_get_stats(lv_obj_raster_rows_stats_t * stats)
{
    *stats = s_stats;
    stats->bytes = s_bytes;
    stats->budget = ROW_BUDGET;
}

#else
lv_obj_rows_t * lv_obj_rows_create(lv_obj_t * obj, lv_color_t matte, lv_draw_buf_malloc_cb_t alloc,
                                  lv_draw_buf_free_cb_t release, lv_obj_t * focus, uint16_t preview_scale)
{ LV_UNUSED(obj); LV_UNUSED(matte); LV_UNUSED(alloc); LV_UNUSED(release); LV_UNUSED(focus); LV_UNUSED(preview_scale); return NULL; }
void lv_obj_rows_destroy(lv_obj_rows_t * rows) { LV_UNUSED(rows); }
bool lv_obj_rows_draw(lv_obj_rows_t * rows, lv_layer_t * layer, const lv_area_t * area,
                     uint16_t scale, lv_opa_t opa, lv_color_t matte)
{ LV_UNUSED(rows); LV_UNUSED(layer); LV_UNUSED(area); LV_UNUSED(scale); LV_UNUSED(opa); LV_UNUSED(matte); return false; }
bool lv_obj_rows_sample(lv_obj_rows_t * rows, const lv_area_t * area, uint16_t scale,
                       int32_t x, int32_t y, lv_color32_t * out)
{ LV_UNUSED(rows); LV_UNUSED(area); LV_UNUSED(scale); LV_UNUSED(x); LV_UNUSED(y); LV_UNUSED(out); return false; }
bool lv_obj_rows_sample_area(lv_obj_rows_t * rows, const lv_area_t * area, uint16_t scale,
                            const lv_area_t * sample, lv_color32_t * out)
{ LV_UNUSED(rows); LV_UNUSED(area); LV_UNUSED(scale); LV_UNUSED(sample); LV_UNUSED(out); return false; }
void lv_obj_raster_rows_get_stats(lv_obj_raster_rows_stats_t * stats) { lv_memzero(stats, sizeof(*stats)); }
void lv_obj_raster_rows_drop_unused(void) { }
void lv_obj_raster_rows_set_font_size_cb(uint16_t (*cb)(const lv_font_t * font)) { LV_UNUSED(cb); }
#endif
