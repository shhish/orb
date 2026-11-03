/* Home's retained room rows: construction is separate from live patching so
 * a real on/off change never replaces the row under a finger (HIG §§1/7). */
#include "orb_home_view_private.h"
#include "orb_input.h"
#include "orb_sound.h"
#include <stdio.h>
#include <string.h>

static uint64_t hash_text(uint64_t hash, const char *text)
{
    do { hash = (hash ^ (unsigned char)*text) * UINT64_C(1099511628211); } while (*text++);
    return hash;
}

static const char *room_name(const orb_home_view_t *v)
{
    for (unsigned i = 0; i < v->model->room_count; i++)
        if (!strcmp(v->room, v->model->rooms[i].id)) return v->model->rooms[i].name[0] ? v->model->rooms[i].name : "Unnamed room";
    return v->room;
}
static void room_actions_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (orb_home_ui_dragged(v)) return;
    v->sections = true; orb_sound_play(ORB_SOUND_OPEN); orb_home_ui_render(v);
}
static void room_event(lv_event_t *e)
{
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (v->fixture || orb_home_ui_dragged(v) || orb_home_view_moving(v)) return;
    const char *semantic = lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (semantic) orb_home_view_room(v, semantic + strlen("room:"));
}

static void climate_event(lv_event_t *e)
{
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (v->fixture || orb_home_ui_dragged(v) || orb_home_view_moving(v)) return;
    orb_home_view_select(v, ORB_HOME_CLIMATE);
}
static void room_row_click(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_LONG_PRESSED) return;
    /* Only the named row owns an action; its glyph is passive content. */
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) return;
    if (orb_home_view_moving(lv_event_get_user_data(e))) return;
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (v->fixture || orb_home_ui_dragged(v)) return;
    const char *id = lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (!id) return;
    const orb_home_accessory_t *a = orb_home_find(v->model, id);
    if (!a) return;
    if (code == LV_EVENT_LONG_PRESSED) {
        orb_home_view_open(v, a->id);
        return;
    }
    if (a->kind != ORB_HOME_SWITCH && a->kind != ORB_HOME_OUTLET &&
        !(a->kind == ORB_HOME_LIGHT && !(a->state.present & ORB_HOME_BRIGHTNESS))) {
        orb_home_view_open(v, a->id);
    } else {
        /* An unknown or pending power state opens detail instead of guessing. */
        if (!orb_home_ui_ready(v->model) || !a->reachable || a->pending || !(a->writable & ORB_HOME_ON) || !(a->state.present & ORB_HOME_ON)) {
            orb_home_view_open(v, a->id); return;
        }
        if (!orb_input_require_physical(v->root)) return;
        if (orb_home_command(&v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_TOGGLE, .id = a->id})) {
            orb_sound_play(a->state.on ? ORB_SOUND_HOME_OFF : ORB_SOUND_HOME_ON);
            orb_home_view_poll(v);
        }
    }
}

static int room_acc_rank(const orb_home_accessory_t *a)
{
    if (a->kind == ORB_HOME_THERMOSTAT) return 0;
    if (a->kind == ORB_HOME_LIGHT) return 1;
    if (a->kind == ORB_HOME_LOCK) return 4;
    if (a->kind == ORB_HOME_SWITCH || a->kind == ORB_HOME_OUTLET) return 5;
    if (a->kind == ORB_HOME_FAN) return 6;
    if (a->kind == ORB_HOME_TEMPERATURE || a->kind == ORB_HOME_HUMIDITY || a->kind == ORB_HOME_SENSOR ||
        a->kind == ORB_HOME_MOTION || a->kind == ORB_HOME_CONTACT) return 7;
    return 8;
}

/* A single formatter keeps the peripheral and central rows equally honest:
 * cached booleans are never presented as reported states (HIG §§5/8). While
 * Home is stale a row keeps its dimmed last value and identity; the header
 * says "Last known" once, so no row name is truncated to fit a qualifier. */
