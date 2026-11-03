/* HIG §§1/3/4/7: Stack content stays matte, with only focused actions
 * using material. The shell retains routing and explicit refresh timing. */
#include "stack_screen.h"
#include "orb_shell.h"
#include "orb_feedback.h"
#include "orb_input.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_kit_pages.h"
#include "orb_kit_effects.h"
#include "orb_notification_history.h"
#include "orb_notifications_view.h"
#include "orb_page.h"
#include "orb_usage_copy.h"
#include "orb_agents_copy.h"
#include "orb_ui.h"
#include "orb_system_copy.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    ACT_PAGE_NEXT = 103, ACT_CARD = 160, ACT_HOME_OFF = 384, ACT_TIME = 480,
    ACT_STACK_SCENES = 489, ACT_MEDIA_PREVIOUS = 580, ACT_MEDIA_TOGGLE, ACT_MEDIA_NEXT,
};

typedef struct { uint32_t wait_started, sync_started; bool waiting, syncing; } stack_context_t;

typedef struct {
    orb_stack_source_t source;
    orb_screen_instance_t *instance;
    /* These read views exist only during composition or an explicit refresh.
     * The source callback always borrows this workspace's current COW generation. */
    const orb_model_snapshot_t *model;
    const orb_home_snapshot_t *home;
    orb_time_snapshot_t time;
    struct { const char *clock_time, *clock_date; } status;
    const char *notice, *card;
    unsigned page_index;
    int clock_hour;
    lv_obj_t *page, *heading, *list, *time_value, *time_status;
    lv_obj_t *stack_neighbors[2], *stack_hero, *stack_detail;
    bool rebuild, stack_surface, compact_time, waiting, syncing;
    uint32_t wait_started, sync_started;
    lv_obj_t *sync_caption;
} stack_state_t;

static void stack_neighbors_update(stack_state_t *w);
static bool ready(const stack_state_t *w) { return w->model->online && w->model->synced; }
static lv_color_t agent_color(stack_state_t *w, orb_agent_t agent)
{ (void)w; return agent == ORB_AGENT_CLAUDE ? ORB_KIT_CLAUDE : agent == ORB_AGENT_CODEX ? ORB_KIT_CODEX : ORB_KIT_DOT; }
static bool stack_date_known(const stack_state_t *w)
{ return w->status.clock_date[0] && strcmp(w->status.clock_date, "--") && strcmp(w->status.clock_date, "—"); }

orb_stack_read_t orb_stack_project(const orb_model_snapshot_t *model, const orb_home_snapshot_t *home,
    const orb_time_snapshot_t *time, const orb_shell_snapshot_t *status, const char *card,
    const char *notice, unsigned page_index, orb_shell_surface_t surface)
{
    unsigned hour;
    return (orb_stack_read_t){.model = model, .home = home, .time = time,
        .clock_time = status ? status->clock_time : "", .clock_date = status ? status->clock_date : "",
        .card = card, .notice = notice, .page_index = page_index,
        .stack_surface = surface == ORB_SHELL_STACK, .compact_time = surface == ORB_SHELL_FACE,
        .clock_hour = status && orb_system_clock_parts(status->clock_time, &hour, NULL) ? (int)hour : -1};
}

