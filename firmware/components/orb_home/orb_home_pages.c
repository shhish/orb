/* Home's list pages: Scenes, Climate and Sensors. Each
 * is content rows (never glass), uses the one Home vocabulary, and empties
 * into the shared state composition (HIG §§1/2/4/7/8). */
#include "orb_home_view_private.h"
#include "orb_kit_pages.h"
#include "orb_input.h"
#include "orb_sound.h"
#include "orb_icons.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *orb_home_ui_stale_header(const orb_home_view_t *v, char *out, size_t capacity)
{
    orb_home_status_t status = orb_home_status_home(v->model);
    if (status == ORB_HOME_STATUS_READY) out[0] = 0;
    else snprintf(out, capacity, ORB_HOME_TEXT_LAST_KNOWN " · %s", orb_home_status_text(status));
    return out;
}

lv_obj_t *orb_home_ui_state(orb_home_view_t *v, const char *glyph, const char *title, const char *detail)
{
    lv_obj_t *state = orb_kit_state_create(v->current.page, &(orb_kit_state_config_t){
        .glyph = glyph, .title = title, .detail = detail,
        .secondary = SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA});
    orb_home_ui_state_wait(state, title, &v->wait);
    return state;
}

static lv_obj_t *page_row(orb_home_view_t *v, lv_obj_t *list, const orb_kit_row_config_t *config,
                          lv_event_cb_t cb, uintptr_t data)
{
    lv_obj_t *row = orb_kit_row_create(list, config);
    lv_obj_set_user_data(row, (void *)data);
    lv_obj_set_event_bubble(row, true);
    lv_obj_set_scroll_on_focus(row, false);
    if (cb && !v->fixture) {
        lv_obj_add_event_cb(row, orb_home_ui_guard, LV_EVENT_CLICKED, v);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, v);
    } else if (!cb) lv_obj_set_clickable(row, false);
    return row;
}

/* A status lane under the rim title, never over the first row. */
static void status_lane(orb_home_view_t *v, const char *text, bool error)
{
    lv_obj_t *o = orb_home_ui_text(v->current.page, text, ORB_TEXT_FOOTNOTE2, false,
        233, 70, 300, 1, error ? ORB_RED : SECONDARY);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    orb_home_ui_wait(o, text, &v->wait);
}

void orb_home_pages_scenes_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (orb_home_ui_dragged(v) || orb_home_view_moving(v)) return;
    const char *want = v->hour < 12 ? "Good morning" : v->hour >= 22 ? "Good Night" : v->hour >= 17 ? "Evening" : "I'm home";
    unsigned safe = 0;
    for (unsigned i = 0; i < v->model->scene_count; i++) {
        if (v->model->scenes[i].contains_unlock) continue;
        if (!strcasecmp(v->model->scenes[i].name, want)) v->scene_index = safe;
        safe++;
    }
    v->sections = false; v->scene_page = true; orb_sound_play(ORB_SOUND_OPEN); orb_home_ui_render(v);
}

