#include "orb_kit_value.h"
#include "orb_ui_private.h"

#include <math.h>
#include <string.h>

#include "src/misc/lv_text_private.h"
#include "src/widgets/label/lv_label_private.h"

typedef struct {
    lv_obj_t *obj;
    orb_delight_spring_t spring;
    int32_t value, target;
    lv_point_t origin;
    lv_area_t press_bounds;
    bool cancelled, pointer, primary;
} kit_press_t;

static void press_event(lv_event_t *e);

static kit_press_t *press_find(lv_obj_t *obj)
{
    for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(d) == press_event) return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

static void press_set(kit_press_t *p, int32_t value)
{
    p->value = value;
    int32_t scale = orb_ui_reduce_motion() ? 256 : 256 - (256 - ORB_KIT_PRESS_SCALE) * value / 1024;
    bool glass = orb_glass_kit_press(p->obj, value);
    lv_obj_set_style_transform_scale(p->obj, glass ? 256 : scale, 0);
    if (glass) {
        for (unsigned i = 0; i < lv_obj_get_child_count(p->obj); i++) {
            lv_obj_t *child = lv_obj_get_child(p->obj, i);
            if (lv_obj_get_raster(child)) lv_obj_set_raster_scale(child, scale);
        }
    }
}

static bool press_tick(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    kit_press_t *p = context;
    uint32_t duration = p->target ? ORB_KIT_PRESS_MS : ORB_KIT_RELEASE_MS;
    finish |= elapsed >= duration;
    if (finish) orb_delight_spring_reset(&p->spring, p->target);
    else orb_delight_spring_step(&p->spring, dt / 1000.f, duration / 1000.f, 0);
    press_set(p, lroundf(p->spring.value));
    return !finish;
}

void orb_kit_press_cancel(lv_obj_t *obj)
{
    kit_press_t *p = press_find(obj);
    if (!p || p->cancelled) return;
    p->cancelled = true;
    orb_kit_motion_cancel(p->obj, p, false);
    p->target = 0;
    /* Retiring a contact also retires its velocity. The next press starts
     * from the visible rest pose, never the cancelled contact's spring. */
    orb_delight_spring_reset(&p->spring, 0);
    lv_obj_remove_state(obj, LV_STATE_PRESSED);
    press_set(p, 0);
    /* Controls can retire touched-knob glass in the same input frame. A
     * listener may delete obj, so do not access the context afterwards. */
    lv_obj_send_event(obj, LV_EVENT_CANCEL, NULL);
}

bool orb_kit_press_is_cancelled(lv_obj_t *obj)
{
    kit_press_t *p = press_find(obj);
    return p && p->cancelled;
}

bool orb_kit_is_primary(lv_obj_t *obj)
{
    kit_press_t *p = obj ? press_find(obj) : NULL;
    return p && p->primary;
}

static void press_event(lv_event_t *e)
{
    kit_press_t *p = lv_event_get_user_data(e);
    if (lv_event_get_target_obj(e) != p->obj) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        orb_kit_motion_cancel(p->obj, p, false);
        lv_free(p);
        return;
    }
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_SCROLL_BEGIN) {
        orb_kit_press_cancel(p->obj);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (p->cancelled) return;
        lv_indev_t *input = lv_indev_active();
        if (input) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            int32_t dx = point.x - p->origin.x, dy = point.y - p->origin.y;
            if (lv_indev_get_scroll_obj(input) || (p->pointer && dx * dx + dy * dy >= 100))
                orb_kit_press_cancel(p->obj);
        }
        return;
    }
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED ||
        code == LV_EVENT_LONG_PRESSED || code == LV_EVENT_LONG_PRESSED_REPEAT) {
        if (p->cancelled) {
            lv_event_stop_bubbling(e);
            lv_event_stop_processing(e);
        }
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED) return;
    if (code == LV_EVENT_PRESSED) {
        p->cancelled = false;
        lv_indev_t *input = lv_indev_active();
        p->pointer = input && lv_indev_get_type(input) == LV_INDEV_TYPE_POINTER;
        if (p->pointer) {
            lv_indev_get_point(input, &p->origin);
            lv_obj_get_coords(p->obj, &p->press_bounds);
        }
    }
    if (p->cancelled) return;
    if (code == LV_EVENT_RELEASED && p->pointer) {
        lv_indev_t *input = lv_indev_active();
        if (input) {
            lv_point_t point;
            lv_indev_get_point(input, &point);
            /* HIG §7: a contact belongs to its down-time hit. An incoming
             * app may continue sliding under a stationary finger; that is
             * not pointer travel and must not retire a valid tap. */
            if (point.x < p->press_bounds.x1 || point.x > p->press_bounds.x2 ||
                point.y < p->press_bounds.y1 || point.y > p->press_bounds.y2) {
                orb_kit_press_cancel(p->obj);
                return;
            }
        }
    }
    p->target = code == LV_EVENT_PRESSED ? 1024 : 0;
    if (orb_ui_reduce_motion()) {
        orb_kit_motion_cancel(p->obj, p, false);
        orb_delight_spring_reset(&p->spring, p->target);
        press_set(p, p->target);
    } else {
        orb_delight_spring_target(&p->spring, p->target);
        orb_kit_motion_start_feedback(p->obj, p, press_tick, p);
    }
}

