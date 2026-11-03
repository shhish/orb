/* Home's bezel is a Crown: retained flat content, a touch-only glass knob,
 * and bounded damage along the rim (hig.md §§2/3/6/7, home-app.md §4).
 * This half owns input, writes and lifetime; orb_home_detail_view.c owns
 * the one accessory template every kind shares. */
#include "orb_home_detail_private.h"
#include "orb_model.h"
#include "orb_home_payload.h"
#include "orb_home_view.h"
#include "orb_sound.h"
#include "orb_kit.h"
#include "orb_input.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wake(orb_home_detail_t *v);

static float clamp(float value, float low, float high)
{
    return fminf(high, fmaxf(low, value));
}

bool orb_home_detail_ready(const orb_home_snapshot_t *model)
{
    return model->online && model->synced && model->available;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    lv_obj_set_event_bubble(o, true);
    return o;
}

float orb_home_detail_model_value(const orb_home_detail_t *v)
{
    const orb_home_state_t *s = &v->accessory.state;
    if (v->accessory.kind == ORB_HOME_THERMOSTAT) return clamp(orb_temperature_position(s->target, v->temperature), 0, 1);
    if (v->accessory.kind == ORB_HOME_FAN) return clamp(s->speed / 100, 0, 1);
    if (v->accessory.kind == ORB_HOME_COVER) return clamp(s->position / 100, 0, 1);
    if (v->colour_page) {
        if (v->accessory.writable & ORB_HOME_HUE) return clamp(s->hue / 360, 0, 1);
        return clamp((s->ct - 140) / 360, 0, 1);
    }
    return clamp(s->brightness / 100, 0, 1);
}

bool orb_home_detail_has_dial(const orb_home_detail_t *v)
{
    if (v->page > 1) return false;
    return (v->accessory.kind == ORB_HOME_LIGHT && v->page < 2) ||
        (v->accessory.kind == ORB_HOME_THERMOSTAT && !v->page) || v->accessory.kind == ORB_HOME_FAN ||
        (v->accessory.kind == ORB_HOME_COVER && !v->page);
}

uint32_t orb_home_detail_field(const orb_home_detail_t *v)
{
    if (v->accessory.kind == ORB_HOME_THERMOSTAT) return ORB_HOME_TARGET;
    if (v->accessory.kind == ORB_HOME_FAN) return ORB_HOME_SPEED;
    if (v->accessory.kind == ORB_HOME_COVER) return ORB_HOME_POSITION;
    return v->colour_page ? ((v->accessory.writable & ORB_HOME_HUE) ? ORB_HOME_HUE : ORB_HOME_CT) : ORB_HOME_BRIGHTNESS;
}

/* A level or target field is known while reported by authority or live
 * under a physical drag (mirroring orb_home_detail_view.c). */
static bool field_known(const orb_home_detail_t *v, unsigned field)
{
    if (!v) return false;
    return v->dragging || (v->accessory.state.present & field);
}

bool orb_home_detail_writable(const orb_home_detail_t *v)
{
    return v->available && orb_home_detail_ready(&v->snapshot) && v->accessory.reachable &&
        !v->accessory.pending && (v->accessory.kind != ORB_HOME_LOCK ||
        (v->accessory.writable & ORB_HOME_LOCKED));
}

orb_home_status_t orb_home_detail_status(const orb_home_detail_t *v)
{
    return orb_home_status_accessory(&v->snapshot, &v->accessory, v->failure_visible);
}

static const orb_rim_config_t *rim_config(const orb_home_detail_t *v)
{
    /* BOOT supplies Back, so there are no toolbar gaps (HIG §7). */
    (void)v;
    static const orb_rim_config_t s_continuous = {.gaps = 0, .start_angle = 35, .sweep_angle = 110};
    return &s_continuous;
}

