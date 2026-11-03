#include "power_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_ui.h"
#include "src/misc/lv_text_private.h"

static void bare(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
}

static lv_obj_t *label(lv_obj_t *parent, orb_text_style_t style,
                       orb_text_variant_t variant)
{
    lv_obj_t *obj = lv_label_create(parent);
    bare(obj);
    orb_text_apply(obj, style, variant, 0);
    lv_obj_set_style_text_color(obj, ORB_LABEL, 0);
    lv_obj_set_hidden(obj, true);
    return obj;
}

void power_view_create(power_view_t *view, lv_obj_t *parent, bool increase_contrast)
{
    memset(view, 0, sizeof(*view));
    view->root = lv_obj_create(parent);
    bare(view->root);
    lv_obj_set_size(view->root, 466, 466);
    lv_obj_set_style_bg_color(view->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view->root, LV_OPA_COVER, 0);
    /* HIG §§4,7: a semantic Inter/tabular clock and Headline attention line.
     * A retained line never disappears merely to replay an entrance. */
    view->clock = label(view->root, ORB_TEXT_LARGE_TITLE, ORB_TEXT_TABULAR);
    view->mark = label(view->root, ORB_TEXT_HEADLINE, ORB_TEXT_TABULAR);
    view->hand = label(view->root, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR);
    orb_icon_apply(view->hand, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_label_set_text(view->hand, ORB_ICON_HAND);
    lv_color_t ink = increase_contrast ? lv_color_hex(0xFFA056) : ORB_ORANGE;
    lv_obj_set_style_text_color(view->hand, ink, 0);
    lv_obj_set_style_text_color(view->mark, ink, 0);
    view->source_image = lv_image_create(view->root);
    bare(view->source_image);
    lv_obj_set_hidden(view->source_image, true);
}

/* Ink bounds, rather than font line boxes, centre the visible group. Inter's
 * ascender space otherwise leaves a clock apparently above the equator.
 * These labels are single-line numeric/ASCII text and one Framework7 glyph. */
static void ink_bounds(lv_obj_t *obj, int *top, int *bottom)
{
    const lv_font_t *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    const char *text = lv_label_get_text(obj);
    *top = font->line_height;
    *bottom = 0;
    uint32_t i = 0;
    while (text[i]) {
        uint32_t cp = lv_text_encoded_next(text, &i);
        lv_font_glyph_dsc_t glyph;
        if (lv_font_get_glyph_dsc(font, &glyph, cp, 0) && glyph.box_h) {
            int y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
            if (y < *top) *top = y;
            if (y + glyph.box_h > *bottom) *bottom = y + glyph.box_h;
        }
    }
}

static void centred(lv_obj_t *obj, float x, float y)
{
    lv_obj_update_layout(obj);
    int top, bottom;
    ink_bounds(obj, &top, &bottom);
    /* A half-pixel tie uses one stable direction, including spring values
     * within 0.0005 px of the endpoint (well below display precision). */
    lv_obj_set_pos(obj, (int)floorf(x - lv_obj_get_width(obj) / 2.0f + .5005f),
                  (int)floorf(y - (top + bottom) / 2.0f + .5005f));
}

/* HIG §6, exact critically damped response .35 s, normalized at the bounded
 * endpoint. No piecewise pauses, overshoot or frame-rate-dependent stepping. */
static float settle(float elapsed, float duration)
{
    const float w = 2.0f * (float)M_PI / .35f;
    float end = 1 - (1 + w * duration) * expf(-w * duration);
    return (1 - (1 + w * elapsed) * expf(-w * elapsed)) / end;
}

static float mix(float a, float b, float p) { return a + (b - a) * p; }

static power_face_pose_t interpolate(power_face_pose_t from, power_face_pose_t to, float p)
{
    return (power_face_pose_t){
        mix(from.clock_x, to.clock_x, p), mix(from.clock_y, to.clock_y, p),
        mix(from.mark_x, to.mark_x, p), mix(from.mark_y, to.mark_y, p),
        mix(from.clock_opa, to.clock_opa, p), mix(from.mark_opa, to.mark_opa, p),
        mix(from.scale, to.scale, p)
    };
}

static void apply(lv_obj_t *clock, lv_obj_t *hand, lv_obj_t *mark, power_face_pose_t pose)
{
    lv_obj_update_layout(hand);
    lv_obj_update_layout(mark);
    int hw = lv_obj_get_width(hand), mw = lv_obj_get_width(mark);
    float start = pose.mark_x - (hw + 16 + mw) / 2.0f;
    centred(clock, pose.clock_x, pose.clock_y);
    centred(hand, start + hw / 2.0f, pose.mark_y);
    centred(mark, start + hw + 16 + mw / 2.0f, pose.mark_y);
    lv_obj_t *objects[] = {clock, hand, mark};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *obj = objects[i];
        float opacity = i ? pose.mark_opa : pose.clock_opa;
        lv_obj_set_hidden(obj, opacity <= 0);
        lv_obj_set_style_opa(obj, (lv_opa_t)lrintf(opacity * 255), 0);
        /* Scale the whole mark about its shared centre so the gap scales too. */
        lv_obj_set_style_transform_pivot_x(obj, (int)lrintf(pose.mark_x - lv_obj_get_x(obj)), 0);
        lv_obj_set_style_transform_pivot_y(obj, (int)lrintf(pose.mark_y - lv_obj_get_y(obj)), 0);
        lv_obj_set_style_transform_scale(obj, (int)lrintf(256 * (i ? pose.scale : 1)), 0);
    }
}

