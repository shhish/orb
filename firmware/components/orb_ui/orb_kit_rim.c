#include "orb_kit.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "src/misc/lv_text_private.h"

/* HIG §§3–4: glyph baselines, rather than label rectangles, follow the rim.
 * Arclabel's rotated glyph renderer needs temporary glyph buffers. Retain its
 * transparent result between content/font changes, so refresh and scrolling
 * only composite the prepared raster, including on the RGB565 display. */
#define RIM_TRACKING 1 /* half-pixel gaps plus the font's baked tracking */
#define RIM_PI 3.14159265358979323846f

typedef struct {
    lv_obj_t *first[2], *second[2], *slice[2];
    char *text, *suffix, *prepared;
    orb_kit_rim_text_info_t info;
    orb_text_style_t caption_style;
    orb_text_variant_t caption_variant;
    lv_color_t suffix_colour;
    lv_opa_t suffix_opacity;
    bool caption, constructed, prepare_pending;
    uint16_t radius;
    float max_degrees; /* 0 keeps ORB_KIT_TITLE_MAX_DEGREES */
    int32_t base_x, base_y, base_width, base_height, presentation_scale;
    lv_opa_t presentation_opacity;
    bool presentation;
} rim_text_t;

typedef struct {
    float width, centres, first, last;
    unsigned count;
} rim_measure_t;

static void rim_event(lv_event_t *e);

static rim_text_t *rim_context(lv_obj_t *obj)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(d) == rim_event) return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

static uint32_t next_codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    if (!*p) return 0;
    uint32_t cp = *p++;
    unsigned n = cp < 0x80 ? 0 : cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
    if (n) cp &= (1u << (6 - n)) - 1;
    while (n-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
    *text = (const char *)p;
    return cp;
}

static rim_measure_t measure(const char *text, const lv_font_t *font)
{
    rim_measure_t out = {0};
    if (!text) return out;
    while (*text) {
        uint32_t cp = next_codepoint(&text);
        const char *peek = text;
        uint32_t next = next_codepoint(&peek);
        float w = lv_font_get_glyph_width(font, cp, next);
        if (!out.count) out.first = w;
        else out.width += RIM_TRACKING / 2.0f;
        out.width += w;
        out.last = w;
        ++out.count;
    }
    if (out.count) out.centres = out.width - (out.first + out.last) / 2.0f;
    return out;
}

float orb_kit_rim_text_measure(const char *text, orb_text_style_t style, int32_t radius)
{
    if (radius <= 0 || style < 0 || style >= ORB_TEXT_STYLE_COUNT) return 0;
    rim_measure_t m = measure(text, orb_font_variant(style, ORB_TEXT_EMPHASIZED));
    return m.width * 180.0f / (RIM_PI * radius);
}

static char *text_copy(const char *text, bool caps)
{
    if (!text) text = "";
    size_t len = strlen(text);
    char *copy = lv_malloc(len + 1);
    if (!copy) return NULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)text[i];
        copy[i] = caps && c < 0x80 ? (char)toupper(c) : (char)c;
    }
    copy[len] = 0;
    return copy;
}

static bool text_matches(const char *stored, const char *text, bool caps)
{
    if (!text) text = "";
    for (; *stored && *text; ++stored, ++text) {
        unsigned char c = (unsigned char)*text;
        if (*stored != (caps && c < 0x80 ? (char)toupper(c) : (char)c)) return false;
    }
    return *stored == *text;
}

static size_t previous_character(const char *text, size_t end)
{
    if (!end) return 0;
    --end;
    while (end && ((unsigned char)text[end] & 0xC0) == 0x80) --end;
    return end;
}

static void set_arc(lv_obj_t *arc, const char *text, const lv_font_t *font,
                    float start, float span, int32_t radius, bool caption)
{
    lv_obj_set_style_text_font(arc, font, 0);
    lv_obj_set_style_text_letter_space(arc, RIM_TRACKING, 0);
    /* Both slices borrow one fitted string, rather than retaining another
     * allocated copy for each arclabel. The rim context owns its lifetime. */
    lv_arclabel_set_text_static(arc, text);
    lv_arclabel_set_radius(arc, radius);
    lv_arclabel_set_angle_start(arc, start);
    lv_arclabel_set_angle_size(arc, span);
    lv_arclabel_set_dir(arc, caption ? LV_ARCLABEL_DIR_COUNTER_CLOCKWISE : LV_ARCLABEL_DIR_CLOCKWISE);
    lv_arclabel_set_text_horizontal_align(arc, LV_ARCLABEL_TEXT_ALIGN_LEADING);
    lv_arclabel_set_text_vertical_align(arc, LV_ARCLABEL_TEXT_ALIGN_DEFAULT);
    lv_arclabel_set_overflow(arc, LV_ARCLABEL_OVERFLOW_VISIBLE);
}

