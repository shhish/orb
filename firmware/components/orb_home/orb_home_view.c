/* Home's content is never glass (HIG §§1/2). The selected accessory colour is
 * the owner's light output, not a decorative tint (home-app.md, App identity). */
#include "orb_home_view_private.h"
#include "orb_model.h"
#include "orb_home_payload.h"
#include "orb_home_detail.h"
#include "orb_kit.h"
#include "orb_input.h"
#include "orb_sound.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool orb_home_ui_ready(const orb_home_snapshot_t *model)
{
    return model->online && model->synced && model->available;
}

static void render(orb_home_view_t *v);

static void home_readouts(orb_home_view_t *v)
{
    if (v->current.surface == ORB_HOME_ROOMS) orb_home_rooms_summary(v);
    else orb_home_pages_readouts(v);
}
static void render_later(void *ctx);
int orb_home_ui_row_height(void) { return ORB_KIT_ROW_H; }
static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false); lv_obj_set_clickable(o, false);
    lv_obj_set_event_bubble(o, true); return o;
}
lv_obj_t *orb_home_ui_text(lv_obj_t *p, const char *s, orb_text_style_t style, bool bold,
                      int x, int y, int width, int lines, lv_color_t color)
{
    lv_obj_t *o = lv_label_create(p);
    orb_text_apply(o, style, bold ? ORB_TEXT_EMPHASIZED : ORB_TEXT_REGULAR, 0);
    lv_label_set_text(o, s); lv_obj_set_style_text_color(o, color, 0);
    if (lv_color_eq(color, SECONDARY))
        lv_obj_set_style_text_opa(o, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    int h = lv_obj_get_style_text_font(o, 0)->line_height * lines;
    lv_obj_update_layout(p);
    lv_area_t parent_box; lv_obj_get_coords(p, &parent_box);
    /* A list row can be constructed offscreen. Its local line box must
     * survive scrolling; the round-list helper computes the live chord. */
    if (lv_obj_get_width(p) >= ORB_SCREEN_D) {
        int chord = orb_chord_width(parent_box.y1 + y, parent_box.y1 + y + h, ORB_RIM_MARGIN_TEXT);
        width = LV_MIN(width, chord);
    }
    lv_obj_set_size(o, width, h); lv_obj_set_pos(o, x - width/2, y);
    lv_obj_set_event_bubble(o, true); return o;
}

lv_color_t orb_home_light_colour(const orb_home_state_t *s)
{
    if ((s->present & (ORB_HOME_HUE | ORB_HOME_SAT)) == (ORB_HOME_HUE | ORB_HOME_SAT) && s->sat > 10)
        return lv_color_hsv_to_rgb((uint16_t)lroundf(s->hue) % 360, (uint8_t)lroundf(s->sat), 100);
    /* HomeKit CT is mireds (not kelvin). Low-saturation bulbs can retain a
     * stale hue alongside CT; use their white temperature (HIG §5: meaning). */
    if (s->present & ORB_HOME_CT) {
        float warm = fminf(1, fmaxf(0, (s->ct - 140) / 360));
        return lv_color_make(ORB_LABEL.red, (uint8_t)lroundf(249 - 62 * warm), (uint8_t)lroundf(255 - 143 * warm));
    }
    if ((s->present & (ORB_HOME_HUE | ORB_HOME_SAT)) == (ORB_HOME_HUE | ORB_HOME_SAT))
        return lv_color_hsv_to_rgb((uint16_t)lroundf(s->hue) % 360, (uint8_t)lroundf(s->sat), 100);
    return orb_color_from_rgb(ORB_RGB_HOME_WARM_WHITE);
}
static uint64_t hash_text(uint64_t hash, const char *text)
{
    do { hash = (hash ^ (unsigned char)*text) * UINT64_C(1099511628211); } while (*text++);
    return hash;
}
static uint64_t topology(const orb_home_snapshot_t *model)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < model->room_count; i++) {
        hash = hash_text(hash, model->rooms[i].id);
        hash = hash_text(hash, model->rooms[i].name);
    }
    for (unsigned i = 0; i < model->accessory_count; i++) {
        const orb_home_accessory_t *a = &model->accessories[i];
        hash = hash_text(hash_text(hash_text(hash, a->id), a->name), a->room);
        hash = (hash ^ a->kind ^ ((uint64_t)a->favourite << 16) ^
            (a->state.present & (ORB_HOME_TEMP | ORB_HOME_HUMIDITY_VALUE))) * UINT64_C(1099511628211);
    }
    for (unsigned i = 0; i < model->scene_count; i++) {
        const orb_home_scene_t *scene = &model->scenes[i];
        hash = hash_text(hash_text(hash, scene->id), scene->name);
        hash = (hash ^ scene->contains_unlock) * UINT64_C(1099511628211);
    }
    return hash;
}
/* An unrelated room's live traffic must not rebuild the rows under a finger.
 * Hash explicit scalar values, never structure padding or an identity alias. */
