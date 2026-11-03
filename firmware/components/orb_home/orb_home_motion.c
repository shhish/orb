/* Alive presentation only (delight §6): the stable list and every hit target
 * retain their geometry. Only reported-on, visible room discs wake in order. */
#include "orb_home_view_private.h"
#include "orb_kit_motion.h"
#include <string.h>

bool orb_home_room_is_on(const orb_home_view_t *v, unsigned room)
{
    if (!v || room >= v->model->room_count) return false;
    for (unsigned i = 0; i < v->model->accessory_count; i++) {
        const orb_home_accessory_t *a = &v->model->accessories[i];
        if (!strcmp(a->room, v->model->rooms[room].id) && orb_home_summary_accessory(a) &&
            (a->state.present & ORB_HOME_ON) && a->state.on) return true;
    }
    return false;
}

static void room_final(orb_home_view_t *v, unsigned index, bool animated)
{
    lv_obj_t *row = v->current.room_rows[index];
    if (!row) return;
    bool on = orb_home_room_is_on(v, index);
    lv_color_t fill = on ? ORB_KIT_HOME : ORB_GRAY4;
    lv_color_t ink = on ? lv_color_black() : ORB_LABEL;
    if (animated) orb_kit_row_set_live_accent_animated(row, fill, ink);
    else orb_kit_row_set_live_accent(row, fill, ink);
}

static bool wake_tick(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    (void)dt;
    orb_home_view_t *v = context;
    unsigned ordinal = 0;
    uint16_t pending = v->arrival_rows & ~v->arrival_started;
    for (unsigned i = 0; i < v->model->room_count; i++) {
        if (!(v->arrival_rows & (1u << i))) continue;
        /* Ordinal counts all admitted rows, including ones already started;
         * the mask remains stable until the whole finite sequence completes. */
        uint32_t delay = ordinal++ * 45;
        lv_obj_t *row = v->current.room_rows[i];
        bool started = row && !(v->arrival_started & (1u << i)) && elapsed >= delay;
        if (finish || started) {
            /* Each admitted row starts once; later delay ticks cannot
             * restart its fill or its glyph pop. */
            room_final(v, i, !finish);
            pending &= ~(1u << i);
            v->arrival_started |= 1u << i;
        }
    }
    if (!pending || finish) {
        v->arrival_rows = 0;
        v->arrival_armed = false;
        return false;
    }
    return true;
}

static void arrived(void *context, bool animate)
{
    orb_home_view_t *v = context;
    if (!v->arrival_armed) return;
    if (!animate) { wake_tick(v, 0, 0, true); return; }
    if (wake_tick(v, 0, 0, false))
        orb_kit_motion_start(v->current.page, &v->arrival_rows, wake_tick, v);
}

static uint16_t visible_on_rows(orb_home_view_t *v)
{
    lv_obj_update_layout(v->root);
    uint16_t rows = 0;
    for (unsigned i = 0; i < v->model->room_count; i++) {
        lv_obj_t *row = v->current.room_rows[i];
        lv_area_t area;
        if (!row || !orb_home_room_is_on(v, i) || !orb_round_list_row_bounds(row, &area) ||
            !lv_obj_area_is_visible(row, &area)) continue;
        rows |= 1u << i;
    }
    return rows;
}

void orb_home_view_arrival_snapshot(orb_home_view_t *v)
{
    if (!v || !v->current.landing || !v->current.room_list || !orb_home_ui_ready(v->model) ||
        orb_ui_reduce_motion()) return;
    /* A capture-only preview stays suspended: prepare its pixels without
     * admitting a timer or changing its lifecycle gate. */
    uint16_t rows = visible_on_rows(v);
    for (unsigned i = 0; i < v->model->room_count; i++)
        if (rows & (1u << i))
            orb_kit_row_set_live_accent(v->current.room_rows[i], ORB_GRAY4, ORB_LABEL);
}

void orb_home_view_arrival_prepare(orb_home_view_t *v)
{
    if (!v || !v->current.landing || !v->current.room_list || !orb_home_ui_ready(v->model)) return;
    orb_home_motion_cancel(v);
    orb_kit_scroll_arc_arrival_prepare(v->current.room_list);
    v->arrival_rows = visible_on_rows(v);
    v->arrival_scroll = lv_obj_get_scroll_y(v->current.room_list);
    v->arrival_started = 0;
    if (!v->arrival_rows) return;
    v->arrival_armed = true;
    if (!orb_kit_arrival_prepare(v->current.page, arrived, v)) {
        arrived(v, false);
        return;
    }
    for (unsigned i = 0; i < v->model->room_count; i++)
        if (v->arrival_rows & (1u << i))
            orb_kit_row_set_live_accent(v->current.room_rows[i], ORB_GRAY4, ORB_LABEL);
}

void orb_home_view_arrival_complete(orb_home_view_t *v, bool fresh)
{
    if (v) orb_kit_arrival_complete(v->root, fresh);
}