static lv_obj_t *arc_create(lv_obj_t *parent, lv_color_t colour, lv_opa_t opacity)
{
    lv_obj_t *arc = lv_arclabel_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_scrollable(arc, false);
    lv_obj_set_size(arc, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_set_style_text_color(arc, colour, 0);
    lv_obj_set_style_text_opa(arc, opacity, 0);
    return arc;
}

/* Artwork previews retain the two bounded sources. Presentation changes
 * geometry/opacity only; screens never need the kit's private slice objects. */
static void rim_present(lv_obj_t *obj, rim_text_t *ctx)
{
    int32_t scale = ctx->presentation_scale;
    if (ctx->presentation) {
        lv_obj_set_size(obj, ORB_SCREEN_D * scale / 256, ORB_SCREEN_D * scale / 256);
        lv_obj_set_pos(obj, 0, 0);
    }
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *slice = ctx->slice[i];
        int32_t width = i ? ctx->base_width - ctx->base_width / 2 : ctx->base_width / 2;
        int32_t offset = i ? ctx->base_width / 2 : 0;
        if (ctx->presentation) {
            lv_obj_set_style_transform_pivot_x(slice, width / 2, 0);
            lv_obj_set_style_transform_pivot_y(slice, ctx->base_height / 2, 0);
            lv_obj_set_style_transform_scale(slice, scale, 0);
            lv_obj_set_pos(slice, (ctx->base_x + offset + width / 2) * scale / 256 - width / 2,
                (ctx->base_y + ctx->base_height / 2) * scale / 256 - ctx->base_height / 2);
        }
        lv_obj_set_raster_opa(slice, ctx->presentation_opacity);
    }
}

static void prepare_rasters(void *data)
{
    rim_text_t *ctx = data;
    ctx->prepare_pending = false;
    for (unsigned i = 0; i < 2; ++i) lv_obj_raster_prepare(ctx->slice[i]);
}

