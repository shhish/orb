/* Stack reads the current COW generations only at the shell's original
 * refresh opportunities. Test ownership and cadence through public contracts. */
#include "stack_screen.h"
#include "orb_app.h"
#include "orb_kit.h"
#include "orb_input.h"
#include "orb_memory.h"
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
static size_t s_workspace_baseline;
static unsigned s_timer_baseline, s_commands, s_reads;
static orb_model_snapshot_t *s_model;
static orb_home_snapshot_t *s_home;
static orb_time_snapshot_t s_time;
static unsigned s_page;
static const char *s_card = "", *s_notice = "";
static bool s_readable = true, s_stack_surface = true;
static orb_intent_t s_last;
static lv_indev_data_t s_pointer;
static orb_app_host_t *s_destroy_host;
static lv_obj_t *s_destroy_parent;
static uint32_t tick(void) { return s_now; }
static void advance(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 16) { s_now += 16; lv_timer_handler(); }
}
static size_t used(void)
{
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats); return stats.used;
}
static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}
static void quiescent(void)
{
    unsigned commands = s_commands;
    advance(1600);
    assert(used() == s_workspace_baseline && timers() == s_timer_baseline && commands == s_commands);
}
static lv_obj_t *page(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root); lv_obj_set_size(root, 466, 466); lv_obj_set_scrollable(root, false);
    return root;
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
static lv_obj_t *action(lv_obj_t *root, unsigned id)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == id) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = action(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static bool read_current(void *context, orb_stack_read_t *read)
{
    assert(context == &s_reads); s_reads++;
    if (!s_readable) return false;
    *read = (orb_stack_read_t){.model = s_model, .home = s_home, .time = &s_time,
        .clock_time = "9:41 AM", .clock_date = "Tuesday, October 29", .clock_hour = 9,
        .card = s_card, .notice = s_notice, .page_index = s_page, .stack_surface = s_stack_surface};
    return true;
}
static orb_stack_source_t s_source = {.read = read_current, .context = &s_reads};
static orb_screen_model_t s_projection = {&s_source, sizeof(s_source)};
static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    (void)context; assert(screen == ORB_SHELL_STACK);
    *update = (orb_screen_update_t){.model = &s_projection, .revisions.device = 1}; return true;
}
static bool record(void *context, const orb_intent_t *intent)
{ (void)context; s_last = *intent; s_commands++; return true; }
static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    assert(orb_screen_alive(instance));
    bool accepted = record(context, intent);
    if (s_destroy_host) {
        orb_app_host_t *host = s_destroy_host; s_destroy_host = NULL;
        lv_obj_delete(s_destroy_parent); s_destroy_parent = NULL;
        orb_app_host_destroy(host);
    }
    return accepted;
}
static bool allow(void *context, const orb_intent_t *intent)
{ (void)context; return intent->kind == ORB_INTENT_APP_COMMAND; }
static void reset(void)
{
    memset(s_model, 0, sizeof(*s_model)); memset(s_home, 0, sizeof(*s_home));
    s_model->online = s_model->synced = true;
    s_home->online = s_home->synced = s_home->available = true;
    s_time = (orb_time_snapshot_t){0}; s_page = 0; s_card = s_notice = ""; s_readable = s_stack_surface = true;
}
static orb_app_host_t *attach(lv_obj_t *parent, bool preview)
{
    orb_app_host_t *host = orb_stack_host_create(preview, command, NULL);
    assert(host && orb_stack_host_attach(host, parent, &s_source)); return host;
}
static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    (void)phase;
    assert(orb_screen_page_title(instance));
    assert(text(orb_screen_root(instance), "Timer running"));
    orb_rim_binding_t binding;
    assert(!orb_stack_descriptor()->ops->rim_binding(instance, &binding));
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
}
static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_commands;
    lv_obj_send_event(action(orb_screen_root(instance), 480), LV_EVENT_CLICKED, NULL);
    assert(s_commands == before + 1 && s_last.value.command.action == 480 && s_last.launch.valid);
}
static void lifecycle(void)
{
    reset(); s_time.timer_state = ORB_TIME_RUNNING; s_time.timer_remaining_us = 10000000;
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.intent = record,
        .allow_synthetic = allow, .project = project, .defer_rim = true});
    orb_screen_update_t initial = {.model = &s_projection, .revisions.device = 1};
    orb_screen_update_t changed = {.model = &s_projection, .revisions.device = 2};
    orb_screen_update_t removed = {.revisions.device = 3}, empty = {.revisions.device = 4};
    orb_test_screen_lifecycle(&host, &(orb_lifecycle_case_t){.descriptor = orb_stack_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent});
}
static void generations_and_cadence(void)
{
    reset(); s_time.timer_state = ORB_TIME_RUNNING; s_time.timer_remaining_us = 10000000;
    s_model->usage.count = 1; s_model->prefs.sources[0] = true;
    s_model->usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE,
        .source = ORB_USAGE_STATUSLINE, .used_percent = 80};
    snprintf(s_model->usage.windows[0].label, sizeof(s_model->usage.windows[0].label), "week");
    lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, false);
    orb_screen_instance_t *instance = orb_app_host_current(host);
    assert(text(parent, "0:10") && text(parent, "Claude · 80%"));
    unsigned reads = s_reads;
    orb_app_host_poll(host); orb_app_host_environment(host);
    assert(s_reads == reads);
    orb_model_snapshot_t *model = malloc(sizeof(*model)); *model = *s_model; free(s_model); s_model = model;
    orb_home_snapshot_t *home = malloc(sizeof(*home)); *home = *s_home; free(s_home); s_home = home;
    s_time.timer_remaining_us = 9000000; s_model->usage.windows[0].used_percent = 90;
    assert(orb_stack_screen_refresh(instance, ORB_STACK_REFRESH_TIME));
    assert(text(parent, "0:09") && text(parent, "Claude · 90%"));
    assert(orb_screen_page_title(instance) && lv_obj_get_parent(orb_screen_page_title(instance)) == parent);
    unsigned commands = s_commands;
    lv_obj_send_event(action(parent, 480), LV_EVENT_CLICKED, NULL);
    assert(s_commands == commands + 1 && s_last.value.command.target == ORB_SHELL_STACK && !s_last.physical);
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    lv_obj_send_event(action(parent, 480), LV_EVENT_CLICKED, NULL); assert(s_commands == commands + 1);
    orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    s_readable = false; assert(orb_stack_screen_refresh(instance, ORB_STACK_REFRESH_TIME));
    assert(text(parent, "0:09")); s_readable = true;
    /* A route may change before queued page deletion; the outgoing Time
     * labels still receive their original non-Stack caption in that gap. */
    s_stack_surface = false; s_time.timer_state = ORB_TIME_PAUSED;
    assert(orb_stack_screen_refresh(instance, ORB_STACK_REFRESH_TIME));
    assert(text(parent, "Paused")); s_stack_surface = true;
    orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();

    reset(); s_model->usage.count = 1; s_model->prefs.sources[0] = true;
    s_model->usage.windows[0] = (orb_usage_window_t){.provider = ORB_AGENT_CLAUDE,
        .source = ORB_USAGE_STATUSLINE, .used_percent = 80, .resets_at = 4000};
    snprintf(s_model->usage.windows[0].label, sizeof(s_model->usage.windows[0].label), "week");
    s_time.clock_valid = true; s_time.clock_now = 1000;
    parent = page(); host = attach(parent, false); instance = orb_app_host_current(host);
    assert(text(parent, "80%") && text(parent, "Used this week"));
    s_model->usage.windows[0].used_percent = 90; s_time.clock_now = 3000;
    assert(orb_stack_screen_refresh(instance, ORB_STACK_REFRESH_TIME)); assert(text(parent, "80%"));
    assert(!orb_stack_screen_refresh(instance, ORB_STACK_REFRESH_USAGE));
    assert(text(parent, "80%") && text(parent, "Used this week"));
    orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
}
static void home_and_override(void)
{
    reset(); s_home->room_count = 1; s_home->accessory_count = 1;
    snprintf(s_home->rooms[0].id, sizeof(s_home->rooms[0].id), "r1");
    snprintf(s_home->rooms[0].name, sizeof(s_home->rooms[0].name), "Living Room");
    orb_home_accessory_t *light = &s_home->accessories[0];
    snprintf(light->room, sizeof(light->room), "r1"); light->kind = ORB_HOME_LIGHT; light->reachable = true;
    light->state.present = ORB_HOME_ON; light->state.on = true;
    lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, false);
    orb_screen_instance_t *instance = orb_app_host_current(host);
    lv_obj_t *reader = orb_screen_page_viewport(instance), *off = action(parent, 384);
    assert(reader && off && text(parent, "Living Room") && text(parent, "1 light on"));
    s_notice = "Try in Home"; s_home->available = false;
    assert(orb_stack_screen_refresh(instance, ORB_STACK_REFRESH_HOME));
    assert(text(parent, "Try in Home") && orb_screen_page_viewport(instance) == reader);
    assert(action(parent, 384) == off && !lv_obj_has_state(off, LV_STATE_DISABLED));
    unsigned commands = s_commands;
    lv_obj_send_event(off, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(action(parent, 489), LV_EVENT_CLICKED, NULL);
    assert(s_commands == commands + 2 && s_last.value.command.action == 489);
    orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();

    reset(); s_model->card_count = 1;
    snprintf(s_model->cards[0].id, sizeof(s_model->cards[0].id), "explicit");
    snprintf(s_model->cards[0].title, sizeof(s_model->cards[0].title), "Read all of this");
    snprintf(s_model->cards[0].body, sizeof(s_model->cards[0].body), "command body");
    s_model->cards[0].kind = ORB_CARD_APPROVAL; s_card = "explicit"; s_page = 1;
    parent = page(); host = attach(parent, false); instance = orb_app_host_current(host);
    assert(text(parent, "Read all of this") && text(parent, "command body") && action(parent, 160));
    assert(!orb_stack_screen_has_metrics(instance));
    orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
    s_card = "gone"; parent = page(); host = attach(parent, true);
    assert(text(parent, "All caught up"));
    commands = s_commands;
    assert(!action(parent, 160));
    orb_app_host_destroy(host); lv_obj_delete(parent); assert(s_commands == commands); quiescent();
}
static void pointer_read(lv_indev_t *input, lv_indev_data_t *data) { (void)input; *data = s_pointer; }
static void pointer_send(lv_indev_t *input, lv_point_t point, lv_indev_state_t state)
{
    s_pointer.point = point; s_pointer.state = state; lv_indev_read(input); advance(16);
}
static void physical_pointer(bool synthetic)
{
    reset(); s_time.timer_state = ORB_TIME_RUNNING;
    lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, false);
    lv_obj_t *open = action(parent, 480); lv_obj_update_layout(parent);
    lv_area_t bounds; lv_obj_get_coords(open, &bounds);
    lv_point_t point = {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER); lv_indev_set_read_cb(input, pointer_read);
    if (synthetic) orb_input_mark_synthetic(input);
    unsigned commands = s_commands;
    pointer_send(input, point, LV_INDEV_STATE_PRESSED);
    pointer_send(input, point, LV_INDEV_STATE_RELEASED);
    assert(s_commands == commands + 1 && s_last.physical == !synthetic && s_last.value.command.action == 480);
    assert(s_last.launch.valid && s_last.launch.rect.width > 0);
    lv_indev_delete(input); orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
}
static void destroy_on_command(void)
{
    reset(); s_time.timer_state = ORB_TIME_RUNNING;
    s_destroy_parent = page(); s_destroy_host = attach(s_destroy_parent, false);
    lv_obj_t *open = action(s_destroy_parent, 480);
    unsigned commands = s_commands;
    lv_obj_send_event(open, LV_EVENT_CLICKED, NULL);
    assert(s_commands == commands + 1 && !s_destroy_host && !s_destroy_parent);
    quiescent();
}
static void admission_and_preview(void)
{
    reset(); s_time.timer_state = ORB_TIME_RUNNING;
    lv_obj_t *parent = page(); orb_app_host_t *host = attach(parent, true);
    unsigned commands = s_commands;
    lv_obj_send_event(action(parent, 480), LV_EVENT_CLICKED, NULL); assert(s_commands == commands);
    orb_app_host_destroy(host); lv_obj_delete(parent); quiescent();
    host = orb_stack_host_create(false, command, NULL); assert(host); parent = page();
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used; assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    assert(!orb_stack_host_attach(host, parent, &s_source) && !orb_app_host_current(host));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_stack_host_attach(host, parent, &s_source));
    lv_obj_delete(parent); assert(!orb_app_host_current(host)); orb_app_host_destroy(host); quiescent();
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    s_model = calloc(1, sizeof(*s_model)); s_home = calloc(1, sizeof(*s_home)); assert(s_model && s_home);
    const orb_screen_descriptor_t *descriptor = orb_stack_descriptor();
    assert(orb_screen_descriptor_valid(descriptor, NULL) && descriptor->id == ORB_SHELL_STACK);
    const orb_screen_descriptor_t *screens[] = {descriptor};
    orb_app_manifest_t manifest = {.id = descriptor->id, .name = "Stack", .screens = screens,
        .screen_count = 1, .entry = descriptor->id};
    orb_screen_registry_t registry; orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &manifest, NULL));
    reset(); s_time.timer_state = ORB_TIME_RUNNING;
    lv_obj_t *parent = page(); orb_app_host_t *warm = attach(parent, false);
    orb_app_host_destroy(warm); lv_obj_delete(parent); advance(1600);
    s_workspace_baseline = used(); s_timer_baseline = timers();
    lifecycle(); generations_and_cadence(); home_and_override(); admission_and_preview();
    physical_pointer(false); physical_pointer(true); destroy_on_command();
    free(s_model); free(s_home);
    puts("stack_screen: lifecycle, borrowing, refresh cadence, retained readers, overrides, input and cleanup passed");
    return 0;
}
