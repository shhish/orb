/* The production descriptors, copied projections and workspace-lifetime flows
 * share the screen lifecycle harness. Service storage stays behind a port. */
#include "system/system_views.h"
#include "orb_app.h"
#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_timer_baseline, s_commands, s_changed, s_done, s_checks, s_requests;
static size_t s_workspace_baseline;
static bool s_warming = true;
static bool s_allowed = true, s_physical, s_needed, s_saving, s_error;
static orb_shell_surface_t s_surface;
static orb_onboarding_model_t s_onboarding;
static orb_updates_model_t s_updates = {.online = true, .firmware_version = "v0.5.2-test"};
static orb_screen_model_t s_model;

static uint32_t tick(void) { return s_now; }
static void settle(void)
{
    for (unsigned i = 0; i < 1600; i += 16) { s_now += 16; lv_timer_handler(); }
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
    settle(); if (s_warming) return; assert(used() == s_workspace_baseline); assert(timers() == s_timer_baseline);
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (lv_obj_is_hidden(root)) return NULL;
    const char *caption = orb_kit_button_get_text(root);
    if ((caption && !strcmp(caption, text)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text); if (found) return found;
    }
    return NULL;
}
static bool record(void *context, const orb_intent_t *intent)
{
    (void)context;
    assert(intent->kind == ORB_INTENT_APP_COMMAND && intent->value.command.target == s_surface);
    s_commands++; return true;
}
static bool synthetic(void *context, const orb_intent_t *intent) { (void)context; (void)intent; return true; }
static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context; assert(id == s_surface);
    *update = (orb_screen_update_t){.revisions.device = 1, .revisions.time = 1, .model = &s_model}; return true;
}
static void activate(orb_screen_instance_t *instance)
{
    lv_obj_t *button = find(orb_screen_root(instance), s_surface == ORB_SHELL_ONBOARDING ? "Got it" : "Check now");
    assert(button); lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
}
static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    (void)phase;
    assert(find(orb_screen_root(instance), s_surface == ORB_SHELL_ONBOARDING ? "Top edge" : "Orb 0.5.2"));
    assert(orb_screen_page_viewport(instance) && orb_screen_page_title(instance));
    orb_screen_chrome_t chrome; orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, s_surface == ORB_SHELL_ONBOARDING ? "Welcome to Orb" : "Updates"));
}
static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_commands;
    activate(instance); assert(s_commands == before + 1);
    orb_screen_context_t context = {0};
    assert(orb_screen_descriptor(instance)->context_bytes == sizeof(context));
    assert(orb_screen_save(instance, &context, sizeof(context)));
    context.version++; assert(!orb_screen_restore(instance, &context, sizeof(context)));
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    lv_obj_t *button = find(orb_screen_root(instance), s_surface == ORB_SHELL_ONBOARDING ? "Got it" : "Check now");
    assert(button); orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL); assert(s_commands == before + 1);
    orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
}
static void lifecycle(const orb_screen_descriptor_t *descriptor)
{
    s_surface = descriptor->id;
    s_model = s_surface == ORB_SHELL_ONBOARDING ?
        (orb_screen_model_t){&s_onboarding, sizeof(s_onboarding)} : (orb_screen_model_t){&s_updates, sizeof(s_updates)};
    assert(orb_screen_descriptor_valid(descriptor, NULL));
    const orb_screen_descriptor_t *screens[] = {descriptor};
    orb_app_manifest_t manifest = {.id = s_surface, .name = "System", .screens = screens, .screen_count = 1, .entry = s_surface};
    orb_screen_registry_t registry; orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &manifest, NULL));
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project, .intent = record,
        .allow_synthetic = synthetic, .defer_rim = true});
    orb_screen_update_t initial = {.revisions.time = 1, .model = &s_model};
    orb_screen_update_t changed = {.revisions.time = 2, .model = &s_model};
    orb_screen_update_t removed = {.revisions.time = 3};
    orb_screen_update_t empty = {.revisions.time = 4};
    orb_lifecycle_case_t test = {.descriptor = descriptor, .initial = &initial, .changed = &changed,
        .removed = &removed, .empty = &empty, .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(&host, &test);
    host.config.preview = true;
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, descriptor, lv_screen_active(), NULL, true));
    unsigned before = s_commands; activate(&preview); assert(s_commands == before);
    assert(orb_screen_back(&preview) == ORB_BACK_CONSUMED);
    orb_screen_destroy(&preview); quiescent();
}
static bool needed(void) { return s_needed; }
static bool saving(void) { return s_saving; }
static bool error(void) { return s_error; }
static bool dismiss(void) { s_requests++; s_saving = true; s_error = false; return true; }
static bool event(void *context, orb_system_view_event_t value)
{
    (void)context;
    switch (value) {
    case ORB_SYSTEM_VIEW_INPUT: return s_allowed;
    case ORB_SYSTEM_VIEW_PHYSICAL: return s_physical;
    case ORB_SYSTEM_VIEW_DONE: s_done++; break;
    case ORB_SYSTEM_VIEW_CHECK: s_checks++; break;
    case ORB_SYSTEM_VIEW_CHANGED: s_changed++; break;
    }
    return true;
}
static lv_obj_t *page(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 466, 466); lv_obj_set_scrollable(root, false); return root;
}
static void attach(orb_system_views_t *views, lv_obj_t **parent, orb_shell_surface_t surface, const orb_shell_snapshot_t *status)
{
    if (*parent) { orb_system_views_detach(views, *parent); lv_obj_delete(*parent); }
    *parent = page(); assert(orb_system_views_attach(views, *parent, surface, status)); s_surface = surface;
}
static void flows(bool preview)
{
    orb_system_views_t *views = orb_system_views_create(preview, event, NULL); assert(views);
    orb_system_views_onboarding_port(views, &(orb_onboarding_port_t){needed, saving, error, dismiss});
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .firmware_version = "v0.5.2-test"};
    lv_obj_t *parent = NULL;
    attach(views, &parent, ORB_SHELL_UPDATES, &status);
    orb_screen_instance_t *instance = orb_system_views_current(views, s_surface);
    assert(find(orb_screen_root(instance), "Orb 0.5.2"));
    /* The descriptor's copied projection outlives the caller's temporary text. */
    status.firmware_version[1] = '9';
    assert(find(orb_screen_root(instance), "Orb 0.5.2"));
    status.firmware_version[1] = '0';
    unsigned checks = s_checks;
    s_allowed = false; activate(instance); assert(s_checks == checks);
    s_allowed = true; activate(instance); activate(instance);
    assert(s_checks == checks + (preview ? 0 : 1));
    attach(views, &parent, ORB_SHELL_UPDATES, &status);
    instance = orb_system_views_current(views, s_surface);
    assert(find(orb_screen_root(instance), preview ? "Check for software from your Mac." : "Checking for updates…"));
    orb_shell_snapshot_t old = status; status.update_check_revision++;
    snprintf(status.update_check_result, sizeof(status.update_check_result), "Up to date");
    orb_system_views_update(views, &status, &old);
    attach(views, &parent, ORB_SHELL_UPDATES, &status);
    assert(find(orb_screen_root(orb_system_views_current(views, s_surface)), "Up to date"));
    old = status; status.net = ORB_SHELL_NET_CONNECTING; orb_system_views_update(views, &status, &old);
    attach(views, &parent, ORB_SHELL_UPDATES, &status);
    assert(find(orb_screen_root(orb_system_views_current(views, s_surface)), "Connect your Mac to check for updates."));
    s_needed = true; s_saving = s_error = s_physical = false;
    attach(views, &parent, ORB_SHELL_ONBOARDING, &status);
    instance = orb_system_views_current(views, s_surface);
    lv_obj_t *list = orb_screen_page_viewport(instance);
    lv_obj_update_layout(list); lv_obj_scroll_to_y(list, 100, LV_ANIM_OFF);
    unsigned done = s_done, requests = s_requests;
    activate(instance); assert(s_requests == requests && s_done == done + (preview ? 0 : 1));
    /* Ordinary departure forgets tutorial scroll; it does not forget a save. */
    attach(views, &parent, ORB_SHELL_ONBOARDING, &status);
    instance = orb_system_views_current(views, s_surface);
    assert(lv_obj_get_scroll_y(orb_screen_page_viewport(instance)) == 0);
    s_physical = true; activate(instance);
    assert(s_requests == requests + (preview ? 0 : 1));
    if (!preview) {
        attach(views, &parent, ORB_SHELL_ONBOARDING, &status);
        assert(find(orb_screen_root(orb_system_views_current(views, s_surface)), "Saving…"));
        s_saving = false; s_error = true; unsigned changed = s_changed;
        orb_system_views_poll(views, true); assert(s_changed == changed + 1);
        attach(views, &parent, ORB_SHELL_ONBOARDING, &status);
        assert(find(orb_screen_root(orb_system_views_current(views, s_surface)), "Not saved"));
        lv_obj_t *retry = find(orb_screen_root(orb_system_views_current(views, s_surface)), "Try again");
        assert(retry); lv_obj_send_event(retry, LV_EVENT_CLICKED, NULL);
        attach(views, &parent, ORB_SHELL_UPDATES, &status);
        s_saving = s_needed = false; done = s_done;
        orb_system_views_poll(views, false); assert(s_done == done);
        attach(views, &parent, ORB_SHELL_ONBOARDING, &status);
        orb_system_views_poll(views, true); assert(s_done == done + 1);
    }
    lv_obj_delete(parent); assert(!orb_system_views_current(views, s_surface));
    orb_system_views_destroy(views); s_saving = s_needed = s_error = false; quiescent();
}
static size_t reserve_remaining(void)
{
    orb_memory_stats_t stats; orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t size = stats.budget - stats.used; assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, size)); return size;
}
static void admission(void)
{
    size_t reserve = reserve_remaining(); assert(!orb_system_views_create(false, event, NULL));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    orb_system_views_t *views = orb_system_views_create(false, event, NULL); assert(views);
    lv_obj_t *parent = page(); orb_shell_snapshot_t status = {0};
    reserve = reserve_remaining(); assert(!orb_system_views_attach(views, parent, ORB_SHELL_ONBOARDING, &status));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_system_views_attach(views, parent, ORB_SHELL_ONBOARDING, &status));
    orb_system_views_detach(views, parent);
    reserve = reserve_remaining(); assert(!orb_system_views_attach(views, parent, ORB_SHELL_UPDATES, &status));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_system_views_attach(views, parent, ORB_SHELL_UPDATES, &status));
    orb_system_views_destroy(views); lv_obj_delete(parent); quiescent();
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); assert(sim_display_create()); orb_ui_init(NULL);
    flows(false); flows(true); settle();
    s_workspace_baseline = used(); s_timer_baseline = timers(); s_warming = false;
    lifecycle(orb_onboarding_descriptor()); lifecycle(orb_updates_descriptor());
    flows(false); flows(true); admission();
    puts("system_views: lifecycle, contexts, preview, service latches, copied projections and admission passed");
    return 0;
}