static void rebuild(lv_obj_t *obj, rim_text_t *ctx)
{
    size_t first_len = strlen(ctx->text), second_len = strlen(ctx->suffix);
    char *full = lv_malloc(first_len + second_len + 4);
    if (!full) return;
    memcpy(full, ctx->text, first_len);
    memcpy(full + first_len, ctx->suffix, second_len + 1);
    int32_t radius = ctx->radius;
    orb_text_style_t style = ctx->caption ? ctx->caption_style : ORB_TEXT_FOOTNOTE1;
    const lv_font_t *font = orb_font_variant(style, ctx->caption ? ctx->caption_variant : ORB_TEXT_EMPHASIZED);
    float budget = radius * RIM_PI *
        (ctx->max_degrees > 0 ? ctx->max_degrees : ORB_KIT_TITLE_MAX_DEGREES) / 180.0f;
    rim_measure_t total = measure(full, font);
    if (!ctx->caption && total.width > budget) {
        style = ORB_TEXT_FOOTNOTE2;
        font = orb_font_variant(style, ORB_TEXT_EMPHASIZED);
        total = measure(full, font);
    }
    bool ellipsis = total.width > budget;
    size_t cut = first_len + second_len;
    if (ellipsis) {
        /* Remove complete UTF-8 characters and keep the ellipsis at the arc
         * end, including when the colour boundary itself is truncated. HIG §4:
         * one typographic U+2026, never three periods or a space before it. */
        do {
            cut = previous_character(full, cut);
            /* Glossary §1.3: one U+2026 straight after the last kept glyph. */
            for (uint32_t previous = (uint32_t)cut; cut; cut = previous) {
                previous = (uint32_t)cut;
                if (!lv_label_trims_before_ellipsis(lv_text_encoded_prev(full, &previous))) break;
            }
            memcpy(full + cut, "\xE2\x80\xA6", 4);
            total = measure(full, font);
        } while (cut && total.width > budget);
    }
    size_t split = first_len;
    if (ellipsis && cut < first_len) split = strlen(full);
    if (split > strlen(full)) split = strlen(full);
    char saved = full[split];
    full[split] = 0;
    rim_measure_t a = measure(full, font);
    full[split] = saved;
    const char *suffix = full + split;
    rim_measure_t b = measure(suffix, font);
    float gap = a.count && b.count ? (a.last + b.first + RIM_TRACKING) / 2.0f : 0;
    float centres = a.centres + b.centres + gap;
    float first_width = a.count ? a.first : b.first;
    float last_width = b.count ? b.last : a.last;
    float centre_correction = (first_width - last_width) / 4.0f;
    float deg = 180.0f / (RIM_PI * radius);
    float first_start, second_start;
    if (ctx->caption) {
        /* Counter-clockwise text reads upright from left to right at 6 o'clock. */
        first_start = 90.0f + (centres / 2.0f - centre_correction - a.centres) * deg;
        second_start = 90.0f + (centres / 2.0f - centre_correction - a.centres - gap - b.centres) * deg;
    }
    else {
        first_start = 270.0f + (centre_correction - centres / 2.0f) * deg;
        second_start = first_start + (a.centres + gap) * deg;
    }
    /* Store the colour lanes as adjacent C strings. All four arclabels share
     * these prepared bytes until the next explicit content/font change. */
    size_t fitted_length = strlen(full);
    memmove(full + split + 1, full + split, fitted_length - split + 1);
    full[split] = 0;
    suffix = full + split + 1;
    for (unsigned i = 0; i < 2; ++i) {
        set_arc(ctx->first[i], full, font, first_start, a.centres * deg, radius, ctx->caption);
        if (b.count) {
            if (!ctx->second[i]) ctx->second[i] = arc_create(ctx->slice[i], ctx->suffix_colour, ctx->suffix_opacity);
            set_arc(ctx->second[i], suffix, font, second_start, b.centres * deg, radius, ctx->caption);
        } else if (ctx->second[i]) {
            lv_obj_delete(ctx->second[i]);
            ctx->second[i] = NULL;
        }
    }

    /* Only the text's bounded arc is cached. This keeps short headings small
     * and leaves the centre transparent for content and concentric gauges. */
    float half = fminf(total.width / radius / 2.0f, RIM_PI * 0.5f);
    int32_t margin = font->line_height + 3;
    int32_t x1 = (int32_t)floorf(233 - (radius + margin) * sinf(half) - margin / 2.0f);
    int32_t x2 = 466 - x1;
    int32_t y1 = ctx->caption ? (int32_t)floorf(233 + radius * cosf(half) - margin) : 233 - radius - margin;
    int32_t y2 = ctx->caption ? 233 + radius + margin : (int32_t)ceilf(233 - radius * cosf(half) + margin);
    x1 = LV_MAX(x1, 0); x2 = LV_MIN(x2, 466);
    y1 = LV_MAX(y1, 0); y2 = LV_MIN(y2, 466);
    lv_obj_set_pos(obj, x1, y1);
    int32_t width = LV_MAX(2, x2 - x1), height = LV_MAX(1, y2 - y1);
    lv_obj_set_size(obj, width, height);
    for (unsigned i = 0; i < 2; ++i) {
        int32_t offset = i ? width / 2 : 0;
        lv_obj_set_pos(ctx->slice[i], offset, 0);
        lv_obj_set_size(ctx->slice[i], i ? width - offset : width / 2, height);
        lv_obj_set_pos(ctx->first[i], -x1 - offset, -y1);
        if (ctx->second[i]) lv_obj_set_pos(ctx->second[i], -x1 - offset, -y1);
    }
    lv_obj_update_layout(obj);
    ctx->base_x = x1; ctx->base_y = y1;
    ctx->base_width = width; ctx->base_height = height;
    ctx->info = (orb_kit_rim_text_info_t){style, total.width * deg, ellipsis};
    lv_free(ctx->prepared);
    ctx->prepared = full;
    /* Two narrow sources keep even AX3 at the full 120° below the fork's
     * 256 KiB cold raster workspace ceiling. Sources are transparent, so a
     * Timer gauge behind its heading remains visible between the letters. */
    for (unsigned i = 0; i < 2; ++i) lv_obj_invalidate_raster(ctx->slice[i]);
    /* Initial scene construction prepares before a container can translate
     * into a navigation capture. Later updates can run below ui_tick's large
     * frame: defer their cold glyph capture until that callback unwinds, while
     * geometry/metadata and dirty native output stay current. One owned job
     * reads the latest slices. Allocation failure retains normal pre-refresh
     * preparation/native fallback, never a stale ready bitmap. */
    if (!ctx->constructed) prepare_rasters(ctx);
    else if (!ctx->prepare_pending)
        ctx->prepare_pending = lv_async_call(prepare_rasters, ctx) == LV_RESULT_OK;
    rim_present(obj, ctx);
}