static void set_text(lv_obj_t *obj, const char *text)
{
    if (strcmp(lv_label_get_text(obj), text)) lv_label_set_text(obj, text);
}

bool power_view_update(power_view_t *view, const orb_power_snapshot_t *power,
                       power_face_data_t data, bool reduced, uint64_t now_ms)
{
    bool moment = power->moment != ORB_POWER_MOMENT_NONE && power->panel_on;
    uint64_t start_ms = moment ? power->now_ms - power->moment_elapsed_ms : 0;
    bool structural = !view->initialized || power->state != view->previous.state ||
        power->panel_on != view->previous.panel_on || power->moment != view->previous.moment ||
        reduced != view->reduced || (moment && start_ms != view->transition_ms);
    bool data_changed = data.hour != view->data.hour || data.minute != view->data.minute ||
        data.waiting != view->data.waiting;
    bool presence_changed = view->initialized && !!data.waiting != !!view->data.waiting;
    bool begin_reflow = presence_changed && power->state == ORB_POWER_ALWAYS_ON && !moment;
    bool changed = structural || data_changed || view->reflow ||
        power->shift_x != view->previous.shift_x || power->shift_y != view->previous.shift_y ||
        (moment && power->moment_elapsed_ms != view->previous.moment_elapsed_ms);
    if (!changed) return false;
    /* HIG §7: coalesce only the settled face, never suppress the endpoint of
     * a caused transition. No invalidation for an unchanged public minute. */
    if (!structural && !view->reflow && power->state == ORB_POWER_ALWAYS_ON && !moment &&
        now_ms - view->painted_ms < 1000) return false;

    if (begin_reflow) {
        view->reflow = true;
        view->reflow_ms = now_ms;
    } else if (structural) view->reflow = false;
    bool animate = moment || view->reflow;
    bool begin = structural || begin_reflow;
    if (begin && animate) {
        view->crossfade = reduced || (view->reduced && view->previous.moment != ORB_POWER_MOMENT_NONE);
        if (view->crossfade) {
            /* A fixed snapshot includes any partially dissolved source. It
             * preserves every pixel when sleep or an accessibility change
             * interrupts a fade, without restarting at an invented endpoint. */
            lv_draw_buf_t *source = lv_snapshot_take(view->root, LV_COLOR_FORMAT_RGB565);
            if (!source) return false;
            lv_obj_set_pos(view->source_image, 0, 0);
            lv_image_set_src(view->source_image, source);
            if (view->source_buffer) lv_draw_buf_destroy(view->source_buffer);
            view->source_buffer = source;
        }
    }
    if (begin && view->initialized) view->from = view->pose;
    if (view->reflow && (!begin || begin_reflow)) {
        /* Minute protection remains a discrete shift even during a reflow. */
        int dx = power->shift_x - view->previous.shift_x;
        int dy = power->shift_y - view->previous.shift_y;
        view->from.clock_x += dx; view->from.mark_x += dx;
        view->from.clock_y += dy; view->from.mark_y += dy;
        if (view->crossfade && (dx || dy))
            lv_obj_set_pos(view->source_image, lv_obj_get_x(view->source_image) + dx,
                           lv_obj_get_y(view->source_image) + dy);
    }
    char text[32];
    snprintf(text, sizeof(text), "%02u" ORB_TIME_SEP "%02u", data.hour % 24, data.minute % 60);
    set_text(view->clock, text);
    if (data.waiting > 99) snprintf(text, sizeof(text), "99+ waiting");
    else snprintf(text, sizeof(text), "%u waiting", data.waiting);
    /* A disappearing mark keeps its existing count until its ink is gone. */
    if (data.waiting || !view->initialized || !animate) set_text(view->mark, text);
    int ct, cb, mt, mb;
    ink_bounds(view->clock, &ct, &cb);
    ink_bounds(view->mark, &mt, &mb);
    int ht, hb;
    ink_bounds(view->hand, &ht, &hb);
    int mark_height = mb - mt > hb - ht ? mb - mt : hb - ht;
    /* Centre the visible ink union with 20 px breathing room, at every type
     * size. Zero attention centres the clock itself on the circle's equator. */
    float clock_y = data.waiting ? 233 - (20 + mark_height) / 2.0f : 233;
    float mark_y = 233 + (cb - ct + 20) / 2.0f;
    bool desk = power->state != ORB_POWER_ACTIVE;
    power_face_pose_t target = {233 + power->shift_x,
        clock_y + power->shift_y - (!desk && !reduced ? 2 : 0),
        233 + power->shift_x, mark_y + power->shift_y,
        power->panel_on ? 1 : 0, power->panel_on && data.waiting ? 1 : 0, 1};
    if (power->moment == ORB_POWER_MOMENT_SLEEP) target.clock_opa = target.mark_opa = 0;
    if (structural && power->moment == ORB_POWER_MOMENT_WAKE && power->wake_from_sleep &&
        (!view->initialized || !view->previous.panel_on)) {
        view->from = target;
        view->from.clock_opa = view->from.mark_opa = 0;
        view->from.clock_y += reduced ? 0 : 2;
        view->from.scale = reduced ? 1 : .9f;
    }
    if (structural) {
        view->transition_ms = start_ms;
        if (!view->initialized) {
            view->from = target;
            if (moment && power->moment == ORB_POWER_MOMENT_WAKE) {
                view->from.clock_opa = view->from.mark_opa = 0;
                view->from.clock_y += reduced ? 0 : 2;
                view->from.scale = reduced ? 1 : .9f;
            }
        }
    }
    float progress = 1;
    if (animate) {
        float duration = (moment ? power->moment_duration_ms : reduced ? 150 : 250) / 1000.0f;
        float elapsed = (moment ? power->moment_elapsed_ms : now_ms - view->reflow_ms) / 1000.0f;
        progress = elapsed >= duration ? 1 : reduced ? elapsed / duration : settle(elapsed, duration);
    }
    view->progress = progress;
    view->pose = target;
    if (animate && view->crossfade) {
        /* HIG §6: a full, opaque source over the fixed target produces an
         * exact cross-fade, including overlapping ink and interrupted fades. */
        view->pose.scale = 1;
    } else if (animate) view->pose = interpolate(view->from, target, progress);
    lv_obj_set_hidden(view->source_image, !animate || !view->crossfade || progress == 1);
    if (animate && view->crossfade && progress < 1)
        lv_obj_set_style_opa(view->source_image, (lv_opa_t)lrintf(255 * (1 - progress)), 0);
    else if (view->source_buffer) {
        lv_image_set_src(view->source_image, NULL);
        lv_draw_buf_destroy(view->source_buffer);
        view->source_buffer = NULL;
    }
    if (progress == 1) view->reflow = false;
    apply(view->clock, view->hand, view->mark, view->pose);
    view->initialized = true;
    view->reduced = reduced;
    view->data = data;
    view->previous = *power;
    view->painted_ms = now_ms;
    return true;
}

uint32_t power_view_frame_interval_ms(const power_view_t *view)
{
    return view && view->reflow ? 16 : 0;
}
