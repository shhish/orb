#include "orb_status.h"
#include "orb_ui.h"

#include <stdio.h>

/* The class extends lv_obj, as LVGL's built-in widgets do. The private include is
 * needed only for the embedded base object and queued draw-task clip areas. */
#include "lvgl_private.h"

typedef enum { STATUS_BATTERY, STATUS_WIFI } status_kind_t;

typedef struct {
    lv_obj_t obj;
    status_kind_t kind;
    orb_text_style_t style;
    orb_text_variant_t variant;
    orb_battery_state_t battery;
    orb_wifi_state_t wifi;
    lv_color_t background;
    int32_t baseline;
    int32_t cap_height;
    int32_t stroke;
    int32_t body_width;
    int32_t body_height;
    int32_t nub_width;
    int32_t gap;
} status_t;

static void status_event(const lv_obj_class_t *class_p, lv_event_t *e);
static const lv_obj_class_t status_class = {
    .base_class = &lv_obj_class,
    .event_cb = status_event,
    .instance_size = sizeof(status_t),
    .name = "orb_status",
};

static int32_t rounded_ratio(int32_t n, int32_t numerator, int32_t denominator)
{
    return (n * numerator + denominator / 2) / denominator;
}

static void measure(lv_obj_t *obj)
{
    status_t *s = (status_t *)obj;
    const lv_font_t *font = orb_font_variant(s->style, s->variant);
    const orb_text_style_info_t *info = orb_text_style_info(s->style);
    lv_font_glyph_dsc_t cap;
    s->baseline = font->line_height - font->base_line;
    s->cap_height = lv_font_get_glyph_dsc(font, &cap, 'H', 0) ? (int32_t)cap.box_h :
                    rounded_ratio(info->size_px, 72, 100);
    uint16_t weight = s->variant & ORB_TEXT_EMPHASIZED ? info->emphasized_weight : info->weight;
    s->stroke = LV_MAX(1, rounded_ratio(s->cap_height, weight >= 600 ? 9 : 6, 100));
    s->body_height = LV_MAX(9, rounded_ratio(s->cap_height, 9, 10));
    s->body_width = rounded_ratio(s->body_height, 22, 10);
    s->nub_width = LV_MAX(2, rounded_ratio(s->cap_height, 12, 100));
    s->gap = LV_MAX(1, rounded_ratio(s->cap_height, 7, 100));
    int32_t width = s->kind == STATUS_BATTERY ? s->body_width + s->gap + s->nub_width :
                    rounded_ratio(s->cap_height, 17, 10) + 2;
    lv_obj_set_size(obj, width, font->line_height);
    lv_obj_invalidate(obj);
}

static void fonts_changed(lv_observer_t *observer, lv_subject_t *subject)
{
    LV_UNUSED(subject);
    measure(lv_observer_get_target_obj(observer));
}

static lv_obj_t *create(lv_obj_t *parent, status_kind_t kind, orb_text_style_t style,
                         orb_text_variant_t variant)
{
    lv_obj_t *obj = lv_obj_class_create_obj(&status_class, parent);
    if (!obj) return NULL;
    lv_obj_class_init_obj(obj);
    status_t *s = (status_t *)obj;
    s->kind = kind;
    s->style = style >= 0 && style < ORB_TEXT_STYLE_COUNT ? style : ORB_TEXT_BODY;
    s->variant = variant & ORB_TEXT_EMPHASIZED;
    s->background = lv_color_black();
    s->battery.level = 100;
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    /* LVGL unsubscribes this observer on object deletion. No font pointer is
     * retained across a font-pack eviction or content-size change. */
    if (!lv_subject_add_observer_obj(orb_fonts_subject(), fonts_changed, obj, NULL)) {
        lv_obj_delete(obj);
        return NULL;
    }
    return obj;
}

lv_obj_t *orb_battery_create(lv_obj_t *parent, orb_text_style_t style,
                             orb_text_variant_t variant)
{
    return create(parent, STATUS_BATTERY, style, variant);
}

lv_obj_t *orb_wifi_create(lv_obj_t *parent, orb_text_style_t style,
                          orb_text_variant_t variant)
{
    return create(parent, STATUS_WIFI, style, variant);
}