static void room_state(const orb_home_view_t *v, const orb_home_accessory_t *a,
                       char *value, size_t capacity)
{
    const orb_home_state_t *s = &a->state;
    orb_home_status_t status = orb_home_status_accessory(v->model, a, v->failure_active);
    if (status != ORB_HOME_STATUS_READY && orb_home_status_home(v->model) == ORB_HOME_STATUS_READY)
        snprintf(value, capacity, "%s", orb_home_status_text(status));
    else if (a->kind == ORB_HOME_THERMOSTAT || (!(s->present & ORB_HOME_HUMIDITY_VALUE) && (s->present & ORB_HOME_TEMP) &&
             a->kind != ORB_HOME_HUMIDITY)) {
        /* One room-row rail fits the whole sensor domain at AX3. Show the
         * current reading here; the opened dial owns target. */
        if (s->present & ORB_HOME_TEMP) orb_home_format_temperature(value, capacity, s->temp, v->temperature, false);
        else snprintf(value, capacity, "—");
    } else if (a->kind == ORB_HOME_LOCK) {
        snprintf(value, capacity, "%s", !(s->present & ORB_HOME_LOCKED) ? "—" : s->locked ? "Locked" : "Unlocked");
    } else if (a->kind == ORB_HOME_COVER && (s->present & ORB_HOME_POSITION)) {
        snprintf(value, capacity, "%.0f%%", (double)s->position);
    } else if (s->present & ORB_HOME_HUMIDITY_VALUE) snprintf(value, capacity, "%.0f%%", (double)s->humidity);
    else if (s->present & ORB_HOME_MOTION_VALUE) snprintf(value, capacity, "%s", s->motion ? "Motion" : "Clear");
    else if (s->present & ORB_HOME_CONTACT_VALUE) snprintf(value, capacity, "%s", s->contact ? "Open" : "Closed");
    else if (!(s->present & ORB_HOME_ON)) snprintf(value, capacity, "—");
    else if (!s->on) snprintf(value, capacity, "Off");
    else if (a->kind == ORB_HOME_LIGHT && (s->present & ORB_HOME_BRIGHTNESS))
        snprintf(value, capacity, "%.0f%%", (double)s->brightness);
    else if (a->kind == ORB_HOME_FAN && (s->present & ORB_HOME_SPEED))
        snprintf(value, capacity, "%.0f%%", (double)s->speed);
    else snprintf(value, capacity, "On");
}

static const char *room_value_template(const orb_home_view_t *v, const orb_home_accessory_t *a)
{
    /* Numeric patches share a fixed rail. A semantic state change may reserve
     * a new lane once while idle, without making every percentage pay for the
     * widest outage wording or truncating an otherwise complete device name. */
    orb_home_status_t status = orb_home_status_accessory(v->model, a, v->failure_active);
    if (status != ORB_HOME_STATUS_READY && orb_home_status_home(v->model) == ORB_HOME_STATUS_READY)
        return orb_home_status_text(status);
    if (a->kind == ORB_HOME_LOCK) return "Unlocked";
    if (a->kind == ORB_HOME_THERMOSTAT || ((a->state.present & ORB_HOME_TEMP) &&
        !(a->state.present & ORB_HOME_HUMIDITY_VALUE))) return ORB_HOME_TEMPERATURE_TEMPLATE;
    if ((a->kind == ORB_HOME_LIGHT && (a->state.present & ORB_HOME_BRIGHTNESS)) ||
        (a->kind == ORB_HOME_FAN && (a->state.present & ORB_HOME_SPEED)) ||
        (a->kind == ORB_HOME_COVER && (a->state.present & ORB_HOME_POSITION)) ||
        (a->state.present & ORB_HOME_HUMIDITY_VALUE)) return "100%";
    if (a->state.present & ORB_HOME_MOTION_VALUE) return "Motion";
    if (a->state.present & ORB_HOME_CONTACT_VALUE) return "Closed";
    return "Off";
}

