#pragma once

#include "orb_notification_history.h"
#include "orb_kit.h"

typedef struct orb_notifications_view orb_notifications_view_t;

typedef struct {
    int recent, clear, undo, reply, open_session, dismiss, home, updates;
} orb_notifications_actions_t;

/* Model/history are immutable, call-scoped projections. The presenter retains
 * only its live age widgets; the shell owns the page and generic route anchor. */
typedef struct {
    lv_obj_t *page, *title, *title_glyph, *navigation;
    const orb_notification_history_t *history;
    const orb_model_snapshot_t *model;
    const char *card;
    uint32_t now;
    lv_event_cb_t clicked, scrolled;
    void *context;
    /* A framework instance handles clicks while the shell keeps its original
     * scroll/restore callbacks. Direct presenters leave this NULL. */
    void *clicked_context;
    void (*restore)(void *context, lv_obj_t *area, const char *identity);
    orb_notifications_actions_t actions;
} orb_notifications_view_config_t;

typedef struct {
    lv_obj_t *title, *title_glyph, *navigation, *viewport;
    bool curved;
} orb_notifications_page_t;

/* Exact private state reservation, excluding allocator metadata and LVGL descendants. */
size_t orb_notifications_view_bytes(void);
orb_notifications_view_t *orb_notifications_view_create(void);
void orb_notifications_view_destroy(orb_notifications_view_t *view);
/* Forget handles before their page is deleted, without changing history. */
void orb_notifications_view_reset(orb_notifications_view_t *view);
orb_notifications_page_t orb_notifications_view_list(orb_notifications_view_t *view,
    const orb_notifications_view_config_t *config);
orb_notifications_page_t orb_notifications_view_detail(orb_notifications_view_t *view,
    const orb_notifications_view_config_t *config);
/* The shell calls this only on a visible notification route. No timer, model
 * subscription or service command is owned by the presenter. */
bool orb_notifications_view_tick(orb_notifications_view_t *view,
    const orb_notification_history_t *history, bool detail, uint32_t now);
lv_color_t orb_notifications_source_colour(const orb_notification_source_t *source);
void notification_reading_text(char *out, size_t capacity, const char *text);