static bool commit_value(orb_home_detail_t *v, float value)
{
    if (!orb_input_require_physical(v->root)) return false;
    uint32_t field = orb_home_detail_field(v);
    if (!orb_home_detail_writable(v) || !(v->accessory.writable & field) ||
        !field_known(v, field)) return false;
    value = clamp(value, 0, 1);
    orb_home_state_t s = {.present = field};
    if (s.present == ORB_HOME_TARGET) s.target = orb_temperature_celsius(orb_temperature_target(value, v->temperature), v->temperature);
    else if (s.present == ORB_HOME_SPEED) s.speed = roundf(value * 100);
    else if (s.present == ORB_HOME_POSITION) s.position = roundf(value * 100);
    else if (s.present == ORB_HOME_HUE) s.hue = roundf(value * 360);
    else if (s.present == ORB_HOME_CT) s.ct = roundf(140 + value * 360);
    else s.brightness = roundf(value * 100);
    return orb_home_command(v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_LEVEL, .id = v->id, .values = s});
}

static void hold_cancel(orb_home_detail_t *v)
{
    if (!v->holding) return;
    v->holding = false;
    if (v->canvas) lv_obj_set_hidden(v->canvas, true);
    orb_home_detail_refresh(v, false);
}

static void pointer(lv_event_t *event)
{
    orb_home_detail_t *v = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    lv_indev_t *indev = lv_event_get_indev(event);
    if (code == LV_EVENT_PRESSED) orb_kit_motion_finish(v->root);
    if (!indev) return;
    /* Another pointer cannot complete or replace a hold/drag it did not
     * begin. In particular, a synthetic release cannot finish a real drag. */
    if ((v->holding || v->dragging) && indev != v->gesture_input) {
        orb_input_require_physical(v->root);
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t bounds;
    lv_obj_get_coords(v->root, &bounds);
    float x = p.x - bounds.x1 - 233, y = p.y - bounds.y1 - 233;
    float radius = sqrtf(x*x + y*y);
    /* The lock's hold target is the template's identity disc. */
    float disc_y = y + 233 - DETAIL_DISC_CY;
    bool on_disc = sqrtf(x*x + disc_y*disc_y) <= 40;
    bool lock = v->accessory.kind == ORB_HOME_LOCK;
    if (code == LV_EVENT_PRESSED) {
        v->press = p; v->press_at = lv_tick_get(); v->contact_moved = false;
        v->system_touch = v->system_navigation && radius >= 193;
        if (v->system_touch) v->suppress = false;
    } else {
        int dx = p.x - v->press.x, dy = p.y - v->press.y;
        if (dx*dx + dy*dy >= 10*10) v->contact_moved = true;
    }
    if (v->system_touch) {
        /* The shell arbitrates the outer 40px before any dial can capture it.
         * Keeping this contact suppressed until lift prevents a Back/crown
         * drag from becoming a Home write when it crosses into the dial. */
        int dx = p.x - v->press.x, dy = p.y - v->press.y;
        if (dx*dx + dy*dy >= 12*12) v->suppress = true;
        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) v->system_touch = false;
        return;
    }
    /* A visible button retains its own action inside the rim. Bubbling gives
     * the shell system ownership without turning the page/power button into
     * this detail's legacy dial gesture. */
    if (lv_event_get_target_obj(event) != v->root &&
        lv_event_get_target_obj(event) != v->lock_disc) {
        if (code == LV_EVENT_PRESSED) { v->press = p; v->suppress = false; }
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        v->press = p; v->suppress = false; v->hold_fired = false;
        v->physical_gesture = false; v->gesture_input = NULL;
        /* Keep the shared rim admission math, but do not adopt a developer
         * contact's drag state before its physical-input guard has passed. */
        orb_rim_drag_t drag = {0};
        uint32_t field = orb_home_detail_field(v);
        if (orb_home_detail_has_dial(v) && orb_home_detail_writable(v) &&
            (v->accessory.writable & field) && field_known(v, field) &&
            orb_rim_drag_begin_arc(rim_config(v), &drag, x + 233, y + 233, 213, 46)) {
            if (!orb_input_require_physical(v->root)) return;
            v->rim_drag = drag;
            v->gesture_input = indev; v->physical_gesture = true;
            v->dragging = true;
            if (v->canvas) lv_obj_send_event(v->canvas, LV_EVENT_PRESSED, NULL);
            orb_home_detail_present(v, 1 - clamp(orb_rim_value(rim_config(v), v->rim_drag.angle), 0, 1));
            orb_home_detail_refresh(v, false);
        } else if (lock && on_disc && orb_home_detail_writable(v) &&
                   (v->accessory.state.present & ORB_HOME_LOCKED)) {
            if (!orb_input_require_physical(v->root)) return;
            v->gesture_input = indev; v->physical_gesture = true;
            if (v->accessory.state.locked) {
                v->holding = true; v->hold_at = lv_tick_get();
                if (v->canvas) { orb_kit_ring_set_value(v->canvas, 0, false); lv_obj_set_hidden(v->canvas, false); }
                orb_home_detail_refresh(v, false);
                wake(v);
            }
        }
    } else if (code == LV_EVENT_PRESSING) {
        if (v->holding && (!on_disc || v->contact_moved)) hold_cancel(v);
        if (v->dragging) {
            orb_home_detail_present(v, 1 - orb_rim_drag_update(rim_config(v), &v->rim_drag,
                x + 233, y + 233, orb_ui_reduce_motion()));
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (v->dragging) {
            v->dragging = false; v->rim_drag.active = false; v->suppress = true;
            if (v->canvas && orb_home_detail_has_dial(v)) lv_obj_send_event(v->canvas, LV_EVENT_RELEASED, NULL);
            float value = clamp(v->presented, 0, 1);
            if (code != LV_EVENT_RELEASED || !commit_value(v, value)) value = orb_home_detail_model_value(v);
            orb_home_detail_start_settle(v, value);
            orb_home_detail_poll(v);
            orb_home_detail_refresh(v, false);
        }
        if (v->holding) hold_cancel(v);
        if (code == LV_EVENT_RELEASED && lock && on_disc &&
            !v->contact_moved && lv_tick_elaps(v->press_at) < 500 &&
            !v->accessory.state.locked && !v->hold_fired && orb_home_detail_writable(v) &&
            (v->accessory.state.present & ORB_HOME_LOCKED)) {
            if (!v->physical_gesture || !orb_input_require_physical(v->root)) return;
            if (orb_home_command(v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_SET, .id = v->id,
                .values = {.present = ORB_HOME_LOCKED, .locked = true}})) { orb_sound_play(ORB_SOUND_HOME_OFF); orb_home_detail_poll(v); }
        }
        if (code == LV_EVENT_RELEASED && !v->suppress && !v->hold_fired &&
            abs(p.y - v->press.y) > 70 && abs(p.y - v->press.y) > abs(p.x - v->press.x) &&
            (v->accessory.kind == ORB_HOME_LIGHT || v->accessory.kind == ORB_HOME_THERMOSTAT)) {
            int next = (int)v->page + (p.y < v->press.y ? 1 : -1);
            int pages = v->accessory.kind == ORB_HOME_LIGHT ? 3 : 2;
            if (next >= 0 && next < pages) orb_home_detail_page(v, (unsigned)next);
        }
    }
}