static void scene_event(lv_event_t *e)
{
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (orb_home_view_moving(v) || v->fixture) return;
    unsigned i = (unsigned)(uintptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (!orb_input_require_physical(v->root)) return;
    if (i < v->model->scene_count && orb_home_command(&v->commands, &(orb_home_command_t){
            .kind = ORB_HOME_COMMAND_SCENE, .id = v->model->scenes[i].id}))
        orb_home_view_poll(v);
}

/* The failed scene's own row carries the one error glyph and colour, so
 * the short status lane names its target by association (HIG §§3/8). A
 * trailing outcome word would truncate the scene's name instead. */
static bool scene_failed(const orb_home_view_t *v, const orb_home_scene_t *scene)
{
    return orb_home_ui_ready(v->model) && v->failure_active && v->model->error[0] &&
        !strcmp(v->model->error_id, scene->id);
}

void orb_home_pages_scenes(orb_home_view_t *v)
{
    unsigned count = 0;
    bool failed = false, busy = !strcmp(v->model->error, "Home is busy");
    /* The first row must clear the rim-preview band: a long scene can use
     * the kit's 21 px fallback, which cannot be scaled below 20 px. */
    lv_obj_t *list = orb_home_ui_list(v, 110, 412);
    v->current.scene_list = true;
    for (unsigned i = 0; i < v->model->scene_count; i++) {
        const orb_home_scene_t *scene = &v->model->scenes[i];
        if (scene->contains_unlock) continue;
        bool error = scene_failed(v, scene);
        failed |= error;
        lv_obj_t *row = page_row(v, list, &(orb_kit_row_config_t){.title = scene->name,
            .glyph = error ? ORB_ICON_WARNING_FILL : ORB_ICON_SPARKLE, .accent = error ? ORB_RED : ORB_KIT_HOME},
            scene_event, i);
        if (!orb_home_ui_ready(v->model) || v->model->scene_pending || v->model->room_off_pending)
            lv_obj_add_state(row, LV_STATE_DISABLED);
        count++;
    }
    orb_home_ui_list_finish(list, v->scene_scroll);
    if (!count) {
        orb_home_ui_state(v, ORB_ICON_SPARKLE, "No scenes", "Add scenes in the Home app.");
        return;
    }
    char status[64];
    orb_home_ui_stale_header(v, status, sizeof(status));
    if (status[0]) failed = false;
    else if (v->model->scene_pending) snprintf(status, sizeof(status), "Running\u2026");
    else if (failed) snprintf(status, sizeof(status), ORB_ICON_WARNING_FILL " %s", busy ? "Home is busy" : "Couldn’t run");
    else if (v->scene_feedback_at) snprintf(status, sizeof(status), "Requested");
    status_lane(v, status, failed);
    orb_home_ui_check(v->current.page, &v->current.outcome, !strcmp(status, "Requested"),
        v->outcome_changed, 122, 65);
    v->outcome_changed = false;
}

static const orb_home_accessory_t *climate_thermostat(const orb_home_view_t *v)
{
    for (unsigned i = 0; i < v->model->accessory_count; i++)
        if (v->model->accessories[i].kind == ORB_HOME_THERMOSTAT) return &v->model->accessories[i];
    return NULL;
}

static unsigned sensor_rows(const orb_home_view_t *v)
{
    unsigned n = 0;
    for (unsigned r = 0; r < v->model->room_count; r++) {
        uint32_t readings = 0;
        for (unsigned i = 0; i < v->model->accessory_count; i++)
            if (!strcmp(v->model->accessories[i].room, v->model->rooms[r].id))
                readings |= v->model->accessories[i].state.present & (ORB_HOME_TEMP | ORB_HOME_HUMIDITY_VALUE);
        n += !!(readings & ORB_HOME_TEMP) + !!(readings & ORB_HOME_HUMIDITY_VALUE);
    }
    return n;
}

static void thermostat_event(lv_event_t *e)
{
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (orb_home_view_moving(v) || v->fixture) return;
    const orb_home_accessory_t *t = climate_thermostat(v);
    if (t) orb_home_view_open(v, t->id);
}

static void sensors_event(lv_event_t *e)
{
    orb_home_view_t *v = lv_event_get_user_data(e);
    if (orb_home_ui_dragged(v)) return;
    v->sensors = true; orb_sound_play(ORB_SOUND_OPEN); orb_home_ui_render(v);
}

static void sensor_list(orb_home_view_t *v)
{
    /* Quantity + room rows sit on the wide central chords (AX3 names). */
    lv_obj_t *list = orb_home_ui_list(v, 146, 420);
    v->current.sensor_list = true;
    for (unsigned r = 0; r < v->model->room_count; r++) {
        uint32_t readings = 0;
        for (unsigned i = 0; i < v->model->accessory_count; i++) {
            const orb_home_accessory_t *a = &v->model->accessories[i];
            if (!strcmp(a->room, v->model->rooms[r].id))
                readings |= a->state.present & (ORB_HOME_TEMP | ORB_HOME_HUMIDITY_VALUE);
        }
        /* Rows name their quantity, with the room beneath: two "Living
         * Room" rows read as a duplicate (HIG §1). One unit per trailing rail. */
        for (unsigned kind = 0; kind < 2; kind++) {
            if (!(readings & (kind ? ORB_HOME_HUMIDITY_VALUE : ORB_HOME_TEMP))) continue;
            orb_home_accessory_t identity = {.kind = kind ? ORB_HOME_HUMIDITY : ORB_HOME_TEMPERATURE};
            page_row(v, list, &(orb_kit_row_config_t){
                .title = orb_home_kind_name(identity.kind), .subtitle = v->model->rooms[r].name,
                .value = "—", .glyph = orb_home_accessory_icon(&identity), .accent = ORB_GRAY4}, NULL, r * 2 + kind + 1);
        }
    }
    orb_home_ui_list_finish(list, v->sensor_scroll);
    char stale[48];
    if (!sensor_rows(v)) orb_home_ui_state(v, ORB_ICON_SENSOR, "No sensors", "Temperature and humidity sensors appear here.");
    else status_lane(v, orb_home_ui_stale_header(v, stale, sizeof(stale)), false);
}

void orb_home_pages_climate(orb_home_view_t *v)
{
    if (v->sensors) { sensor_list(v); return; }
    const orb_home_accessory_t *t = climate_thermostat(v);
    unsigned sensors = sensor_rows(v);
    if (!t && !sensors) {
        orb_home_ui_state(v, ORB_ICON_SLIDERS, "No climate accessories",
            "Add a thermostat or sensors in the Home app.");
        return;
    }
    /* Equal chord-fitted rows replace unequal stacked capsules (HIG §7).
     * Two rows sit on the wide central chords, so AX3 names stay whole. */
    lv_obj_t *list = orb_home_ui_list(v, 146, 420);
    if (t) {
        lv_color_t fill = orb_home_disc_colour(t, orb_home_ui_ready(v->model));
        v->current.climate_value = page_row(v, list, &(orb_kit_row_config_t){
            .title = t->name[0] ? t->name : "Thermostat", .subtitle = "—", .value = "—",
            .glyph = orb_home_accessory_icon(t), .accent = fill}, thermostat_event, 0);
    }
    if (sensors) {
        char count[12]; snprintf(count, sizeof(count), "%u", sensors);
        orb_home_accessory_t identity = {.kind = ORB_HOME_HUMIDITY};
        v->current.climate_target = page_row(v, list, &(orb_kit_row_config_t){
            .title = "Sensors", .value = count, .glyph = orb_home_accessory_icon(&identity), .accent = ORB_GRAY4}, sensors_event, 0);
    }
    orb_home_ui_list_finish(list, 0);
    char stale[48];
    status_lane(v, orb_home_ui_stale_header(v, stale, sizeof(stale)), false);
}

void orb_home_pages_readouts(orb_home_view_t *v)
{
    bool ready = orb_home_ui_ready(v->model);
    for (unsigned r = 0; r < v->model->room_count && v->current.sensor_list && v->current.room_list; r++) {
        const char *room = v->model->rooms[r].id;
        const orb_home_accessory_t *temp = NULL, *humidity = NULL;
        for (unsigned i = 0; i < v->model->accessory_count; i++) {
            const orb_home_accessory_t *a = &v->model->accessories[i];
            if (strcmp(a->room, room)) continue;
            /* Prefer a reachable reported source; never label a cached value fresh. */
            if ((a->state.present & ORB_HOME_TEMP) && (!temp || !temp->reachable)) temp = a;
            if ((a->state.present & ORB_HOME_HUMIDITY_VALUE) && (!humidity || !humidity->reachable)) humidity = a;
        }
        for (unsigned i = 0; i < lv_obj_get_child_count(v->current.room_list); i++) {
            lv_obj_t *row = lv_obj_get_child(v->current.room_list, i);
            uintptr_t semantic = (uintptr_t)lv_obj_get_user_data(row);
            if (!semantic || (semantic - 1) / 2 != r) continue;
            bool humidity_row = (semantic - 1) % 2 != 0;
            char value[32];
            const orb_home_accessory_t *source = humidity_row ? humidity : temp;
            /* Offline keeps the dimmed last value (the header says so once);
             * an unreachable sensor has no current value to show. */
            if (source && ready && !source->reachable) snprintf(value, sizeof(value), "%s", orb_home_status_text(ORB_HOME_STATUS_NO_RESPONSE));
            else if (source && humidity_row) snprintf(value, sizeof(value), "%.0f%%", (double)source->state.humidity);
            else if (source) orb_home_format_temperature(value, sizeof(value), source->state.temp, v->temperature, false);
            else snprintf(value, sizeof(value), "—");
            orb_kit_row_set_live_value(row, value, source && ready && !source->reachable ? value :
                humidity_row ? "100%" : ORB_HOME_TEMPERATURE_TEMPLATE);
        }
    }
    if (v->current.surface != ORB_HOME_CLIMATE || v->sensors || !v->current.climate_value) return;
    const orb_home_accessory_t *t = climate_thermostat(v);
    if (!t) return;
    char current[32], target[64], degrees[24];
    orb_home_status_t status = orb_home_status_accessory(v->model, t, false);
    if (t->state.present & ORB_HOME_TEMP) orb_home_format_temperature(current, sizeof(current), t->state.temp, v->temperature, false);
    else snprintf(current, sizeof(current), "—");
    orb_home_format_temperature(degrees, sizeof(degrees), t->state.target, v->temperature, true);
    if (status == ORB_HOME_STATUS_NO_RESPONSE) snprintf(target, sizeof(target), "%s", orb_home_status_text(status));
    else if (!(t->state.present & ORB_HOME_MODE)) snprintf(target, sizeof(target), "Mode unknown");
    else if (t->state.mode == ORB_HOME_MODE_OFF) snprintf(target, sizeof(target), "Off");
    else if (!(t->state.present & ORB_HOME_TARGET)) snprintf(target, sizeof(target), "%s", t->state.mode == ORB_HOME_MODE_AUTO ? "Auto" : t->state.mode == ORB_HOME_MODE_COOL ? "Cooling" : "Heating");
    else snprintf(target, sizeof(target), "%s to %s", t->state.mode == ORB_HOME_MODE_COOL ? "Cooling" : t->state.mode == ORB_HOME_MODE_AUTO ? "Auto" : "Heating", degrees);
    /* An indoor reading needs three figures; the full sensor template would
     * cost the thermostat's name its AX3 lane. Rarer values re-reserve once. */
    orb_kit_row_set_live_value(v->current.climate_value, current, strlen(current) > strlen("100°") ? current : "100°");
    /* Mode words change rarely: a static relayout keeps one clean lane. */
    orb_kit_row_set_text(v->current.climate_value, ORB_KIT_ROW_TEXT_SUBTITLE, target);
}