static void font_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    (void)subject;
    lv_obj_t *obj = lv_observer_get_target_obj(observer);
    rim_text_t *ctx = lv_observer_get_user_data(observer);
    rebuild(obj, ctx);
}

static void rim_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    rim_text_t *ctx = lv_event_get_user_data(e);
    if (ctx->prepare_pending) lv_async_call_cancel(prepare_rasters, ctx);
    lv_free(ctx->text);
    lv_free(ctx->suffix);
    lv_free(ctx->prepared);
    lv_free(ctx);
}

static lv_obj_t *create(lv_obj_t *parent, const char *text, const char *suffix,
                        lv_color_t colour, bool caption, bool date, uint16_t radius)
{
    rim_text_t *ctx = lv_malloc_zeroed(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->text = text_copy(text, !caption);
    ctx->suffix = text_copy(suffix, !caption);
    if (!ctx->text || !ctx->suffix) {
        lv_free(ctx->text); lv_free(ctx->suffix); lv_free(ctx);
        return NULL;
    }
    ctx->presentation_scale = 256;
    ctx->presentation_opacity = LV_OPA_COVER;
    ctx->caption = caption;
    ctx->radius = radius;
    ctx->caption_style = ORB_TEXT_FOOTNOTE2;
    ctx->caption_variant = ORB_TEXT_REGULAR;
    ctx->suffix_colour = date ? ORB_LABEL : ORB_KIT_SECONDARY;
    ctx->suffix_opacity = date ? LV_OPA_COVER : ORB_KIT_SECONDARY_OPA;
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_add_event_cb(obj, rim_event, LV_EVENT_DELETE, ctx);
    for (unsigned i = 0; i < 2; ++i) {
        ctx->slice[i] = lv_obj_create(obj);
        lv_obj_remove_style_all(ctx->slice[i]);
        lv_obj_set_scrollable(ctx->slice[i], false);
        lv_obj_set_clickable(ctx->slice[i], false);
        ctx->first[i] = arc_create(ctx->slice[i], colour, LV_OPA_COVER);
        if (lv_color_eq(colour, ORB_KIT_SECONDARY)) {
            lv_obj_set_style_text_opa(ctx->first[i], ORB_KIT_SECONDARY_OPA, 0);
        }
        lv_obj_set_raster(ctx->slice[i], true);
    }
    lv_subject_add_observer_obj(orb_fonts_subject(), font_changed, obj, ctx);
    ctx->constructed = true;
    return obj;
}

lv_obj_t *orb_kit_rim_title_create(lv_obj_t *parent, const char *text, lv_color_t colour)
{
    return create(parent, text, "", colour, false, false, ORB_KIT_TITLE_RADIUS);
}

lv_obj_t *orb_kit_rim_title_inset_create(lv_obj_t *parent, const char *text, lv_color_t colour)
{
    /* HIG §§3–4: a concentric title's glyphs, not just its baseline, must
     * clear the ring. The 176px Design 2.0 baseline reserves AX3 cap height. */
    return create(parent, text, "", colour, false, false, ORB_KIT_TITLE_INSET_RADIUS);
}

bool orb_kit_rim_title_set_max_degrees(lv_obj_t *obj, float degrees)
{
    rim_text_t *ctx = rim_context(obj);
    if (!ctx || ctx->caption || degrees < 0 || degrees > ORB_KIT_TITLE_MAX_DEGREES) return false;
    if (ctx->max_degrees != degrees) {
        ctx->max_degrees = degrees;
        rebuild(obj, ctx);
    }
    return true;
}

bool orb_kit_rim_title_set_inset(lv_obj_t *obj, bool inset)
{
    rim_text_t *ctx = rim_context(obj);
    if (!ctx || ctx->caption) return false;
    uint16_t radius = inset ? ORB_KIT_TITLE_INSET_RADIUS : ORB_KIT_TITLE_RADIUS;
    if (ctx->radius != radius) {
        ctx->radius = radius;
        rebuild(obj, ctx);
    }
    return true;
}

lv_obj_t *orb_kit_rim_title_two_colour_create(lv_obj_t *parent, const char *text,
                                             lv_color_t colour, const char *suffix)
{
    return create(parent, text, suffix, colour, false, false, ORB_KIT_TITLE_RADIUS);
}

lv_obj_t *orb_kit_rim_date_create(lv_obj_t *parent, const char *weekday, const char *day)
{
    size_t len = strlen(day ? day : "");
    char *suffix = lv_malloc(len + 2);
    if (!suffix) return NULL;
    suffix[0] = ' ';
    memcpy(suffix + 1, day ? day : "", len + 1);
    lv_obj_t *obj = create(parent, weekday, suffix, ORB_KIT_DATE, false, true, ORB_KIT_TITLE_RADIUS);
    lv_free(suffix);
    return obj;
}

lv_obj_t *orb_kit_rim_caption_create(lv_obj_t *parent, const char *text)
{
    return create(parent, text, "", ORB_KIT_SECONDARY, true, false, ORB_KIT_CAPTION_RADIUS);
}

bool orb_kit_rim_caption_set_style(lv_obj_t *obj, orb_text_style_t style,
                                  orb_text_variant_t variant, lv_color_t colour, lv_opa_t opacity)
{
    rim_text_t *ctx = rim_context(obj);
    if (!ctx || !ctx->caption || (unsigned)style >= ORB_TEXT_STYLE_COUNT) return false;
    ctx->caption_style = style;
    ctx->caption_variant = variant;
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_set_style_text_color(ctx->first[i], colour, 0);
        lv_obj_set_style_text_opa(ctx->first[i], opacity, 0);
    }
    rebuild(obj, ctx);
    return true;
}