static uint64_t room_content(const orb_home_view_t *v)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < v->model->accessory_count; i++) {
        const orb_home_accessory_t *a = &v->model->accessories[i];
        if (strcmp(a->room, v->room)) continue;
        const orb_home_state_t *s = &a->state;
        uint32_t flags = a->reachable | ((uint32_t)a->pending << 1) |
            ((uint32_t)s->on << 2) | ((uint32_t)s->motion << 3) |
            ((uint32_t)s->contact << 4) | ((uint32_t)s->locked << 5);
        hash = (hash ^ flags ^ s->present ^ ((uint64_t)s->mode << 32)) * UINT64_C(1099511628211);
        const float values[] = {s->brightness, s->temp, s->target, s->humidity, s->speed, s->position, s->hue, s->sat, s->ct};
        const unsigned char *bytes = (const unsigned char *)values;
        for (unsigned j = 0; j < sizeof(values); j++) hash = (hash ^ bytes[j]) * UINT64_C(1099511628211);
        if (!strcmp(v->model->error_id, a->id)) hash = hash_text(hash, v->model->error);
    }
    return hash ^ (v->model->room_off_pending && !strcmp(v->model->room_off_id, v->room));
}

void orb_home_ui_render(orb_home_view_t *v)
{
    if (!v->render_pending) { v->render_pending = true; lv_async_call(render_later, v); }
}

lv_area_t orb_home_ui_token_area(orb_home_view_t *v, lv_obj_t *object)
{
    lv_area_t area = {193, 193, 272, 272};
    if (!object || !lv_obj_is_visible(object)) return area;
    lv_area_t root;
    lv_obj_get_coords(v->root, &root);
    /* Chord-fitted tokens use their presented area, including shoulders. */
    bool row = lv_obj_get_parent(object) == v->current.room_list;
    if (!(row ? orb_round_list_row_bounds(object, &area) :
          orb_round_list_child_bounds(lv_obj_get_parent(object), object, &area)))
        lv_obj_get_coords(object, &area);
    lv_area_move(&area, -root.x1, -root.y1);
    return area;
}

static void object_transition(orb_home_view_t *v, const lv_area_t *token, bool reverse)
{
    if (!v->transition || v->fixture) return;
    v->transition_token = *token;
    v->transition_reverse = reverse;
    v->transition_pending = true;
    v->transition(v->transition_ctx, token, reverse, true);
}

void orb_home_ui_guard(lv_event_t *e)
{
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (v->render_pending) { lv_event_stop_processing(e); return; }
    lv_indev_t *input = lv_event_get_indev(e);
    if (!input) return;
    lv_point_t point; lv_indev_get_point(input, &point);
    int dx = point.x - v->press.x, dy = point.y - v->press.y;
    /* Recognition can outlive the originally pressed widget. Do not let its
     * late Click or Long Press turn a captured crown/edge drag into an action. */
    lv_obj_t *target = lv_event_get_current_target_obj(e);
    if (v->press_moved || dx*dx + dy*dy >= 10*10 ||
        (lv_event_get_code(e) == LV_EVENT_CLICKED && lv_tick_elaps(v->press_at) >= 500) ||
        (lv_obj_get_parent(target) == v->current.room_list &&
         !orb_round_list_hit_test(target, &point)))
        lv_event_stop_processing(e);
}

/* All actions use the kit's geometry and contact retirement (HIG §§2/7/8).
 * Position is content-specific; size/material never comes from the caller. */
lv_obj_t *orb_home_ui_button(orb_home_view_t *v, const char *caption, int x, int y,
                        bool primary, lv_event_cb_t cb, intptr_t data)
{
    lv_obj_t *o = primary ? orb_kit_button_primary_create(v->current.page, caption, ORB_KIT_HOME) :
        orb_kit_button_secondary_create(v->current.page, caption);
    lv_obj_update_layout(o);
    lv_obj_set_pos(o, x-lv_obj_get_width(o)/2, y-lv_obj_get_height(o)/2);
    lv_obj_set_event_bubble(o, true);
    lv_obj_set_user_data(o, (void *)data);
    if (cb && !v->fixture) {
        lv_obj_add_event_cb(o, orb_home_ui_guard, LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(o, orb_home_ui_guard, LV_EVENT_LONG_PRESSED, v);
        lv_obj_add_event_cb(o, cb, LV_EVENT_ALL, v);
    }
    return o;
}
bool orb_home_ui_dragged(const orb_home_view_t *v)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) return false;
    lv_point_t point; lv_indev_get_point(in, &point);
    return v->press_moved || abs(point.x-v->press.x) >= 10 || abs(point.y-v->press.y) >= 10;
}
lv_obj_t *orb_home_ui_header(orb_home_view_t *v, const char *title)
{
    lv_obj_t *label = orb_kit_rim_title_create(v->current.page, title, ORB_KIT_HOME);
    orb_kit_rim_title_set_max_degrees(label, 90); /* one Home rim budget (HIG §3) */
    lv_obj_set_event_bubble(label, true);
    return label;
}
static bool room_present(const orb_home_view_t *v, const char *id)
{
    for (unsigned i = 0; i < v->model->room_count; i++)
        if (!strcmp(id, v->model->rooms[i].id)) return true;
    return false;
}