void orb_home_detail_clicked(lv_event_t *event)
{
    orb_home_detail_t *v = lv_event_get_user_data(event);
    lv_obj_t *o = lv_event_get_current_target_obj(event);
    int action = (int)(intptr_t)lv_obj_get_user_data(o);
    if (lv_event_get_indev(event) &&
        (v->contact_moved || lv_tick_elaps(v->press_at) >= 500)) return;
    if (v->suppress) { v->suppress = false; return; }
    /* The thermostat's identity disc opens its mode list; Off is a mode. */
    if (action == ACTION_MODE) { orb_home_detail_page(v, v->page ? 0 : 1); return; }
    if (!orb_home_detail_writable(v)) return;
    if (!orb_input_require_physical(v->root)) return;
    if (action == ACTION_POWER) {
        if (!(v->accessory.state.present & ORB_HOME_ON)) return;
        bool was_on = v->accessory.state.on;
        if (orb_home_command(v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_TOGGLE, .id = v->id})) {
            orb_sound_play(was_on ? ORB_SOUND_HOME_OFF : ORB_SOUND_HOME_ON);
            orb_home_detail_poll(v);
        }
    } else if (action >= ACTION_SCENE && action < ACTION_SCENE + ORB_HOME_MAX_SCENES) {
        const orb_home_scene_t *scene = &v->snapshot.scenes[action - ACTION_SCENE];
        if ((size_t)(action - ACTION_SCENE) >= v->snapshot.scene_count || scene->contains_unlock) return;
        snprintf(v->requested_scene, sizeof(v->requested_scene), "%s", scene->id);
        memcpy(v->scene_focus_id, v->requested_scene, sizeof(v->scene_focus_id));
        v->scene_acknowledged = false;
        (void)orb_home_command(v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_SCENE, .id = scene->id});
        orb_home_detail_poll(v);
        orb_home_detail_refresh(v, false);
    } else if (action >= ACTION_MODE_SET && action < ACTION_MODE_SET + 4) {
        orb_home_state_t s = {.present = ORB_HOME_MODE, .mode = (orb_home_mode_t)(action - ACTION_MODE_SET)};
        if (orb_home_command(v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_SET, .id = v->id, .values = s})) {
            orb_sound_play(ORB_SOUND_TAP);
            orb_home_detail_page(v, 0);
        }
    }
}