void orb_kit_rim_text_set(lv_obj_t *obj, const char *text, const char *suffix)
{
    rim_text_t *ctx = rim_context(obj);
    if (!ctx) return;
    if (text_matches(ctx->text, text, !ctx->caption) &&
        text_matches(ctx->suffix, suffix, !ctx->caption)) return;
    char *next = text_copy(text, !ctx->caption), *extra = text_copy(suffix, !ctx->caption);
    if (!next || !extra) { lv_free(next); lv_free(extra); return; }
    if (!strcmp(ctx->text, next) && !strcmp(ctx->suffix, extra)) {
        lv_free(next); lv_free(extra);
        return;
    }
    lv_free(ctx->text); lv_free(ctx->suffix);
    ctx->text = next; ctx->suffix = extra;
    rebuild(obj, ctx);
}

bool orb_kit_rim_text_info(lv_obj_t *obj, orb_kit_rim_text_info_t *out)
{
    rim_text_t *ctx = rim_context(obj);
    if (!ctx || !out) return false;
    *out = ctx->info;
    return true;
}

const char *orb_kit_rim_text_get(lv_obj_t *obj)
{
    rim_text_t *ctx = rim_context(obj);
    return ctx ? ctx->text : NULL;
}

void orb_kit_rim_text_set_presentation(lv_obj_t *obj, int32_t scale, lv_opa_t opacity)
{
    rim_text_t *ctx = obj ? rim_context(obj) : NULL;
    if (!ctx) return;
    ctx->presentation = true;
    ctx->presentation_scale = LV_CLAMP(1, scale, 256);
    ctx->presentation_opacity = opacity;
    rim_present(obj, ctx);
}

void orb_kit_rim_text_set_opa(lv_obj_t *obj, lv_opa_t opacity)
{
    rim_text_t *ctx = obj ? rim_context(obj) : NULL;
    if (!ctx || ctx->presentation_opacity == opacity) return;
    ctx->presentation_opacity = opacity;
    rim_present(obj, ctx);
}