static void context_resolve(orb_home_view_t *v)
{
    /* IDs, rather than indices, define return context. A missing accessory
     * retains an explicit unavailable detail; BOOT returns to its room. */
    if (v->selected[0] && !orb_home_find(v->model, v->selected)) {
        v->detail_page = 0;
        v->restore_detail = false;
    }
    if (v->room[0] && !room_present(v, v->room)) {
        v->room[0] = v->selected[0] = 0;
        v->current.surface = ORB_HOME_ROOMS;
        v->sections = v->sensors = v->scene_page = false;
        v->room_scroll = 0;
        v->restore_detail = false;
    }
    if (v->current.surface == ORB_HOME_ROOM && !v->room[0]) v->current.surface = ORB_HOME_ROOMS;
    for (unsigned i = 0; i < ORB_HOME_MAX_ROOMS; i++)
        if (v->room_positions[i].id[0] && !room_present(v, v->room_positions[i].id))
            v->room_positions[i] = (orb_home_room_position_t){0};
    v->scene_index = v->model->scene_count ? v->scene_index % v->model->scene_count : 0;
}

lv_obj_t *orb_home_ui_list(orb_home_view_t *v, int top, int bottom)
{
    lv_obj_t *list = plain(v->current.page);
    v->current.room_list = list;
    lv_obj_set_pos(list, 0, top);
    lv_obj_set_size(list, ORB_SCREEN_D, bottom - top);
    lv_obj_set_clickable(list, true);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scroll_momentum(list, true);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

void orb_home_ui_list_finish(lv_obj_t *list, int scroll)
{
    /* Settings' 78px tail must fit inside the shorter AX3 Rooms viewport;
     * otherwise its final row can never bring both stacked lines into view. */
    int tail = LV_MIN(78, LV_MAX(0, (lv_obj_get_height(list)-orb_home_ui_row_height())/2));
    lv_obj_set_style_pad_bottom(list, LV_MAX(tail, lv_obj_get_style_pad_bottom(list, 0)), 0);
    orb_kit_list_apply(list);
    orb_kit_scroll_arc_attach(list);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, scroll, LV_ANIM_OFF);
    orb_round_list_update(list);
    lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
    /* HIG §2: dissolve departing content below the fixed Home header and
     * above scene controls/the bezel, rather than cutting a preview's text. */
    orb_scroll_edge_follow(list, 24, 52);
}

static void render_later(void *ctx) { orb_home_view_t *v=ctx;v->render_pending=false;render(v); }
static void build_page(orb_home_view_t *v)
{
    orb_home_page_t surface = v->current.surface;
    unsigned offset = v->current.offset;
    v->current = (home_page_t){.surface = surface, .offset = offset};
    v->current.page = plain(v->root);
    lv_obj_set_size(v->current.page, ORB_SCREEN_D, ORB_SCREEN_D);
    if(v->selected[0]) {
        v->detail=orb_home_detail_create(v->current.page,v->selected,&v->commands);
        snprintf(v->detail_id, sizeof(v->detail_id), "%s", v->selected);
        if (v->detail) {
            orb_home_detail_set_contrast(v->detail, v->contrast);
            orb_home_detail_system_navigation(v->detail, v->system_navigation);
        }
        if (v->detail && v->restore_detail) {
            orb_home_detail_restore(v->detail, &v->detail_state);
            v->restore_detail = false;
        } else if (v->detail && v->detail_page) {
            orb_home_detail_state_t state = {.page = v->detail_page};
            orb_home_detail_restore(v->detail, &state);
        }
        if (v->detail && v->fresh_detail) orb_home_detail_arrival(v->detail);
        v->fresh_detail = false;
    } else {
        if (v->sections && v->current.surface == ORB_HOME_ROOM) orb_home_room_actions_build(v);
        else if (v->scene_page) orb_home_pages_scenes(v);
        else if(v->current.surface==ORB_HOME_ROOMS) orb_home_rooms_build(v);
        else if(v->current.surface==ORB_HOME_CLIMATE) { orb_home_pages_climate(v); orb_home_pages_readouts(v); }
        else if(v->current.surface==ORB_HOME_ROOM) orb_home_room_build(v);
    }
    lv_obj_update_layout(v->current.page);
}