static void room_action_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (v->fixture || orb_home_ui_dragged(v)) return;
    if ((uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e))) {
        if (!orb_input_require_physical(v->root) ||
            !orb_home_command(&v->commands, &(orb_home_command_t){.kind = ORB_HOME_COMMAND_ROOM_OFF, .id = v->room})) return;
        orb_home_view_poll(v);
    } else { v->sections = false; orb_home_ui_render(v); }
}

void orb_home_room_actions_build(orb_home_view_t *v)
{
    orb_home_ui_header(v, room_name(v));
    orb_home_ui_text(v->current.page, "Lights, switches & outlets", ORB_TEXT_BODY, false, 233, 170, 320, 2, SECONDARY);
    lv_obj_t *action = orb_home_ui_button(v, "All off", 233, 380, true, room_action_event, 1);
    if (!orb_home_ui_ready(v->model) || v->model->room_off_pending || v->model->scene_pending)
        lv_obj_add_state(action, LV_STATE_DISABLED);
    bool failed = !strcmp(v->model->room_off_id, v->room) && !v->model->room_off_pending && v->model->room_off_failed;
    const char *status = !strcmp(v->model->room_off_id, v->room) ?
        v->model->room_off_pending ? "Turning off…" : failed ? ORB_ICON_WARNING_FILL " Couldn’t turn off some accessories" :
        "All off requested" : "";
    lv_obj_t *label = orb_home_ui_text(v->current.page, status, ORB_TEXT_FOOTNOTE2, false, 233, 280, 320, 2, failed ? ORB_RED : SECONDARY);
    orb_home_ui_wait(label, status, &v->wait);
    bool done = !strcmp(v->model->room_off_id, v->room) && !v->model->room_off_pending && !failed;
    lv_point_t size;
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    lv_text_get_size(&size, status, font, lv_obj_get_style_text_letter_space(label, 0), 0, 320, LV_TEXT_FLAG_NONE);
    /* Keep the real outcome beside its status, inside the same wide chord;
     * AX3's two-line explanation occupies the space above this lane. */
    orb_home_ui_check(v->current.page, &v->current.outcome, done, v->outcome_changed,
        233 - size.x / 2 - 38 - 14, 280 + ((int)font->line_height - 34) / 2);
    v->outcome_changed = false;
}

/* Room header: "Last known · Offline" while stale, else the room's count. */
static bool room_header(const orb_home_view_t *v, unsigned on_count, char *status, size_t capacity)
{
    if (orb_home_ui_stale_header(v, status, capacity)[0]) return false;
    if (v->model->room_off_pending && !strcmp(v->model->room_off_id, v->room)) snprintf(status, capacity, "Turning off…");
    else snprintf(status, capacity, "%u on", on_count);
    return on_count > 0;
}