/* The stopwatch ticks are content, not glass (HIG §§1/7). Prepare their
 * geometry once; refreshing the moving marker never rebuilds a ring raster. */
typedef struct {
    lv_obj_t *marker;
    lv_point_precise_t points[120];
    int32_t radius;
    unsigned count;
} tick_ring_t;

static void tick_ring_event(lv_event_t *event)
{
    tick_ring_t *ctx = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        lv_free(ctx);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t *obj = lv_event_get_target_obj(event);
    lv_area_t bounds;
    lv_obj_get_coords(obj, &bounds);
    lv_layer_t *layer = lv_event_get_layer(event);
    for (unsigned i = 0; i < ctx->count; ++i) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.p1 = ctx->points[2 * i];
        line.p2 = ctx->points[2 * i + 1];
        line.p1.x += bounds.x1; line.p1.y += bounds.y1;
        line.p2.x += bounds.x1; line.p2.y += bounds.y1;
        line.width = ctx->count == 12 || !(i % 5) ? 3 : 2;
        line.color = ORB_GRAY4;
        line.round_start = line.round_end = true;
        lv_draw_line(layer, &line);
    }
}

static tick_ring_t *tick_ring_context(lv_obj_t *obj)
{
    if (!obj) return NULL;
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); ++i) {
        lv_event_dsc_t *descriptor = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(descriptor) == tick_ring_event)
            return lv_event_dsc_get_user_data(descriptor);
    }
    return NULL;
}

lv_obj_t *orb_kit_tick_ring_create(lv_obj_t *parent, int32_t radius,
                                  unsigned tick_count, lv_color_t accent)
{
    if (radius < 28 || radius > ORB_SCREEN_R - 10 ||
        (tick_count != 12 && tick_count != 60)) return NULL;
    tick_ring_t *ctx = lv_malloc_zeroed(sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->radius = radius;
    ctx->count = tick_count;
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_center(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_add_event_cb(obj, tick_ring_event, LV_EVENT_ALL, ctx);
    for (unsigned i = 0; i < tick_count; ++i) {
        float angle = (float)i * 2 * RIM_PI / tick_count - RIM_PI / 2;
        int32_t length = tick_count == 12 || !(i % 5) ? 14 : 7;
        ctx->points[2 * i] = (lv_point_precise_t){
            ORB_SCREEN_R + (radius - length) * cosf(angle),
            ORB_SCREEN_R + (radius - length) * sinf(angle)};
        ctx->points[2 * i + 1] = (lv_point_precise_t){
            ORB_SCREEN_R + radius * cosf(angle),
            ORB_SCREEN_R + radius * sinf(angle)};
    }
    ctx->marker = lv_obj_create(obj);
    lv_obj_remove_style_all(ctx->marker);
    lv_obj_set_size(ctx->marker, 14, 14);
    lv_obj_set_style_radius(ctx->marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ctx->marker, accent, 0);
    lv_obj_set_style_bg_opa(ctx->marker, LV_OPA_COVER, 0);
    lv_obj_set_clickable(ctx->marker, false);
    lv_obj_set_scrollable(ctx->marker, false);
    orb_kit_tick_ring_set_position(obj, 0, 60);
    return obj;
}

void orb_kit_tick_ring_set_position(lv_obj_t *obj, uint32_t elapsed, uint32_t period)
{
    tick_ring_t *ctx = tick_ring_context(obj);
    if (!ctx || !period) return;
    /* Caller owns elapsed time. Pause and Reduce Motion therefore cannot
     * leave an autonomous animation running behind the stopwatch's state. */
    float angle = (elapsed % period) * (2 * RIM_PI / period) - RIM_PI / 2;
    int32_t radius = ctx->radius - 14;
    int32_t x = ORB_SCREEN_R + (int32_t)lroundf(radius * cosf(angle)) - 7;
    int32_t y = ORB_SCREEN_R + (int32_t)lroundf(radius * sinf(angle)) - 7;
    if (lv_obj_get_x(ctx->marker) != x || lv_obj_get_y(ctx->marker) != y)
        lv_obj_set_pos(ctx->marker, x, y);
}