static void navigation(orb_home_view_t *v)
{
    if (v->navigation) lv_obj_delete(v->navigation);
    v->navigation = plain(v->root);
    lv_obj_set_size(v->navigation, ORB_SCREEN_D, ORB_SCREEN_D);
    lv_obj_t *content = v->current.page;
    v->current.page = v->navigation;
    if (!v->selected[0]) {
        if (v->scene_page || v->current.surface == ORB_HOME_CLIMATE)
            orb_home_ui_header(v, v->scene_page ? "Scenes" : v->sensors ? "Sensors" : "Climate");
    }
    v->current.page = content;
    lv_obj_update_layout(v->navigation);
}

static int *room_position(orb_home_view_t *v, const char *id)
{
    unsigned empty = ORB_HOME_MAX_ROOMS;
    for (unsigned i = 0; i < ORB_HOME_MAX_ROOMS; i++) {
        if (!strcmp(v->room_positions[i].id, id)) return &v->room_positions[i].scroll;
        if (!v->room_positions[i].id[0] && empty == ORB_HOME_MAX_ROOMS) empty = i;
    }
    /* Topology can replace every room during a boot; recycle an orphan only. */
    if (empty == ORB_HOME_MAX_ROOMS) {
        for (unsigned i = 0; i < ORB_HOME_MAX_ROOMS; i++) {
            bool present = false;
            for (unsigned r = 0; r < v->model->room_count; r++)
                present |= !strcmp(v->room_positions[i].id, v->model->rooms[r].id);
            if (!present) { empty = i; break; }
        }
    }
    if (empty == ORB_HOME_MAX_ROOMS) return NULL;
    snprintf(v->room_positions[empty].id, sizeof(v->room_positions[empty].id), "%s", id);
    v->room_positions[empty].scroll = 0;
    return &v->room_positions[empty].scroll;
}

static int landing_position(const orb_home_view_t *v, int scroll)
{
    /* Zero means fresh/resting. Preserve the pulled-to-equator position too,
     * using -1 for native zero so Back never jumps to the resting shoulder. */
    return scroll == v->current.rooms_rest ? 0 : scroll == 0 ? -1 : scroll;
}

static void save_scroll(orb_home_view_t *v)
{
    if (!v->current.room_list) return;
    int scroll = lv_obj_get_scroll_y(v->current.room_list);
    if (v->current.room_devices) {
        int *position = room_position(v, v->current.room_id);
        if (position) *position = scroll;
        if (!strcmp(v->room, v->current.room_id)) v->room_scroll = scroll;
    } else if (v->current.scene_list) v->scene_scroll = scroll;
    else if (v->current.sensor_list) v->sensor_scroll = scroll;
    else if (v->current.landing) v->rooms_scroll = landing_position(v, scroll);
}

static void render(orb_home_view_t *v)
{
    v->rebuilding = true;
    orb_home_motion_cancel(v);
    save_scroll(v);
    if(v->detail) {
        if (!v->restore_detail && !strcmp(v->detail_id, v->selected)) {
            orb_home_detail_save(v->detail, &v->detail_state);
            v->restore_detail = true;
        }
        orb_home_detail_destroy(v->detail); v->detail = NULL;
    }
    if(v->current.page) lv_obj_delete(v->current.page);
    bool local_arrival = v->fresh_detail && !v->transition;
    build_page(v);
    if (v->current.landing || v->selected[0]) v->wait.kind = 0;
    navigation(v);
    lv_obj_update_layout(v->root);
    lv_obj_invalidate(v->root);
    v->rebuilding = false;
    /* A direct embedding has no outer transition to finish. Its live detail
     * arrival starts after construction, using the same kit completion path. */
    if (local_arrival) orb_home_view_arrival_complete(v, true);
    if (v->transition_pending && v->transition) {
        v->transition_pending = false;
        v->transition(v->transition_ctx, &v->transition_token, v->transition_reverse, false);
    }
}