static void timer_tick(lv_timer_t *timer)
{
    orb_home_detail_t *v = lv_timer_get_user_data(timer);
    bool dial = orb_home_detail_has_dial(v);
    if (!lv_obj_is_visible(v->root)) {
        v->dragging = false; hold_cancel(v);
        orb_kit_motion_finish(v->root);
        if (v->canvas && dial) lv_obj_send_event(v->canvas, LV_EVENT_RELEASED, NULL);
        if (dial) orb_home_detail_present(v, orb_home_detail_model_value(v));
        lv_timer_pause(timer); return;
    }
    if (v->holding) {
        if (v->canvas) orb_kit_ring_set_value(v->canvas,
            (int32_t)lroundf(clamp((float)lv_tick_elaps(v->hold_at) / 1000, 0, 1) * 100), false);
        /* The timer has no active LVGL input; retain the initiating pointer's
         * provenance instead of treating an absent indev as a real hold. */
        if (lv_tick_elaps(v->hold_at) >= 1000 && orb_home_detail_writable(v) && v->physical_gesture) {
            v->holding = false; v->hold_fired = true;
            orb_home_state_t state = {.present = ORB_HOME_LOCKED, .locked = false};
            if (orb_home_command(v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_SET, .id = v->id, .values = state, .deliberate_unlock = true})) {
                orb_sound_play(ORB_SOUND_HOME_ON);
                orb_home_detail_poll(v);
            }
            if (v->canvas) lv_obj_set_hidden(v->canvas, true);
        }
    }
    if (v->failure_visible && lv_tick_elaps(v->failure_at) >= 3500) {
        v->failure_visible = false;
        orb_home_detail_refresh(v,false);
    }
    if (!v->holding) {
        if (v->failure_visible) lv_timer_set_period(timer,3500-lv_tick_elaps(v->failure_at));
        else lv_timer_pause(timer);
    }
}

static void wake(orb_home_detail_t *v)
{
    lv_timer_set_period(v->timer,16);
    lv_timer_resume(v->timer); lv_timer_reset(v->timer);
}