unsigned orb_stack_screen_entries(const orb_stack_read_t *read, orb_stack_entry_t *entries)
{
    orb_stack_entry_t scratch[ORB_STACK_MAX];
    orb_stack_entry_t *items = entries ? entries : scratch;
    unsigned count = orb_stack_build(read->model, read->time, read->home, read->clock_hour,
        read->clock_date[0] && strcmp(read->clock_date, "--") && strcmp(read->clock_date, "—"), items, ORB_STACK_MAX);
    bool home = false;
    for (unsigned i = 0; i < count; i++) home |= items[i].kind == ORB_STACK_HOME;
    /* Optimistic off must not remove the still-running batch's own status. */
    if (read->home->room_off_pending && !home && count < ORB_STACK_MAX) {
        unsigned slot = LV_MIN(read->page_index, count);
        memmove(items + slot + 1, items + slot, (count - slot) * sizeof(*items));
        items[slot] = (orb_stack_entry_t){ORB_STACK_HOME, 0}; count++;
    }
    return count;
}
static unsigned stack_entries(const stack_state_t *w, orb_stack_entry_t *entries)
{
    return orb_stack_screen_entries(&(orb_stack_read_t){.model = w->model, .home = w->home,
        .time = &w->time, .clock_date = w->status.clock_date, .clock_hour = w->clock_hour,
        .page_index = w->page_index}, entries);
}
static unsigned stack_count(const stack_state_t *w) { return stack_entries(w, NULL); }
static orb_stack_entry_t stack_selected(const stack_state_t *w)
{
    orb_stack_entry_t entries[ORB_STACK_MAX];
    unsigned count = stack_entries(w, entries);
    return count && w->page_index < count ? entries[w->page_index] : (orb_stack_entry_t){0};
}
static const orb_card_t *selected_card(stack_state_t *w)
{
    if (!w->card[0]) {
        unsigned count = stack_count(w);
        if (count) w->page_index = LV_MIN(w->page_index, count - 1);
        orb_stack_entry_t entry = stack_selected(w);
        return entry.kind == ORB_STACK_CARD ? &w->model->cards[entry.index] : NULL;
    }
    for (unsigned i = 0; i < w->model->card_count; i++)
        if (!strcmp(w->card, w->model->cards[i].id)) return &w->model->cards[i];
    return NULL;
}
static const orb_session_t *card_session(stack_state_t *w, const orb_card_t *card)
{
    for (unsigned i = 0; i < w->model->session_count; i++)
        if (!strcmp(card->session, w->model->sessions[i].id)) return &w->model->sessions[i];
    return NULL;
}
static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_scrollable(object, false); lv_obj_set_clickable(object, false);
    lv_obj_set_event_bubble(object, true);
    return object;
}
static lv_obj_t *label(lv_obj_t *parent, const char *text, orb_text_style_t style,
    bool bold, int x, int y, int width, int lines, lv_color_t colour)
{ return orb_kit_page_label(parent, text, style, bold, x, y, width, lines, colour); }
static lv_obj_t *prose(stack_state_t *w, int y, int height)
{
    w->list = orb_kit_detail_page(w->page, &(orb_kit_detail_page_config_t){
        .top = y, .height = height, .system = true, .arc = false});
    return w->list;
}
static void header(stack_state_t *w, const char *text)
{ w->heading = orb_kit_rim_title_create(w->page, text, ORB_KIT_SECONDARY); }
static void empty_state(stack_state_t *w, const char *glyph, const char *title, const char *detail)
{
    orb_kit_state_create(w->page, &(orb_kit_state_config_t){.glyph = glyph, .title = title,
        .detail = detail, .secondary = ORB_KIT_SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA,
        .top = 88, .bottom = 378});
}
static void clicked_event(lv_event_t *event)
{
    orb_screen_instance_t *instance = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_target_obj(event);
    unsigned action = (unsigned)(uintptr_t)lv_obj_get_user_data(target);
    lv_area_t bounds; lv_obj_get_coords(target, &bounds);
    orb_intent_t intent = {.kind = ORB_INTENT_APP_COMMAND, .origin = ORB_INPUT_TOUCH,
        .physical = lv_indev_active() && !orb_input_is_synthetic(lv_indev_active()),
        .launch = {.valid = true, .rect = {bounds.x1, bounds.y1,
            bounds.x2 - bounds.x1 + 1, bounds.y2 - bounds.y1 + 1}},
        .value.command = {.target = ORB_SHELL_STACK, .action = action}};
    /* The original generic route path froze this exact resting control before
     * navigation. The shell applies its live input/opening guard first. */
    (void)orb_screen_emit(instance, &intent);
}
static lv_obj_t *kit_action(stack_state_t *w, const char *text, int action,
    int x, int y, bool primary, bool glyph, lv_color_t colour)
{
    lv_obj_t *object = glyph ? orb_kit_glyph_button_create(w->page, text, primary, colour) :
        primary ? orb_kit_button_primary_create(w->page, text, colour) :
        orb_kit_button_secondary_create(w->page, text);
    lv_obj_update_layout(object);
    lv_obj_set_pos(object, x - lv_obj_get_width(object) / 2, y - lv_obj_get_height(object) / 2);
    lv_obj_set_user_data(object, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(object, true);
    lv_obj_add_event_cb(object, clicked_event, LV_EVENT_CLICKED, w->instance);
    return object;
}

static bool home_light_on(const orb_home_accessory_t *a)
{
    return a->kind == ORB_HOME_LIGHT && (a->state.present & ORB_HOME_ON) && a->state.on && a->reachable;
}
/* The Stack's Home card answers "did I leave lights on?" for whichever room
 * has them (the frame's "Living Room" was only its sample). One lit room names
 * the card and scopes All off to it; lights in several rooms make it "Home". */
const orb_home_room_t *orb_stack_home_room(const orb_home_snapshot_t *home)
{
    const orb_home_room_t *lit = NULL;
    if (home->room_off_pending)
        for (unsigned r = 0; r < home->room_count; r++)
            if (!strcmp(home->rooms[r].id, home->room_off_id)) return &home->rooms[r];
    for (unsigned i = 0; i < home->accessory_count; i++) {
        const orb_home_accessory_t *a = &home->accessories[i];
        if (!home_light_on(a)) continue;
        const orb_home_room_t *room = NULL;
        for (unsigned r = 0; r < home->room_count; r++)
            if (!strcmp(home->rooms[r].id, a->room)) room = &home->rooms[r];
        if (!room || (lit && lit != room)) return NULL;
        lit = room;
    }
    return lit;
}
static const orb_home_room_t *home_lit_room(const stack_state_t *w)
{ return orb_stack_home_room(w->home); }
static unsigned home_lit_room_count(const stack_state_t *w)
{
    unsigned rooms = 0;
    for (unsigned i = 0; i < w->home->accessory_count; i++) {
        const orb_home_accessory_t *a = &w->home->accessories[i];
        if (!home_light_on(a)) continue;
        bool seen = false;
        for (unsigned j = 0; j < i && !seen; j++)
            seen = home_light_on(&w->home->accessories[j]) && !strcmp(w->home->accessories[j].room, a->room);
        rooms += !seen;
    }
    return rooms;
}
static bool home_in_scope(const orb_home_room_t *room, const orb_home_accessory_t *a)
{
    return !room || !strcmp(room->id, a->room);
}
static bool home_ready(const stack_state_t *w)
{
    return w->home && w->home->online && w->home->synced && w->home->available;
}

static void time_stack_caption(const stack_state_t *w, char *value, size_t capacity,
                                char *detail, size_t detail_capacity)
{
    orb_stack_entry_t entry = stack_selected(w);
    bool alarm = w->time.clock_valid && w->time.alarm_enabled && w->time.alarm_next > w->time.clock_now;
    if (entry.kind == ORB_STACK_TIME_ALARM || (entry.kind == ORB_STACK_TIME_FLOOR && alarm && !orb_stack_alarm_today(&w->time))) {
        struct tm next;
        if (localtime_r(&w->time.alarm_next, &next)) {
            orb_time_format_clock(value, capacity, (unsigned)next.tm_hour, (unsigned)next.tm_min);
            snprintf(detail, detail_capacity, "%s", entry.kind == ORB_STACK_TIME_ALARM ? "Alarm today" : "Next alarm · tomorrow");
            return;
        }
    }
    if (entry.kind == ORB_STACK_TIME_FLOOR) {
        detail[0] = '\0';
        if (stack_date_known(w)) {
            const char *comma = strchr(w->status.clock_date, ',');
            if (comma) {
                size_t length = LV_MIN((size_t)(comma - w->status.clock_date), detail_capacity - 1);
                memcpy(detail, w->status.clock_date, length); detail[length] = '\0';
                const char *date = comma + 1;
                while (*date == ' ') date++;
                snprintf(value, capacity, "%s", date);
            } else snprintf(value, capacity, "%s", w->status.clock_date);
        } else {
            struct tm now;
            if (w->time.clock_valid && localtime_r(&w->time.clock_now, &now)) {
                strftime(value, capacity, "%B %d", &now);
                strftime(detail, detail_capacity, "%A", &now);
            } else value[0] = '\0';
        }
        return;
    }
    orb_stack_time_caption(&w->time, value, capacity, false);
    const char *state = w->time.reminder_due ? "Reminder due" :
        w->time.timer_state == ORB_TIME_FINISHED ? "Timer finished" :
        w->time.alarm_ringing ? "Alarm ringing" :
        w->time.timer_state == ORB_TIME_PAUSED ? "Timer paused" :
        w->time.timer_state == ORB_TIME_RUNNING ? "Timer running" :
        w->time.stopwatch_state == ORB_TIME_PAUSED ? "Stopwatch paused" : "Stopwatch running";
    snprintf(detail, detail_capacity, "%s", state);
}
static void time_labels(stack_state_t *w)
{
    char stack_value[96], stack_detail[64];
    if (w->stack_surface)
        time_stack_caption(w, stack_value, sizeof(stack_value), stack_detail, sizeof(stack_detail));
    if (w->time_value) {
        char text[96];
        if (w->stack_surface) snprintf(text, sizeof(text), "%s", stack_value);
        else orb_stack_time_caption(&w->time, text, sizeof(text), w->compact_time);
        orb_kit_value_set(w->time_value, text, "88:88:88");
    }
    if (w->time_status) {
        const char *state = w->stack_surface ? stack_detail : w->time.reminder_due ? "Reminder due" :
            w->time.timer_state == ORB_TIME_FINISHED ? "Timer finished" :
            w->time.alarm_ringing ? "Alarm ringing" :
            w->time.timer_state == ORB_TIME_PAUSED ? "Paused" : "Timer running";
        if (strcmp(lv_label_get_text(w->time_status), state)) lv_label_set_text(w->time_status, state);
    }
    if (w->stack_surface) stack_neighbors_update(w);
}

/* Design 1.0 Stack: neighbouring cards use fixed position/opacity steps.
 * They are retained content, with no glass or continuously scaled text.
 * HIG §§1/3/7: only the focused card has live glass actions. */
static lv_obj_t *stack_preview(stack_state_t *w, orb_stack_entry_t entry)
{
    const char *glyph = ORB_ICON_CLOCK;
    lv_color_t accent = ORB_KIT_TIME;
    switch (entry.kind) {
    case ORB_STACK_CARD: {
        const orb_notification_source_t *source = orb_notification_card_source(w->model, &w->model->cards[entry.index], NULL);
        glyph = source->glyph; accent = orb_notifications_source_colour(source); break;
    }
    case ORB_STACK_HOME: glyph = ORB_ICON_HOUSE; accent = ORB_KIT_HOME; break;
    case ORB_STACK_NOWPLAYING: glyph = ORB_ICON_MUSIC_NOTE; accent = ORB_KIT_SOUND; break;
    case ORB_STACK_USAGE: {
        orb_agent_t agent = w->model->usage.windows[entry.index].provider;
        glyph = agents_icon(agent); accent = agent_color(w, agent); break;
    }
    case ORB_STACK_TIME_ACTIVE: glyph = ORB_ICON_TIMER; break;
    case ORB_STACK_TIME_ALARM: glyph = ORB_ICON_ALARM; break;
    default: break;
    }
    /* HIG §§1/3/7: a peek is retained content, not a reduced list row.
     * Its short metadata keeps a full text rail alongside the source glyph. */
    lv_obj_t *card = plain(w->page);
    lv_obj_set_pos(card, 96, 348); lv_obj_set_size(card, 274, 76);
    lv_obj_set_style_radius(card, 38, 0);
    lv_obj_set_style_bg_color(card, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_t *disc = orb_kit_disc_create(card, ORB_KIT_DISC_KIND_ROW, ORB_KIT_ACCENT_FILL, accent, glyph);
    lv_obj_set_pos(disc, 16, 18); lv_obj_set_clickable(disc, false);
    lv_obj_t *caption = lv_label_create(card);
    orb_text_apply(caption, ORB_TEXT_FOOTNOTE2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_width(caption, 190);
    lv_obj_set_style_max_width(caption, 190, 0);
    lv_obj_set_style_text_color(caption, ORB_KIT_SECONDARY, 0);
    lv_obj_set_style_text_opa(caption, ORB_KIT_SECONDARY_OPA, 0);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 68, 0);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_clickable(card, true); lv_obj_set_event_bubble(card, true);
    orb_kit_press_apply(card);
    lv_obj_set_user_data(card, (void *)(intptr_t)ACT_PAGE_NEXT);
    lv_obj_add_event_cb(card, clicked_event, LV_EVENT_CLICKED, w->instance);
    return caption;
}


static void stack_neighbors_update(stack_state_t *w)
{
    orb_stack_entry_t entries[ORB_STACK_MAX];
    unsigned count = stack_entries(w, entries);
    for (unsigned i = 0; i < 2; i++) {
        if (!w->stack_neighbors[i]) continue;
        int index = (int)w->page_index + (i ? 1 : -1);
        if (index < 0 || index >= (int)count) continue;
        char text[96];
        const orb_home_room_t *room = home_lit_room(w);
        orb_stack_preview_copy(w->model, &w->time, room ? room->name : NULL, entries[index], text, sizeof(text));
        orb_kit_value_set(w->stack_neighbors[i], text, "Stopwatch · 88:88:88");
    }
}

static void stack_neighbors(stack_state_t *w)
{
    orb_stack_entry_t entries[ORB_STACK_MAX];
    unsigned count = stack_entries(w, entries);
    if (w->page_index + 1 < count) w->stack_neighbors[1] = stack_preview(w, entries[w->page_index + 1]);
    stack_neighbors_update(w);
}

static lv_obj_t *stack_card(stack_state_t *w, const char *name, const char *glyph, lv_color_t color)
{
    /* A travelling sheet names its place; the Face beneath owns the clock
     * (HIG §§1/6). A second time label would move through the large clock. */
    w->heading = orb_kit_rim_title_create(w->page, "Smart Stack", ORB_LABEL);
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    lv_obj_t *card = plain(w->page);
    lv_obj_set_pos(card, 63, 96);
    lv_obj_set_size(card, 340, 236);
    lv_obj_set_style_radius(card, 44, 0);
    lv_obj_set_style_bg_color(card, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_t *disc = orb_kit_disc_create(card, ORB_KIT_DISC_KIND_ROW,
        ORB_KIT_ACCENT_FILL, color, glyph);
    lv_obj_set_pos(disc, 28, 18); lv_obj_set_clickable(disc, false);
    lv_obj_t *provider = lv_label_create(card);
    lv_label_set_text(provider, name);
    orb_text_apply(provider, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
    lv_obj_set_style_text_color(provider, color, 0);
    lv_obj_set_pos(provider, 80, ax ? 18 : 24); lv_obj_set_width(provider, 232);
    lv_label_set_long_mode(provider, LV_LABEL_LONG_MODE_DOTS);
    orb_kit_scroll_arc_create(w->page, w->page_index, stack_count(w));
    if (!ready(w)) { /* Shared unavailable caption between title and card (review #12). */
        w->sync_caption = orb_kit_status_caption_create(w->page,
            w->model->online ? ORB_KIT_SYNCING_CAPTION : ORB_KIT_OFFLINE_CAPTION, 64);
        w->syncing = w->model->online;
        w->sync_started = lv_tick_get();
        orb_kit_breathe_since(w->sync_caption, w->syncing, w->sync_started);
    }
    return card;
}

static void time_stack(stack_state_t *w)
{
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    bool floor = stack_selected(w).kind == ORB_STACK_TIME_FLOOR;
    bool alarm = stack_selected(w).kind == ORB_STACK_TIME_ALARM;
    stack_neighbors(w);
    stack_card(w, "Time", alarm ? ORB_ICON_BELL : ORB_ICON_TIMER, ORB_TEAL);
    orb_text_style_t style = floor || ax ? ORB_TEXT_TITLE3 : ORB_TEXT_TITLE2;
    w->time_value = label(w->page, "", style, !floor, 233, 164, 280, 1, ORB_LABEL);
    orb_text_apply(w->time_value, style, floor ? ORB_TEXT_REGULAR : ORB_TEXT_EMPHASIZED | ORB_TEXT_TABULAR, 0);
    lv_label_set_long_mode(w->time_value, LV_LABEL_LONG_MODE_WRAP);
    w->time_status = label(w->page, "", ORB_TEXT_FOOTNOTE2, false, 233, 220, 280, 1, ORB_KIT_SECONDARY);
    lv_obj_set_style_text_align(w->time_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(w->time_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *open = kit_action(w, "Open Time", ACT_TIME, 233, 298, false, false, ORB_KIT_TIME);
    orb_kit_disc_set_text_style(open, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
    lv_obj_set_ext_click_area(open, ax ? 4 : 10);
    time_labels(w);
}
static void home_stack(stack_state_t *w)
{
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    char summary[80];
    bool first = !w->stack_hero;
    if (first) {
        if (!ax) stack_neighbors(w);
        lv_obj_t *card = stack_card(w, "Home", ORB_ICON_HOUSE, ORB_YELLOW);
        if (ax) lv_obj_set_height(card, 304);
    }
    const orb_home_room_t *room = home_lit_room(w);
    unsigned lights = 0;
    bool have_temperature = false;
    float temperature = 0;
    for (unsigned i = 0; i < w->home->accessory_count; i++) {
        const orb_home_accessory_t *a = &w->home->accessories[i];
        if (!a->reachable) continue;
        if (home_light_on(a)) lights++;
        /* A whole-home summary has no single temperature worth showing. */
        if (room && home_in_scope(room, a) && (a->state.present & ORB_HOME_TEMP)) {
            temperature = a->state.temp;
            have_temperature = true;
        }
    }
    const char *noun = lights == 1 ? "light" : "lights";
    char heading[48];
    if (room) {
        snprintf(heading, sizeof(heading), "%s", room->name);
        if (have_temperature) snprintf(summary, sizeof(summary), "%u %s on · %.0f°%s", lights, noun,
            (double)orb_temperature_display(temperature, w->model->prefs.temperature),
            w->model->prefs.temperature == ORB_TEMPERATURE_F ? "F" : "C");
        else snprintf(summary, sizeof(summary), "%u %s on", lights, noun);
    } else {
        snprintf(heading, sizeof(heading), "%u %s on", lights, noun);
        snprintf(summary, sizeof(summary), "In %u rooms", home_lit_room_count(w));
    }
    if (w->notice[0]) snprintf(summary, sizeof(summary), "%s", w->notice);
    else if (w->home->room_off_pending) snprintf(summary, sizeof(summary), "Turning off · %u of %u",
        w->home->room_off_completed, w->home->room_off_total);
    if (first) {
        /* HIG §§4/7/8: a two-line room name and its summary share one reader.
         * The action lane stays clear even when the summary needs scrolling. */
        lv_obj_t *names = prose(w, 158, 94);
        lv_obj_set_pos(names, 93, 158); lv_obj_set_width(names, 280);
        orb_scroll_edge_follow(names, 4, 4);
        orb_kit_paragraph(names, heading, ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, false);
        w->stack_hero = lv_obj_get_child(names, 0);
        orb_kit_paragraph(names, summary, ORB_TEXT_FOOTNOTE2, false, 52, ORB_KIT_SECONDARY, false);
        w->stack_detail = lv_obj_get_child(names, 1);
    }
    /* A room name remains a wrapping reader. Roll only its numeric summary;
     * a fixed live-value lane must never collapse the owner's long name. */
    if (room) {
        if (strcmp(lv_label_get_text(w->stack_hero), heading)) lv_label_set_text(w->stack_hero, heading);
    } else orb_kit_value_set_rolling(w->stack_hero, heading, NULL);
    if (w->notice[0]) {
        if (strcmp(lv_label_get_text(w->stack_detail), summary)) lv_label_set_text(w->stack_detail, summary);
    } else orb_kit_value_set_rolling(w->stack_detail, summary, NULL);
    if (w->home->room_off_pending && !w->waiting) w->wait_started = lv_tick_get();
    w->waiting = w->home->room_off_pending;
    orb_kit_breathe_since(w->stack_detail, w->waiting, w->wait_started);
    lv_obj_update_layout(w->stack_hero);
    lv_obj_set_y(w->stack_detail, LV_MAX(52, lv_obj_get_height(w->stack_hero) + 12));
    if (!first) return;
    /* HIG §§3/4/7: preserve both labels and 20 px separation. AX3 stacks
     * them in an expanded content card instead of pushing glass past its edge. */
    const char *off_text = room ? "All off" : "Open Home";
    lv_obj_t *off = kit_action(w, off_text, room ? ACT_HOME_OFF : ORB_SHELL_HOME,
        233, ax ? 288 : 298, false, false, ORB_KIT_HOME);
    lv_obj_t *scenes = kit_action(w, "Scenes", ACT_STACK_SCENES, 233,
        ax ? 360 : 298, false, false, ORB_KIT_HOME);
    orb_kit_disc_set_text_style(off, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
    orb_kit_disc_set_text_style(scenes, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
    orb_kit_chip_set_value(off, off_text, NULL);
    orb_kit_chip_set_value(scenes, "Scenes", NULL);
    lv_obj_update_layout(w->page);
    int off_width = lv_obj_get_width(off), scenes_width = lv_obj_get_width(scenes);
    if (!ax) {
        int left = 233 - (off_width + 20 + scenes_width) / 2;
        lv_obj_set_pos(off, left, 272);
        lv_obj_set_pos(scenes, left + off_width + 20, 272);
    } else {
        lv_obj_set_x(off, 233 - off_width / 2);
        lv_obj_set_x(scenes, 233 - scenes_width / 2);
    }
    orb_kit_control_set_enabled(off, home_ready(w) && !w->home->room_off_pending);
    orb_kit_control_set_enabled(scenes, home_ready(w));
    time_labels(w);
}

static void usage_stack(stack_state_t *w)
{
    orb_stack_entry_t entry = stack_selected(w);
    const orb_usage_window_t *window = &w->model->usage.windows[entry.index];
    bool ax = orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3;
    bool first = !w->stack_hero;
    if (first) {
        stack_neighbors(w);
        stack_card(w, window->provider == ORB_AGENT_CLAUDE ? "Claude" : "Codex",
            agents_icon(window->provider), agent_color(w, window->provider));
    }
    bool previous_reset = w->stack_detail && strchr(lv_label_get_text(w->stack_detail), '\n');
    char value[40], detail[100], window_name[48];
    orb_usage_window_name(window_name, sizeof(window_name), window->label);
    snprintf(value, sizeof(value), "%.0f%%", window->used_percent);
    if (window->resets_at && w->time.clock_valid && window->resets_at > (uint64_t)w->time.clock_now &&
        window->resets_at - (uint64_t)w->time.clock_now <= 30u * 60u) {
        unsigned minutes = (unsigned)((window->resets_at - (uint64_t)w->time.clock_now + 59u) / 60u);
        snprintf(detail, sizeof(detail), "Used · %s\nResets in %u min", window_name, minutes);
    } else if (!strcmp(window_name, "week") || !strcmp(window_name, "Week"))
        snprintf(detail, sizeof(detail), "Used this week");
    else snprintf(detail, sizeof(detail), "Used · %s", window_name);
    bool reset = strchr(detail, '\n') != NULL;
    /* Retained metrics can cross the reset threshold; its second line moves
     * the action to a different band, so rebuild that layout once. */
    if (!first && reset != previous_reset) { w->rebuild = true; return; }
    if (!w->stack_hero) w->stack_hero = label(w->page, value, ORB_TEXT_TITLE3, true, 233, 166, 280, 1, ORB_LABEL);
    if (!w->stack_detail) w->stack_detail = label(w->page, detail, ORB_TEXT_FOOTNOTE2, false, 233, 220, 280, 2, ORB_KIT_SECONDARY);
    orb_kit_value_set_rolling(w->stack_hero, value, "100%");
    if (strcmp(lv_label_get_text(w->stack_detail), detail)) lv_label_set_text(w->stack_detail, detail);
    lv_obj_set_style_text_align(w->stack_hero, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_align(w->stack_detail, LV_TEXT_ALIGN_LEFT, 0);
    if (!first) return;
    if (reset && w->stack_neighbors[1]) {
        lv_obj_delete(lv_obj_get_parent(w->stack_neighbors[1])); w->stack_neighbors[1] = NULL;
    }
    lv_obj_t *open = kit_action(w, "Open Usage", ORB_SHELL_USAGE, 233, reset ? 380 : 298, false, false, ORB_KIT_CLAUDE);
    orb_kit_disc_set_text_style(open, ORB_TEXT_FOOTNOTE2, ORB_TEXT_EMPHASIZED);
    lv_obj_set_ext_click_area(open, ax ? 4 : 10);
}
/* Stack previews contain no consequential controls. The explicit Open tap
 * enters the provider's full reader/approval view, whose stream owns actions. */
static void system_card_stack(stack_state_t *w)
{
    const orb_card_t *card = selected_card(w);
    if (!card) {
        header(w, "Smart Stack");
        empty_state(w, ORB_ICON_COPY, "All caught up", "Useful updates appear here");
        return;
    }
    const orb_session_t *session = card_session(w, card);
    const orb_notification_source_t *source = orb_notification_card_source(w->model, card, NULL);
    stack_neighbors(w);
    stack_card(w, source->name, source->glyph, orb_notifications_source_colour(source));
    /* HIG §4: a full question title wraps before the body in one reader;
     * the preview never hides a word behind a live consequential action. */
    lv_obj_t *body = prose(w, 158, 174);
    lv_obj_set_pos(body, 93, 158); lv_obj_set_width(body, 280);
    orb_scroll_edge_follow(body, 8, 16);
    int y = orb_kit_paragraph(body, card->title, ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, false) + 8;
    orb_kit_paragraph(body, card->body[0] ? card->body : session ? session->project : "Open to read the update",
        ORB_TEXT_FOOTNOTE2, false, y, ORB_KIT_SECONDARY, false);
    if (card->kind == ORB_CARD_APPROVAL && card->body[0])
        orb_kit_label_set_command(lv_obj_get_child(body, -1), card->body);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    kit_action(w, card->kind == ORB_CARD_APPROVAL ? "Review" : "Open",
        ACT_CARD + (int)(card - w->model->cards), 233, 380, false, false, ORB_KIT_GREY);
    /* The bottom action and a preview cannot occupy the same arc. */
    if (w->stack_neighbors[1]) { lv_obj_delete(lv_obj_get_parent(w->stack_neighbors[1])); w->stack_neighbors[1] = NULL; }
}


static void system_nowplaying_stack(stack_state_t *w)
{
    const orb_nowplaying_t *media = &w->model->nowplaying;
    stack_card(w, "Now Playing", ORB_ICON_MUSIC_NOTE, ORB_KIT_SOUND);
    const char *status = w->model->nowplaying_pending ? "Updating…" :
        w->model->nowplaying_error[0] ? ORB_ICON_WARNING " Couldn’t control playback" : NULL;
    /* HIG §§3/4/8: titles wrap before truncating. Keep their artist/status
     * in the same reader so longer names cannot overlap the transport arc. */
    lv_obj_t *body = prose(w, 158, 136);
    lv_obj_set_pos(body, 93, 158); lv_obj_set_width(body, 280);
    orb_scroll_edge_follow(body, 4, 4);
    int y = orb_kit_paragraph(body, media->title[0] ? media->title : "Nothing playing",
        ORB_TEXT_HEADLINE, true, 0, ORB_LABEL, false) + 12;
    orb_kit_paragraph(body, status ? status : media->artist, ORB_TEXT_FOOTNOTE2, false,
        y, ORB_KIT_SECONDARY, false);
    /* HIG §§3/7: all three centres lie on the same 147 px bottom arc. */
    lv_obj_t *previous = kit_action(w, ORB_ICON_SKIP_BACK_FILL, ACT_MEDIA_PREVIOUS,
        133, 341, false, true, ORB_KIT_SOUND);
    lv_obj_t *toggle = kit_action(w, media->playing ? ORB_ICON_PAUSE_FILL : ORB_ICON_PLAY_FILL,
        ACT_MEDIA_TOGGLE, 233, 380, true, true, ORB_KIT_SOUND);
    lv_obj_t *next = kit_action(w, ORB_ICON_SKIP_FORWARD_FILL, ACT_MEDIA_NEXT,
        333, 341, false, true, ORB_KIT_SOUND);
    bool enabled = ready(w) && !w->model->nowplaying_pending;
    orb_kit_control_set_enabled(previous, enabled);
    orb_kit_control_set_enabled(toggle, enabled);
    orb_kit_control_set_enabled(next, enabled);
}


static bool borrow(stack_state_t *w)
{
    orb_stack_read_t read = {0};
    if (!w->source.read || !w->source.read(w->source.context, &read) ||
        !read.model || !read.home || !read.time) return false;
    w->model = read.model; w->home = read.home; w->time = *read.time;
    w->status.clock_time = read.clock_time ? read.clock_time : "";
    w->status.clock_date = read.clock_date ? read.clock_date : "";
    w->notice = read.notice ? read.notice : ""; w->card = read.card ? read.card : "";
    w->page_index = read.page_index; w->clock_hour = read.clock_hour;
    w->stack_surface = read.stack_surface; w->compact_time = read.compact_time;
    return true;
}
static void release(stack_state_t *w)
{
    w->model = NULL; w->home = NULL; w->time = (orb_time_snapshot_t){0};
    w->status.clock_time = w->status.clock_date = w->notice = w->card = NULL;
}
static bool create(orb_screen_instance_t *instance, const orb_screen_args_t *args)
{
    (void)args;
    stack_state_t *w = orb_screen_state(instance);
    w->instance = instance; w->page = orb_screen_root(instance);
    lv_obj_set_clickable(w->page, false); lv_obj_set_event_bubble(w->page, true);
    orb_screen_host_t *host = orb_screen_host(instance);
    orb_screen_update_t projection = {0};
    if (host->config.project && host->config.project(host->config.context, ORB_SHELL_STACK, &projection) &&
        projection.model && projection.model->data && projection.model->size == sizeof(w->source))
        memcpy(&w->source, projection.model->data, sizeof(w->source));
    if (!borrow(w)) { header(w, "Smart Stack"); empty_state(w, ORB_ICON_COPY, "All caught up", "Useful updates appear here"); }
    else if (w->card[0]) system_card_stack(w);
    else switch (stack_selected(w).kind) {
    case ORB_STACK_NOWPLAYING: system_nowplaying_stack(w); break;
    case ORB_STACK_TIME_ACTIVE: case ORB_STACK_TIME_ALARM: case ORB_STACK_TIME_FLOOR: time_stack(w); break;
    case ORB_STACK_HOME: home_stack(w); break;
    case ORB_STACK_USAGE: usage_stack(w); break;
    default: system_card_stack(w); break;
    }
    release(w);
    orb_screen_page_bind(instance, w->list, w->heading);
    return true;
}
static void update(orb_screen_instance_t *instance, const orb_screen_update_t *value)
{
    /* The shell's distinct refresh opportunities below preserve the original
     * polling cadence. Staging and revision updates must not refit content. */
    (void)instance; (void)value;
}
static void destroy(orb_screen_instance_t *instance) { (void)instance; }
static bool save(const orb_screen_instance_t *instance, orb_screen_context_t *context, size_t capacity)
{
    if (capacity < sizeof(*context) + sizeof(stack_context_t)) return false;
    const stack_state_t *w = orb_screen_state(instance);
    stack_context_t saved = {0};
    saved.wait_started = w->wait_started; saved.sync_started = w->sync_started;
    saved.waiting = w->waiting; saved.syncing = w->syncing;
    memcpy(context->payload, &saved, sizeof(saved));
    return true;
}
static bool restore(orb_screen_instance_t *instance, const orb_screen_context_t *context)
{
    stack_state_t *w = orb_screen_state(instance);
    stack_context_t saved;
    memcpy(&saved, context->payload, sizeof(saved));
    if (saved.waiting && w->waiting) w->wait_started = saved.wait_started;
    if (saved.syncing && w->syncing) w->sync_started = saved.sync_started;
    if (w->stack_detail) orb_kit_breathe_since(w->stack_detail, w->waiting, w->wait_started);
    if (w->sync_caption) orb_kit_breathe_since(w->sync_caption, w->syncing, w->sync_started);
    return true;
}
static orb_back_result_t back(orb_screen_instance_t *instance)
{ (void)instance; return ORB_BACK_LEAVE_SCREEN; }
static bool rim(orb_screen_instance_t *instance, orb_rim_binding_t *binding)
{ (void)instance; (void)binding; return false; }
static void visibility(orb_screen_instance_t *instance, orb_visibility_t value)
{ (void)instance; (void)value; }
static void chrome(const orb_screen_instance_t *instance, orb_screen_chrome_t *value)
{ (void)instance; *value = (orb_screen_chrome_t){.title = "Smart Stack"}; }
static const orb_context_field_t s_context_fields[] = {
    {offsetof(stack_context_t, wait_started), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(stack_context_t, sync_started), sizeof(uint32_t), ORB_CONTEXT_U32},
    {offsetof(stack_context_t, waiting), sizeof(bool), ORB_CONTEXT_BOOL},
    {offsetof(stack_context_t, syncing), sizeof(bool), ORB_CONTEXT_BOOL},
};
static const orb_screen_ops_t s_ops = {create, update, destroy, save, restore, back, rim, visibility, chrome};
static const orb_screen_descriptor_t s_descriptor = {
    .id = ORB_SHELL_STACK, .parent_id = ORB_SHELL_FACE, .ops = &s_ops,
    .state_bytes = sizeof(stack_state_t), .context_bytes = sizeof(orb_screen_context_t) + sizeof(stack_context_t),
    .context_version = 2, .context_fields = s_context_fields,
    .context_field_count = sizeof(s_context_fields) / sizeof(s_context_fields[0]),
    .memory = {.owner = ORB_MEMORY_WORKSPACE, .state_bytes = sizeof(stack_state_t),
        .context_bytes = sizeof(orb_screen_context_t) + sizeof(stack_context_t), .declared = true},
    .subscriptions = ORB_DOMAIN_MODEL | ORB_DOMAIN_HOME | ORB_DOMAIN_TIME | ORB_DOMAIN_DEVICE,
    .privacy = ORB_PRIVACY_PRIVATE, .always_on = ORB_AOD_HIDDEN,
    .feedback = ORB_FEEDBACK_STANDARD, .nav_role = ORB_NAV_ROLE_SCREEN,
};
const orb_screen_descriptor_t *orb_stack_descriptor(void) { return &s_descriptor; }
static bool allow_synthetic(const orb_intent_t *intent)
{ return intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == ORB_SHELL_STACK; }
static void rim_finish(orb_screen_instance_t *instance, bool commit) { (void)instance; (void)commit; }
orb_app_host_t *orb_stack_host_create(bool preview,
    bool (*command)(void *, orb_screen_instance_t *, const orb_intent_t *), void *context)
{
    return orb_app_host_create(preview, &(orb_app_host_config_t){.descriptor = orb_stack_descriptor,
        .name = "Stack", .model_bytes = sizeof(orb_stack_source_t),
        .command = command, .context = context, .allow_synthetic = allow_synthetic, .rim_finish = rim_finish});
}
bool orb_stack_host_attach(orb_app_host_t *host, lv_obj_t *page, const orb_stack_source_t *source)
{
    if (!source || !orb_app_host_set_model(host, source, sizeof(*source)) || !orb_app_host_attach(host, page)) return false;
    lv_obj_t *title = orb_screen_page_title(orb_app_host_current(host));
    if (title) lv_obj_set_parent(title, page);
    return true;
}
bool orb_stack_screen_has_metrics(orb_screen_instance_t *instance)
{ return orb_screen_alive(instance) && ((stack_state_t *)orb_screen_state(instance))->stack_hero; }
bool orb_stack_screen_refresh(orb_screen_instance_t *instance, orb_stack_refresh_t refresh)
{
    if (!orb_screen_alive(instance)) return true;
    stack_state_t *w = orb_screen_state(instance);
    if (!borrow(w)) return true;
    w->rebuild = false;
    if (refresh == ORB_STACK_REFRESH_TIME || refresh == ORB_STACK_REFRESH_CLOCK) {
        time_labels(w);
        if (refresh == ORB_STACK_REFRESH_CLOCK && w->stack_hero && stack_selected(w).kind == ORB_STACK_USAGE) usage_stack(w);
    } else {
        if (refresh == ORB_STACK_REFRESH_HOME) home_stack(w);
        else usage_stack(w);
        stack_neighbors_update(w);
    }
    release(w);
    return !w->rebuild;
}
static lv_obj_t *find_action(lv_obj_t *root, unsigned action)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == action) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_action(lv_obj_get_child(root, i), action);
        if (found) return found;
    }
    return NULL;
}
void orb_stack_prepare_launch(orb_screen_instance_t *instance, unsigned action)
{
    if (!orb_screen_alive(instance)) return;
    lv_obj_t *target = find_action(orb_screen_root(instance), action);
    if (target) { lv_obj_remove_state(target, LV_STATE_PRESSED); orb_feedback_cancel(target); }
}
