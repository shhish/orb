#include "orb_game_view_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw_mask_private.h"
#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "orb_memory.h"
#include "esp_timer.h"
#else
#include <time.h>
#endif

#define INTRO_MS 1400u
#define MARBLE_BLOOM_MS 350u
#define MARBLE_PLANT_MS 850u
#define BEAD_DAMAGE_HEIGHT 32
#define BEAD_DAMAGE_ROWS ((466 + BEAD_DAMAGE_HEIGHT - 1) / BEAD_DAMAGE_HEIGHT)
#define BEAD_DIAMETER (ORB_GAME_BEAD_DRAW_RADIUS * 2u + 1u)
static lv_opa_t s_bead_body_mask[BEAD_DIAMETER * BEAD_DIAMETER];
static bool s_bead_masks_ready;
static uint64_t now_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
#endif
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *value, int x, int y,
                      int width, orb_text_style_t style)
{
    lv_obj_t *o = lv_label_create(parent);
    orb_text_apply(o, style, ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    lv_obj_set_style_text_color(o, ORB_LABEL, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(o, width);
    lv_label_set_text_static(o, value);
    lv_obj_update_layout(o);
    lv_obj_set_pos(o, x - width / 2, y - lv_obj_get_height(o) / 2);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_area_t bounds(const orb_game_sprite_t *s)
{
    int x = (int)lroundf(s->x), y = (int)lroundf(s->y), r = s->radius;
    if (s->shape == ORB_GAME_GATE && s->arc_radius > 0) {
        float x1 = cosf(s->arc_start), x2 = cosf(s->arc_end);
        float y1 = sinf(s->arc_start), y2 = sinf(s->arc_end);
        float left = fminf(x1, x2), right = fmaxf(x1, x2);
        float top = fminf(y1, y2), bottom = fmaxf(y1, y2);
        for (int i = -4; i <= 12; ++i) {
            float a = i * TAU / 4;
            if (a < s->arc_start || a > s->arc_end) continue;
            left = fminf(left, cosf(a)); right = fmaxf(right, cosf(a));
            top = fminf(top, sinf(a)); bottom = fmaxf(bottom, sinf(a));
        }
        /* LVGL rasterises its arc angles at integer degrees. At r188 that
         * can move a cap by another 4 px beyond the analytic endpoint. */
        int inset = r + 2 + (int)ceilf(s->arc_radius * TAU / 360);
        return (lv_area_t){x + (int)floorf(left * s->arc_radius) - inset,
            y + (int)floorf(top * s->arc_radius) - inset,
            x + (int)ceilf(right * s->arc_radius) + inset,
            y + (int)ceilf(bottom * s->arc_radius) + inset};
    }
    return (lv_area_t){x - r - 1, y - r - 1, x + r + 1, y + r + 1};
}

static void finish_layer(lv_layer_t *layer)
{
    while (layer->draw_task_head) {
        lv_draw_dispatch();
        lv_draw_wait_for_finish();
    }
}

static bool blend_mask565(lv_draw_task_t *task, const lv_area_t *area,
                           const uint8_t *mask, int mask_stride, lv_color_t colour)
{
    lv_layer_t *layer = task->target_layer;
    bool swapped = layer->color_format == LV_COLOR_FORMAT_RGB565_SWAPPED;
    if (!swapped && layer->color_format != LV_COLOR_FORMAT_RGB565) return false;
    int left = LV_MAX(area->x1, task->clip_area.x1), right = LV_MIN(area->x2, task->clip_area.x2);
    int top = LV_MAX(area->y1, task->clip_area.y1), bottom = LV_MIN(area->y2, task->clip_area.y2);
    if (left > right || top > bottom) return true;
    uint16_t ink = lv_color_to_u16(colour);
    uint16_t stored_ink = swapped ? lv_color_swap_16(ink) : ink;
    uint32_t prepared = LV_COLOR_MIX_16_PREPARE(ink);
    size_t stride = layer->draw_buf->header.stride;
    uint8_t *destination = layer->draw_buf->data + (top - layer->buf_area.y1) * stride +
        (left - layer->buf_area.x1) * 2;
    if (mask) mask += (top - area->y1) * mask_stride + left - area->x1;
    /* Identical branches and prepared RGB565 arithmetic to LVGL's native
     * masked fill, inlined around tiny stamps/spans to avoid a full blend
     * dispatcher, format selection and descriptor setup for every bead. */
    for (int y = top; y <= bottom; ++y) {
        uint16_t *pixels = (uint16_t *)destination;
        for (int x = 0; x <= right - left; ++x) {
            uint32_t alpha = mask ? mask[x] : 255;
            if (!alpha) continue;
            if (alpha >= LV_OPA_MAX) pixels[x] = stored_ink;
            else {
                uint16_t value = swapped ? lv_color_swap_16(pixels[x]) : pixels[x];
                LV_COLOR_MIX_16_TO_16_PREPARED(value, prepared, value, alpha);
                pixels[x] = swapped ? lv_color_swap_16(value) : value;
            }
        }
        destination += stride;
        if (mask) mask += mask_stride;
    }
    return true;
}

static void ring_span(lv_draw_task_t *task, int y, int left, int right,
                       const uint8_t *alpha, bool mirror, lv_color_t colour)
{
    if (left > right) return;
    lv_area_t area = {left, y, right, y};
    uint8_t reversed[234];
    if (mirror) {
        area.x1 = 466 - right; area.x2 = 466 - left;
        if (area.x2 < task->clip_area.x1 || area.x1 > task->clip_area.x2) return;
        if (alpha) {
            for (int i = 0; i <= right - left; ++i) reversed[i] = alpha[right - left - i];
            alpha = reversed;
        }
    }
    if (area.x2 < task->clip_area.x1 || area.x1 > task->clip_area.x2) return;
    if (blend_mask565(task, &area, alpha, right - left + 1, colour)) return;
    lv_draw_sw_blend_dsc_t blend = {0};
    blend.color = colour;
    blend.opa = LV_OPA_COVER;
    blend.blend_area = &area;
    blend.mask_area = &area;
    blend.mask_buf = alpha;
    blend.mask_res = alpha ? LV_DRAW_SW_MASK_RES_CHANGED : LV_DRAW_SW_MASK_RES_FULL_COVER;
    lv_draw_sw_blend(task, &blend);
}

static void draw_cached_ring(lv_layer_t *layer, const ring_cache_t *cache, uint32_t colour)
{
    int radius = cache->radius, origin = 233 - radius;
    const lv_area_t *clip = &layer->_clip_area;
    int near_x = clip->x1 > 233 ? clip->x1 - 233 : clip->x2 < 233 ? 233 - clip->x2 : 0;
    int near_y = clip->y1 > 233 ? clip->y1 - 233 : clip->y2 < 233 ? 233 - clip->y2 : 0;
    int far_x = LV_MAX(abs(clip->x1 - 233), abs(clip->x2 - 233));
    int far_y = LV_MAX(abs(clip->y1 - 233), abs(clip->y2 - 233));
    int outer = radius + 2, inner = LV_MAX(0, radius - (int)cache->width - 2);
    if (near_x * near_x + near_y * near_y > outer * outer ||
        far_x * far_x + far_y * far_y < inner * inner) return;
    finish_layer(layer);
    int first_y = LV_MAX(origin, layer->_clip_area.y1);
    int last_y = LV_MIN(233 + radius, layer->_clip_area.y2);
    const uint8_t *samples = (const uint8_t *)(cache->rows + radius + 1);
    lv_draw_task_t task = {0};
    task.target_layer = layer;
    task.clip_area = layer->_clip_area;
    for (int y = first_y; y <= last_y; ++y) {
        int row_y = y - origin;
        if (row_y > radius) row_y = radius * 2 - row_y;
        const ring_row_t *row = &cache->rows[row_y];
        if (row->first > row->last) continue;
        const uint8_t *alpha = samples + row->alpha_offset;
        int before = row->opaque_first - row->first;
        for (unsigned side = 0; side < 2; ++side) {
            /* The centre column is stored/drawn on the left half only. */
            int limit = side ? radius - 1 : radius;
            int span_first = origin + row->first, span_last = origin + LV_MIN(row->last, limit);
            if (side) { int old_first = span_first; span_first = 466 - span_last; span_last = 466 - old_first; }
            if (span_last < clip->x1 || span_first > clip->x2) continue;
            ring_span(&task, y, origin + row->first,
                origin + LV_MIN((int)row->opaque_first - 1, limit), alpha, side != 0, orb_color_from_rgb(colour));
            ring_span(&task, y, origin + row->opaque_first,
                origin + LV_MIN(row->opaque_last, limit), NULL, side != 0, orb_color_from_rgb(colour));
            ring_span(&task, y, origin + row->opaque_last + 1,
                origin + LV_MIN(row->last, limit), alpha + before, side != 0, orb_color_from_rgb(colour));
        }
    }
}

static void prepare_bead_masks(void)
{
    if (s_bead_masks_ready) return;
    s_bead_masks_ready = orb_game_native_circle(s_bead_body_mask, ORB_GAME_BEAD_DRAW_RADIUS);
}

static bool draw_bead_batch(orb_game_view_t *v, lv_layer_t *layer)
{
    ++v->stats.bead_draw_calls;
    v->stats.bead_masks_ready = s_bead_masks_ready;
    v->stats.bead_source_ready = v->beads_draw != NULL;
    if (!s_bead_masks_ready) { ++v->stats.bead_masks_missing; return false; }
    if (!v->bead_batch_eligible) { ++v->stats.bead_ineligible; return false; }
    if (orb_game_beads_draw_dispatch(v->beads_draw, layer, v->bead_batch_count,
        v->preview.game.beads.target_x, v->preview.game.beads.target_y,
        v->preview.game.beads.target_opacity, v->sprites, v->stats.updates)) {
        ++v->stats.bead_dispatched;
        return true;
    }
    /* Optional source/pose OOM and a busy lease retain the exact serial path. */
    ++v->stats.bead_serial;
    uint64_t started = now_us();
    finish_layer(layer);
    v->stats.bead_drain_us += now_us() - started;
    started = now_us();
    orb_game_native_beads(layer, &v->bead_masks, v->bead_batch_count, v->preview.game.beads.target_x,
        v->preview.game.beads.target_y, v->preview.game.beads.target_opacity, v->sprites);
    v->stats.bead_native_us += now_us() - started;
    return true;
}

static void draw(lv_event_t *e)
{
    orb_game_view_t *v = lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (v->kind == ORB_GAME_BEADS && draw_bead_batch(v, layer)) return;
    if (v->scene == SCENE_PLAY && v->kind == ORB_GAME_ORBIT && !v->serial_orbit &&
        orb_game_orbit_dispatch(v->orbit, layer, v->sprites, v->count, v->stats.updates, 0x2c2c2e)) return;
    /* These are content paths on black, never glass. LVGL clips them to the
     * submitted old/new sprite damage, even when a path spans the full field. */
    if (v->scene == SCENE_START && v->kind == ORB_GAME_ORBIT) {
        /* Review #33: the thumbnail is the real arena, so it needs the three
         * lanes and the sun the score disc stands for. Gray3 keeps a 1 px
         * lane visible on the art disc's Gray6. */
        static const float lanes[] = {ORB_GAME_ORBIT_R0, ORB_GAME_ORBIT_R1, ORB_GAME_ORBIT_R2};
        for (unsigned i = 0; i < 3; ++i)
            game_sprite_disc(layer, 233, ART_Y, (int)lroundf(lanes[i] * PREVIEW_SCALE), 0x48484a, 255, 1);
        game_sprite_disc(layer, 233, ART_Y, (int)lroundf(ORB_GAME_ORBIT_SUN * PREVIEW_SCALE), 0xffffff, 255, 0);
    }
    if (v->scene == SCENE_PLAY && v->kind == ORB_GAME_ORBIT) {
        /* Three thin orbit rings and the white sun that carries the score. */
        static const int radii[] = {(int)ORB_GAME_ORBIT_R0 + 1,
            (int)ORB_GAME_ORBIT_R1 + 1, (int)ORB_GAME_ORBIT_R2 + 1};
        for (unsigned i = 0; i < 3; ++i) {
            if (v->rings[i]) draw_cached_ring(layer, v->rings[i], 0x2c2c2e);
            else game_sprite_disc(layer, 233, 233, radii[i], 0x2c2c2e, 255, 2);
        }
    }
    for (size_t i = 0; i < v->count; ++i) {
        /* Reject before draw-task allocation, including the exact gate bounds. */
        const orb_game_sprite_t *sprite = &v->sprites[i];
        lv_area_t area = sprite->shape == ORB_GAME_GATE && i >= 2 &&
            i - 2 < ORB_MARBLE_GATES_MAX * ORB_MARBLE_GATE_SEGMENTS ?
            v->gate_areas[i - 2] : bounds(sprite);
        const lv_area_t *clip = &layer->_clip_area;
        if (area.x2 < clip->x1 || area.x1 > clip->x2 ||
            area.y2 < clip->y1 || area.y1 > clip->y2) continue;
        if (v->kind == ORB_GAME_POP && v->scene == SCENE_PLAY && sprite->shape == ORB_GAME_BUBBLE &&
            orb_game_pop_draw(v->pop_coverage, layer, sprite)) continue;
        game_sprite_draw(layer, &v->sprites[i]);
    }
}

static void clicked(lv_event_t *e)
{
    orb_game_view_t *v = lv_event_get_user_data(e);
    if (!v->callback) return;
    lv_obj_t *o = lv_event_get_target_obj(e);
    int action = (int)(intptr_t)lv_obj_get_user_data(o);
    if (v->kind == ORB_GAME_MARBLE && v->field_long_contact &&
        action >= ORB_GAME_LEFT && action <= ORB_GAME_DOWN) return;
    v->callback(action, 0, 0, v->context);
}

static void field_contact(lv_event_t *e)
{
    orb_game_view_t *v = lv_event_get_user_data(e);
    if (!v->callback) return;
    lv_event_code_t code = lv_event_get_code(e);
    /* HIG §7: cumulative travel chooses a drag even on a non-scrollable
     * canvas. LVGL otherwise emits CLICKED/SHORT_CLICKED after such a drag.
     * Once cancelled, returning to the origin cannot turn it back into a tap. */
    lv_indev_t *contact = lv_event_get_indev(e);
    if (contact && (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED)) {
        lv_point_t p;
        lv_indev_get_point(contact, &p);
        if (code == LV_EVENT_PRESSED) {
            v->contact_x = (float)p.x;
            v->contact_y = (float)p.y;
            v->contact_at = lv_tick_get();
            v->field_long_contact = false;
        } else if (hypotf((float)p.x - v->contact_x, (float)p.y - v->contact_y) >= 10 ||
            lv_tick_get() - v->contact_at >= 500) {
            v->field_long_contact = true;
        }
    }
    if (v->kind == ORB_GAME_ORBIT || v->kind == ORB_GAME_POP) return;
    if (v->kind == ORB_GAME_SNAKE || (v->kind == ORB_GAME_BEADS && v->controls)) {
        /* HIG §§6/8: hold anywhere in the free arena, track the finger
         * directly, and always deliver cancellation. No relative arrows or
         * glass targets cover the snake's continuous movement. */
        if ((code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) && v->scene == SCENE_PLAY) {
            lv_indev_t *indev = lv_event_get_indev(e);
            if (indev) {
                lv_point_t p;
                lv_indev_get_point(indev, &p);
                v->field_held = true;
                v->callback(ORB_GAME_STEER_START, (float)p.x, (float)p.y, v->context);
            }
        } else if (code == LV_EVENT_SHORT_CLICKED && v->scene == SCENE_PLAY && !v->field_long_contact) {
            v->callback(ORB_GAME_PAUSE, 0, 0, v->context);
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            v->field_held = false;
            v->callback(ORB_GAME_STEER_STOP, 0, 0, v->context);
        }
        return;
    }
    bool marble = v->kind == ORB_GAME_MARBLE;
    /* LVGL's SHORT_CLICKED excludes contacts that reached LONG_PRESSED.
     * A rewind's release can therefore never also open Pause (HIG §8). The
     * same full-screen target includes the rim, with no floating game chrome. */
    if (code == LV_EVENT_PRESSED) {
        v->field_long_contact = false;
        v->rim_contact = false;
        lv_indev_t *indev = lv_event_get_indev(e);
        if (marble && indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            float dx = (float)p.x - 233, dy = (float)p.y - 233;
            v->rim_contact = hypotf(dx, dy) >= 193;
            v->rim_angle = atan2f(dy, dx);
            v->rim_travel = 0;
        }
    } else if (code == LV_EVENT_PRESSING && v->scene == SCENE_PLAY &&
               marble && v->rim_contact && !v->field_held) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        float dx = (float)p.x - 233, dy = (float)p.y - 233;
        if (hypotf(dx, dy) < 193) { v->rim_contact = false; return; }
        float angle = atan2f(dy, dx), delta = angle - v->rim_angle;
        if (delta > TAU / 2) delta -= TAU;
        if (delta < -TAU / 2) delta += TAU;
        v->rim_angle = angle;
        v->rim_travel += delta;
        /* HIG §§6/7: a tangential rim gesture answers immediately; it does
         * not wait for the hold timeout. Release stops, without a fling. */
        if (fabsf(v->rim_travel) * 215 >= 8) {
            v->field_held = v->field_long_contact = true;
            v->callback(ORB_GAME_REWIND_START, 0, 0, v->context);
        }
    } else if (code == LV_EVENT_LONG_PRESSED && v->scene == SCENE_PLAY && marble) {
        v->field_held = true;
        v->field_long_contact = true;
        v->callback(ORB_GAME_REWIND_START, 0, 0, v->context);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && v->field_held) {
        v->field_held = false;
        v->callback(ORB_GAME_REWIND_STOP, 0, 0, v->context);
    } else if (code == LV_EVENT_SHORT_CLICKED && v->scene == SCENE_PLAY &&
               !v->field_long_contact) {
        lv_indev_t *indev = lv_event_get_indev(e);
        if (marble && v->controls && indev) {
            /* Owner requirement: no buttons on the garden. Touch fallback
             * uses the field itself; its centre and PWR still open Pause. */
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int dx = p.x - 233, dy = p.y - 233;
            if (abs(dx) > 46 || abs(dy) > 46) {
                int action = abs(dx) > abs(dy) ?
                    (dx < 0 ? ORB_GAME_LEFT : ORB_GAME_RIGHT) :
                    (dy < 0 ? ORB_GAME_UP : ORB_GAME_DOWN);
                v->callback(action, 0, 0, v->context);
                return;
            }
        }
        v->callback(ORB_GAME_PAUSE, 0, 0, v->context);
    }
}

static lv_obj_t *action_button(orb_game_view_t *v, const char *value, int action, bool primary)
{
    lv_obj_t *obj = primary ? orb_kit_button_primary_create(v->root, value, game_accent(v->kind)) :
        orb_kit_button_secondary_create(v->root, value);
    lv_obj_set_user_data(obj, (void *)(intptr_t)action);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, v);
    return obj;
}

/* The kit caches immutable ink at creation; its children are private, so read
 * the caption through the kit. Replace an action only at an idle scene boundary;
 * never rebuild ink while its material opacity is zero. Sizes stay factory-owned. */
void game_action_content(orb_game_view_t *v, lv_obj_t **slot,
                           const char *caption, int action, bool primary)
{
    if (strcmp(orb_kit_button_get_text(*slot), caption)) {
        lv_obj_delete(*slot);
        *slot = action_button(v, caption, action, primary);
        orb_glass_set_amount(*slot, 0);
        lv_obj_set_hidden(*slot, true);
    }
    lv_obj_set_user_data(*slot, (void *)(intptr_t)action);
}

static void controls_changed(lv_event_t *e)
{
    orb_game_view_t *v = lv_event_get_user_data(e);
    if (v->callback) v->callback(ORB_GAME_CONTROLS, 0, 0, v->context);
}

static void enter_play(orb_game_view_t *v)
{
    /* A retracting reading surface must not leave ink or hit targets over
     * live play. Materialise the next reading scene from zero (HIG §§1/2). */
    lv_obj_t *glass[] = {v->close, v->primary, v->replay, v->done,
        v->finish, v->garden};
    for (unsigned i = 0; i < sizeof(glass) / sizeof(glass[0]); ++i) {
        if (!glass[i]) continue;
        orb_glass_set_amount(glass[i], 0);
        lv_obj_set_hidden(glass[i], true);
        lv_obj_set_clickable(glass[i], false);
    }
}
static void tapped(lv_event_t *e)
{
    orb_game_view_t *v = lv_event_get_user_data(e);
    if (!v->callback || v->scene != SCENE_PLAY || v->field_long_contact ||
        lv_tick_get() - v->contact_at >= 500 || v->kind == ORB_GAME_MARBLE ||
        v->kind == ORB_GAME_BEADS || v->kind == ORB_GAME_SNAKE) return;
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    v->callback(ORB_GAME_TAP, (float)p.x, (float)p.y, v->context);
}

static void prepare_content_caches(orb_game_view_t *v, orb_game_kind_t kind)
{
    if (kind == ORB_GAME_BEADS) {
        prepare_bead_masks();
        if (s_bead_masks_ready) orb_game_native_beads_prepare(&v->bead_masks, s_bead_body_mask);
        if (s_bead_masks_ready) v->beads_draw = orb_game_beads_draw_create(&v->bead_masks);
    }
    if (kind == ORB_GAME_ORBIT) {
        static const unsigned orbit_radii[] = {(unsigned)ORB_GAME_ORBIT_R0 + 1,
            (unsigned)ORB_GAME_ORBIT_R1 + 1, (unsigned)ORB_GAME_ORBIT_R2 + 1};
        for (unsigned i = 0; i < 3; ++i) {
            v->rings[i] = orb_game_ring_prepare(orbit_radii[i], 2,
                &v->stats.ring_cache_bytes);
        }
        v->orbit = orb_game_orbit_create(v->rings);
    }
}

orb_game_view_t *orb_game_view_create(lv_obj_t *parent, orb_game_kind_t kind,
                                     orb_game_view_action_cb_t callback, void *context)
{
    if (!parent || kind >= ORB_GAME_COUNT) return NULL;
#ifdef ESP_PLATFORM
    orb_game_view_t *v = orb_memory_calloc(ORB_MEMORY_GAMES, 1, sizeof(*v));
#else
    orb_game_view_t *v = calloc(1, sizeof(*v));
#endif
    if (!v) return NULL;
    prepare_content_caches(v, kind);
    v->kind = kind; v->callback = callback; v->context = context; v->first = true;
    v->root = plain(parent);
    lv_obj_set_size(v->root, 466, 466);
    lv_obj_set_style_bg_color(v->root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(v->root, true);
    lv_obj_add_event_cb(v->root, tapped, LV_EVENT_CLICKED, v);
    {
        lv_obj_set_press_lock(v->root, false);
        lv_obj_add_event_cb(v->root, field_contact, LV_EVENT_PRESSED, v);
        lv_obj_add_event_cb(v->root, field_contact, LV_EVENT_PRESSING, v);
        lv_obj_add_event_cb(v->root, field_contact, LV_EVENT_LONG_PRESSED, v);
        lv_obj_add_event_cb(v->root, field_contact, LV_EVENT_RELEASED, v);
        lv_obj_add_event_cb(v->root, field_contact, LV_EVENT_PRESS_LOST, v);
        lv_obj_add_event_cb(v->root, field_contact, LV_EVENT_SHORT_CLICKED, v);
    }
    v->canvas = plain(v->root);
    lv_obj_set_size(v->canvas, 466, 466);
    lv_obj_set_clickable(v->canvas, false);
    lv_obj_add_event_cb(v->canvas, draw, LV_EVENT_DRAW_MAIN, v);

    /* HIG §§1/2/7: artwork and text are content. Only factory controls
     * carry glass; all sizes, press cancellation and focus come from the kit. */
    v->art = orb_kit_disc_create(v->root, ORB_KIT_DISC_ART, ORB_KIT_ACCENT_FILL, ORB_GRAY6, NULL);
    lv_obj_set_clickable(v->art, false);
    game_place(v->art, 233, ART_Y);
    lv_obj_move_to_index(v->art, 0);
    v->title = orb_kit_rim_title_create(v->root, orb_game_name(kind), game_accent(kind));
    v->detail = text(v->root, "", 233, 274, 346, ORB_TEXT_FOOTNOTE1);
    v->best = text(v->root, v->best_text, 233, 247, 326, ORB_TEXT_CAPTION2);
    v->hud_title = text(v->root, "", 233, 120, 280, ORB_TEXT_FOOTNOTE2);
    v->close = orb_kit_disc_create(v->root, ORB_KIT_DISC_KIND_MINI,
        ORB_KIT_NEUTRAL_GLASS, ORB_KIT_GREY, ORB_ICON_X);
    game_place(v->close, 101, 101);
    lv_obj_set_user_data(v->close, (void *)(intptr_t)ORB_GAME_CLOSE);
    lv_obj_add_event_cb(v->close, clicked, LV_EVENT_CLICKED, v);
    /* Reserve the longest semantic label at creation: content changes reuse
     * the factory's prepared geometry instead of resizing glass during play. */
    v->primary = action_button(v, "Resume", ORB_GAME_START, true);
    v->finish = action_button(v, "End round", ORB_GAME_FINISH, false);
    v->done = action_button(v, "Exit", ORB_GAME_CLOSE, false);
    v->replay = action_button(v, "Play again", ORB_GAME_RESTART, true);
    if (game_tilt(kind)) {
        if (kind == ORB_GAME_MARBLE) {
            /* Keepsakes use the existing star until the next glyph release
             * supplies a flower. The Garden stays reachable before/after a
             * round without a fourth glass action on Pause (HIG §§2/7). */
            v->garden = orb_kit_disc_create(v->root, ORB_KIT_DISC_KIND_MINI,
                ORB_KIT_NEUTRAL_GLASS, ORB_KIT_GREY, ORB_ICON_STAR);
            game_place(v->garden, 365, 101);
            lv_obj_set_user_data(v->garden, (void *)(intptr_t)ORB_GAME_GARDEN);
            lv_obj_add_event_cb(v->garden, clicked, LV_EVENT_CLICKED, v);
        }
        v->settings_list = plain(v->root);
        lv_obj_set_size(v->settings_list, 466, ORB_KIT_ROW_H);
        lv_obj_set_y(v->settings_list, 184);
        lv_obj_set_clickable(v->settings_list, false);
        /* Review #32: the input option names itself with a hand in the
         * game's own accent, never Play's green transport glyph. */
        v->settings = plain(v->settings_list);
        lv_obj_set_style_radius(v->settings, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(v->settings, ORB_GRAY6, 0);
        lv_obj_set_style_bg_opa(v->settings, LV_OPA_COVER, 0);
        lv_obj_set_clickable(v->settings, true);
        orb_kit_press_apply(v->settings);
        v->settings_icon = orb_kit_disc_create(v->settings, ORB_KIT_DISC_KIND_ROW,
            ORB_KIT_ACCENT_FILL, game_accent(kind), ORB_ICON_HAND_TAP);
        lv_obj_set_clickable(v->settings_icon, false);
        v->settings_label = text(v->settings, "Touch controls", 0, 0, 240, ORB_TEXT_FOOTNOTE2);
        lv_obj_set_style_text_align(v->settings_label, LV_TEXT_ALIGN_LEFT, 0);
        v->controls_track = orb_kit_switch_create(v->settings, false, game_accent(kind));
        lv_obj_add_event_cb(v->settings, controls_changed, LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(v->controls_track, controls_changed, LV_EVENT_VALUE_CHANGED, v);
    }
    v->score_hit = orb_kit_disc_create(v->root, ORB_KIT_DISC_STANDARD,
        ORB_KIT_NEUTRAL_GLASS, ORB_KIT_GREY, NULL);
    lv_obj_set_user_data(v->score_hit, (void *)(intptr_t)ORB_GAME_PAUSE);
    lv_obj_add_event_cb(v->score_hit, clicked, LV_EVENT_CLICKED, v);
    v->score_clip = plain(v->score_hit);
    lv_obj_set_size(v->score_clip, 80, 42);
    lv_obj_set_y(v->score_clip, 19);
    lv_obj_set_clickable(v->score_clip, false);
    v->old_score = text(v->score_clip, v->old_score_text, 40, 21, 80, ORB_TEXT_FOOTNOTE2);
    v->score = text(v->score_clip, v->score_text, 40, 21, 80, ORB_TEXT_FOOTNOTE2);
    game_hud_create(v);
    v->big_score = text(v->root, v->big_score_text, 233, 171, 326, ORB_TEXT_TITLE1);
    lv_obj_set_style_text_color(v->big_score, ORB_LABEL, 0);
    v->bonus = text(v->root, v->bonus_text, 233, 233, 120, ORB_TEXT_FOOTNOTE2);
    lv_obj_set_style_text_color(v->bonus, game_accent(kind), 0);
    for (unsigned i = 0; i < 4; ++i) {
        static const char *const glyphs[] = {ORB_ICON_ARROW_LEFT, ORB_ICON_ARROW_RIGHT,
            ORB_ICON_ARROW_UP, ORB_ICON_ARROW_DOWN};
        v->arrows[i] = orb_kit_glyph_button_create(v->root, glyphs[i], false, ORB_KIT_GREY);
        lv_obj_set_user_data(v->arrows[i], (void *)(intptr_t)(ORB_GAME_LEFT + i));
        lv_obj_add_event_cb(v->arrows[i], clicked, LV_EVENT_CLICKED, v);
        game_place(v->arrows[i], i == 0 ? 183 : i == 1 ? 283 : 233, i < 2 ? 302 : i == 2 ? 183 : 343);
    }
    v->stage_dots = orb_kit_page_dots_create(v->root, ORB_MARBLE_STAGES);
    /* Review #34: the kit's r186 dots sat on Marble's outer wall (r182–194).
     * Translating the whole row puts them on the rim at r214, inside the
     * 10 px glass margin; three dots stay concentric to within 0.1 px. */
    lv_obj_align(v->stage_dots, LV_ALIGN_CENTER, 0, 214 - 186);
    v->timer_ring = orb_kit_ring_create(v->root, 218, 10, ORB_KIT_HOME);
    /* Review #33: the thumbnail is a real round's opening pose, the same
     * still the Play picker shows (HIG §1), never a staged arrangement. */
    orb_game_preview(&v->preview, kind);
    /* Start scene-specific chrome hidden for safe cold entry and transitions. */
    for (uint32_t i = 0; i < lv_obj_get_child_count(v->root); ++i) {
        lv_obj_t *child = lv_obj_get_child(v->root, (int32_t)i);
        if (child != v->canvas) lv_obj_set_hidden(child, true);
    }
    return v;
}

static uint32_t damage_area(orb_game_view_t *v, lv_area_t a)
{
    lv_area_t sprite_area = a;
    bool dependent = false;
    if (v->kind == ORB_GAME_ORBIT) lv_obj_invalidate_area(v->canvas, &a);
    /* Attract/reward sprites can pass under a floating control. Its blur must
     * receive the complete backdrop again, including unchanged neighbouring
     * pixels, rather than a tiny particle-shaped slice of the old material. */
    /* Design 2.0's secondary actions, score disc and steering buttons sample
     * glass too. Retain their full backdrops, but keep damage bounded when no
     * visible control depends on the changed sprite pixels. */
    lv_obj_t *chrome[] = {v->close, v->primary, v->replay, v->done,
        v->finish, v->garden, v->score_hit,
        v->arrows[0], v->arrows[1], v->arrows[2], v->arrows[3]};
    for (unsigned i = 0; i < sizeof(chrome) / sizeof(chrome[0]); ++i) {
        if (!chrome[i]) continue;
        if (lv_obj_is_hidden(chrome[i])) continue;
        lv_area_t area;
        lv_obj_get_coords(chrome[i], &area);
        lv_area_increase(&area, 26, 26);
        if (sprite_area.x1 <= area.x2 && sprite_area.x2 >= area.x1 &&
            sprite_area.y1 <= area.y2 && sprite_area.y2 >= area.y1) {
            /* Keep the blur/dither sample origin identical to a complete
             * render. Only this affected band widens; the field stays retained. */
            if (v->kind != ORB_GAME_ORBIT) {
                dependent = true;
                a.x1 = 0; a.x2 = 465;
                a.y1 = LV_MIN(a.y1, area.y1);
                a.y2 = LV_MAX(a.y2, area.y2);
            } else {
                /* Orbit's complete80px control+26px dependency halo is
                 * retained. Its absolute origin and bandguard stay unchanged;
                 * only unrelated columns leave this damage request. */
                lv_obj_invalidate_area(v->canvas, &area);
            }
        }
    }
    /* All sprites depending on the same glass must share one backdrop pass.
     * Besides duplicate work, overlapping invalidations can blur an already
     * painted control again when LVGL retains both rectangles. */
    if (dependent) {
        if (v->chrome_dirty) {
            v->chrome_damage.y1 = LV_MIN(v->chrome_damage.y1, a.y1);
            v->chrome_damage.y2 = LV_MAX(v->chrome_damage.y2, a.y2);
        } else {
            v->chrome_damage = a;
            v->chrome_dirty = true;
        }
        return 0;
    }
    if (v->kind != ORB_GAME_ORBIT) lv_obj_invalidate_area(v->canvas, &a);
    return (uint32_t)((a.x2 - a.x1 + 1) * (a.y2 - a.y1 + 1));
}

static uint32_t damage(orb_game_view_t *v, const orb_game_sprite_t *s)
{
    if (!s->radius || !s->opacity) return 0;
    return damage_area(v, bounds(s));
}

static void area_join(lv_area_t *out, const lv_area_t *a, const lv_area_t *b)
{
    *out = (lv_area_t){LV_MIN(a->x1, b->x1), LV_MIN(a->y1, b->y1),
                      LV_MAX(a->x2, b->x2), LV_MAX(a->y2, b->y2)};
}

static uint32_t damage_pair(orb_game_view_t *v, const orb_game_sprite_t *a,
                             const orb_game_sprite_t *b)
{
    if (!a->radius || !a->opacity) return damage(v, b);
    if (!b->radius || !b->opacity) return damage(v, a);
    lv_area_t before = bounds(a), after = bounds(b), joined;
    area_join(&joined, &before, &after);
    if (lv_area_get_size(&joined) <= lv_area_get_size(&before) + lv_area_get_size(&after))
        return damage_area(v, joined);
    return damage_area(v, before) + damage_area(v, after);
}

static void bead_damage_rows(lv_area_t *rows, const orb_game_sprite_t *sprite)
{
    if (!sprite->radius || !sprite->opacity) return;
    lv_area_t area = bounds(sprite);
    if (area.y2 < 0 || area.y1 > 465) return;
    int first = LV_MAX(0, area.y1) / BEAD_DAMAGE_HEIGHT;
    int last = LV_MIN(465, area.y2) / BEAD_DAMAGE_HEIGHT;
    for (int i = first; i <= last; ++i) {
        rows[i].x1 = LV_MIN(rows[i].x1, area.x1);
        rows[i].x2 = LV_MAX(rows[i].x2, area.x2);
        rows[i].y1 = LV_MIN(rows[i].y1, LV_MAX(area.y1, i * BEAD_DAMAGE_HEIGHT));
        rows[i].y2 = LV_MAX(rows[i].y2, LV_MIN(area.y2, i * BEAD_DAMAGE_HEIGHT + BEAD_DAMAGE_HEIGHT - 1));
    }
}

static uint32_t damage_gate_sweep(orb_game_view_t *v, const orb_game_sprite_t *s,
                                  float from, float to)
{
    if (from == to) return 0;
    orb_game_sprite_t sweep = *s;
    float start = fminf(from, to), end = fmaxf(from, to);
    /* HIG §§1/6: the expanded garden keeps its untouched stone retained.
     * Small arc pieces follow the moving cap instead of erasing the circle's
     * bounding square. This is fixed stack storage, never a frame allocation. */
    unsigned pieces = (unsigned)ceilf((end - start) * s->arc_radius / 24);
    if (pieces > 16) pieces = 16;
    uint32_t pixels = 0;
    for (unsigned i = 0; i < pieces; ++i) {
        sweep.arc_start = start + (end - start) * (float)i / pieces;
        sweep.arc_end = start + (end - start) * (float)(i + 1) / pieces;
        pixels += damage_area(v, bounds(&sweep));
    }
    return pixels;
}

static bool gate_on_ring(const orb_game_sprite_t *s, float radius)
{
    return s->shape == ORB_GAME_GATE && s->radius && s->opacity && s->arc_radius == radius;
}

static int gate_degree(float angle)
{
    int degree = (int)(angle * 360 / TAU) % 360;
    return degree < 0 ? degree + 360 : degree;
}

static bool gate_covers(const orb_game_sprite_t *sprites, size_t count,
                        float radius, float degree)
{
    if (degree < 0) degree += 360;
    if (degree >= 360) degree -= 360;
    for (size_t i = 0; i < count; ++i) {
        const orb_game_sprite_t *s = &sprites[i];
        if (!gate_on_ring(s, radius)) continue;
        int start = gate_degree(s->arc_start), end = gate_degree(s->arc_end);
        if (start == end) continue;
        if (start < end ? degree >= start && degree <= end :
                         degree >= start || degree <= end) return true;
    }
    return false;
}

static bool same(const orb_game_sprite_t *a, const orb_game_sprite_t *b)
{
    if ((!a->radius || !a->opacity) && (!b->radius || !b->opacity)) return true;
    return a->radius == b->radius && a->colour == b->colour && a->shape == b->shape &&
           a->opacity == b->opacity && a->light_x == b->light_x && a->light_y == b->light_y &&
           a->arc_radius == b->arc_radius && a->arc_start == b->arc_start && a->arc_end == b->arc_end &&
           (a->shape == ORB_GAME_BUBBLE ? lroundf(a->x * ORB_GAME_NATIVE_PHASES) == lroundf(b->x *
            ORB_GAME_NATIVE_PHASES) && lroundf(a->y * ORB_GAME_NATIVE_PHASES) == lroundf(b->y *
            ORB_GAME_NATIVE_PHASES) : lroundf(a->x) == lroundf(b->x) && lroundf(a->y) == lroundf(b->y));
}

static uint32_t damage_gate_ring(orb_game_view_t *v, size_t n, float radius)
{
    /* Control cut-outs split one physical gate into variable sprite slots.
     * Compare the union of the ring's intervals instead of pairing slots;
     * slot reordering must not turn a moving cap into whole-ring damage. */
    bool unchanged = true;
    const orb_game_sprite_t empty = {0};
    size_t length = n > v->count ? n : v->count;
    for (size_t i = 0; i < length; ++i) {
        const orb_game_sprite_t *a = i < v->count ? &v->sprites[i] : &empty;
        const orb_game_sprite_t *b = i < n ? &v->next[i] : &empty;
        if ((gate_on_ring(a, radius) || gate_on_ring(b, radius)) && !same(a, b)) {
            unchanged = false;
            break;
        }
    }
    if (unchanged) return 0;
    int edges[4 * ORB_MARBLE_GATE_SEGMENTS + 2] = {0, 360};
    unsigned count = 2;
    const orb_game_sprite_t *sample = NULL;
    for (unsigned side = 0; side < 2; ++side) {
        const orb_game_sprite_t *sprites = side ? v->next : v->sprites;
        size_t length = side ? n : v->count;
        for (size_t i = 0; i < length; ++i) {
            const orb_game_sprite_t *s = &sprites[i];
            if (!gate_on_ring(s, radius)) continue;
            if (!sample) sample = s;
            /* Stage/material changes may alter the whole stone's appearance.
             * Such transitions retain the conservative old/new sprite path. */
            if (s->x != sample->x || s->y != sample->y || s->colour != sample->colour ||
                s->radius != sample->radius || s->opacity != sample->opacity) {
                uint32_t pixels = 0;
                for (size_t j = 0; j < v->count; ++j)
                    if (gate_on_ring(&v->sprites[j], radius)) pixels += damage_gate_sweep(v, &v->sprites[j],
                        v->sprites[j].arc_start, v->sprites[j].arc_end);
                for (size_t j = 0; j < n; ++j)
                    if (gate_on_ring(&v->next[j], radius)) pixels += damage_gate_sweep(v, &v->next[j],
                        v->next[j].arc_start, v->next[j].arc_end);
                return pixels;
            }
            int values[] = {gate_degree(s->arc_start), gate_degree(s->arc_end)};
            for (unsigned e = 0; e < 2; ++e) {
                bool found = false;
                for (unsigned j = 0; j < count; ++j) if (edges[j] == values[e]) found = true;
                if (!found && count < sizeof(edges) / sizeof(edges[0])) edges[count++] = values[e];
            }
        }
    }
    for (unsigned i = 1; i < count; ++i) {
        int edge = edges[i];
        unsigned j = i;
        while (j && edges[j - 1] > edge) { edges[j] = edges[j - 1]; --j; }
        edges[j] = edge;
    }
    uint32_t pixels = 0;
    for (unsigned i = 1; i < count; ++i) {
        float middle = (edges[i - 1] + edges[i]) * .5f;
        if (gate_covers(v->sprites, v->count, radius, middle) ==
            gate_covers(v->next, n, radius, middle)) continue;
        pixels += damage_gate_sweep(v, sample, edges[i - 1] * TAU / 360,
                                    edges[i] * TAU / 360);
    }
    /* A quantised endpoint which changes contributes its round cap even when
     * the neighbouring body interval is covered by another clipped segment. */
    {
        for (unsigned side = 0; side < 2; ++side) {
            const orb_game_sprite_t *sprites = side ? v->next : v->sprites;
            size_t length = side ? n : v->count;
            for (size_t i = 0; i < length; ++i) {
                if (!gate_on_ring(&sprites[i], radius)) continue;
                float values[] = {sprites[i].arc_start, sprites[i].arc_end};
                for (unsigned e = 0; e < 2; ++e) {
                    const orb_game_sprite_t *other = side ? v->sprites : v->next;
                    size_t other_count = side ? v->count : n;
                    bool matched = false;
                    for (size_t j = 0; j < other_count; ++j) {
                        if (!gate_on_ring(&other[j], radius)) continue;
                        float starts = remainderf(values[e] - other[j].arc_start, TAU);
                        float ends = remainderf(values[e] - other[j].arc_end, TAU);
                        if (fabsf(starts) < .00001f || fabsf(ends) < .00001f) matched = true;
                    }
                    if (matched) continue;
                    float degree = gate_degree(values[e]);
                    bool old_before = gate_covers(v->sprites, v->count, radius, degree - .1f);
                    bool new_before = gate_covers(v->next, n, radius, degree - .1f);
                    bool old_after = gate_covers(v->sprites, v->count, radius, degree + .1f);
                    bool new_after = gate_covers(v->next, n, radius, degree + .1f);
                    /* Body damage already includes both adjacent round caps.
                     * Float-only cap clipping still changes within a degree. */
                    if (old_before == new_before && old_after == new_after)
                        pixels += damage_gate_sweep(v, sample, values[e] - .001f, values[e] + .001f);
                }
            }
        }
    }
    return pixels;
}

static void odometer(orb_game_view_t *v, uint32_t now)
{
    uint32_t elapsed = now - v->roll_at;
    /* Analytic critically damped spring, response .5 s (HIG §6). Updating a
     * static label offset allocates no game objects or buffers per frame. */
    float t = (float)elapsed / 1000;
    int offset = elapsed >= 550 ? 0 : (int)lroundf(42 * (1 + 18 * t) * expf(-18 * t));
    bool reduced = orb_ui_reduce_motion();
    if (reduced) offset = 0;
    game_centre_text(v->score, 21 + offset);
    game_centre_text(v->old_score, 21 + offset - 42);
    lv_obj_set_hidden(v->old_score, !offset);

}

static void prepare_bead_batch(orb_game_view_t *v, size_t n)
{
    ++v->stats.bead_prepares;
    v->stats.bead_total_sprites = (uint16_t)n;
    v->bead_batch_eligible = true;
    v->bead_batch_count = 0;
    for (size_t i = 0; i < n; ++i) {
        const orb_game_sprite_t *sprite = &v->next[i];
        if (!sprite->radius || sprite->opacity <= LV_OPA_MIN) {
            if (i < ORB_GAME_BEAD_COUNT) v->preview.game.beads.target_opacity[i] = 0;
            continue;
        }
        unsigned rejected = (i >= ORB_GAME_BEAD_COUNT ? 1u : 0u) |
            (sprite->shape != ORB_GAME_BEAD ? 2u : 0u) |
            (sprite->radius != ORB_GAME_BEAD_DRAW_RADIUS ? 4u : 0u);
        if (rejected) {
            if (!v->stats.bead_rejected_prepares++) {
                v->stats.bead_reject_index = (uint16_t)i;
                v->stats.bead_reject_radius = sprite->radius;
                v->stats.bead_reject_shape = sprite->shape;
                v->stats.bead_reject_flags = (uint8_t)rejected;
            }
            v->bead_batch_eligible = false;
            break;
        }
        v->preview.game.beads.target_x[i] = (int16_t)lroundf(sprite->x);
        v->preview.game.beads.target_y[i] = (int16_t)lroundf(sprite->y);
        v->preview.game.beads.target_opacity[i] = sprite->opacity;
        v->bead_batch_count = (uint16_t)(i + 1);
    }
    v->stats.bead_batch_sprites = v->bead_batch_count;
    v->stats.bead_eligible = v->bead_batch_eligible;
}

static void submit_sprites(orb_game_view_t *v, size_t n, uint64_t start)
{
    if (v->kind == ORB_GAME_POP && v->scene == SCENE_PLAY) orb_game_pop_coverage_prepare(v->pop_coverage, v->next, n);
    for (size_t i = 0; i < n; ++i) {
        orb_game_sprite_t *next = &v->next[i];
        if (next->shape != ORB_GAME_BEAD) continue;
        const orb_game_sprite_t *old = i < v->count ? &v->sprites[i] : NULL;
        /* Discs are drawn at integer pixels. A 0.15 px presentation hysteresis
         * prevents a physically still contact on a half-pixel boundary from
         * toggling its ink; force, integration and all collision data remain
         * continuous. Identity lives in this view, never a global/model cache. */
        next->x = old && old->shape == ORB_GAME_BEAD && fabsf(next->x - old->x) <= 0.65f ?
            old->x : roundf(next->x);
        next->y = old && old->shape == ORB_GAME_BEAD && fabsf(next->y - old->y) <= 0.65f ?
            old->y : roundf(next->y);
    }
    uint32_t pixels = 0;
    v->chrome_dirty = false;
    /* Cold entry already invalidates the complete canvas. Only retained
     * poses need old/new damage; repainting every new arena wall again also
     * double-counts cold work as a moving-frame damage peak. */
    size_t max = v->first ? 0 : n > v->count ? n : v->count;
    const orb_game_sprite_t empty = {0};
    lv_area_t bead_rows[BEAD_DAMAGE_ROWS];
    bool strips = v->kind == ORB_GAME_BEADS || v->kind == ORB_GAME_ORBIT;
    if (strips)
        for (unsigned i = 0; i < BEAD_DAMAGE_ROWS; ++i) bead_rows[i] = (lv_area_t){466, 466, -1, -1};
    size_t effect_start = v->kind == ORB_GAME_MARBLE ?
        2u + ORB_MARBLE_GATES_MAX * ORB_MARBLE_GATE_SEGMENTS + 4u + ORB_MARBLE_KEEPSAKES_MAX :
        v->kind == ORB_GAME_ORBIT ? 7u + ORB_GAME_OBSTACLES * 3u + ORB_GAME_ORBIT_STARS :
        /* Games 4 adds eight independent rim-warning gate segments after
         * food. Keep these in the tiled gate-damage path; grouping them with
         * event particles would request their nearly full-field union. */
        v->kind == ORB_GAME_SNAKE ? ORB_GAME_SNAKE_MAX + 9u : ORB_GAME_BUBBLES;
    bool effect_changed = false, effect_dirty = false;
    lv_area_t effect_area = {0};
    for (size_t i = 0; i < max; ++i) {
        const orb_game_sprite_t *a = i < v->count ? &v->sprites[i] : &empty;
        const orb_game_sprite_t *b = i < n ? &v->next[i] : &empty;
        if (!same(a, b)) {
            if (v->kind == ORB_GAME_BEADS || (v->kind == ORB_GAME_ORBIT && i < effect_start)) {
                /* Fifteen 32px unions cover every changed old/new bound, fit
                 * a 42px half-band and halve tiny-band/task setup. Unchanged
                 * poses still invalidate nothing; particle geometry is exact.
                 * Orbit's sprites otherwise spread over ~18 tiny bands; each
                 * strip still repaints any glass control it touches whole. */
                bead_damage_rows(bead_rows, a);
                bead_damage_rows(bead_rows, b);
            } else if (i >= effect_start) effect_changed = true;
            else if (a->shape != ORB_GAME_GATE && b->shape != ORB_GAME_GATE)
                pixels += damage_pair(v, a, b);
        }
        if (v->kind != ORB_GAME_BEADS && i >= effect_start) {
            const orb_game_sprite_t *sides[] = {a, b};
            for (unsigned side = 0; side < 2; ++side) {
                if (!sides[side]->radius || !sides[side]->opacity) continue;
                lv_area_t area = bounds(sides[side]);
                if (effect_dirty) area_join(&effect_area, &effect_area, &area);
                else { effect_area = area; effect_dirty = true; }
            }
        }
    }
    if (strips)
        for (unsigned i = 0; i < BEAD_DAMAGE_ROWS; ++i)
            if (bead_rows[i].x1 <= bead_rows[i].x2) pixels += damage_area(v, bead_rows[i]);
    if (v->kind != ORB_GAME_BEADS && effect_changed && effect_dirty) pixels += damage_area(v, effect_area);
    float rings[2 * ORB_MARBLE_GATES_MAX];
    unsigned rings_count = 0;
    for (unsigned side = 0; side < 2; ++side) {
        const orb_game_sprite_t *sprites = side ? v->next : v->sprites;
        size_t length = v->first ? 0 : side ? n : v->count;
        for (size_t i = 0; i < length; ++i) {
            if (sprites[i].shape != ORB_GAME_GATE || !sprites[i].radius || !sprites[i].opacity) continue;
            bool found = false;
            for (unsigned j = 0; j < rings_count; ++j) if (rings[j] == sprites[i].arc_radius) found = true;
            if (!found && rings_count < sizeof(rings) / sizeof(rings[0])) rings[rings_count++] = sprites[i].arc_radius;
        }
    }
    for (unsigned i = 0; i < rings_count; ++i) pixels += damage_gate_ring(v, n, rings[i]);
    if (v->chrome_dirty) {
        lv_obj_invalidate_area(v->canvas, &v->chrome_damage);
        pixels += (uint32_t)lv_area_get_size(&v->chrome_damage);
    }
    for (size_t i = 2; i < n && i - 2 < ORB_MARBLE_GATES_MAX * ORB_MARBLE_GATE_SEGMENTS; ++i)
        if (v->next[i].shape == ORB_GAME_GATE) v->gate_areas[i - 2] = bounds(&v->next[i]);
    if (v->kind == ORB_GAME_BEADS) prepare_bead_batch(v, n);
    memcpy(v->sprites, v->next, n * sizeof(*v->sprites));
    v->count = n;
    v->first = false;
    ++v->stats.updates;
    if (pixels) ++v->stats.changed_updates;
    v->stats.last_dirty_pixels = pixels;
    if (pixels > v->stats.max_dirty_pixels) v->stats.max_dirty_pixels = pixels;
    v->stats.dirty_pixels += pixels;
    v->stats.update_us += now_us() - start;
}

static orb_game_sprite_t keepsake_sprite(const orb_marble_keepsake_t *k, unsigned i,
                                          float x, float y, unsigned radius)
{
    /* Deterministic planting variation uses record identity, never gameplay
     * RNG. These quiet content colours reserve green for goal/success (§5). */
    unsigned variation = i * 37u + k->stage * 11u + k->kind * 17u;
    static const uint32_t stones[] = {0x89847c, 0x9c968b, 0x77756f};
    orb_game_sprite_t s = {.x = x, .y = y, .radius = radius,
        .colour = k->kind ? (k->kind == 1 ? 0xff8ac4 : 0xebebf5) : stones[variation % 3],
        .shape = k->kind ? ORB_GAME_FLOWER : ORB_GAME_STONE, .opacity = 255,
        .light_x = (int8_t)((int)(variation % 43) - 21)};
    return s;
}

/* Start-screen thumbnail: the whole field scaled into the art disc. Eye and
 * reflection offsets scale with their sprite; nothing shrinks below 1 px. */
static size_t preview_sprites(orb_game_view_t *v)
{
    size_t n = orb_game_sprites(&v->preview, v->next, ORB_GAME_MAX_SPRITES);
    for (size_t i = 0; i < n; ++i) {
        orb_game_sprite_t *s = &v->next[i];
        s->x = 233 + (s->x - ORB_GAME_CX) * PREVIEW_SCALE;
        s->y = ART_Y + (s->y - ORB_GAME_CY) * PREVIEW_SCALE;
        if (s->radius) s->radius = (uint16_t)LV_MAX(1, lroundf(s->radius * PREVIEW_SCALE));
        s->arc_radius *= PREVIEW_SCALE;
        if (s->shape == ORB_GAME_HEAD || s->shape == ORB_GAME_MARBLE_SPRITE) {
            s->light_x = (int8_t)lroundf(s->light_x * PREVIEW_SCALE);
            s->light_y = (int8_t)lroundf(s->light_y * PREVIEW_SCALE);
        }
    }
    return n;
}

static void marble_update(orb_game_view_t *v, const orb_game_state_t *state,
                          bool paused, bool save_error, uint64_t start)
{
    uint32_t now = lv_tick_get();
    orb_content_size_t size = orb_fonts_content_size();
    bool bold = orb_fonts_bold_text(), reduced = orb_ui_reduce_motion();
    bool opaque = orb_ui_reduce_transparency();
    bool prefs = v->first || v->content_size != size || v->bold != bold ||
        v->reduced != reduced || v->opaque != opaque;
    game_scene_t scene = state->game.marble.garden_open ? SCENE_GARDEN :
        state->game_over ? SCENE_END : !state->started ? SCENE_START :
        paused || state->user_paused ? SCENE_PAUSE :
        state->game.marble.stage_cleared ? SCENE_CLEAR : SCENE_PLAY;
    bool scene_changed = v->first || scene != v->scene;
    if (scene_changed) {
        lv_obj_invalidate(v->canvas);
        v->scene = scene; v->scene_at = now;
        if (scene == SCENE_PLAY) enter_play(v);
        v->repaint_chrome = !v->first;
        if (scene != SCENE_PLAY) v->field_held = false;
    }
    uint32_t elapsed = now - v->scene_at;
    /* Review #39: Reduce Motion has no flight to wait for. Its reading chrome
     * cross-fades in with the token, so no frame holds a part-faded action. */
    bool settled = scene != SCENE_CLEAR || reduced || elapsed >= MARBLE_PLANT_MS;
    bool changed = prefs || scene_changed || v->controls != state->controls ||
        v->stage != state->game.marble.stage || v->keepsakes != state->game.marble.keepsake_count ||
        v->save_error != save_error || v->rewinding != state->game.marble.rewind_active ||
        v->reward_settled != settled ||
        v->tilt_calibrating != state->tilt_calibrating ||
        v->motion_unavailable != state->motion_unavailable;
    if (prefs) lv_obj_invalidate(v->root);
    if (changed) {
        v->controls = state->controls; v->stage = state->game.marble.stage;
        v->keepsakes = state->game.marble.keepsake_count; v->save_error = save_error;
        v->rewinding = state->game.marble.rewind_active;
        v->reward_settled = settled;
        v->tilt_calibrating = state->tilt_calibrating;
        v->motion_unavailable = state->motion_unavailable;
        v->content_size = size; v->bold = bold; v->reduced = reduced; v->opaque = opaque;
    }
    if (changed) game_chrome(v, state, save_error);
    game_play_overlay(v, state, now, scene_changed);
    size_t n = scene == SCENE_PLAY ? orb_game_sprites(state, v->next, ORB_GAME_MAX_SPRITES) : 0;
    if (scene == SCENE_START) {
        n = preview_sprites(v);
    } else if (scene == SCENE_CLEAR) {
        uint8_t at = state->game.marble.keepsake_count;
        if (at) {
            const orb_marble_keepsake_t *k = &state->game.marble.keepsakes[at - 1u];
            /* Caused, brief, then still (delight.md): an analytic 350 ms spring
             * opens the goal, then the earned token lifts and lands by Garden.
             * Reduce Motion cross-fades stationary endpoints, never travels. */
            float bloom_t = fminf(1, (float)elapsed / MARBLE_BLOOM_MS);
            float spring = 1 - expf(-6 * bloom_t) * cosf(8 * bloom_t);
            float scale = .6f + .4f * spring;
            float flight = fmaxf(0, fminf(1, (float)((int)elapsed - MARBLE_BLOOM_MS) /
                (MARBLE_PLANT_MS - MARBLE_BLOOM_MS)));
            float ease = flight * flight * (3 - 2 * flight);
            if (reduced) {
                if (elapsed < 150) {
                    v->next[n++] = (orb_game_sprite_t){.x = 233, .y = 233, .radius = 24,
                        .colour = ORB_RGB_GREEN, .shape = ORB_GAME_FLOWER,
                        .opacity = (uint8_t)(255 * (1 - fminf(1, (float)elapsed / 150)))};
                }
                orb_game_sprite_t token = keepsake_sprite(k, at - 1, 233, 318, 22);
                token.opacity = (uint8_t)(255 * fminf(1, (float)elapsed / 150));
                v->next[n++] = token;
            } else if (elapsed < MARBLE_BLOOM_MS) {
                v->next[n++] = (orb_game_sprite_t){.x = 233, .y = 233,
                    .radius = (uint16_t)lroundf(26 * scale), .colour = ORB_RGB_GREEN,
                    .shape = ORB_GAME_FLOWER, .opacity = 255};
            } else {
                /* Review #39: a straight lift-and-land on the centre line; the
                 * old sideways arc left a mid-flight frame 38 px off-centre. */
                orb_game_sprite_t token = keepsake_sprite(k, at - 1, 233,
                    233 + 85 * ease - sinf(flight * TAU / 2) * 32,
                    (unsigned)lroundf(26 - 4 * ease));
                float handoff = fminf(1, flight * 4);
                token.opacity = (uint8_t)(255 * handoff);
                if (handoff < 1)
                    v->next[n++] = (orb_game_sprite_t){.x = token.x, .y = token.y,
                        .radius = token.radius, .colour = ORB_RGB_GREEN,
                        .shape = ORB_GAME_FLOWER, .opacity = (uint8_t)(255 * (1 - handoff))};
                v->next[n++] = token;
            }
        }
    } else if (scene == SCENE_GARDEN) {
        /* HIG §§1/3: one still spiral grows to 145 px, so the outer petals
         * reach ~70% of the screen radius. Its phase leaves the title/count
         * chord and the lower glass action clear, including AX3 text. */
        for (unsigned i = 0; i < state->game.marble.keepsake_count; ++i) {
            const orb_marble_keepsake_t *k = &state->game.marble.keepsakes[i];
            float a = 217 * TAU / 360 + i * .55f;
            float r = i == 0 ? 0 : 45 + (i - 1) * 10;
            unsigned radius = 13 + (i * 7 + k->kind * 3 + k->stage) % 4;
            v->next[n++] = keepsake_sprite(k, i, 233 + cosf(a) * r * .85f,
                                           223 + sinf(a) * r * .85f, radius);
        }
    }
    submit_sprites(v, n, start);
}

/* Full-field Beads: a tap pauses, and PWR remains the OS escape. */
static void beads_update(orb_game_view_t *v, const orb_game_state_t *state,
                         bool paused, uint64_t start)
{
    uint32_t now = lv_tick_get();
    orb_content_size_t size = orb_fonts_content_size();
    bool bold = orb_fonts_bold_text(), reduced = orb_ui_reduce_motion();
    bool opaque = orb_ui_reduce_transparency();
    bool prefs = v->first || v->content_size != size || v->bold != bold ||
        v->reduced != reduced || v->opaque != opaque;
    if (prefs) lv_obj_invalidate(v->root);
    game_scene_t scene = !state->started ? SCENE_START :
        (paused || state->user_paused) ? SCENE_PAUSE : SCENE_PLAY;
    bool scene_changed = v->first || scene != v->scene;
    bool controls_changed = v->controls != state->controls;
    v->controls = state->controls;
    if (scene_changed || prefs || controls_changed) {
        lv_obj_invalidate(v->canvas);
        v->scene = scene; v->scene_at = now;
        if (scene == SCENE_PLAY) enter_play(v);
        v->repaint_chrome = !v->first;
        game_chrome(v, state, false);
        v->content_size = size; v->bold = bold; v->reduced = reduced; v->opaque = opaque;
    }
    if (v->tilt_calibrating != state->tilt_calibrating || v->motion_unavailable != state->motion_unavailable ||
        v->time_valid != state->game.beads.time_valid) {
        v->tilt_calibrating = state->tilt_calibrating;
        v->motion_unavailable = state->motion_unavailable;
        v->time_valid = state->game.beads.time_valid;
        game_chrome(v, state, false);
    }
    game_play_overlay(v, state, now, scene_changed);
    size_t n = scene == SCENE_PLAY ? orb_game_sprites(state, v->next, ORB_GAME_MAX_SPRITES) : 0;
    if (scene == SCENE_START) n = preview_sprites(v);
    submit_sprites(v, n, start);
}

static void pop_reward_update(orb_game_view_t *v, const orb_game_state_t *state, game_scene_t scene)
{
    bool reward = scene == SCENE_PLAY && state->effect.duration &&
        state->effect.age < state->effect.duration;
    game_visible(v->bonus, reward);
    if (reward) {
        if (v->bonus_value != state->game.pop.reward) {
            lv_obj_invalidate(v->bonus);
            v->bonus_value = state->game.pop.reward;
            snprintf(v->bonus_text, sizeof(v->bonus_text), "+%u", v->bonus_value);
            lv_label_set_text_static(v->bonus, v->bonus_text);
        }
        int y = LV_CLAMP(112, (int)lroundf(state->effect.y) + 257, 390);
        game_centre_text(v->bonus, y);
        int half_height = lv_obj_get_height(v->bonus) / 2;
        int chord = orb_chord_width(y - half_height, y + half_height, 16);
        int left = (466 - chord) / 2;
        lv_obj_set_x(v->bonus, LV_CLAMP(left,
            (int)lroundf(state->effect.x) + 233 - 60, 466 - left - 120));
        lv_obj_set_style_text_opa(v->bonus,
            (lv_opa_t)(255 * (state->effect.duration - state->effect.age) / state->effect.duration), 0);
    }
}

void orb_game_view_update(orb_game_view_t *v, const orb_game_state_t *state,
                          uint32_t high_score, bool paused, bool save_error)
{
    if (!v || !state || state->kind != v->kind) return;
    uint64_t start = now_us();
    if (v->kind == ORB_GAME_MARBLE) {
        marble_update(v, state, paused, save_error, start);
        return;
    }
    if (v->kind == ORB_GAME_BEADS) {
        beads_update(v, state, paused, start);
        return;
    }
    uint32_t now = lv_tick_get();
    orb_content_size_t size = orb_fonts_content_size();
    bool bold = orb_fonts_bold_text(), reduced = orb_ui_reduce_motion();
    bool opaque = orb_ui_reduce_transparency();
    bool preferences_changed = v->first || v->content_size != size || v->bold != bold ||
        v->reduced != reduced || v->opaque != opaque;
    if (preferences_changed) {
        /* HIG §8: a preference change replaces all text/material pixels,
         * including an outgoing control's sampled backdrop. Do this once,
         * rather than leaving a partial old material at a new font boundary. */
        lv_obj_invalidate(v->root);
        v->content_size = size; v->bold = bold;
        v->reduced = reduced; v->opaque = opaque;
    }
    game_scene_t scene = state->game_over ? SCENE_END : !state->started ? SCENE_START :
        paused || state->user_paused ? SCENE_PAUSE : SCENE_PLAY;
    bool scene_changed = v->first || scene != v->scene;
    bool controls_changed = v->controls != state->controls;
    if (scene_changed) {
        /* A scene changes the complete static content (lane paths versus a
         * reading surface). Only this boundary needs a full content repaint;
         * fixed-step play continues to invalidate old/new sprite bounds. */
        lv_obj_invalidate(v->canvas);
        v->repaint_chrome = !v->first;
        v->scene = scene; v->scene_at = now;
        if (scene == SCENE_PLAY) enter_play(v);
    }
    v->controls = state->controls;
    if (v->kind == ORB_GAME_POP) pop_reward_update(v, state, scene);
    if (v->first || v->score_value != state->score || scene_changed || preferences_changed) {
        snprintf(v->old_score_text, sizeof(v->old_score_text), "%s", v->score_text);
        char score[12]; game_compact(state->score, score, sizeof(score));
        snprintf(v->score_text, sizeof(v->score_text), "%s", score);
        lv_label_set_text_static(v->score, v->score_text);
        lv_label_set_text_static(v->old_score, v->old_score_text);
        /* Cold entry has no previous value to roll away. Show its score on
         * the first frame; only an actual score change gets motion (HIG §6). */
        v->roll_at = v->first ? now - 550u : now;
        v->score_value = state->score;
    }
    if (scene_changed || preferences_changed || controls_changed || v->best_value != high_score ||
        v->save_error != save_error || v->lives != state->lives ||
        v->tilt_calibrating != state->tilt_calibrating || v->motion_unavailable != state->motion_unavailable) {
        v->best_value = high_score;
        v->save_error = save_error;
        v->lives = state->lives;
        v->tilt_calibrating = state->tilt_calibrating;
        v->motion_unavailable = state->motion_unavailable;
        game_chrome(v, state, save_error);
    }
    game_play_overlay(v, state, now, scene_changed);
    if (v->kind == ORB_GAME_POP && scene == SCENE_PLAY) {
        /* Whole seconds left on the rim countdown: one small label change a
         * second, inside the disc that already owns its glass backdrop. */
        int32_t seconds = (int32_t)((state->game.pop.time_left + 59u) / 60u);
        if (seconds != v->pop_seconds || scene_changed || preferences_changed) {
            v->pop_seconds = seconds;
            snprintf(v->time_text, sizeof(v->time_text), "%ld s", (long)seconds);
            lv_label_set_text_static(v->hud_time, v->time_text);
            game_centre_text(v->hud_time, 58);
            /* Ink on glass: repaint the whole disc and its blur halo in full
             * rows, as sprite damage does, so the sample origin matches a
             * complete render (a label-sized patch re-blurs differently). */
            lv_area_t disc;
            lv_obj_get_coords(v->score_hit, &disc);
            lv_area_increase(&disc, 26, 26);
            disc.x1 = 0; disc.x2 = 465;
            lv_obj_invalidate_area(v->canvas, &disc);
        }
    }
    odometer(v, now);
    if (v->repaint_chrome && now - v->scene_at < 650) {
        /* A Reduce Motion change can finish an outgoing material between
         * animation callbacks. Repaint its small old footprint until settled;
         * no full-field invalidation or ongoing moving-frame cost is added. */
        lv_obj_t *chrome[] = {v->close, v->primary, v->replay, v->done,
            v->finish, v->garden};
        for (unsigned i = 0; i < sizeof(chrome) / sizeof(chrome[0]); ++i) {
            lv_area_t area;
            if (!chrome[i]) continue;
            lv_obj_get_coords(chrome[i], &area);
            /* Include the blur's backdrop sample halo, not just the AA rim. */
            lv_area_increase(&area, 26, 26);
            area.x1 = 0; area.x2 = 465;
            lv_obj_invalidate_area(v->canvas, &area);
        }
    }

    size_t n;
    if (scene == SCENE_START) {
        v->preview.reduce_motion = orb_ui_reduce_motion();
        v->preview.ticks = v->preview.reduce_motion ? 0 : (now - v->scene_at < INTRO_MS ? (now - v->scene_at) / 17 : INTRO_MS / 17);
        n = preview_sprites(v);
    } else n = orb_game_sprites(state, v->next, ORB_GAME_MAX_SPRITES);
    /* HIG §1: pause and result are composed reading surfaces. The previous
     * game is retained in its model, rather than pasted behind the final score. */
    if (scene == SCENE_PAUSE || scene == SCENE_END) n = 0;
    if (v->kind == ORB_GAME_POP && scene == SCENE_PLAY) {
        int32_t percent = (int32_t)((int64_t)state->game.pop.time_left * 100 / ORB_GAME_POP_ROUND_STEPS);
        if (percent != v->pop_arc) {
            orb_kit_ring_set_value(v->timer_ring, percent, false);
            v->pop_arc = percent;
        }
    }
    submit_sprites(v, n, start);
}

void orb_game_view_stats(const orb_game_view_t *v, orb_game_view_stats_t *out)
{
    if (!out) return;
    *out = v ? v->stats : (orb_game_view_stats_t){0};
    if (v && v->beads_draw) {
        orb_game_beads_draw_stats_t retired;
        orb_game_beads_draw_stats(v->beads_draw, &retired);
        out->bead_custom_slices = retired.custom_calls;
        out->bead_custom_us = retired.custom_us;
        out->bead_custom_max_us = retired.custom_max_us;
        out->bead_sync_slices = retired.sync_calls;
        out->bead_sync_us = retired.sync_us;
    }
}

#ifndef ESP_PLATFORM
void orb_game_view_test_native_rings(orb_game_view_t *v)
{
    if (v->orbit) {
        orb_game_orbit_release(v->orbit);
        v->orbit = NULL;
        for (unsigned i = 0; i < 3; ++i) v->rings[i] = NULL;
    } else {
        for (unsigned i = 0; i < 3; ++i) { free(v->rings[i]); v->rings[i] = NULL; }
    }
    v->stats.ring_cache_bytes = 0;
    lv_obj_invalidate(v->root);
}

void orb_game_view_test_serial_orbit(orb_game_view_t *v, bool serial)
{
    v->serial_orbit = serial;
    lv_obj_invalidate(v->root);
}
#endif

void orb_game_view_destroy(orb_game_view_t *v)
{
    if (!v) return;
    orb_game_pop_coverage_release(v->pop_coverage);
    orb_game_beads_draw_release(v->beads_draw);
    if (v->orbit) orb_game_orbit_release(v->orbit);
    else for (unsigned i = 0; i < 3; ++i) {
#ifdef ESP_PLATFORM
        orb_memory_free(v->rings[i]);
#else
        free(v->rings[i]);
#endif
    }
    lv_obj_delete(v->root);
#ifdef ESP_PLATFORM
    orb_memory_free(v);
#else
    free(v);
#endif
}