void orb_kit_press_apply(lv_obj_t *obj)
{
    if (press_find(obj)) return;
    kit_press_t *p = lv_malloc_zeroed(sizeof(*p));
    if (!p) return;
    p->obj = obj;
    orb_kit_motion_prepare(obj);
    orb_delight_spring_reset(&p->spring, 0);
    lv_obj_update_layout(obj);
    lv_obj_set_style_transform_pivot_x(obj, lv_obj_get_width(obj) / 2, 0);
    lv_obj_set_style_transform_pivot_y(obj, lv_obj_get_height(obj) / 2, 0);
    lv_obj_set_style_transform_scale(obj, 256, 0);
    /* Keep ownership of the cancelled physical contact when it leaves and
     * reenters a target; only the next down can rearm activation. */
    lv_obj_set_press_lock(obj, true);
    orb_glass_kit_press(obj, 0);
    lv_obj_add_event_cb(obj, press_event, LV_EVENT_ALL, p);
}

static float linear_channel(uint8_t value)
{
    float x = value / 255.f;
    return x <= .04045f ? x / 12.92f : powf((x + .055f) / 1.055f, 2.4f);
}

bool orb_kit_accent_uses_black(lv_color_t accent)
{
    float luminance = .2126f * linear_channel(accent.red) +
        .7152f * linear_channel(accent.green) + .0722f * linear_channel(accent.blue);
    /* HIG §5's 3:1 icon contrast wins over the frame's white orange/green
     * marks. (1.0 + .05) / (L + .05) reaches 3 at L=.30. */
    return luminance >= .30f;
}

lv_color_t orb_kit_accent_ink(lv_color_t accent)
{
    return orb_kit_accent_uses_black(accent) ? lv_color_black() : lv_color_white();
}

static lv_obj_t *base(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, true);
    return obj;
}

static void ink_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    lv_obj_t *label = lv_observer_get_target_obj(observer);
    if (orb_kit_value_find(label)) return;
    lv_obj_update_layout(label);
    lv_obj_raster_prepare(label);
}

static void ink_prepare(lv_obj_t *label)
{
    if (orb_kit_value_find(label)) return;
    /* Scale only prepared ink, keeping backdrop glass in its native layer.
     * Font changes prepare a new source while idle, never on a press tick. */
    if (lv_obj_get_raster(label)) {
        lv_obj_raster_prepare(label);
        return;
    }
    lv_obj_set_raster(label, true);
    lv_subject_add_observer_obj(orb_fonts_subject(), ink_fonts, label, NULL);
}

