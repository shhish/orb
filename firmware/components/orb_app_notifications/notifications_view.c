/* HIG §§1/2/5/7: unchanged matte notification content and neutral actions;
 * source identity is content, while the shell retains navigation policy. */
#include "orb_notifications_view.h"
#include "orb_kit_pages.h"
#include "orb_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct orb_notifications_view {
    unsigned age;
    lv_obj_t *ages[ORB_NOTIFICATION_HISTORY_COUNT];
};

size_t orb_notifications_view_bytes(void)
{
    return sizeof(orb_notifications_view_t);
}

orb_notifications_view_t *orb_notifications_view_create(void)
{
    return orb_memory_calloc(ORB_MEMORY_WORKSPACE, 1, sizeof(orb_notifications_view_t));
}

void orb_notifications_view_destroy(orb_notifications_view_t *view)
{
    orb_memory_free(view);
}

void orb_notifications_view_reset(orb_notifications_view_t *view)
{
    if (view) memset(view->ages, 0, sizeof(view->ages));
}

lv_color_t orb_notifications_source_colour(const orb_notification_source_t *source)
{
    switch (source->color) {
    case ORB_NOTIFICATION_ORANGE: return ORB_KIT_CLAUDE;
    case ORB_NOTIFICATION_YELLOW: return ORB_KIT_HOME;
    case ORB_NOTIFICATION_INDIGO: return ORB_KIT_DOT;
    default: return ORB_KIT_CODEX;
    }
}

/* HIG §4: a duration is one reading unit. Preserve admitted copy and only
 * replace its display separator so line wrapping cannot orphan "14 s". */
void notification_reading_text(char *out, size_t capacity, const char *text)
{
    size_t used = 0;
    for (size_t i = 0; text[i] && used + 1 < capacity; i++) {
        bool duration_space = false;
        if (text[i] == ' ' && i && text[i - 1] >= '0' && text[i - 1] <= '9') {
            static const char *units[] = {"s", "min", "hr", "h", "d"};
            for (unsigned u = 0; u < sizeof(units) / sizeof(units[0]); u++) {
                size_t length = strlen(units[u]);
                if (!strncmp(text + i + 1, units[u], length)) {
                    char next = text[i + 1 + length];
                    duration_space |= !next || next == ' ' || next == '.' || next == ',' || next == '\n';
                }
            }
        }
        if (duration_space) {
            if (used + 2 >= capacity) break;
            out[used++] = '\xC2'; out[used++] = '\xA0';
        } else out[used++] = text[i];
    }
    out[used] = '\0';
}

/* Glossary §1.6: one relative form everywhere ("now", "10 s ago", "2 hr ago"). */
static void elapsed(const orb_notification_record_t *recent, char *text, size_t capacity, uint32_t now)
{
    text[0] = '\0';
    if (!recent || !recent->received_known) return;
    unsigned seconds = (now - recent->received_at) / 1000;
    unsigned minutes = seconds / 60;
    if (minutes >= 1440) snprintf(text, capacity, "%u d ago", minutes / 1440);
    else if (minutes >= 60) snprintf(text, capacity, "%u hr ago", minutes / 60);
    else if (minutes) snprintf(text, capacity, "%u min ago", minutes);
    else if (seconds) snprintf(text, capacity, "%u s ago", seconds);
    else snprintf(text, capacity, "now");
}

static unsigned age(const orb_notification_history_t *history, uint32_t now)
{
    unsigned value = 0;
    for (unsigned i = 0; i < orb_notification_history_count(history); i++) {
        const orb_notification_record_t *recent = orb_notification_history_at(history, i);
        if (recent->received_known) {
            unsigned seconds = (now - recent->received_at) / 1000;
            value += seconds < 60 ? seconds : 60 + seconds / 60;
        }
    }
    return value;
}

