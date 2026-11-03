/* HIG §§4/6: keep the label's reserved lane and authoritative text intact.
 * Changed digits are drawn in place; unchanged code points never move. */
#include "orb_kit_value.h"
#include "orb_kit_effects.h"
#include "orb_kit_motion.h"
#include <math.h>
#include <string.h>
#include "src/misc/lv_area_private.h"
#include "src/misc/lv_text_private.h"

#define ROLL_BYTES 128
#define ROLL_GLYPHS 64

typedef struct {
    char before[5], after[5];
    int32_t before_x, after_x;
    bool changed;
} kit_roll_glyph_t;

typedef struct {
    lv_obj_t *label;
    orb_delight_spring_t spring;
    uint32_t changed_at;
    int32_t height;
    lv_opa_t opacity;
    unsigned count;
    kit_roll_glyph_t glyphs[ROLL_GLYPHS];
    bool seen, active;
} kit_roll_t;

static void roll_event(lv_event_t *event);

static kit_roll_t *roll_find(lv_obj_t *label)
{
    if (!label) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(label); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(label, i);
        if (lv_event_dsc_get_cb(event) == roll_event) return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}

void orb_kit_value_roll_prepare(lv_obj_t *label)
{
    if (!label || roll_find(label)) return;
    kit_roll_t *roll = lv_malloc_zeroed(sizeof(*roll));
    if (!roll) return;
    roll->label = label;
    roll->seen = true;
    roll->changed_at = lv_tick_get();
    lv_obj_add_event_cb(label, roll_event, LV_EVENT_ALL, roll);
    orb_kit_motion_prepare(label);
}

static bool roll_frame(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_roll_t *roll = context;
    if (finish || elapsed >= 350) {
        roll->active = false;
        lv_obj_set_style_text_opa(roll->label, roll->opacity, 0);
        return false;
    }
    orb_delight_spring_step(&roll->spring, dt / 1000.f, .28f, 0);
    lv_obj_invalidate(roll->label);
    return true;
}

void orb_kit_value_roll_finish(lv_obj_t *label)
{
    kit_roll_t *roll = roll_find(label);
    if (roll && roll->active) orb_kit_motion_cancel(label, roll, true);
}

bool orb_kit_value_roll_set_ink_opa(lv_obj_t *label, lv_opa_t opacity)
{
    kit_roll_t *roll = roll_find(label);
    if (!roll || !roll->active) return false;
    if (roll->opacity != opacity) {
        roll->opacity = opacity;
        lv_obj_invalidate(label);
    }
    return true;
}

static void roll_draw(kit_roll_t *roll, lv_layer_t *layer)
{
    if (!roll->active) return;
    lv_area_t box;
    lv_obj_get_content_coords(roll->label, &box);
    lv_area_t original = layer->_clip_area, clip;
    if (!lv_area_intersect(&clip, &box, &original)) return;
    layer->_clip_area = clip;
    float p = LV_CLAMP(0.f, roll->spring.value, 1.f);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = lv_obj_get_style_text_font(roll->label, 0);
    dsc.color = lv_obj_get_style_text_color(roll->label, 0);
    dsc.opa = roll->opacity;
    dsc.text_local = true;
    /* Like native owned text, queued glyphs are frozen by LVGL. A later
     * model update or deletion cannot rewrite a render worker's ink. */
    for (unsigned i = 0; i < roll->count; i++) {
        kit_roll_glyph_t *glyph = &roll->glyphs[i];
        lv_area_t area = {box.x1 + glyph->after_x, box.y1,
            box.x1 + glyph->after_x + roll->height * 2, box.y2};
        if (!glyph->changed) {
            dsc.text = glyph->after;
            lv_draw_label(layer, &dsc, &area);
            continue;
        }
        int32_t delta = (int32_t)lroundf(roll->height * p);
        area.x1 = box.x1 + glyph->before_x;
        area.x2 = area.x1 + roll->height * 2;
        area.y1 -= delta;
        area.y2 = area.y1 + roll->height - 1;
        dsc.text = glyph->before;
        if (glyph->before[0]) lv_draw_label(layer, &dsc, &area);
        area.x1 = box.x1 + glyph->after_x;
        area.x2 = area.x1 + roll->height * 2;
        area.y1 += roll->height;
        area.y2 += roll->height;
        dsc.text = glyph->after;
        if (glyph->after[0]) lv_draw_label(layer, &dsc, &area);
    }
    layer->_clip_area = original;
}