void orb_battery_set_state(lv_obj_t *obj, int level, bool charging, bool low_power)
{
    LV_CHECK_OBJ(obj, &status_class, return);
    status_t *s = (status_t *)obj;
    if (s->kind != STATUS_BATTERY) return;
    level = LV_CLAMP(0, level, 100);
    if (s->battery.level == level && s->battery.charging == charging &&
        s->battery.low_power == low_power) return;
    s->battery.level = level;
    s->battery.charging = charging;
    s->battery.low_power = low_power;
    lv_obj_invalidate(obj);
}

void orb_battery_set_percentage(lv_obj_t *obj, bool show)
{
    LV_CHECK_OBJ(obj, &status_class, return);
    status_t *s = (status_t *)obj;
    if (s->kind != STATUS_BATTERY || s->battery.percentage == show) return;
    s->battery.percentage = show;
    lv_obj_invalidate(obj);
}

orb_battery_state_t orb_battery_get_state(const lv_obj_t *obj)
{
    LV_CHECK_OBJ(obj, &status_class, return ((orb_battery_state_t){0}));
    return ((const status_t *)obj)->battery;
}

uint8_t orb_wifi_bars_for_rssi(int rssi_dbm)
{
    if (rssi_dbm >= -55) return 3;
    if (rssi_dbm >= -67) return 2;
    if (rssi_dbm >= -80) return 1;
    return 0;
}

void orb_wifi_set_bars(lv_obj_t *obj, int bars, bool connected)
{
    LV_CHECK_OBJ(obj, &status_class, return);
    status_t *s = (status_t *)obj;
    if (s->kind != STATUS_WIFI) return;
    bars = LV_CLAMP(0, bars, 3);
    if (s->wifi.bars == bars && s->wifi.connected == connected) return;
    s->wifi.bars = (uint8_t)bars;
    s->wifi.connected = connected;
    lv_obj_invalidate(obj);
}

void orb_wifi_set_rssi(lv_obj_t *obj, int rssi_dbm, bool connected)
{
    orb_wifi_set_bars(obj, orb_wifi_bars_for_rssi(rssi_dbm), connected);
}

orb_wifi_state_t orb_wifi_get_state(const lv_obj_t *obj)
{
    LV_CHECK_OBJ(obj, &status_class, return ((orb_wifi_state_t){0}));
    return ((const status_t *)obj)->wifi;
}

void orb_status_set_text_style(lv_obj_t *obj, orb_text_style_t style,
                               orb_text_variant_t variant)
{
    LV_CHECK_OBJ(obj, &status_class, return);
    if (style < 0 || style >= ORB_TEXT_STYLE_COUNT) return;
    status_t *s = (status_t *)obj;
    variant &= ORB_TEXT_EMPHASIZED;
    if (s->style == style && s->variant == variant) return;
    s->style = style;
    s->variant = variant;
    measure(obj);
}

int32_t orb_status_baseline(const lv_obj_t *obj)
{
    LV_CHECK_OBJ(obj, &status_class, return 0);
    return ((const status_t *)obj)->baseline;
}

void orb_status_set_background(lv_obj_t *obj, lv_color_t color)
{
    LV_CHECK_OBJ(obj, &status_class, return);
    status_t *s = (status_t *)obj;
    if (lv_color_eq(s->background, color)) return;
    s->background = color;
    lv_obj_invalidate(obj);
}

static void rect(lv_layer_t *layer, const lv_area_t *area, int32_t radius,
                 lv_color_t color, lv_opa_t opa, int32_t border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = radius;
    if (border) {
        d.bg_opa = LV_OPA_TRANSP;
        d.border_color = color;
        d.border_opa = opa;
        d.border_width = border;
    } else {
        d.bg_color = color;
        d.bg_opa = opa;
    }
    lv_draw_rect(layer, &d, area);
}

/* LVGL copies the clip area into each draw task; restoring it immediately after
 * enqueueing is safe, including with two asynchronous draw units. */
static void label_clipped(lv_layer_t *layer, const lv_area_t *clip,
                          const lv_draw_label_dsc_t *d, const lv_area_t *area)
{
    lv_area_t saved = layer->_clip_area;
    if (lv_area_intersect(&layer->_clip_area, &saved, clip)) lv_draw_label(layer, d, area);
    layer->_clip_area = saved;
}