static void contact_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_SCROLL_BEGIN) orb_kit_motion_interrupt(v->root);
    if (code == LV_EVENT_SCROLL_BEGIN) { orb_home_motion_cancel(v); return; }
    if (code == LV_EVENT_SCROLL) {
        int32_t position = v->current.room_list ? lv_obj_get_scroll_y(v->current.room_list) : 0;
        bool moved = position != v->arrival_scroll;
        v->arrival_scroll = position;
        /* Layout can publish SCROLL without moving content. Only real travel
         * cancels arrival; start-state preparation must survive its layout. */
        if (moved) orb_home_motion_cancel(v);
        return;
    }
    if (code == LV_EVENT_PRESSED) orb_home_motion_cancel(v);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p; lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        v->press = p; v->press_moved = false; v->press_at = lv_tick_get();
    }
    else if ((p.x-v->press.x)*(p.x-v->press.x) +
             (p.y-v->press.y)*(p.y-v->press.y) >= 10*10) v->press_moved = true;
    if (v->press_moved) {
        /* The kit retires this contact until a fresh physical press. */
        for (lv_obj_t *target = lv_event_get_target_obj(e); target && target != v->root;
             target = lv_obj_get_parent(target)) {
            orb_kit_press_cancel(target);
        }
    }
}
static void deleted(lv_event_t *e)
{
    orb_home_view_t *v=lv_event_get_user_data(e);
    lv_async_call_cancel(render_later,v);orb_home_payload_destroy(v->model,0,sizeof(*v->model));orb_home_payload_destroy(v,sizeof(*v),0);
}
static orb_home_view_t *view_create(lv_obj_t *parent,const orb_home_commands_t *commands,
                                    const orb_home_snapshot_t *fixture)
{
    if(!orb_home_init())return NULL;
    orb_home_view_t *v=orb_home_payload_create(sizeof(*v),0);if(!v)return NULL;
    v->model=orb_home_payload_create(0,sizeof(*v->model));if(!v->model){orb_home_payload_destroy(v,sizeof(*v),0);return NULL;}
    v->commands=commands?*commands:(orb_home_commands_t){0};v->hour=19;v->current.surface=ORB_HOME_ROOMS;v->fixture=fixture!=NULL;
    if(fixture)*v->model=*fixture;else orb_home_snapshot(v->model);
    orb_home_waits_sync(v);
    v->failure_active = v->model->error[0] != 0; v->failure_at = lv_tick_get();
    v->temperature=orb_model_temperature();
    if (v->temperature == ORB_TEMPERATURE_C && (v->fixture || (v->model && v->model->demo)))
        v->temperature = ORB_TEMPERATURE_F;
    v->root=plain(parent);lv_obj_set_size(v->root,466,466);
    lv_obj_set_style_bg_color(v->root,lv_color_black(),0);lv_obj_set_style_bg_opa(v->root,255,0);
    lv_obj_add_event_cb(v->root,deleted,LV_EVENT_DELETE,v);
    lv_obj_set_clickable(v->root,true);lv_obj_set_press_lock(v->root,true);
    lv_obj_add_event_cb(v->root,contact_event,LV_EVENT_PRESSED,v);lv_obj_add_event_cb(v->root,contact_event,LV_EVENT_RELEASED,v);
    lv_obj_add_event_cb(v->root,contact_event,LV_EVENT_PRESSING,v);
    lv_obj_add_event_cb(v->root,contact_event,LV_EVENT_PRESS_LOST,v);
    lv_obj_add_event_cb(v->root,contact_event,LV_EVENT_SCROLL_BEGIN,v);
    lv_obj_add_event_cb(v->root,contact_event,LV_EVENT_SCROLL,v);
    render(v);return v;
}
orb_home_view_t *orb_home_view_create(lv_obj_t *parent,const orb_home_commands_t *commands)
{return view_create(parent,commands,NULL);}
orb_home_view_t *orb_home_view_create_fixture(lv_obj_t *parent,const orb_home_snapshot_t *snapshot)
{return snapshot?view_create(parent,NULL,snapshot):NULL;}