void orb_home_room_build(orb_home_view_t *v)
{
    unsigned matches[ORB_HOME_MAX_ACCESSORIES], count = 0, on_count = 0;
    for (unsigned i = 0; i < v->model->accessory_count; i++) {
        const orb_home_accessory_t *a = &v->model->accessories[i];
        if (strcmp(a->room, v->room)) continue;
        matches[count++] = i;
        on_count += orb_home_summary_accessory(a) && (a->state.present & ORB_HOME_ON) && a->state.on;
    }
    for (unsigned i = 1; i < count; i++) {
        unsigned item = matches[i], j = i;
        while (j && room_acc_rank(&v->model->accessories[matches[j-1]]) >
                    room_acc_rank(&v->model->accessories[item])) {
            matches[j] = matches[j-1]; j--;
        }
        matches[j] = item;
    }
    lv_obj_t *heading = orb_home_ui_header(v, room_name(v));
    char status[64];
    bool lit = room_header(v, on_count, status, sizeof(status));
    v->current.room_status = orb_home_ui_text(v->current.page, status, ORB_TEXT_FOOTNOTE2, false, 233, 70, 300, 1,
         lit ? ORB_KIT_HOME : SECONDARY);
    orb_kit_value_set_rolling(v->current.room_status, status, "64 on");
    orb_home_ui_wait(v->current.room_status, status, &v->wait);
    /* Long rim names occupy a deeper chord. Keep the viewport below the
     * public title bounds so its opaque list plane cannot erase the ends. */
    lv_obj_update_layout(heading);
    int top = LV_MAX(110, lv_obj_get_y(heading) + lv_obj_get_height(heading) + 12);
    lv_obj_t *list = orb_home_ui_list(v, top, 420);
    v->current.room_devices = true;
    snprintf(v->current.room_id, sizeof(v->current.room_id), "%s", v->room);
    if (!count) orb_home_ui_state(v, ORB_ICON_HOUSE, "No accessories", "Add accessories in the Home app.");
    for (unsigned slot = 0; slot < count; slot++) {
        const orb_home_accessory_t *a = &v->model->accessories[matches[slot]];
        char value[48]; room_state(v, a, value, sizeof(value));
        /* The row's disc is the detail's identity disc at row size. */
        lv_color_t fill = orb_home_disc_colour(a, orb_home_ui_ready(v->model));
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
            .title = orb_home_accessory_name(v->model, a, true), .value = value,
            .glyph = orb_home_accessory_icon(a), .accent = fill});
        /* Numeric/state changes stay above the row cache. One reservation
         * admits future values without re-typesetting a source on each patch. */
        orb_kit_row_set_live_value(row, value, room_value_template(v, a));
        orb_kit_row_set_live_accent(row, fill, orb_kit_accent_ink(fill));
        orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_VALUE, !strcmp(value, "Updating…"),
            orb_home_accessory_wait_start(v, a->id));
        lv_obj_set_user_data(row, (void *)a->id);
        lv_obj_set_event_bubble(row, true);
        lv_obj_set_scroll_on_focus(row, false);
        lv_obj_add_event_cb(row, orb_home_ui_guard, LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(row, orb_home_ui_guard, LV_EVENT_LONG_PRESSED, v);
        lv_obj_add_event_cb(row, room_row_click, LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(row, room_row_click, LV_EVENT_LONG_PRESSED, v);
    }
    /* A named row, not a tappable rim title. It opens the room's All off page,
     * so reaching it never changes an accessory (HIG §§1/7). */
    bool switchable = false;
    for (unsigned slot = 0; slot < count; slot++)
        switchable |= orb_home_summary_accessory(&v->model->accessories[matches[slot]]);
    if (switchable) {
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = "All off",
            .glyph = ORB_ICON_POWER, .accent = ORB_GRAY4});
        lv_obj_set_scroll_on_focus(row, false);
        if (!v->fixture) {
            lv_obj_add_event_cb(row, orb_home_ui_guard, LV_EVENT_CLICKED, v);
            lv_obj_add_event_cb(row, room_actions_event, LV_EVENT_CLICKED, v);
        }
    }
    orb_home_ui_list_finish(list, v->room_scroll);
}