static void content_run(lv_layer_t *layer, const status_t *s, const lv_area_t *body,
                         const lv_area_t *fill, lv_color_t color, lv_opa_t opa,
                         const lv_font_t *font, const char *text, int32_t x,
                         int32_t width, int32_t baseline)
{
    lv_area_t area = {x, baseline - (font->line_height - font->base_line),
                      x + width - 1, baseline + font->base_line - 1};
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.text_local = 1;
    d.font = font;
    d.bidi_dir = LV_BASE_DIR_LTR;
    d.align = LV_TEXT_ALIGN_LEFT;
    d.color = color;
    d.opa = opa;
    d.flag = LV_TEXT_FLAG_EXPAND;
    if (s->battery.level == 0 || fill->x2 < fill->x1) {
        lv_draw_label(layer, &d, &area);
        return;
    }
    /* Colour only the empty portion. Drawing the same translucent ink over the
     * filled portion first would brighten its antialiased edges twice. */
    lv_area_t clips[] = {
        {body->x1, body->y1, fill->x1 - 1, body->y2},
        {fill->x2 + 1, body->y1, body->x2, body->y2},
        {fill->x1, body->y1, fill->x2, fill->y1 - 1},
        {fill->x1, fill->y2 + 1, fill->x2, body->y2},
    };
    for (size_t i = 0; i < sizeof(clips) / sizeof(clips[0]); i++) {
        label_clipped(layer, &clips[i], &d, &area);
    }
    /* A knock-out restores the supplied flat backdrop at full coverage, even
     * when the ink is faded. Fading this erasure would leave fill inside digits.
     * Glyph coverage supplies edge antialiasing; the glyphs sit inside the fill's
     * corner clearance, so the rectangular split never cuts a rounded corner. */
    d.color = s->background;
    d.opa = LV_OPA_COVER;
    label_clipped(layer, fill, &d, &area);
}