static void roll_event(lv_event_t *event)
{
    kit_roll_t *roll = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(roll->label, roll, false);
        lv_free(roll);
    } else if (code == LV_EVENT_DRAW_MAIN) roll_draw(roll, lv_event_get_layer(event));
    else if (code == LV_EVENT_PRESSED || code == LV_EVENT_SCROLL_BEGIN)
        orb_kit_value_roll_finish(roll->label);
}

static bool digit(char byte)
{
    return byte >= '0' && byte <= '9';
}

static int32_t roll_origin(lv_obj_t *label, const char *text, const lv_font_t *font,
                            const lv_text_attributes_t *attributes)
{
    int32_t width = lv_text_get_width(text, strlen(text), font, attributes);
    int32_t available = lv_obj_get_content_width(label);
    lv_text_align_t align = lv_obj_get_style_text_align(label, 0);
    return align == LV_TEXT_ALIGN_CENTER ? (available - width) / 2 :
        align == LV_TEXT_ALIGN_RIGHT ? available - width : 0;
}

static int32_t roll_x(const char *text, size_t at, const lv_font_t *font,
                      const lv_text_attributes_t *attributes, int32_t origin)
{
    return origin + (at ? lv_text_get_width(text, at, font, attributes) + attributes->letter_space : 0);
}

static bool roll_append(kit_roll_t *roll, const char *before, size_t before_bytes, int32_t before_x,
                         const char *after, size_t after_bytes, int32_t after_x)
{
    if (roll->count >= ROLL_GLYPHS || before_bytes > 4 || after_bytes > 4) return false;
    kit_roll_glyph_t *glyph = &roll->glyphs[roll->count++];
    memset(glyph, 0, sizeof(*glyph));
    if (before_bytes) memcpy(glyph->before, before, before_bytes);
    if (after_bytes) memcpy(glyph->after, after, after_bytes);
    glyph->before_x = before_x;
    glyph->after_x = after_x;
    glyph->changed = before_x != after_x || before_bytes != after_bytes ||
        (before_bytes && memcmp(before, after, before_bytes));
    return true;
}

static bool roll_numeric(kit_roll_t *roll, const char *before, uint32_t old_at, uint32_t old_end,
                          const char *after, uint32_t new_at, uint32_t new_end,
                          const lv_font_t *font, const lv_text_attributes_t *attributes)
{
    unsigned old_count = old_end - old_at, new_count = new_end - new_at;
    if (old_count > ROLL_GLYPHS || new_count > ROLL_GLYPHS) return false;
    int32_t old_x[ROLL_GLYPHS], new_x[ROLL_GLYPHS];
    bool used[ROLL_GLYPHS] = {0};
    int32_t old_origin = roll_origin(roll->label, before, font, attributes);
    int32_t new_origin = roll_origin(roll->label, after, font, attributes);
    for (unsigned i = 0; i < old_count; i++) old_x[i] = roll_x(before, old_at + i, font, attributes, old_origin);
    for (unsigned i = 0; i < new_count; i++) new_x[i] = roll_x(after, new_at + i, font, attributes, new_origin);
    /* Columns belong to their actual reserved lane positions. Common digits
     * stay still on 19 -> 119 in a right-aligned value; added/removed columns
     * arrive/leave vertically at their own X without moving the label. */
    for (unsigned next = 0; next < new_count; next++) {
        unsigned previous = 0;
        while (previous < old_count && old_x[previous] != new_x[next]) previous++;
        bool matched = previous < old_count;
        if (matched) used[previous] = true;
        if (!roll_append(roll, matched ? before + old_at + previous : "", matched ? 1 : 0,
            matched ? old_x[previous] : new_x[next], after + new_at + next, 1, new_x[next])) return false;
    }
    for (unsigned previous = 0; previous < old_count; previous++) {
        if (used[previous]) continue;
        if (!roll_append(roll, before + old_at + previous, 1, old_x[previous], "", 0, old_x[previous])) return false;
    }
    return true;
}