void orb_home_room_values_refresh(orb_home_view_t *v)
{
    if (!v->current.room_devices || !v->current.room_list) return;
    unsigned on_count = 0;
    for (unsigned i = 0; i < v->model->accessory_count; i++) {
        const orb_home_accessory_t *a = &v->model->accessories[i];
        if (!strcmp(a->room, v->room))
            on_count += orb_home_summary_accessory(a) && (a->state.present & ORB_HOME_ON) && a->state.on;
    }
    char status[64];
    bool lit = room_header(v, on_count, status, sizeof(status));
    if (v->current.room_status) {
        if (strcmp(lv_label_get_text(v->current.room_status), status))
            orb_kit_motion_finish(v->current.room_status);
        lv_obj_set_style_text_color(v->current.room_status, lit ? ORB_KIT_HOME : SECONDARY, 0);
        /* Active motion owns native ink opacity. Configure its resting ink
         * before Roll starts; unchanged waits retain their current breath. */
        if (strcmp(status, "Turning off…") || !v->wait.kind)
            lv_obj_set_style_text_opa(v->current.room_status, lit ? LV_OPA_COVER : ORB_KIT_SECONDARY_OPA, 0);
    }
    orb_kit_value_set_rolling(v->current.room_status, status, "64 on");
    orb_home_ui_wait(v->current.room_status, status, &v->wait);
    for (unsigned i = 0; i < lv_obj_get_child_count(v->current.room_list); i++) {
        lv_obj_t *row = lv_obj_get_child(v->current.room_list, i);
        if (lv_obj_is_floating(row)) continue;
        const char *id = lv_obj_get_user_data(row);
        const orb_home_accessory_t *a = id ? orb_home_find(v->model, id) : NULL;
        if (!a) continue;
        char value[48]; room_state(v, a, value, sizeof(value));
        orb_kit_row_set_live_text_rolling(row, ORB_KIT_ROW_TEXT_VALUE, value, room_value_template(v, a));
        lv_color_t fill = orb_home_disc_colour(a, orb_home_ui_ready(v->model));
        orb_kit_row_set_live_accent_animated(row, fill, orb_kit_accent_ink(fill));
        orb_kit_row_set_live_glyph(row, orb_home_accessory_icon(a));
        orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_VALUE, !strcmp(value, "Updating…"),
            orb_home_accessory_wait_start(v, a->id));
    }
}

static bool climate_content(const orb_home_snapshot_t *model)
{
    for (unsigned i = 0; i < model->accessory_count; i++) {
        const orb_home_accessory_t *a = &model->accessories[i];
        if (a->kind == ORB_HOME_THERMOSTAT || a->kind == ORB_HOME_TEMPERATURE ||
            a->kind == ORB_HOME_HUMIDITY || a->kind == ORB_HOME_SENSOR ||
            a->kind == ORB_HOME_MOTION || a->kind == ORB_HOME_CONTACT ||
            (a->state.present & (ORB_HOME_TEMP | ORB_HOME_HUMIDITY_VALUE))) return true;
    }
    return false;
}

static bool scenes_content(const orb_home_snapshot_t *model)
{
    for (unsigned i = 0; i < model->scene_count; i++)
        if (!model->scenes[i].contains_unlock) return true;
    return false;
}

uint64_t orb_home_rooms_topology(const orb_home_snapshot_t *model)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned r = 0; r < model->room_count; r++) {
        hash = hash_text(hash_text(hash, model->rooms[r].id), model->rooms[r].name);
    }
    return hash ^ climate_content(model) ^ ((uint64_t)scenes_content(model) << 1);
}

enum { ROOMS_LIST_TOP = 66, ROOMS_LIST_BOTTOM = 440, ROOMS_REST_TOP_MIN = 104 };