void orb_home_motion_cancel(orb_home_view_t *v)
{
    if (!v) return;
    /* An arrival must never survive its first contact or a model rebuild.
     * This also finishes any fill that has started in the row overlay. */
    orb_kit_arrival_complete(v->root, false);
    orb_kit_motion_cancel(v->current.page, &v->arrival_rows, true);
    v->arrival_armed = false;
    v->arrival_rows = v->arrival_started = 0;
    for (unsigned i = 0; i < v->model->room_count; i++)
        if (v->current.room_rows[i]) orb_kit_row_live_accent_cancel(v->current.room_rows[i]);
}

bool orb_home_waiting(const char *text, orb_home_wait_t *wait)
{
    unsigned kind = !strcmp(text, "Loading") || !strcmp(text, "Loading…") ? 1 :
        !strcmp(text, "Turning off…") ? 2 : !strcmp(text, "Running…") ? 3 :
        !strcmp(text, "Updating…") ? 4 : 0;
    if (kind != wait->kind) { wait->kind = kind; wait->started = lv_tick_get(); }
    /* A rebuild may replace the word during the same pending request.
     * Keep its real wait budget instead of restarting 30 s. */
    return kind && lv_tick_elaps(wait->started) < 30000;
}

void orb_home_ui_wait(lv_obj_t *label, const char *text, orb_home_wait_t *wait)
{
    bool waiting = orb_home_waiting(text, wait);
    if (label) orb_kit_breathe_since(label, waiting, wait->started);
}

void orb_home_ui_state_wait(lv_obj_t *state, const char *text, orb_home_wait_t *wait)
{
    bool waiting = orb_home_waiting(text, wait);
    if (state) orb_kit_state_set_waiting_since(state, waiting, wait->started);
}

void orb_home_ui_check(lv_obj_t *parent, lv_obj_t **check, bool show, bool animate, int x, int y)
{
    if (!show) {
        if (*check) { lv_obj_delete(*check); *check = NULL; }
        return;
    }
    if (*check) return;
    *check = orb_kit_check_create(parent, ORB_GREEN, false);
    if (!*check) return;
    lv_obj_set_pos(*check, x, y);
    if (!animate) orb_kit_motion_finish(*check);
}

void orb_home_waits_sync(orb_home_view_t *v)
{
    /* The service admits four independent pending requests. A room batch
     * shares its one clock, including accessories still awaiting a slot. */
    for (unsigned i = 0; i < ORB_HOME_MAX_PENDING; i++) {
        orb_home_pending_wait_t *wait = &v->pending_waits[i];
        const orb_home_accessory_t *a = orb_home_find(v->model, wait->id);
        if (!a || !a->pending || (v->model->room_off_pending &&
            !strcmp(a->room, v->model->room_off_id))) wait->id[0] = 0;
    }
    for (unsigned a = 0; a < v->model->accessory_count; a++) {
        const orb_home_accessory_t *accessory = &v->model->accessories[a];
        if (!accessory->pending || (v->model->room_off_pending &&
            !strcmp(accessory->room, v->model->room_off_id))) continue;
        unsigned found = ORB_HOME_MAX_PENDING, empty = ORB_HOME_MAX_PENDING;
        for (unsigned i = 0; i < ORB_HOME_MAX_PENDING; i++) {
            if (!strcmp(v->pending_waits[i].id, accessory->id)) found = i;
            if (!v->pending_waits[i].id[0]) empty = i;
        }
        if (found == ORB_HOME_MAX_PENDING && empty != ORB_HOME_MAX_PENDING) {
            orb_home_pending_wait_t *wait = &v->pending_waits[empty];
            memcpy(wait->id, accessory->id, sizeof(wait->id));
            wait->started = lv_tick_get();
        }
    }
    bool room_pending = v->model->room_off_pending;
    if (room_pending && !v->room_off_wait.kind) v->room_off_wait.started = lv_tick_get();
    v->room_off_wait.kind = room_pending;
}

uint32_t orb_home_accessory_wait_start(const orb_home_view_t *v, const char *id)
{
    const orb_home_accessory_t *a = orb_home_find(v->model, id);
    if (a && v->model->room_off_pending && !strcmp(a->room, v->model->room_off_id))
        return v->room_off_wait.started;
    for (unsigned i = 0; i < ORB_HOME_MAX_PENDING; i++)
        if (!strcmp(v->pending_waits[i].id, id)) return v->pending_waits[i].started;
    return lv_tick_get();
}

uint32_t orb_home_room_wait_start(const orb_home_view_t *v, const char *id)
{
    if (v->model->room_off_pending && !strcmp(v->model->room_off_id, id))
        return v->room_off_wait.started;
    uint32_t now = lv_tick_get(), age = 0;
    for (unsigned i = 0; i < ORB_HOME_MAX_PENDING; i++) {
        const orb_home_pending_wait_t *wait = &v->pending_waits[i];
        const orb_home_accessory_t *a = orb_home_find(v->model, wait->id);
        if (a && !strcmp(a->room, id)) age = LV_MAX(age, now - wait->started);
    }
    return now - age;
}