static bool roll_prepare(kit_roll_t *roll, const char *before, const char *after)
{
    if (strlen(before) >= ROLL_BYTES || strlen(after) >= ROLL_BYTES || !strcmp(before, after)) return false;
    const lv_font_t *font = lv_obj_get_style_text_font(roll->label, 0);
    lv_text_attributes_t attributes = {.letter_space = lv_obj_get_style_text_letter_space(roll->label, 0)};
    int32_t origin = roll_origin(roll->label, after, font, &attributes);
    roll->height = lv_font_get_line_height(font);
    roll->count = 0;
    for (uint32_t old_at = 0, new_at = 0; before[old_at] || after[new_at];) {
        if (digit(before[old_at]) || digit(after[new_at])) {
            if (!digit(before[old_at]) || !digit(after[new_at])) return false;
            uint32_t old_end = old_at, new_end = new_at;
            while (digit(before[old_end])) old_end++;
            while (digit(after[new_end])) new_end++;
            if (!roll_numeric(roll, before, old_at, old_end, after, new_at, new_end, font, &attributes)) return false;
            old_at = old_end;
            new_at = new_end;
        } else {
            uint32_t old_start = old_at, new_start = new_at;
            if (lv_text_encoded_next(before, &old_at) != lv_text_encoded_next(after, &new_at) ||
                old_at - old_start != new_at - new_start) return false;
            int32_t x = roll_x(after, new_start, font, &attributes, origin);
            size_t bytes = new_at - new_start;
            if (!bytes || !roll_append(roll, after + new_start, bytes, x, after + new_start, bytes, x)) return false;
        }
    }
    return true;
}

void orb_kit_value_set_rolling(lv_obj_t *label, const char *text, const char *reserve_template)
{
    if (!label || !lv_obj_check_type(label, &lv_label_class)) return;
    text = text ? text : "";
    kit_roll_t *roll = roll_find(label);
    if (!roll) {
        orb_kit_value_roll_prepare(label);
        roll = roll_find(label);
        if (!roll) { orb_kit_value_set(label, text, reserve_template); return; }
    }
    if (!strcmp(lv_label_get_text(label), text)) {
        if (!roll->seen) { roll->seen = true; roll->changed_at = lv_tick_get(); }
        if (!roll->active) orb_kit_value_set(label, text, reserve_template);
        return;
    }
    bool eligible = roll->seen && lv_tick_get() - roll->changed_at >= 1000;
    char before[ROLL_BYTES];
    const char *previous = lv_label_get_text(label);
    bool fits = strlen(previous) < sizeof(before);
    if (fits) memcpy(before, previous, strlen(previous) + 1);
    orb_kit_value_roll_finish(label);
    orb_kit_value_set(label, text, reserve_template);
    roll->seen = true;
    roll->changed_at = lv_tick_get();
    if (!eligible || !fits || !orb_kit_motion_allowed(label) ||
        !roll_prepare(roll, before, lv_label_get_text(label))) return;
    roll->opacity = lv_obj_get_style_text_opa(label, 0);
    roll->active = true;
    orb_delight_spring_reset(&roll->spring, 0);
    orb_delight_spring_target(&roll->spring, 1);
    lv_obj_set_style_text_opa(label, LV_OPA_TRANSP, 0);
    orb_kit_motion_start(label, roll, roll_frame, roll);
}

bool orb_kit_value_roll_semantic_text(lv_obj_t *label, const char **out)
{
    kit_roll_t *roll = roll_find(label);
    if (!roll || !roll->active) return false;
    if (out) *out = lv_label_get_text(label);
    return true;
}