void orb_home_view_destroy(orb_home_view_t *v)
{
    if(!v)return;
    /* Detail owns an independent paused timer; release before its parent. */
    if(v->detail){orb_home_detail_destroy(v->detail);v->detail=NULL;}
    lv_obj_delete(v->root);
}
void orb_home_view_poll(orb_home_view_t *v)
{
    if(!v||v->fixture||v->rebuilding)return;
    if(v->detail)orb_home_detail_poll(v->detail);
    orb_temperature_t temperature = orb_model_temperature();
    if (temperature == ORB_TEMPERATURE_C && (v->fixture || (v->model && v->model->demo)))
        temperature = ORB_TEMPERATURE_F;
    if (temperature != v->temperature) {
        v->temperature = temperature;
        /* Units are content: refresh retained labels without rebuilding a
         * touched control or adding glass (HIG §§1/4/7). */
        if (!v->detail) {
            if (v->current.surface == ORB_HOME_ROOMS || v->current.surface == ORB_HOME_CLIMATE) home_readouts(v);
            else orb_home_ui_render(v);
        }
    }
    if (v->failure_active && lv_tick_elaps(v->failure_at) >= 3500) {
        v->failure_active = false;
        if (!v->detail && v->current.surface == ORB_HOME_ROOMS) orb_home_rooms_summary(v);
        else if (!v->detail && v->current.surface == ORB_HOME_ROOM && !v->sections) orb_home_ui_render(v);
    }
    if (v->scene_feedback_at && lv_tick_elaps(v->scene_feedback_at) >= 4000) {
        v->scene_feedback_at = 0;
        if (!v->detail && v->current.surface == ORB_HOME_ROOMS) orb_home_rooms_summary(v);
    }
    /* Idle Home must not hash/copy the entire 64-device model on every shell
     * poll. The revision is cheap; topology is only examined after a change. */
    if (orb_home_revision() == v->model->revision) return;
    uint64_t old_topology = topology(v->model);
    uint64_t old_landing = orb_home_rooms_topology(v->model);
    uint64_t old_room_content = v->current.surface == ORB_HOME_ROOM ? room_content(v) : 0;
    size_t accessories = v->model->accessory_count, rooms = v->model->room_count, scenes_count = v->model->scene_count;
    bool online = v->model->online, synced = v->model->synced, available = v->model->available;
    bool scene_pending = v->model->scene_pending;
    bool all_off_pending = v->model->room_off_pending;
    bool had_error = v->model->error[0] != 0;
    if(!orb_home_snapshot_if_changed(v->model->revision,v->model))return;
    orb_home_waits_sync(v);
    if (v->arrival_armed) orb_home_motion_cancel(v);
    if (!v->model->error[0]) v->failure_active = false;
    else if (!had_error) {
        v->failure_active = true; v->failure_at = lv_tick_get();
        if (!v->detail) orb_sound_play(ORB_SOUND_FAILED);
    }
    if (scene_pending && !v->model->scene_pending && !v->model->error[0] && orb_home_ui_ready(v->model)) {
        v->scene_feedback_at = lv_tick_get();
        /* A result first seen later is final; Back never replays it. */
        v->outcome_changed = v->scene_page && !v->selected[0] && orb_kit_motion_allowed(v->root);
        orb_sound_play(ORB_SOUND_DONE);
    }
    if (all_off_pending && !v->model->room_off_pending && !v->model->room_off_failed)
        v->outcome_changed = v->sections && v->current.surface == ORB_HOME_ROOM &&
            !v->selected[0] && !strcmp(v->model->room_off_id, v->room) && orb_kit_motion_allowed(v->root);
    bool topology_changed = old_topology != topology(v->model) || accessories != v->model->accessory_count ||
        rooms != v->model->room_count || scenes_count != v->model->scene_count;
    if (topology_changed) {
        bool selected = v->selected[0] != 0;
        context_resolve(v);
        if (selected && !v->selected[0]) { orb_home_ui_render(v); return; }
    }
    bool landing = !v->selected[0] && !v->scene_page && v->current.surface == ORB_HOME_ROOMS;
    bool rebuild = landing ? old_landing != orb_home_rooms_topology(v->model) : topology_changed;
    if (!v->detail && (rebuild || online != v->model->online || synced != v->model->synced || available != v->model->available)) {
        orb_home_ui_render(v); return;
    }
    if(v->detail) {
        if (online != v->model->online || synced != v->model->synced || available != v->model->available)
            orb_home_ui_render(v);
        return;
    }
    if (v->scene_page) { orb_home_ui_render(v); return; }
    if (v->sections) {
        if (v->current.surface == ORB_HOME_ROOM) orb_home_ui_render(v);
        return;
    }
    if(v->current.surface==ORB_HOME_ROOMS||v->current.surface==ORB_HOME_CLIMATE){home_readouts(v);return;}
    if (v->current.surface == ORB_HOME_ROOM) {
        /* Refresh state and readouts in place so ongoing input and live
         * disc effects retain their existing rows and sources. */
        if (old_room_content != room_content(v)) orb_home_room_values_refresh(v);
        return;
    }
    orb_home_ui_render(v);
}
void orb_home_view_select(orb_home_view_t *v,orb_home_page_t page)
{
    if (!v) return;
    save_scroll(v);
    if (page == ORB_HOME_ROOMS) {
        v->rooms_scroll = 0;
        v->current.landing = false;
    }
    v->current.surface = page;
    v->selected[0] = 0;
    v->current.offset = 0;
    v->sections = v->sensors = v->scene_page = false;
    orb_home_ui_render(v);
}
void orb_home_view_open(orb_home_view_t *v,const char *id)
{
    if (!v || !id || !id[0] || strlen(id) > ORB_HOME_ID_MAX) return;
    lv_obj_t *disc = NULL;
    if (v->current.room_devices && v->current.room_list) {
        for (unsigned i = 0; i < lv_obj_get_child_count(v->current.room_list); i++) {
            lv_obj_t *row = lv_obj_get_child(v->current.room_list, i);
            if (lv_obj_is_floating(row) || !lv_obj_is_clickable(row)) continue;
            const char *semantic = lv_obj_get_user_data(row);
            if (semantic && !strcmp(semantic, id)) { disc = lv_obj_get_child(row, 0); break; }
        }
    }
    v->accessory_token = orb_home_ui_token_area(v, disc);
    object_transition(v, &v->accessory_token, false);
    snprintf(v->selected, sizeof(v->selected), "%s", id);
    v->detail_page = 0;
    v->restore_detail = false;
    v->fresh_detail = true;
    orb_sound_play(ORB_SOUND_OPEN);
    orb_home_ui_render(v);
}
void orb_home_view_room(orb_home_view_t *v,const char *id)
{
    if (!v || !id || !room_present(v, id)) return;
    lv_obj_t *row = NULL;
    for (unsigned i = 0; i < v->model->room_count; i++)
        if (!strcmp(id, v->model->rooms[i].id)) { row = v->current.room_rows[i]; break; }
    v->room_token = orb_home_ui_token_area(v, row);
    object_transition(v, &v->room_token, false);
    save_scroll(v);
    snprintf(v->room, sizeof(v->room), "%s", id);
    v->current.surface = ORB_HOME_ROOM;
    v->selected[0] = 0;
    v->current.offset = 0;
    /* Restore by stable identity, including after another room was visited. */
    v->current.room_devices = false;
    int *position = room_position(v, id);
    v->room_scroll = position ? *position : 0;
    orb_sound_play(ORB_SOUND_OPEN);
    orb_home_ui_render(v);
}
void orb_home_view_detail_page(orb_home_view_t *v,unsigned page)
{if(v){v->detail_page=page;if(v->detail&&!v->render_pending)orb_home_detail_page(v->detail,page);}}
void orb_home_view_set_contrast(orb_home_view_t *v, bool increased)
{
    if (!v || v->contrast == increased) return;
    v->contrast = increased;
    if (v->detail) orb_home_detail_set_contrast(v->detail, increased);
}
void orb_home_view_hour(orb_home_view_t *v,unsigned hour)
{if(v)v->hour=hour%24;}
lv_obj_t *orb_home_view_root(orb_home_view_t *v){return v?v->root:NULL;}
void orb_home_view_set_transition(orb_home_view_t *v, orb_home_transition_cb_t transition, void *ctx)
{
    if (!v) return;
    v->transition = transition;
    v->transition_ctx = ctx;
}