void orb_home_rooms_summary(orb_home_view_t *v)
{
    if (!v->current.landing || !v->current.room_list) return;
    for (unsigned r = 0; r < v->model->room_count; r++) {
        lv_obj_t *row = v->current.room_rows[r];
        if (!row) continue;
        const char *id = v->model->rooms[r].id;
        bool pending = v->model->room_off_pending && !strcmp(v->model->room_off_id, id);
        bool failed = v->failure_active && !strcmp(v->model->error_id, id);
        unsigned lights = 0; bool mixed = false, has_temp = false; float temp = 0;
        for (unsigned i = 0; i < v->model->accessory_count; i++) {
            const orb_home_accessory_t *a = &v->model->accessories[i];
            if (strcmp(a->room, id)) continue;
            pending |= a->pending;
            failed |= v->failure_active && v->model->error[0] && !strcmp(v->model->error_id, a->id);
            if (orb_home_summary_accessory(a)) {
                lights += (a->state.present & ORB_HOME_ON) && a->state.on;
                mixed |= a->kind != ORB_HOME_LIGHT;
            }
            if (a->state.present & ORB_HOME_TEMP) { has_temp = true; temp = a->state.temp; }
        }
        char state[64];
        if (failed) snprintf(state, sizeof(state), "%s", orb_home_status_text(ORB_HOME_STATUS_FAILED));
        else if (pending) snprintf(state, sizeof(state), "%s", orb_home_status_text(ORB_HOME_STATUS_UPDATING));
        else if (!lights) snprintf(state, sizeof(state), "Off");
        else snprintf(state, sizeof(state), "%u %s%s on", lights,
            mixed ? "device" : "light", lights == 1 ? "" : "s");
        /* HIG §§3/4: the name owns the row. The temperature follows the state,
         * so a narrow shoulder row shortens this least important word first. */
        if (has_temp && !failed && !pending) {
            char temperature[24];
            orb_home_format_temperature(temperature, sizeof(temperature), temp, v->temperature, false);
            size_t used = strlen(state);
            snprintf(state + used, sizeof(state) - used, " · %s", temperature);
        }
        if (v->rebuilding) orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_SUBTITLE, state, "64 devices on · −188°");
        else orb_kit_row_set_live_text_rolling(row, ORB_KIT_ROW_TEXT_SUBTITLE, state, "64 devices on · −188°");
        orb_kit_row_set_waiting_since(row, ORB_KIT_ROW_TEXT_SUBTITLE, pending && !failed,
            orb_home_room_wait_start(v, id));
        orb_kit_row_set_text_colour(row, ORB_KIT_ROW_TEXT_SUBTITLE, failed ? ORB_RED : SECONDARY);
        if (v->rebuilding) orb_kit_row_set_live_accent(row, lights ? ORB_KIT_HOME : ORB_GRAY4,
            lights ? lv_color_black() : ORB_LABEL);
        else orb_kit_row_set_live_accent_animated(row, lights ? ORB_KIT_HOME : ORB_GRAY4,
            lights ? lv_color_black() : ORB_LABEL);
    }
    if (v->current.climate_value) {
        char temperature[24] = "";
        for (unsigned i = 0; i < v->model->accessory_count; i++) {
            const orb_home_accessory_t *a = &v->model->accessories[i];
            if (a->kind != ORB_HOME_THERMOSTAT) continue;
            if (a->state.present & ORB_HOME_TEMP)
                orb_home_format_temperature(temperature, sizeof(temperature), a->state.temp, v->temperature, false);
            break;
        }
        orb_kit_row_set_live_value(v->current.climate_value, temperature, ORB_HOME_TEMPERATURE_TEMPLATE);
    }
}