bool orb_notifications_view_tick(orb_notifications_view_t *view,
    const orb_notification_history_t *history, bool detail, uint32_t now)
{
    if (!view) return false;
    unsigned current = age(history, now);
    if (view->age == current) return false;
    view->age = current;
    bool changed = false;
    for (unsigned i = 0; i < orb_notification_history_count(history); i++) {
        lv_obj_t *received = view->ages[i];
        if (!received) continue;
        char text[32];
        elapsed(orb_notification_history_at(history, i), text, sizeof(text), now);
        if (!detail) orb_kit_row_set_live_value(received, text, "888 min ago");
        else {
            char reading[48]; notification_reading_text(reading, sizeof(reading), text);
            orb_kit_value_set(received, reading, "888 min ago");
        }
        changed = true;
    }
    return changed;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    return obj;
}

static void header(const orb_notifications_view_config_t *config,
    orb_notifications_page_t *page, const char *title)
{
    if (page->title) lv_obj_delete(page->title);
    page->title = orb_kit_rim_title_create(config->page, title, ORB_KIT_SECONDARY);
    if (page->title_glyph) { lv_obj_delete(page->title_glyph); page->title_glyph = NULL; }
}

static void empty(const orb_notifications_view_config_t *config)
{
    orb_kit_state_create(config->page, &(orb_kit_state_config_t){
        .glyph = ORB_ICON_BELL, .title = "No notifications", .detail = "New updates appear here",
        .secondary = ORB_KIT_SECONDARY, .secondary_opa = ORB_KIT_SECONDARY_OPA,
        .top = 88, .bottom = 378,
    });
}

static lv_obj_t *row(const orb_notifications_view_config_t *config, lv_obj_t *list,
    const char *title, const char *detail, const char *glyph, lv_color_t colour, int action)
{
    orb_kit_row_config_t style = {.title = title, .subtitle = detail,
        .value = action >= config->actions.recent && action < config->actions.recent + ORB_NOTIFICATION_HISTORY_COUNT ? "" : NULL,
        .glyph = glyph && glyph[0] ? glyph : ORB_ICON_INFO, .accent = colour};
    lv_obj_t *item = orb_kit_row_create(list, &style);
    lv_obj_set_user_data(item, (void *)(intptr_t)action);
    lv_obj_set_event_bubble(item, true);
    if (action) lv_obj_add_event_cb(item, config->clicked, LV_EVENT_CLICKED,
        config->clicked_context ? config->clicked_context : config->context);
    else lv_obj_set_clickable(item, false);
    return item;
}

static void undo_action(const orb_notifications_view_config_t *config)
{
    lv_obj_t *obj = orb_kit_button_secondary_create(config->page, "Undo clear");
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, 233 - lv_obj_get_width(obj) / 2, 380 - lv_obj_get_height(obj) / 2);
    lv_obj_set_user_data(obj, (void *)(intptr_t)config->actions.undo);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_add_event_cb(obj, config->clicked, LV_EVENT_CLICKED,
        config->clicked_context ? config->clicked_context : config->context);
}

static orb_notifications_page_t page_from(const orb_notifications_view_config_t *config)
{
    return (orb_notifications_page_t){.title = config->title, .title_glyph = config->title_glyph,
        .navigation = config->navigation};
}