lv_obj_t *orb_kit_disc_create(lv_obj_t *parent, orb_kit_disc_kind_t kind,
                              orb_kit_disc_style_t style, lv_color_t accent, const char *glyph)
{
    static const int32_t sizes[] = {ORB_KIT_DISC, ORB_KIT_DISC_MINI, ORB_KIT_DISC_ROW, ORB_KIT_ART};
    if ((unsigned)kind >= sizeof(sizes) / sizeof(sizes[0])) return NULL;
    lv_obj_t *obj = base(parent, sizes[kind], sizes[kind]);
    lv_color_t ink = orb_kit_accent_ink(accent);
    if (style == ORB_KIT_NEUTRAL_GLASS || style == ORB_KIT_TINTED_GLASS) {
        orb_glass_apply(obj, style == ORB_KIT_TINTED_GLASS ? ORB_GLASS_TINTED : ORB_GLASS_REGULAR, accent);
        if (style == ORB_KIT_TINTED_GLASS) {
            orb_glass_set_tint_opacity(obj, 66);
            lv_obj_set_style_border_width(obj, 1, 0);
            lv_obj_set_style_border_color(obj, accent, 0);
            lv_obj_set_style_border_opa(obj, 140, 0);
        }
        ink = style == ORB_KIT_TINTED_GLASS ? accent : ORB_LABEL;
    } else {
        lv_obj_set_style_bg_color(obj, accent, 0);
        lv_obj_set_style_bg_opa(obj, style == ORB_KIT_TRACK ? 46 : LV_OPA_COVER, 0);
        if (style == ORB_KIT_TRACK) ink = accent;
    }
    if (glyph && glyph[0]) {
        lv_obj_t *label = lv_label_create(obj);
        orb_text_style_t text_style = kind == ORB_KIT_DISC_KIND_ROW ? ORB_TEXT_FOOTNOTE2 :
            kind == ORB_KIT_DISC_KIND_MINI ? ORB_TEXT_CAPTION2 : ORB_TEXT_TITLE3;
        orb_icon_apply(label, text_style, ORB_TEXT_EMPHASIZED, 0);
        lv_obj_set_style_text_color(label, ink, 0);
        lv_obj_set_clickable(label, false);
        lv_label_set_text(label, glyph);
        lv_obj_center(label);
        ink_prepare(label);
    }
    if (style == ORB_KIT_ACCENT_FILL) orb_kit_disc_light_up_prepare(obj);
    /* Prepare focus while building, before any retained crown steps. */
    orb_kit_disc_set_focus(obj, false);
    orb_kit_press_apply(obj);
    kit_press_t *press = press_find(obj);
    if (press) press->primary = style == ORB_KIT_TINTED_GLASS;
    return obj;
}

lv_obj_t *orb_kit_close_create(lv_obj_t *parent)
{
    lv_obj_t *obj = orb_kit_disc_create(parent, ORB_KIT_DISC_KIND_MINI,
        ORB_KIT_NEUTRAL_GLASS, ORB_KIT_GREY, ORB_ICON_X);
    if (obj) lv_obj_set_pos(obj, 101 - ORB_KIT_DISC_MINI / 2, 101 - ORB_KIT_DISC_MINI / 2);
    return obj;
}

typedef struct {
    lv_obj_t *obj, *label, *prefix;
    char *semantic;
    size_t semantic_capacity;
    orb_text_style_t text_style;
    orb_text_variant_t text_variant;
    int32_t min_width;
    bool primary, compact, prefix_active;
} kit_button_t;

static void button_delete(lv_event_t *e);

static kit_button_t *button_find(lv_obj_t *obj)
{
    if (!obj) return NULL;
    for (unsigned i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *event = lv_obj_get_event_dsc(obj, i);
        if (lv_event_dsc_get_cb(event) == button_delete) return lv_event_dsc_get_user_data(event);
    }
    return NULL;
}

static lv_obj_t *control_label(lv_obj_t *obj)
{
    kit_button_t *button = button_find(obj);
    if (button) return button->label;
    if (!obj || !press_find(obj)) return NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class)) return child;
    }
    return NULL;
}

static void button_measure(kit_button_t *b)
{
    lv_obj_update_layout(b->label);
    int32_t prefix_width = 0, prefix_bearing = 0;
    if (b->prefix_active) {
        lv_obj_update_layout(b->prefix);
        prefix_width = lv_obj_get_width(b->prefix) + 8;
        /* HIG §§4/7: the visible gap follows icon ink, not the font's
         * unequal sun/speaker sidebearings. Only semantic/font preparation
         * measures this; percentage updates retain the 100% rail. */
        uint32_t offset = 0;
        uint32_t cp = lv_text_encoded_next(lv_label_get_text(b->prefix), &offset);
        lv_font_glyph_dsc_t glyph;
        if (lv_font_get_glyph_dsc(lv_obj_get_style_text_font(b->prefix, 0), &glyph, cp, 0) && glyph.box_w) {
            prefix_bearing = glyph.ofs_x + glyph.box_w - lv_obj_get_width(b->prefix);
        }
    }
    int32_t height = b->primary ? ORB_KIT_PRIMARY_H : ORB_KIT_SECONDARY_H;
    int32_t width = prefix_width + lv_obj_get_width(b->label) + (b->primary ? 88 : b->compact ? 34 : 68);
    width = LV_MAX(width, b->min_width);
    width = LV_MIN(width, orb_chord_width(380 - height / 2, 380 + height / 2, 12));
    int32_t object_width = LV_MAX(height, width);
    int32_t label_width = LV_MIN(lv_obj_get_width(b->label), width - 32 - prefix_width);
    lv_obj_set_size(b->obj, object_width, height);
    lv_obj_set_width(b->label, label_width);
    if (b->prefix_active) {
        int32_t left = (object_width - prefix_width - label_width) / 2;
        lv_obj_align(b->prefix, LV_ALIGN_TOP_LEFT, left - prefix_bearing, (height - lv_obj_get_height(b->prefix)) / 2);
        lv_obj_align(b->label, LV_ALIGN_TOP_LEFT, left + prefix_width, (height - lv_obj_get_height(b->label)) / 2);
    } else lv_obj_center(b->label);
    if (lv_obj_get_raster(b->label) && !orb_kit_value_find(b->label)) lv_obj_raster_prepare(b->label);
    lv_obj_set_style_transform_pivot_x(b->obj, width / 2, 0);
    lv_obj_set_style_transform_pivot_y(b->obj, height / 2, 0);
    orb_glass_kit_resize(b->obj);
}

