#include "orb_notifications_host.h"
#include "orb_app.h"
#include "orb_input.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_commands, s_direct, s_restores;
static size_t s_baseline;
static unsigned s_timers;
static orb_intent_t s_last;
static orb_notifications_host_t *s_destroy_on_command;
static orb_notification_history_t *s_history;
static orb_notifications_view_t *s_view;
static orb_model_snapshot_t *s_model;
static orb_notifications_view_config_t s_config;
static lv_indev_data_t s_pointer;

static uint32_t tick(void) { return s_now; }
static size_t used(void)
{
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}
static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}
static void advance(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 16) { s_now += 16; lv_timer_handler(); }
}
static void direct(lv_event_t *event) { (void)event; s_direct++; }
static void scroll(lv_event_t *event) { (void)event; }
static void restore(void *context, lv_obj_t *area, const char *identity)
{
    (void)context; (void)area;
    assert(!strcmp(identity, "notice")); s_restores++;
}
static bool accept(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    (void)context;
    assert(intent->value.command.target == orb_screen_descriptor(instance)->id);
    s_commands++; s_last = *intent;
    if (s_destroy_on_command) {
        orb_notifications_host_t *host = s_destroy_on_command;
        s_destroy_on_command = NULL;
        orb_notifications_host_destroy(host);
    }
    return true;
}
static lv_obj_t *page_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent); lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 466, 466); lv_obj_set_scrollable(page, false);
    return page;
}
static lv_obj_t *action(lv_obj_t *root, unsigned id)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == id) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = action(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *text(lv_obj_t *root, const char *value)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = text(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}
static orb_notifications_view_config_t config(lv_obj_t *page)
{
    return (orb_notifications_view_config_t){.page = page, .model = s_model,
        .history = s_history, .card = "notice", .now = s_now,
        .clicked = direct, .scrolled = scroll, .restore = restore,
        .actions = {.recent = 240, .clear = 213, .undo = 105, .reply = 482,
            .open_session = 481, .dismiss = 497, .home = 18, .updates = 15}};
}
static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_APP_NOTIFICATIONS_LIST || id == ORB_APP_NOTIFICATIONS_DETAIL);
    *update = (orb_screen_update_t){.revisions.model = 1};
    return true;
}
static void quiescent(void)
{
    advance(1600);
    assert(used() == s_baseline && timers() == s_timers);
    /* Parent/explicit deletion must retire every age handle before a tick. */
    assert(!orb_notifications_view_tick(s_view, s_history, false, s_now));
}
static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    orb_notifications_page_t page = orb_app_notifications_page(instance);
    if (phase != ORB_LIFECYCLE_IDEMPOTENT && phase != ORB_LIFECYCLE_CHANGED) {
        assert(!page.title && !page.viewport);
        page = orb_app_notifications_compose(instance, s_view, &s_config);
    }
    assert(page.title && page.viewport);
    assert(orb_screen_page_viewport(instance) == page.viewport);
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    orb_screen_chrome_t chrome; orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Notifications"));
    unsigned before = s_commands;
    lv_obj_t *control = action(orb_screen_root(instance), page.curved ? 240 : 497);
    assert(control);
    lv_obj_send_event(control, LV_EVENT_CLICKED, NULL);
    assert(s_commands == before && !s_direct);
}
static void exercise(orb_screen_instance_t *instance)
{
    orb_screen_context_t bad = {.version = 99, .size = sizeof(bad)};
    assert(!orb_screen_restore(instance, &bad, sizeof(bad)));
    assert(orb_app_notifications_page(instance).viewport);
}
static void lifecycle(void)
{
    s_view = orb_notifications_view_create(); assert(s_view);
    s_config = config(NULL);
    orb_screen_update_t initial = {.revisions.model = 1};
    orb_screen_update_t changed = {.revisions.model = 2};
    orb_screen_update_t empty = {.revisions.model = 3};
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project, .preview = true, .defer_rim = true});
    const orb_screen_descriptor_t *screens[] = {orb_app_notifications_list_descriptor(), orb_app_notifications_detail_descriptor()};
    for (unsigned i = 0; i < 2; i++) {
        orb_screen_registry_t registry; orb_screen_registry_init(&registry);
        const orb_screen_descriptor_t *items[] = {screens[i]};
        orb_app_manifest_t manifest = {.id = i + 1, .name = "Notifications", .screens = items,
            .screen_count = 1, .entry = screens[i]->id};
        assert(orb_screen_registry_add(&registry, &manifest, NULL));
        /* Warm the shared focus/ellipsis caches before lifetime accounting. */
        orb_screen_instance_t warm = {0};
        assert(orb_screen_create(&warm, &host, screens[i], lv_screen_active(), NULL, false));
        (void)orb_app_notifications_compose(&warm, s_view, &s_config);
        orb_screen_destroy(&warm); advance(1600);
        s_baseline = used(); s_timers = timers();
        orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){.descriptor = screens[i],
            .initial = &initial, .changed = &changed, .removed = &empty, .empty = &empty,
            .exercise = exercise, .verify = verify, .quiescent = quiescent});
    }
    orb_notifications_view_destroy(s_view); s_view = NULL;
}
static void host_lifetime(bool preview, bool detail)
{
    size_t baseline = used(); unsigned timer_count = timers();
    orb_notifications_host_t *host = orb_notifications_host_create(preview); assert(host);
    lv_obj_t *owner = page_create(lv_screen_active()), *page = page_create(owner);
    orb_notifications_view_config_t projection = config(page);
    projection.title = lv_label_create(page); projection.title_glyph = lv_label_create(page);
    projection.navigation = lv_obj_create(page);
    orb_notifications_page_t result = orb_notifications_host_compose(host, &projection, detail, accept);
    orb_screen_instance_t *instance = orb_notifications_host_current(host);
    assert(instance && result.title && result.viewport && !result.title_glyph && !result.navigation);
    assert(lv_obj_get_parent(result.title) == page);
    assert(orb_screen_descriptor(instance)->id == (detail ? ORB_SHELL_NOTIFICATION_DETAIL : ORB_SHELL_NOTIFICATIONS));
    assert(!orb_notifications_host_tick(host, s_history, detail, s_now));
    s_now += 1000; assert(orb_notifications_host_tick(host, s_history, detail, s_now));
    unsigned before = s_commands;
    lv_obj_t *control = action(page, detail ? 497 : 240); assert(control);
    lv_obj_update_layout(page);
    lv_area_t bounds; lv_obj_get_coords(control, &bounds);
    lv_obj_send_event(control, LV_EVENT_CLICKED, NULL);
    assert(s_commands == before + !preview);
    if (!preview) {
        assert(s_last.kind == ORB_INTENT_APP_COMMAND && !s_last.physical && s_last.launch.valid);
        assert(s_last.launch.rect.x == bounds.x1 && s_last.launch.rect.y == bounds.y1);
        assert(s_last.launch.rect.width == bounds.x2 - bounds.x1 + 1);
        orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
        lv_obj_send_event(control, LV_EVENT_CLICKED, NULL); assert(s_commands == before + 1);
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    }
    /* No source generation/config is used by the subsequent age-only path. */
    memset(&projection, 0, sizeof(projection));
    s_now += 1000; assert(orb_notifications_host_tick(host, s_history, detail, s_now));
    lv_obj_delete(page);
    s_now += 1000; assert(!orb_notifications_host_tick(host, s_history, detail, s_now));
    orb_notifications_host_destroy(host); lv_obj_delete(owner);
    assert(used() == baseline && timers() == timer_count);
}
static void fallback(bool warm)
{
    size_t baseline = used();
    orb_notifications_host_t *host = orb_notifications_host_create(false); assert(host);
    lv_obj_t *owner = page_create(lv_screen_active()), *page = page_create(owner);
    orb_notifications_view_config_t projection = config(page);
    if (warm) {
        assert(orb_notifications_host_compose(host, &projection, false, accept).viewport);
        orb_notifications_host_detach(host, page); lv_obj_delete(page);
        page = page_create(owner); projection = config(page);
    }
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    /* Host history is tracked without admission; it does not consume the
     * allocator headroom that this fixture must exhaust. */
    size_t reserve = stats.budget - stats.used + orb_notification_history_bytes();
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    projection.title = lv_label_create(page); projection.title_glyph = lv_label_create(page);
    projection.navigation = lv_obj_create(page);
    orb_notifications_page_t result = orb_notifications_host_compose(host, &projection, false, accept);
    assert(!orb_notifications_host_current(host) && result.viewport && result.title &&
        !result.title_glyph && !result.navigation);
    unsigned before = s_direct;
    lv_obj_send_event(action(page, 240), LV_EVENT_CLICKED, NULL); assert(s_direct == before + 1);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    orb_notifications_host_reset(host); lv_obj_delete(page);
    page = page_create(owner); projection = config(page);
    result = orb_notifications_host_compose(host, &projection, false, accept);
    assert(result.viewport && orb_notifications_host_current(host));
    orb_notifications_host_destroy(host); lv_obj_delete(owner); assert(used() == baseline);
}
static void empty_and_teardown(void)
{
    size_t baseline = used();
    orb_notifications_host_t *host = orb_notifications_host_create(false); assert(host);
    lv_obj_t *owner = page_create(lv_screen_active()), *page = page_create(owner);
    orb_notification_history_t *empty = orb_notification_history_create(); assert(empty);
    orb_notifications_view_config_t projection = config(page); projection.history = empty;
    lv_obj_t *navigation = projection.navigation = lv_obj_create(page);
    orb_notifications_page_t result = orb_notifications_host_compose(host, &projection, false, accept);
    assert(!result.viewport && result.navigation == navigation && text(page, "No notifications"));
    orb_notifications_host_detach(host, page); lv_obj_delete(page);
    page = page_create(owner); projection = config(page);
    result = orb_notifications_host_compose(host, &projection, true, accept);
    assert(result.viewport);
    s_destroy_on_command = host;
    lv_obj_send_event(action(page, 497), LV_EVENT_CLICKED, NULL);
    assert(!s_destroy_on_command);
    orb_notification_history_destroy(empty); lv_obj_delete(owner); assert(used() == baseline);
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_pointer; }
static void physical(bool synthetic)
{
    orb_notifications_host_t *host = orb_notifications_host_create(false); assert(host);
    lv_obj_t *owner = page_create(lv_screen_active()), *page = page_create(owner);
    orb_notifications_view_config_t projection = config(page);
    orb_notifications_page_t result = orb_notifications_host_compose(host, &projection, false, accept);
    assert(result.viewport); advance(1600); lv_obj_update_layout(page);
    lv_obj_t *control = action(page, 240); lv_area_t bounds; lv_obj_get_coords(control, &bounds);
    s_pointer.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    lv_indev_t *input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, pointer_read); if (synthetic) orb_input_mark_synthetic(input);
    unsigned before = s_commands;
    s_pointer.state = LV_INDEV_STATE_PRESSED; lv_indev_read(input); advance(16);
    s_pointer.state = LV_INDEV_STATE_RELEASED; lv_indev_read(input); advance(16);
    assert(s_commands == before + 1 && s_last.physical == !synthetic);
    lv_indev_delete(input); orb_notifications_host_destroy(host); lv_obj_delete(owner);
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    s_model = calloc(1, sizeof(*s_model)); assert(s_model);
    s_model->online = s_model->synced = true; s_model->session_count = s_model->card_count = 1;
    s_model->sessions[0] = (orb_session_t){.id = "session", .agent = ORB_AGENT_CLAUDE,
        .title = "Test session", .can_message = true};
    s_model->cards[0] = (orb_card_t){.id = "notice", .session = "session", .kind = ORB_CARD_MESSAGE,
        .title = "Notification", .body = "Ready in 2 min 14 s.", .reply = true};
    s_history = orb_notification_history_create(); assert(s_history);
    orb_notification_history_seed(s_history, s_model);
    orb_notification_history_fixture_age(s_history, "notice", true, 1000, s_now);
    lifecycle();
    for (unsigned preview = 0; preview < 2; preview++)
        for (unsigned detail = 0; detail < 2; detail++) host_lifetime(preview, detail);
    fallback(false); fallback(true); empty_and_teardown(); physical(false); physical(true);
    orb_notification_history_destroy(s_history); free(s_model);
    assert(!used());
    puts("app_notifications: lifecycle, borrowed projections, input, fallback and teardown passed");
    return 0;
}