static void deleted(lv_event_t *event)
{
    orb_home_detail_t *v = lv_event_get_user_data(event);
    lv_timer_delete(v->timer);
    orb_home_payload_destroy(v,sizeof(*v)-sizeof(v->snapshot),sizeof(v->snapshot));
}

orb_home_detail_t *orb_home_detail_create(lv_obj_t *parent, const char *id, const orb_home_commands_t *commands)
{
    if (!parent || !id || strlen(id)>ORB_HOME_ID_MAX) return NULL;
    orb_home_detail_t *v = orb_home_payload_create(sizeof(*v)-sizeof(v->snapshot),sizeof(v->snapshot));
    if (!v) return NULL;
    snprintf(v->id,sizeof(v->id),"%s",id); v->commands = commands;
    v->snapshot.revision = UINT64_MAX;
    v->temperature = orb_model_temperature();
    if (v->temperature == ORB_TEMPERATURE_C && v->snapshot.demo) v->temperature = ORB_TEMPERATURE_F;
    v->root = plain(parent);
    lv_obj_set_size(v->root,466,466); lv_obj_set_clickable(v->root,true);
    lv_obj_set_style_bg_color(v->root,lv_color_black(),0); lv_obj_set_style_bg_opa(v->root,LV_OPA_COVER,0);
    lv_obj_add_event_cb(v->root,pointer,LV_EVENT_PRESSED,v);
    lv_obj_add_event_cb(v->root,pointer,LV_EVENT_PRESSING,v);
    lv_obj_add_event_cb(v->root,pointer,LV_EVENT_RELEASED,v);
    lv_obj_add_event_cb(v->root,pointer,LV_EVENT_PRESS_LOST,v);
    v->timer = lv_timer_create(timer_tick,16,v);
    if (!v->timer) { lv_obj_delete(v->root); orb_home_payload_destroy(v,sizeof(*v)-sizeof(v->snapshot),sizeof(v->snapshot)); return NULL; }
    lv_timer_pause(v->timer);
    lv_obj_add_event_cb(v->root,deleted,LV_EVENT_DELETE,v);
    orb_home_detail_poll(v);
    if (!v->content) orb_home_detail_build(v);
    return v;
}

static uint32_t scene_signature(const orb_home_snapshot_t *snapshot)
{
    uint32_t value = 2166136261u;
    const unsigned char *bytes = (const unsigned char *)snapshot->scenes;
    size_t length = snapshot->scene_count * sizeof(snapshot->scenes[0]);
    for (size_t i=0;i<length;i++) value = (value ^ bytes[i])*16777619u;
    return value;
}

static void contact_cancel(orb_home_detail_t *v)
{
    hold_cancel(v);
    v->dragging = v->rotating = v->physical_gesture = v->rim_drag.active = false;
    v->gesture_input = NULL; v->suppress = v->contact_moved = true;
    if (v->canvas && orb_home_detail_has_dial(v)) lv_obj_send_event(v->canvas, LV_EVENT_RELEASED, NULL);
    orb_kit_motion_finish(v->root);
}