static void button_fonts(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    kit_button_t *b = lv_observer_get_user_data(observer);
    kit_value_t *value = orb_kit_value_find(b->label);
    if (value) orb_kit_value_measure(value);
    else lv_obj_set_width(b->label, LV_SIZE_CONTENT);
    if (b->prefix_active) orb_kit_value_set(b->prefix, lv_label_get_text(b->prefix), NULL);
    button_measure(b);
}

static void button_delete(lv_event_t *e)
{
    kit_button_t *button = lv_event_get_user_data(e);
    lv_free(button->semantic);
    lv_free(button);
}

static lv_obj_t *button_create(lv_obj_t *parent, const char *text, bool primary, lv_color_t accent)
{
    kit_button_t *b = lv_malloc_zeroed(sizeof(*b));
    if (!b) return NULL;
    b->primary = primary;
    b->text_style = primary ? ORB_TEXT_HEADLINE : ORB_TEXT_CAPTION2;
    b->text_variant = ORB_TEXT_EMPHASIZED;
    b->obj = base(parent, 160, primary ? ORB_KIT_PRIMARY_H : ORB_KIT_SECONDARY_H);
    b->label = lv_label_create(b->obj);
    orb_text_apply(b->label, primary ? ORB_TEXT_HEADLINE : ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_style_text_color(b->label, primary ? accent : ORB_LABEL, 0);
    lv_label_set_long_mode(b->label, LV_LABEL_LONG_DOT);
    lv_label_set_text(b->label, text ? text : "");
    lv_obj_set_clickable(b->label, false);
    button_measure(b);
    ink_prepare(b->label);
    orb_glass_apply(b->obj, primary ? ORB_GLASS_TINTED : ORB_GLASS_REGULAR, accent);
    if (primary) {
        orb_glass_set_tint_opacity(b->obj, 66);
        lv_obj_set_style_border_width(b->obj, 1, 0);
        lv_obj_set_style_border_color(b->obj, accent, 0);
        lv_obj_set_style_border_opa(b->obj, 140, 0);
    }
    orb_kit_disc_set_focus(b->obj, false);
    orb_kit_press_apply(b->obj);
    kit_press_t *press = press_find(b->obj);
    if (press) press->primary = primary;
    lv_subject_add_observer_obj(orb_fonts_subject(), button_fonts, b->obj, b);
    lv_obj_add_event_cb(b->obj, button_delete, LV_EVENT_DELETE, b);
    return b->obj;
}

lv_obj_t *orb_kit_button_primary_create(lv_obj_t *parent, const char *text, lv_color_t accent)
{
    return button_create(parent, text, true, accent);
}

lv_obj_t *orb_kit_button_secondary_create(lv_obj_t *parent, const char *text)
{
    return button_create(parent, text, false, ORB_GRAY);
}

void orb_kit_button_set_text(lv_obj_t *obj, const char *text)
{
    kit_button_t *button = button_find(obj);
    if (!button) { orb_kit_disc_set_glyph(obj, text); return; }
    if (button->prefix_active) { orb_kit_chip_set_value(obj, text, NULL); return; }
    kit_value_t *value = orb_kit_value_find(button->label);
    if (value) {
        orb_kit_value_set(button->label, text, NULL);
        return;
    }
    text = text ? text : "";
    if (!strcmp(lv_label_get_text(button->label), text)) return;
    lv_label_set_text(button->label, text);
    lv_obj_set_width(button->label, LV_SIZE_CONTENT);
    button_measure(button);
}

void orb_kit_button_set_min_width(lv_obj_t *obj, int32_t width)
{
    kit_button_t *button = button_find(obj);
    if (!button || button->min_width == width) return;
    button->min_width = width;
    button_measure(button);
}

const char *orb_kit_button_get_text(lv_obj_t *obj)
{
    kit_button_t *button = button_find(obj);
    if (button && button->prefix_active) return button->semantic ? button->semantic : "";
    lv_obj_t *label = control_label(obj);
    return label ? lv_label_get_text(label) : "";
}

void orb_kit_button_set_value(lv_obj_t *obj, const char *text, const char *reserve_template)
{
    kit_button_t *button = button_find(obj);
    if (!button) return;
    kit_value_t *previous = orb_kit_value_find(button->label);
    unsigned measures = previous ? previous->template_measures : 0;
    if (!previous) lv_obj_set_width(button->label, LV_SIZE_CONTENT);
    orb_kit_value_set(button->label, text, reserve_template);
    kit_value_t *value = orb_kit_value_find(button->label);
    if (value && value->template_measures != measures) button_measure(button);
}

static unsigned chip_prefix_bytes(const char *text)
{
    if (!text || !text[0]) return 0;
    const unsigned char *p = (const unsigned char *)text;
    unsigned bytes = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : p[0] >= 0xC0 ? 2 : 1;
    uint32_t cp = p[0] & (bytes == 4 ? 7 : bytes == 3 ? 15 : bytes == 2 ? 31 : 127);
    for (unsigned i = 1; i < bytes; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    return cp >= 0xE000 && cp <= 0xF8FF && p[bytes] == ' ' ? bytes : 0;
}

lv_obj_t *orb_kit_chip_value_label(lv_obj_t *chip)
{
    kit_button_t *button = button_find(chip);
    return button ? button->label : NULL;
}

void orb_kit_chip_set_value(lv_obj_t *obj, const char *text, const char *reserve_template)
{
    kit_button_t *button = button_find(obj);
    if (!button) return;
    text = text ? text : "";
    unsigned template_prefix = chip_prefix_bytes(reserve_template);
    unsigned text_prefix = chip_prefix_bytes(text);
    bool prefix = text_prefix && (template_prefix || (!reserve_template && button->prefix_active));
    bool changed = !button->compact || prefix != button->prefix_active;
    button->compact = true;
    button->prefix_active = prefix;
    if (prefix) {
        size_t bytes = strlen(text) + 1;
        size_t capacity = LV_MAX(bytes, reserve_template ? strlen(reserve_template) + 1 : bytes);
        if (capacity > button->semantic_capacity) {
            char *copy = lv_malloc(capacity);
            if (!copy) return;
            memcpy(copy, text, bytes);
            lv_free(button->semantic);
            button->semantic = copy;
            button->semantic_capacity = capacity;
        } else memmove(button->semantic, text, bytes);
        text = button->semantic;
        char glyph[5] = {0}, reservation[5] = {0};
        memcpy(glyph, text, text_prefix);
        if (template_prefix) memcpy(reservation, reserve_template, template_prefix);
        if (!button->prefix) {
            button->prefix = lv_label_create(obj);
            orb_icon_apply(button->prefix, button->text_style, button->text_variant, 0);
            lv_obj_set_style_text_color(button->prefix, lv_obj_get_style_text_color(button->label, 0), 0);
            lv_obj_set_clickable(button->prefix, false);
            changed = true;
        }
        lv_obj_set_hidden(button->prefix, false);
        changed |= strcmp(lv_label_get_text(button->prefix), glyph) != 0;
        orb_kit_value_set(button->prefix, glyph, template_prefix ? reservation : NULL);
        if (lv_obj_get_style_text_align(button->label, 0) != LV_TEXT_ALIGN_RIGHT)
            lv_obj_set_style_text_align(button->label, LV_TEXT_ALIGN_RIGHT, 0);
        /* Keep icon and unit anchored independently. The value rail alone
         * reserves 100%, so 9 -> 10 -> 100 never moves either fixed edge. */
        orb_kit_button_set_value(obj, text + text_prefix + 1,
                                  template_prefix ? reserve_template + template_prefix + 1 : NULL);
    } else {
        if (button->prefix) lv_obj_set_hidden(button->prefix, true);
        orb_kit_button_set_value(obj, text, reserve_template);
    }
    if (changed) button_measure(button);
}

void orb_kit_disc_set_glyph(lv_obj_t *obj, const char *glyph)
{
    lv_obj_t *label = control_label(obj);
    if (!label && obj && press_find(obj)) {
        label = lv_label_create(obj);
        orb_icon_apply(label, ORB_TEXT_TITLE3, ORB_TEXT_EMPHASIZED, 0);
        lv_obj_set_style_text_color(label, ORB_LABEL, 0);
        lv_obj_set_clickable(label, false);
    }
    if (!label) return;
    glyph = glyph ? glyph : "";
    if (!strcmp(lv_label_get_text(label), glyph)) return;
    lv_label_set_text(label, glyph);
    lv_obj_center(label);
    ink_prepare(label);
}

void orb_kit_disc_set_ink(lv_obj_t *obj, lv_color_t colour)
{
    lv_obj_t *label = control_label(obj);
    if (!label) return;
    if (lv_color_eq(lv_obj_get_style_text_color(label, 0), colour)) return;
    lv_obj_set_style_text_color(label, colour, 0);
    kit_button_t *button = button_find(obj);
    if (button && button->prefix) lv_obj_set_style_text_color(button->prefix, colour, 0);
    ink_prepare(label);
}

void orb_kit_disc_set_ink_opa(lv_obj_t *obj, lv_opa_t opacity)
{
    lv_obj_t *label = control_label(obj);
    if (!label) return;
    if (lv_obj_get_raster(label)) lv_obj_set_raster_opa(label, opacity);
    else lv_obj_set_style_text_opa(label, opacity, 0);
    kit_button_t *button = button_find(obj);
    if (button && button->prefix) lv_obj_set_style_text_opa(button->prefix, opacity, 0);
}

void orb_kit_disc_set_text_style(lv_obj_t *obj, orb_text_style_t style, orb_text_variant_t variant)
{
    lv_obj_t *label = control_label(obj);
    if (!label) return;
    kit_value_t *value = orb_kit_value_find(label);
    if (value) variant |= ORB_TEXT_TABULAR;
    if (lv_obj_get_style_text_font(label, 0) == orb_font_variant(style, variant) &&
        lv_obj_get_style_text_letter_space(label, 0) == orb_text_style_info(style)->letter_space) return;
    orb_text_apply(label, style, variant, 0);
    kit_button_t *button = button_find(obj);
    if (button) {
        button->text_style = style;
        button->text_variant = variant;
        if (button->prefix) {
            orb_icon_apply(button->prefix, style, variant, 0);
            orb_kit_value_set(button->prefix, lv_label_get_text(button->prefix), NULL);
        }
    }
    if (value) orb_kit_value_measure(value);
    if (button) {
        if (!value) lv_obj_set_width(label, LV_SIZE_CONTENT);
        button_measure(button);
    } else lv_obj_center(label);
    ink_prepare(label);
}

void orb_kit_disc_set_glyph_hidden(lv_obj_t *obj, bool hidden)
{
    lv_obj_t *label = control_label(obj);
    if (label) lv_obj_set_hidden(label, hidden);
}

bool orb_kit_value_info(lv_obj_t *obj, orb_kit_value_info_t *out)
{
    if (!obj || !out) return false;
    kit_value_t *value = orb_kit_value_find(obj);
    kit_ring_t *ring = kit_event_state(obj, kit_ring_event);
    if (!value && ring) value = orb_kit_value_find(ring->value_label);
    if (!value) value = orb_kit_value_find(control_label(obj));
    if (!value) {
        /* Gauges own an ordinary value label. The label is private to screen
         * code; diagnostics can discover it through this semantic API. */
        for (unsigned i = 0; i < lv_obj_get_child_count(obj) && !value; i++)
            value = orb_kit_value_find(lv_obj_get_child(obj, i));
    }
    if (!value) return false;
    *out = (orb_kit_value_info_t){
        .width = value->width, .template_measures = value->template_measures,
        .updates = value->updates, .raster_prepares = 0,
        .tabular = value->tabular, .live = true,
    };
    return true;
}

lv_obj_t *orb_kit_glyph_button_create(lv_obj_t *parent, const char *glyph, bool primary, lv_color_t accent)
{
    return orb_kit_disc_create(parent, ORB_KIT_DISC_STANDARD,
        primary ? ORB_KIT_TINTED_GLASS : ORB_KIT_NEUTRAL_GLASS, accent, glyph);
}