orb_notifications_page_t orb_notifications_view_list(orb_notifications_view_t *view,
    const orb_notifications_view_config_t *config)
{
    orb_notifications_page_t page = page_from(config);
    view->age = age(config->history, config->now);
    unsigned count = orb_notification_history_count(config->history);
    if (!count) { header(config, &page, "Notifications"); empty(config); return page; }
    if (page.title) { lv_obj_delete(page.title); page.title = NULL; }
    if (page.title_glyph) { lv_obj_delete(page.title_glyph); page.title_glyph = NULL; }
    if (page.navigation) { lv_obj_delete(page.navigation); page.navigation = NULL; }
    page.title = orb_kit_rim_title_create(config->page, "Notifications", ORB_KIT_SECONDARY);
    page.viewport = orb_kit_list_page(config->page, &(orb_kit_list_page_config_t){.top = 88, .height = 340});
    /* Review #7/#33: newest rests below the same shared scroll fade. */
    lv_obj_set_style_pad_top(page.viewport, ORB_KIT_LIST_INSET, 0);
    page.curved = true;
    unsigned shown = 0;
    for (unsigned slot = 0; slot < count; slot++) {
        unsigned index = count - 1 - slot;
        const orb_notification_record_t *recent = orb_notification_history_at(config->history, index);
        if (orb_notification_history_cleared(config->history, recent->id)) continue;
        const orb_notification_source_t *source = orb_notification_source(recent->source);
        lv_obj_t *item = row(config, page.viewport, recent->title, source->name, source->glyph,
            orb_notifications_source_colour(source), config->actions.recent + (int)index);
        if (recent->received_known) {
            char text[24]; elapsed(recent, text, sizeof(text), config->now);
            orb_kit_row_set_live_value(item, text, "888 min");
            view->ages[index] = item;
        }
        shown++;
    }
    if (!shown) {
        lv_obj_delete(page.viewport); page.viewport = NULL;
        empty(config);
        if (orb_notification_history_can_undo(config->history)) undo_action(config);
        return page;
    }
    row(config, page.viewport, "Clear all", "", ORB_ICON_X, ORB_KIT_GREY, config->actions.clear);
    if (orb_notification_history_can_undo(config->history))
        row(config, page.viewport, "Undo clear", "", ORB_ICON_UNDO, ORB_KIT_GREY, config->actions.undo);
    lv_obj_set_style_pad_bottom(page.viewport, 120, 0);
    orb_kit_list_apply(page.viewport);
    orb_kit_scroll_arc_attach(page.viewport);
    lv_obj_add_event_cb(page.viewport, config->scrolled, LV_EVENT_SCROLL, config->context);
    return page;
}

static void nested_rows_scroll(lv_event_t *event)
{
    /* Forward through the kit's allocation-free path, preserving outer fling. */
    if (lv_event_get_target_obj(event) == lv_event_get_current_target_obj(event))
        lv_obj_send_event(lv_event_get_user_data(event), LV_EVENT_SCROLL, NULL);
}

static void actions(const orb_notifications_view_config_t *config, lv_obj_t *area, int y,
    const orb_card_t *card, const orb_session_t *session, const char *source)
{
    struct { const char *title, *glyph; int action; } items[4];
    unsigned count = 0;
    bool reply = card && card->reply && !card->answered && (session || !strcmp(source, "Dot"));
    if (reply) items[count++] = (typeof(items[0])){"Reply by voice", ORB_ICON_MIC, config->actions.reply};
    if (session) items[count++] = (typeof(items[0])){"Open session", ORB_ICON_ARROW_UP_RIGHT, config->actions.open_session};
    else if (!strcmp(source, "Home")) items[count++] = (typeof(items[0])){"Open Home", ORB_ICON_HOUSE, config->actions.home};
    else if (!strcmp(source, "Updates"))
        items[count++] = (typeof(items[0])){"Open Updates", ORB_ICON_DOWNLOAD, config->actions.updates};
    items[count++] = (typeof(items[0])){"Dismiss", ORB_ICON_X, config->actions.dismiss};
    lv_obj_t *rows = plain(area);
    lv_obj_set_pos(rows, 0, y + 12);
    lv_obj_set_size(rows, 466, count * 84);
    for (unsigned i = 0; i < count; i++) {
        lv_obj_t *item = orb_kit_row_create(rows, &(orb_kit_row_config_t){
            .title = items[i].title, .glyph = items[i].glyph, .accent = ORB_KIT_GREY});
        lv_obj_set_user_data(item, (void *)(intptr_t)items[i].action);
        lv_obj_set_event_bubble(item, true);
        lv_obj_add_event_cb(item, config->clicked, LV_EVENT_CLICKED,
            config->clicked_context ? config->clicked_context : config->context);
        if (items[i].action == config->actions.reply &&
            (!(config->model->online && config->model->synced) || (session && !session->can_message)))
            orb_kit_row_set_enabled(item, false);
    }
    orb_kit_list_apply(rows);
    lv_obj_add_event_cb(area, nested_rows_scroll, LV_EVENT_SCROLL, rows);
    orb_kit_scroll_arc_attach(area);
    config->restore(config->context, area, config->card);
}