void orb_home_detail_poll(orb_home_detail_t *v)
{
    if (!v) return;
    orb_temperature_t temperature = orb_model_temperature();
    if (temperature == ORB_TEMPERATURE_C && v->snapshot.demo) temperature = ORB_TEMPERATURE_F;
    if (temperature != v->temperature) {
        v->temperature = temperature;
        /* A change of scale cancels a rim gesture: an old fraction must never
         * be committed in the new unit (HIG §1: predictable control). */
        orb_home_detail_build(v);
    }
    uint64_t generation = v->snapshot.generation;
    bool failed_before = v->snapshot.error[0] && !strcmp(v->snapshot.error_id,v->id);
    bool pending_before = v->accessory.pending;
    uint32_t scenes_before = v->page == 2 ? scene_signature(&v->snapshot) : 0;
    if (!orb_home_snapshot_if_changed(v->snapshot.revision,&v->snapshot)) {
        /* Polls are already driven by the shell. A stale reading's age is
         * the only text that moves with the clock; it never needs a timer. */
        if (v->available && !v->page && v->content) orb_home_detail_refresh_age(v);
        return;
    }
    if (generation != v->snapshot.generation) contact_cancel(v);
    bool failed_now = v->snapshot.error[0] && !strcmp(v->snapshot.error_id,v->id);
    if (failed_now && !failed_before) {
        v->failure_visible = true; v->failure_at = lv_tick_get(); wake(v);
        if (pending_before || v->content) orb_sound_play(ORB_SOUND_FAILED);
    } else if (!failed_now) v->failure_visible = false;
    if (v->page == 2 && scenes_before != scene_signature(&v->snapshot)) {
        const orb_home_accessory_t *current = orb_home_find(&v->snapshot,v->id);
        v->available = current != NULL;
        if (current) v->accessory = *current;
        orb_home_detail_state_t state;
        orb_home_detail_save(v, &state);
        orb_home_detail_restore(v, &state);
        return;
    }
    const orb_home_accessory_t *a = orb_home_find(&v->snapshot,v->id);
    bool available = a != NULL;
    if (v->available == available && (!a || !memcmp(a,&v->accessory,sizeof(*a)))) {
        orb_home_detail_refresh(v,false);
        return;
    }
    bool rebuild = !v->content || available != v->available || (a &&
        (a->kind != v->accessory.kind || a->writable != v->accessory.writable ||
         strcmp(a->name,v->accessory.name) ||
         (v->page == 1 && a->kind == ORB_HOME_THERMOSTAT && a->state.mode != v->accessory.state.mode) ||
         /* A completed write can remove an unsupported/transitional field.
          * Rebuild its controls and sampled glass after that shape changes. */
         a->state.present != v->accessory.state.present));
    bool failed = v->snapshot.error[0] && !strcmp(v->snapshot.error_id,v->id);
    v->available = available;
    if (a) v->accessory = *a;
    if (a && a->kind == ORB_HOME_LOCK && pending_before && !a->pending &&
        !failed && orb_home_detail_ready(&v->snapshot) && (a->state.present & ORB_HOME_LOCKED))
        orb_sound_play(ORB_SOUND_DONE);
    if (rebuild) orb_home_detail_build(v);
    else orb_home_detail_refresh(v,true);
}

void orb_home_detail_page(orb_home_detail_t *v, unsigned page)
{
    if (!v) return;
    if (v->accessory.kind == ORB_HOME_LIGHT) page %= 3;
    else if (v->accessory.kind == ORB_HOME_THERMOSTAT) page %= 2;
    else page = 0;
    if (v->page == page) return;
    orb_sound_play(page > v->page ? ORB_SOUND_OPEN : ORB_SOUND_CLOSE);
    if (v->list && v->page < 3) {
        v->scroll_y[v->page] = lv_obj_get_scroll_y(v->list);
        v->list = NULL;
    }
    v->page = page;
    orb_home_detail_build(v);
}

