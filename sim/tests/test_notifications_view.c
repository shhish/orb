/* The dedicated presenter owns age handles, never service/model generations.
 * Action checks use the supplied semantic IDs rather than child positions. */
#include "orb_notifications_view.h"
#include "orb_memory.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_actions, s_restores;
static int s_last_action;
static char s_identity[ORB_MODEL_CARD_ID_MAX + 1];

static uint32_t tick(void) { return s_now; }

static size_t used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static void clicked(lv_event_t *event)
{
    s_actions++;
    s_last_action = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
}

static void scrolled(lv_event_t *event) { (void)event; }

static void restore(void *context, lv_obj_t *area, const char *identity)
{
    (void)context; (void)area;
    s_restores++;
    snprintf(s_identity, sizeof(s_identity), "%s", identity);
}

static lv_obj_t *find_action(lv_obj_t *root, int action)
{
    if (lv_obj_is_clickable(root) && (int)(intptr_t)lv_obj_get_user_data(root) == action) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_action(lv_obj_get_child(root, i), action);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *page_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 466, 466);
    return page;
}

static orb_notifications_view_config_t config(lv_obj_t *page,
    const orb_notification_history_t *history, const orb_model_snapshot_t *model)
{
    return (orb_notifications_view_config_t){.page = page, .history = history,
        .model = model, .card = "notice", .now = s_now, .clicked = clicked,
        .scrolled = scrolled, .restore = restore,
        .actions = {.recent = 240, .clear = 213, .undo = 105, .reply = 482,
            .open_session = 481, .dismiss = 497, .home = 18, .updates = 7}};
}

static void admission(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    assert(!stats.used);
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, stats.budget));
    assert(!orb_notifications_view_create());
    orb_memory_release(ORB_MEMORY_WORKSPACE, stats.budget);
    orb_notifications_view_t *view = orb_notifications_view_create();
    assert(view && used());
    orb_notifications_view_destroy(view);
    orb_notifications_view_destroy(NULL);
    orb_notifications_view_reset(NULL);
    assert(!used());
}

static void lifetime(const orb_model_snapshot_t *model)
{
    size_t baseline = used();
    unsigned timer_baseline = timers();
    orb_notification_history_t *history = orb_notification_history_create();
    orb_notifications_view_t *view = orb_notifications_view_create();
    assert(history && view);
    orb_notification_history_seed(history, model);
    orb_notification_history_fixture_age(history, "notice", true, 1000, s_now);
    unsigned actions_before = s_actions;
    /* Direct source discs use a kit focus group on the page's parent, as in
     * the workspace. That shared group lives until this fixture owner closes. */
    lv_obj_t *owner = page_create(lv_screen_active());
    lv_obj_t *page = page_create(owner);
    orb_notifications_view_config_t projection = config(page, history, model);
    orb_notifications_page_t result = orb_notifications_view_list(view, &projection);
    assert(result.title && result.viewport && result.curved);
    assert(!orb_notifications_view_tick(view, history, false, s_now));
    s_now += 1000;
    assert(orb_notifications_view_tick(view, history, false, s_now));
    assert(s_actions == actions_before);
    lv_obj_t *clear = find_action(page, projection.actions.clear);
    assert(clear);
    lv_obj_send_event(clear, LV_EVENT_CLICKED, NULL);
    assert(s_actions == actions_before + 1 && s_last_action == projection.actions.clear);
    /* Parent deletion may precede reset during partial teardown. Reset only
     * forgets pointers, and the next age tick must never inspect dead nodes. */
    lv_obj_delete(page);
    orb_notifications_view_reset(view);
    s_now += 3000;
    assert(!orb_notifications_view_tick(view, history, false, s_now));

    page = page_create(owner);
    projection = config(page, history, model);
    unsigned restores_before = s_restores;
    result = orb_notifications_view_detail(view, &projection);
    assert(result.title && result.viewport && !result.curved);
    assert(s_restores == restores_before + 1 && !strcmp(s_identity, "notice"));
    assert(find_action(page, projection.actions.reply));
    assert(find_action(page, projection.actions.open_session));
    lv_obj_t *dismiss = find_action(page, projection.actions.dismiss);
    assert(dismiss);
    actions_before = s_actions;
    s_now += 2000;
    assert(orb_notifications_view_tick(view, history, true, s_now));
    assert(s_actions == actions_before);
    lv_obj_send_event(dismiss, LV_EVENT_CLICKED, NULL);
    assert(s_actions == actions_before + 1 && s_last_action == projection.actions.dismiss);
    orb_notifications_view_reset(view);
    lv_obj_delete(page);
    s_now += 2000;
    assert(!orb_notifications_view_tick(view, history, true, s_now));

    orb_notification_history_clear(history);
    page = page_create(owner); projection = config(page, history, model);
    result = orb_notifications_view_list(view, &projection);
    assert(result.title && !result.viewport && result.curved);
    assert(find_action(page, projection.actions.undo));
    orb_notifications_view_reset(view); lv_obj_delete(page);
    orb_notification_history_destroy(history);
    orb_notifications_view_destroy(view);
    lv_obj_delete(owner);
    assert(used() == baseline);
    assert(timers() == timer_baseline);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    assert(sim_display_create());
    orb_ui_init(NULL);
    admission();
    orb_model_snapshot_t *model = calloc(1, sizeof(*model));
    assert(model);
    model->online = model->synced = true;
    model->session_count = model->card_count = 1;
    model->sessions[0] = (orb_session_t){.id = "session", .agent = ORB_AGENT_CLAUDE,
        .title = "Test session", .can_message = true};
    model->cards[0] = (orb_card_t){.id = "notice", .session = "session", .kind = ORB_CARD_MESSAGE,
        .title = "Notification", .body = "Ready in 2 min 14 s.", .reply = true};
    for (unsigned i = 0; i < 3; i++) lifetime(model);
    free(model);
    puts("notifications_view: admission, borrowed projections, semantic actions and age-handle lifetime passed");
    return 0;
}