static const orb_card_t *selected_card(const orb_notifications_view_config_t *config)
{
    for (unsigned i = 0; i < config->model->card_count; i++)
        if (!strcmp(config->card, config->model->cards[i].id)) return &config->model->cards[i];
    return NULL;
}

static const orb_session_t *card_session(const orb_notifications_view_config_t *config, const orb_card_t *card)
{
    if (card) for (unsigned i = 0; i < config->model->session_count; i++)
        if (!strcmp(card->session, config->model->sessions[i].id)) return &config->model->sessions[i];
    return NULL;
}

orb_notifications_page_t orb_notifications_view_detail(orb_notifications_view_t *view,
    const orb_notifications_view_config_t *config)
{
    orb_notifications_page_t page = page_from(config);
    view->age = age(config->history, config->now);
    if (page.title) { lv_obj_delete(page.title); page.title = NULL; }
    if (page.navigation) { lv_obj_delete(page.navigation); page.navigation = NULL; }
    const orb_card_t *card = selected_card(config);
    const orb_notification_record_t *recent = orb_notification_history_find(config->history, config->card);
    unsigned count = orb_notification_history_count(config->history);
    if (!recent && !config->card[0] && count)
        recent = orb_notification_history_at(config->history,
            LV_MIN(orb_notification_history_selected(config->history), count - 1));
    const orb_session_t *session = card_session(config, card);
    const orb_notification_source_t *source = orb_notification_card_source(config->model, card, recent);
    /* Review #30: the rim names the source, as the long look's sash does. */
    header(config, &page, source->name);
    lv_obj_t *identity = orb_kit_disc_create(config->page, ORB_KIT_DISC_KIND_ROW,
        ORB_KIT_ACCENT_FILL, orb_notifications_source_colour(source), source->glyph);
    lv_obj_set_pos(identity, 213, 80); lv_obj_set_clickable(identity, false);
    char title[130], received[32] = "", body[ORB_MODEL_TEXT_MAX * 2 + 1];
    notification_reading_text(title, sizeof(title), card ? card->title : recent ? recent->title : "Notification unavailable");
    notification_reading_text(body, sizeof(body), card && card->body[0] ? card->body : recent ? recent->state : "No details");
    if (recent && recent->received_known) {
        char text[32]; elapsed(recent, text, sizeof(text), config->now);
        notification_reading_text(received, sizeof(received), text);
    }
    /* HIG §7: content and neutral actions share one long-look reading scroll. */
    page.viewport = orb_kit_detail_page(config->page, &(orb_kit_detail_page_config_t){
        .top = 128, .height = 300, .system = true, .arc = true});
    lv_obj_set_pos(page.viewport, 0, 128); lv_obj_set_width(page.viewport, 466);
    lv_obj_set_clickable(page.viewport, true);
    lv_obj_set_scrollbar_mode(page.viewport, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *content = plain(page.viewport);
    lv_obj_set_pos(content, 60, 0); lv_obj_set_width(content, 346);
    int y = 0;
    if (received[0]) {
        y = orb_kit_paragraph(content, received, ORB_TEXT_FOOTNOTE2, false, 0, ORB_KIT_SECONDARY, true);
        for (unsigned i = 0; i < count; i++) if (orb_notification_history_at(config->history, i) == recent)
            view->ages[i] = lv_obj_get_child(content, 0);
    }
    y = orb_kit_paragraph(content, title, ORB_TEXT_HEADLINE, true, y, ORB_LABEL, true) + 2;
    y = orb_kit_paragraph(content, body, ORB_TEXT_FOOTNOTE2, false, y, ORB_KIT_SECONDARY, true);
    lv_obj_set_height(content, y);
    actions(config, page.viewport, y, card, session, source->name);
    return page;
}