static void content(lv_layer_t *layer, const status_t *s, const lv_area_t *body,
                    const lv_area_t *fill, lv_color_t color, lv_opa_t opa)
{
    if (!s->battery.percentage && !s->battery.charging) return;
    const lv_font_t *font = orb_status_font_for_cap(s->style, s->variant, s->cap_height);
    char number[4];
    snprintf(number, sizeof(number), "%d", s->battery.level);
    lv_point_t number_size = {0, 0}, bolt_size = {0, 0};
    if (s->battery.percentage) {
        lv_text_get_size(&number_size, number, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    if (s->battery.charging) {
        lv_text_get_size(&bolt_size, ORB_ICON_LIGHTNING_FILL, font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    int32_t gap = s->battery.percentage && s->battery.charging ? 1 : 0;
    int32_t width = number_size.x + gap + bolt_size.x;
    int32_t x = body->x1 + (lv_area_get_width(body) - width) / 2;
    lv_font_glyph_dsc_t figure;
    lv_font_get_glyph_dsc(font, &figure, s->battery.percentage ? '0' : 0xE153, 0);
    int32_t baseline = body->y1 + (lv_area_get_height(body) + (int32_t)figure.box_h) / 2 +
                       figure.ofs_y;
    if (s->battery.percentage) {
        content_run(layer, s, body, fill, color, opa, font, number, x, number_size.x, baseline);
    }
    if (s->battery.charging) {
        content_run(layer, s, body, fill, color, opa, font, ORB_ICON_LIGHTNING_FILL,
                     x + number_size.x + gap, bolt_size.x, baseline);
    }
}

static void draw_battery(lv_layer_t *layer, const status_t *s, const lv_area_t *coords,
                         lv_color_t label, lv_opa_t opa)
{
    lv_color_t color = label;
    if (s->battery.low_power) color = orb_color_from_rgb(ORB_RGB_LEGACY_YELLOW);
    else if (s->battery.charging) color = orb_color_from_rgb(ORB_RGB_GREEN);
    else if (s->battery.level <= 20) color = orb_color_from_rgb(ORB_RGB_POWER_LOW);
    int32_t center_y = coords->y1 + s->baseline - s->cap_height / 2;
    lv_area_t body = {coords->x1, center_y - s->body_height / 2,
                      coords->x1 + s->body_width - 1,
                      center_y - s->body_height / 2 + s->body_height - 1};
    int32_t radius = LV_MAX(2, rounded_ratio(s->body_height, 24, 100));
    rect(layer, &body, radius, color, opa, s->stroke);
    int32_t nub_h = LV_MAX(3, rounded_ratio(s->body_height, 36, 100));
    lv_area_t nub = {body.x2 + s->gap + 1, center_y - nub_h / 2,
                     body.x2 + s->gap + s->nub_width,
                     center_y - nub_h / 2 + nub_h - 1};
    rect(layer, &nub, s->nub_width, color, opa, 0);
    int32_t inset = s->stroke + LV_MAX(1, s->gap);
    lv_area_t fill = {body.x1 + inset, body.y1 + inset,
                      body.x2 - inset, body.y2 - inset};
    int32_t full_w = lv_area_get_width(&fill);
    fill.x2 = fill.x1 + rounded_ratio(full_w, s->battery.level, 100) - 1;
    if (fill.x2 >= fill.x1) {
        rect(layer, &fill, LV_MAX(1, radius - inset), color, opa, 0);
    }
    content(layer, s, &body, &fill, color, opa);
}

static void draw_wifi(lv_layer_t *layer, const status_t *s, const lv_area_t *coords,
                      lv_color_t label, lv_opa_t opa)
{
    int32_t radius = rounded_ratio(s->cap_height, 11, 10);
    int32_t center_x = (coords->x1 + coords->x2) / 2;
    int32_t center_y = coords->y1 + s->baseline + 1;
    int32_t stroke = LV_MAX(2, rounded_ratio(s->cap_height, 13, 100));
    if (s->stroke >= 2) stroke++;
    lv_opa_t inactive = (lv_opa_t)((uint32_t)opa * 77 / 255);
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = center_x;
    arc.center.y = center_y;
    arc.start_angle = 225;
    arc.end_angle = 315;
    arc.color = label;
    arc.width = stroke;
    arc.rounded = 0;
    for (int bar = 3; bar >= 1; bar--) {
        arc.radius = (uint16_t)rounded_ratio(radius, bar + 1, 4);
        arc.opa = s->wifi.connected && s->wifi.bars >= bar ? opa : inactive;
        lv_draw_arc(layer, &arc);
    }
    /* A small circular wedge anchors the fan even on a connected 0-bar link. */
    int32_t dot = LV_MAX(3, rounded_ratio(s->cap_height, 18, 100));
    lv_area_t dot_area = {center_x - dot / 2, center_y - dot / 2,
                          center_x - dot / 2 + dot - 1,
                          center_y - dot / 2 + dot - 1};
    rect(layer, &dot_area, LV_RADIUS_CIRCLE, label, s->wifi.connected ? opa : inactive, 0);
    if (!s->wifi.connected) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.p1.x = center_x - rounded_ratio(radius, 60, 100);
        line.p1.y = center_y - radius + 1;
        line.p2.x = center_x + rounded_ratio(radius, 60, 100);
        line.p2.y = center_y;
        line.width = stroke + 2;
        line.round_start = 1;
        line.round_end = 1;
        line.color = s->background;
        line.opa = opa;
        lv_draw_line(layer, &line);
        line.width = LV_MAX(1, stroke - 1);
        line.color = label;
        lv_draw_line(layer, &line);
    }
}

static void status_event(const lv_obj_class_t *class_p, lv_event_t *e)
{
    LV_UNUSED(class_p);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_DRAW_MAIN && code != LV_EVENT_DRAW_MAIN_END) {
        if (lv_obj_event_base(&status_class, e) != LV_RESULT_OK) return;
    }
    if (code != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t *obj = lv_event_get_current_target(e);
    const status_t *s = (const status_t *)obj;
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_color_t color = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    lv_opa_t opa = lv_obj_get_style_text_opa(obj, LV_PART_MAIN);
    /* Like lv_obj_init_draw_label_dsc: normal object/ancestor opacity is already
     * accumulated in this layer, but raw software draw primitives do not apply
     * it. Layered opacity is composited later and must not be multiplied here. */
    if (layer->opa < LV_OPA_MAX) opa = LV_OPA_MIX2(opa, layer->opa);
    if (s->kind == STATUS_BATTERY) draw_battery(layer, s, &coords, color, opa);
    else draw_wifi(layer, s, &coords, color, opa);
}