static lv_obj_t *landing_row(orb_home_view_t *v, lv_obj_t *list, const orb_kit_row_config_t *config,
                             const char *semantic, lv_event_cb_t event)
{
    lv_obj_t *row = orb_kit_row_create(list, config);
    lv_obj_set_user_data(row, (void *)semantic);
    lv_obj_set_scroll_on_focus(row, false);
    if (!v->fixture) {
        lv_obj_add_event_cb(row, orb_home_ui_guard, LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(row, event, LV_EVENT_CLICKED, v);
    }
    return row;
}

void orb_home_rooms_build(orb_home_view_t *v)
{
    orb_home_ui_header(v, "Home");
    if (!orb_home_ui_ready(v->model) || !v->model->room_count) {
        orb_home_status_t status = orb_home_status_home(v->model);
        if (status == ORB_HOME_STATUS_OFFLINE)
            orb_home_ui_state(v, ORB_ICON_DESKTOP, orb_home_status_text(status), "Connect your Mac to load Home.");
        else if (status == ORB_HOME_STATUS_LOADING) orb_home_ui_state(v, ORB_ICON_HOUSE, orb_home_status_text(status), NULL);
        else if (status == ORB_HOME_STATUS_UNAVAILABLE)
            orb_home_ui_state(v, ORB_ICON_HOUSE, orb_home_status_text(status), "Your Mac can't reach Home right now.");
        else orb_home_ui_state(v, ORB_ICON_HOUSE, "No rooms", "Add rooms in the Home app.");
        return;
    }
    lv_obj_t *list = orb_home_ui_list(v, ROOMS_LIST_TOP, ROOMS_LIST_BOTTOM);
    lv_obj_set_user_data(list, "rooms:list");
    v->current.landing = true;
    for (unsigned i = 0; i < v->model->room_count; i++) {
        char name[33];
        snprintf(name, sizeof(name), "%s", v->model->rooms[i].name[0] ? v->model->rooms[i].name : "Unnamed room");
        for (char *c = name; *c; c++)
            if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7f) *c = ' ';
        snprintf(v->room_semantic[i], sizeof(v->room_semantic[i]), "room:%s", v->model->rooms[i].id);
        lv_obj_t *row = landing_row(v, list, &(orb_kit_row_config_t){.title = name,
            .subtitle = "Off", .glyph = orb_home_room_icon(name), .accent = ORB_GRAY4}, v->room_semantic[i], room_event);
        /* HIG §§3/4/8: names wrap; prepared live lanes keep a light's state
         * change from typesetting or restacking any room. */
        orb_kit_row_show_full_title(row, ORB_TEXT_REGULAR);
        orb_kit_row_set_live_text(row, ORB_KIT_ROW_TEXT_SUBTITLE, "Off", "64 devices on · −188°");
        orb_kit_row_set_live_accent(row, ORB_GRAY4, ORB_LABEL);
        v->current.room_rows[i] = row;
    }
    lv_obj_t *last = v->current.room_rows[v->model->room_count - 1];
    if (climate_content(v->model)) {
        orb_home_accessory_t identity = {.kind = ORB_HOME_TEMPERATURE};
        v->current.climate_value = landing_row(v, list, &(orb_kit_row_config_t){.title = "Climate",
            .value = "", .glyph = orb_home_accessory_icon(&identity), .accent = ORB_KIT_HOME}, "home:climate", climate_event);
        orb_kit_row_set_live_value(v->current.climate_value, "", ORB_HOME_TEMPERATURE_TEMPLATE);
        last = v->current.climate_value;
    }
    if (scenes_content(v->model))
        last = landing_row(v, list, &(orb_kit_row_config_t){.title = "Scenes", .glyph = ORB_ICON_SPARKLE,
            .accent = ORB_KIT_HOME}, "scenes:open", orb_home_pages_scenes_event);
    /* Every name can reach the equator, including the first and last. HIG
     * §§3/4: shoulder previews may shorten; the reading position may not. */
    lv_obj_update_layout(list);
    lv_obj_t *first = v->current.room_rows[0];
    int height = lv_obj_get_height(first);
    int pad = LV_MAX(0, 233 - ROOMS_LIST_TOP - height / 2);
    lv_obj_set_style_pad_top(list, pad, 0);
    lv_obj_set_style_pad_bottom(list, LV_MAX(0, ROOMS_LIST_BOTTOM - 233 - lv_obj_get_height(last) / 2), 0);
    orb_home_ui_list_finish(list, LV_MAX(0, v->rooms_scroll));
    /* A fresh list rests with the rooms filling the circle from under the
     * title, three whole rows at default size, rather than one centred row
     * below an empty band. Rows have their final places only after the kit
     * has stacked them. */
    if (v->model->room_count > 1) {
        int pitch = lv_obj_get_y(v->current.room_rows[1]) - lv_obj_get_y(first);
        int rest_top = LV_MAX(ROOMS_REST_TOP_MIN, 233 - height / 2 - pitch);
        v->current.rooms_rest = LV_MAX(0, ROOMS_LIST_TOP + pad - rest_top);
        if (!v->rooms_scroll) {
            lv_obj_scroll_to_y(list, v->current.rooms_rest, LV_ANIM_OFF);
            orb_round_list_update(list);
            lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
        }
    }
    orb_home_rooms_summary(v);
}