void orb_home_detail_destroy(orb_home_detail_t *v) { if (v) lv_obj_delete(v->root); }
lv_obj_t *orb_home_detail_root(const orb_home_detail_t *v) { return v ? v->root : NULL; }
unsigned orb_home_detail_page_index(const orb_home_detail_t *v) { return v ? v->page : 0; }
void orb_home_detail_save(const orb_home_detail_t *v, orb_home_detail_state_t *state)
{
    if (!v || !state) return;
    *state = (orb_home_detail_state_t){.page = v->page, .scene_offset = v->scene_offset};
    memcpy(state->scroll_y, v->scroll_y, sizeof(state->scroll_y));
    if (v->list && v->page < 3) state->scroll_y[v->page] = lv_obj_get_scroll_y(v->list);
    memcpy(state->scene_id, v->scene_focus_id, sizeof(state->scene_id));
}
void orb_home_detail_restore(orb_home_detail_t *v, const orb_home_detail_state_t *state)
{
    if (!v || !state) return;
    /* Restoring presentation never replays a write, cue or requested receipt.
     * Discard the old list pointer so build cannot overwrite restored scroll. */
    v->list = NULL;
    v->page = v->accessory.kind == ORB_HOME_LIGHT ? state->page % 3 :
        v->accessory.kind == ORB_HOME_THERMOSTAT ? state->page % 2 : 0;
    memcpy(v->scroll_y, state->scroll_y, sizeof(v->scroll_y));
    /* The scene list is scrolled, not paged; the remembered stable ID only
     * keeps naming the scene last asked for while it is still safe. */
    v->scene_offset = 0;
    v->scene_focus_id[0] = 0;
    for (unsigned i = 0; i < v->snapshot.scene_count; i++)
        if (!v->snapshot.scenes[i].contains_unlock && !strcmp(state->scene_id, v->snapshot.scenes[i].id))
            memcpy(v->scene_focus_id, v->snapshot.scenes[i].id, sizeof(v->scene_focus_id));
    orb_home_detail_build(v);
}
lv_obj_t *orb_home_detail_scroll(const orb_home_detail_t *v) { return v ? v->list : NULL; }
bool orb_home_detail_dial(orb_home_detail_t *v, float *value, float *step)
{
    if (!v) return false;
    uint32_t field = orb_home_detail_field(v);
    if (!orb_home_detail_has_dial(v) || !orb_home_detail_writable(v) ||
        !(v->accessory.writable & field) || !field_known(v, field)) return false;
    if (value) *value = v->rotating ? v->presented : orb_home_detail_model_value(v);
    if (step) *step = v->accessory.kind == ORB_HOME_THERMOSTAT ? .025f : .01f;
    return true;
}

void orb_home_detail_rotate(orb_home_detail_t *v, float value)
{
    float current, step;
    if (!isfinite(value) || !orb_home_detail_dial(v, &current, &step) ||
        !orb_input_require_physical(v->root)) return;
    v->rotating = v->dragging = true;
    orb_kit_motion_finish(v->root);
    lv_obj_send_event(v->canvas, LV_EVENT_PRESSED, NULL);
    orb_home_detail_present(v, clamp(value, 0, 1));
    orb_home_detail_refresh(v, false);
}

void orb_home_detail_rotary_finish(orb_home_detail_t *v, bool commit)
{
    if (!v) return;
    if (!commit) {
        /* The shell first retires its previous rotary owner on a bubbling
         * DOWN. That must not retire the new pointer contact begun above. */
        if (lv_indev_active() && v->press_at == lv_tick_get()) return;
        contact_cancel(v); orb_home_detail_refresh(v, false); return;
    }
    if (!v->rotating) return;
    float value = v->presented;
    v->rotating = v->dragging = false;
    lv_obj_send_event(v->canvas, LV_EVENT_RELEASED, NULL);
    /* HIG §§1/7: a captured turn previews locally and queues one write at
     * physical lift. Cancellation restores authority and cannot run a fling. */
    if (!commit || !commit_value(v, value)) value = orb_home_detail_model_value(v);
    orb_home_detail_start_settle(v, value);
    orb_home_detail_poll(v);
    orb_home_detail_refresh(v, false);
}

void orb_home_detail_set_contrast(orb_home_detail_t *v, bool increased_contrast)
{
    if (!v || v->contrast == increased_contrast) return;
    v->contrast = increased_contrast; orb_home_detail_build(v);
}

void orb_home_detail_system_navigation(orb_home_detail_t *v, bool enabled)
{
    if (!v) return;
    if (v->dragging) {
        v->dragging = false;
        if (v->canvas && orb_home_detail_has_dial(v)) lv_obj_send_event(v->canvas, LV_EVENT_RELEASED, NULL);
        orb_home_detail_start_settle(v, orb_home_detail_model_value(v));
    }
    hold_cancel(v);
    v->system_navigation = enabled;
    v->system_touch = false;
}