bool orb_home_view_moving(const orb_home_view_t *v)
{ return v && v->render_pending; }
lv_obj_t *orb_home_view_scroll(orb_home_view_t *v)
{ return v ? (v->detail ? orb_home_detail_scroll(v->detail) : v->current.room_list) : NULL; }

void orb_home_view_system_navigation(orb_home_view_t *v, bool enabled)
{
    if (!v) return;
    if (v->system_navigation == enabled) return;
    v->system_navigation = enabled;
    navigation(v);
    if (v->detail) orb_home_detail_system_navigation(v->detail, enabled);
}

bool orb_home_view_can_back(const orb_home_view_t *v)
{
    return v && (v->selected[0] || v->sections || v->sensors || v->scene_page ||
                 v->current.surface == ORB_HOME_ROOM || v->current.surface == ORB_HOME_CLIMATE);
}

bool orb_home_view_back(orb_home_view_t *v)
{
    if (!v) return false;
    if (v->selected[0]) {
        /* Preserve the room list's page, and consume deeper
         * colour/scene pages before leaving the accessory itself. */
        if (v->detail && orb_home_detail_page_index(v->detail)) {
            v->detail_page = 0;
            orb_home_detail_page(v->detail, 0);
            return true; /* detail_page owns this one semantic Close cue */
        } else {
            object_transition(v, &v->accessory_token, true);
            v->selected[0] = 0;
            v->detail_page = 0;
            orb_home_ui_render(v);
        }
    } else if (v->sections || v->sensors || v->scene_page) {
        v->sections = v->sensors = v->scene_page = false;
        orb_home_ui_render(v);
    } else if (v->current.surface == ORB_HOME_ROOM) {
        object_transition(v, &v->room_token, true);
        v->current.surface = ORB_HOME_ROOMS;
        orb_home_ui_render(v);
    } else if (v->current.surface == ORB_HOME_CLIMATE) {
        v->current.surface = ORB_HOME_ROOMS;
        orb_home_ui_render(v);
    } else return false;
    orb_sound_play(ORB_SOUND_CLOSE);
    return true;
}

void orb_home_view_rotary(orb_home_view_t *v, float delta)
{
    if (!v || !isfinite(delta) || v->render_pending || v->detail) return;
    orb_home_motion_cancel(v);
    if (v->current.room_list) {
        lv_obj_update_layout(v->current.room_list);
        int maximum = LV_MAX(0, lv_obj_get_scroll_y(v->current.room_list) +
                            lv_obj_get_scroll_bottom(v->current.room_list));
        int position = LV_CLAMP(0, (int)lroundf(lv_obj_get_scroll_y(v->current.room_list) + delta), maximum);
        if (!orb_round_list_scroll_to(v->current.room_list, position))
            lv_obj_scroll_to_y(v->current.room_list, position, LV_ANIM_OFF);
    }
}

bool orb_home_view_dial(orb_home_view_t *v, float *value, float *step)
{
    if (!v) return false;
    return !v->render_pending && orb_home_detail_dial(v->detail, value, step);
}

bool orb_home_view_dial_writes(const orb_home_view_t *v)
{
    return v && orb_home_view_dial((orb_home_view_t *)v, NULL, NULL);
}

void orb_home_view_rotate(orb_home_view_t *v, float value)
{
    if (!v || !isfinite(value) || v->render_pending) return;
    if (!v->fixture) orb_home_detail_rotate(v->detail, value);
}

void orb_home_view_rotary_finish(orb_home_view_t *v, bool commit)
{
    if (!v) return;
    /* Lock, sheets and navigation retire a held contact through this path.
     * Its later release cannot become a room tap; a fresh DOWN still owns itself. */
    if (!commit && !(lv_indev_active() && v->press_at == lv_tick_get())) v->press_moved = true;
    if (v->current.room_list) orb_round_list_scroll_finish(v->current.room_list);
    if (!v->fixture) orb_home_detail_rotary_finish(v->detail, commit);
}

void orb_home_view_save(const orb_home_view_t *v, orb_home_view_state_t *state)
{
    if (!v || !state) return;
    *state = (orb_home_view_state_t){.surface = v->current.surface,
        .offset = v->current.offset,
        .detail_page = v->detail ? orb_home_detail_page_index(v->detail) : v->detail_page,
        .scene_index = v->scene_index, .rooms_scroll = v->rooms_scroll, .room_scroll = v->room_scroll,
        .scene_scroll = v->scene_scroll, .sensor_scroll = v->sensor_scroll,
        .room_token = v->room_token, .accessory_token = v->accessory_token,
        .scroll = v->current.room_list ? lv_obj_get_scroll_y(v->current.room_list) : 0,
        .sections = v->sections, .sensors = v->sensors, .scene_page = v->scene_page};
    memcpy(state->room, v->room, sizeof(state->room));
    memcpy(state->selected, v->selected, sizeof(state->selected));
    memcpy(state->room_positions, v->room_positions, sizeof(state->room_positions));
    if (v->detail) {
        orb_home_detail_state_t detail;
        orb_home_detail_save(v->detail, &detail);
        memcpy(state->detail_scroll_y, detail.scroll_y, sizeof(state->detail_scroll_y));
        state->detail_scene_offset = detail.scene_offset;
        memcpy(state->detail_scene_id, detail.scene_id, sizeof(state->detail_scene_id));
    }
    if (v->current.room_list) {
        if (v->current.scene_list) state->scene_scroll = state->scroll;
        else if (v->current.sensor_list) state->sensor_scroll = state->scroll;
        else if (v->current.landing)
            state->rooms_scroll = landing_position(v, state->scroll);
        else if (v->current.room_devices) {
            state->room_scroll = state->scroll;
            for (unsigned i = 0; i < ORB_HOME_MAX_ROOMS; i++)
                if (!strcmp(state->room_positions[i].id, v->current.room_id))
                    state->room_positions[i].scroll = state->scroll;
        }
    }
}

void orb_home_view_restore(orb_home_view_t *v, const orb_home_view_state_t *state)
{
    if (!v || !state || state->surface > ORB_HOME_ROOM) return;
    orb_kit_motion_finish(v->root);
    v->fresh_detail = false;
    lv_async_call_cancel(render_later, v);
    v->render_pending = false;
    /* render() normally saves the old Rooms offset; this explicit restore
     * supplies its own and must not let the outgoing tree overwrite it. */
    v->current.room_list = NULL;
    v->current.surface = state->surface;
    v->current.offset = state->offset;
    v->detail_page = state->detail_page;
    v->scene_index = state->scene_index;
    v->rooms_scroll = state->rooms_scroll;
    v->room_scroll = state->room_scroll;
    v->scene_scroll = state->scene_scroll;
    v->sensor_scroll = state->sensor_scroll;
    v->room_token = state->room_token;
    v->accessory_token = state->accessory_token;
    memcpy(v->room_positions, state->room_positions, sizeof(v->room_positions));
    v->sections = state->sections;
    v->sensors = state->sensors;
    v->scene_page = state->scene_page;
    memcpy(v->room, state->room, sizeof(v->room));
    memcpy(v->selected, state->selected, sizeof(v->selected));
    v->detail_state = (orb_home_detail_state_t){.page = state->detail_page,
        .scene_offset = state->detail_scene_offset};
    memcpy(v->detail_state.scroll_y, state->detail_scroll_y, sizeof(v->detail_state.scroll_y));
    memcpy(v->detail_state.scene_id, state->detail_scene_id, sizeof(v->detail_state.scene_id));
    v->restore_detail = v->selected[0] != 0;
    context_resolve(v);
    render(v);
    /* orb_home_rooms_build() already decodes its fresh/resting sentinel. A fresh app context
     * also has scroll=0; replaying that raw value would undo the resting pose. */
    if (v->current.room_list && !v->current.landing)
        lv_obj_scroll_to_y(v->current.room_list, state->scroll, LV_ANIM_OFF);
}
