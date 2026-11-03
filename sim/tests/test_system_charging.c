/* Battery presentation retains no device state or command in its context.
 * Exercise the production descriptor and host through the common lifecycle. */
#include "system/charging_screen.h"
#include "orb_app.h"
#include "orb_icons.h"
#include "orb_memory.h"
#include "orb_screen_lifecycle.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit_controls_motion.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t s_now = 100000;
static unsigned s_intents, s_cues, s_timer_baseline;
static size_t s_workspace_baseline;
static orb_charging_model_t s_initial = {.battery_pct = -1};
static orb_charging_model_t s_changed = {.battery_pct = 42, .usb = true, .charging = true};
static orb_charging_model_t s_removed = {.battery_pct = 0};
static orb_screen_model_t s_initial_view = {&s_initial, sizeof(s_initial)};

static uint32_t tick(void) { return s_now; }

static void settle(void)
{
    for (unsigned elapsed = 0; elapsed < 1600; elapsed += 16) {
        s_now += 16;
        lv_timer_handler();
    }
}

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

static void quiescent(void)
{
    settle();
    assert(used() == s_workspace_baseline);
    assert(timers() == s_timer_baseline);
    assert(!s_intents && !s_cues);
}

static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class)) {
        /* Telemetry assertions read the true target while its ring Follows. */
        const char *value = lv_label_get_text(root);
        (void)orb_kit_ring_semantic_text(root, &value);
        if (!strcmp(value, text)) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context; (void)value;
    s_intents++;
    return true;
}

static void cue(void *context, orb_feedback_role_t role)
{
    (void)context; (void)role;
    s_cues++;
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_SHELL_CHARGING);
    *update = (orb_screen_update_t){.revisions.device = 1, .revisions.time = 1, .model = &s_initial_view};
    return true;
}

static void assert_state(orb_screen_instance_t *instance, const char *number,
    const char *caption, const char *glyph)
{
    lv_obj_t *root = orb_screen_root(instance);
    assert(root && find(root, number) && find(root, caption));
    if (glyph) assert(find(root, glyph));
    assert(!orb_screen_rim_acquire(instance));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Battery") && chrome.inset_ring);
    assert(!s_intents && !s_cues);
}

static void exercise(orb_screen_instance_t *instance)
{
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    assert(!orb_screen_rim_acquire(instance));
    /* A context has no battery reading, input handle or pending command. */
    orb_screen_context_t context = {0};
    assert(orb_charging_descriptor()->context_bytes == sizeof(context));
    assert(orb_screen_save(instance, &context, sizeof(context)));
    assert(context.version == 1 && context.size == sizeof(context));
    context.version++;
    assert(!orb_screen_restore(instance, &context, sizeof(context)));
    assert_state(instance, "42%", "Charging", ORB_ICON_LIGHTNING_FILL);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    if (phase == ORB_LIFECYCLE_CHANGED || phase == ORB_LIFECYCLE_RESTORED)
        assert_state(instance, "42%", "Charging", ORB_ICON_LIGHTNING_FILL);
    else if (phase == ORB_LIFECYCLE_REMOVED_ID || phase == ORB_LIFECYCLE_EMPTY)
        assert_state(instance, "0%", "Low battery", ORB_ICON_BATTERY_LOW);
    else
        assert_state(instance, "—", "Battery unavailable", ORB_ICON_BATTERY_WARNING);
}

static void lifecycle(void)
{
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){
        .project = project, .intent = intent, .cue = cue, .defer_rim = true});
    orb_screen_model_t changed_view = {&s_changed, sizeof(s_changed)};
    orb_screen_model_t removed_view = {&s_removed, sizeof(s_removed)};
    orb_screen_update_t initial = {.revisions.device = 1, .revisions.time = 1, .model = &s_initial_view};
    orb_screen_update_t changed = {.revisions.device = 2, .revisions.time = 2, .model = &changed_view};
    orb_screen_update_t removed = {.revisions.device = 3, .revisions.time = 3, .model = &removed_view};
    orb_screen_update_t empty = {.revisions.device = 4, .revisions.time = 4};
    orb_lifecycle_case_t test = {.descriptor = orb_charging_descriptor(),
        .initial = &initial, .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(&host, &test);
}

static lv_obj_t *page(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 466, 466);
    lv_obj_set_scrollable(root, false);
    return root;
}

static void presentation(void)
{
    orb_app_host_t *host = orb_charging_host_create(false);
    lv_obj_t *parent = page();
    orb_charging_model_t model = s_initial;
    assert(host && !orb_charging_host_update(host, &model));
    assert(!orb_charging_host_attach(host, parent, NULL));
    assert(orb_charging_host_attach(host, parent, &model));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    lv_obj_t *root = orb_screen_root(instance);
    assert_state(instance, "—", "Battery unavailable", ORB_ICON_BATTERY_WARNING);
    assert(!orb_charging_host_update(host, NULL));
    orb_screen_context_t context = {0};
    assert(orb_screen_save(instance, &context, sizeof(context)));

    model = (orb_charging_model_t){.battery_pct = 9};
    assert(orb_charging_host_update(host, &model));
    assert_state(instance, "9%", "Low battery", ORB_ICON_BATTERY_LOW);
    lv_obj_t *number = find(root, "9%");
    size_t reservation = used();
    assert(orb_charging_host_update(host, &model));
    assert(find(root, "9%") == number && used() == reservation);
    model = s_changed;
    assert(orb_charging_host_update(host, &model));
    assert_state(instance, "42%", "Charging", ORB_ICON_LIGHTNING_FILL);
    model = (orb_charging_model_t){.battery_pct = 100, .usb = true};
    assert(orb_charging_host_update(host, &model));
    assert_state(instance, "100%", "Fully charged", ORB_ICON_PLUG);
    model.battery_pct = 80;
    assert(orb_charging_host_update(host, &model));
    assert_state(instance, "80%", "Not charging", ORB_ICON_PLUG);
    lv_obj_t *icon = find(root, ORB_ICON_PLUG);
    model.usb = false;
    assert(orb_charging_host_update(host, &model));
    assert_state(instance, "80%", "On battery", NULL);
    assert(lv_obj_is_hidden(icon));
    assert(orb_screen_restore(instance, &context, sizeof(context)));
    assert_state(instance, "80%", "On battery", NULL);
    assert(orb_screen_root(instance) == root);

    /* The host owns its projection even after the caller reuses its value. */
    model.battery_pct = -1;
    orb_app_host_poll(host);
    assert_state(instance, "80%", "On battery", NULL);
    orb_app_host_detach_page(host, parent);
    assert(!orb_app_host_current(host));
    lv_obj_delete(parent);
    parent = page();
    assert(orb_charging_host_attach(host, parent, &model));
    assert_state(orb_app_host_current(host), "—", "Battery unavailable", ORB_ICON_BATTERY_WARNING);
    lv_obj_delete(parent);
    assert(!orb_app_host_current(host));
    orb_app_host_destroy(host);
    quiescent();
}

static void preview(void)
{
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project,
        .intent = intent, .cue = cue, .defer_rim = true, .preview = true});
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, &host, orb_charging_descriptor(), lv_screen_active(), NULL, true));
    assert_state(&instance, "—", "Battery unavailable", ORB_ICON_BATTERY_WARNING);
    lv_obj_send_event(orb_screen_root(&instance), LV_EVENT_CLICKED, NULL);
    orb_screen_visibility(&instance, ORB_VISIBILITY_ACTIVE);
    assert(orb_screen_visibility_get(&instance) == ORB_VISIBILITY_SUSPENDED);
    assert(orb_screen_back(&instance) == ORB_BACK_CONSUMED);
    orb_screen_destroy(&instance);
    quiescent();

    orb_app_host_t *production = orb_charging_host_create(true);
    lv_obj_t *parent = page();
    assert(production && orb_charging_host_attach(production, parent, &s_changed));
    assert_state(orb_app_host_current(production), "42%", "Charging", ORB_ICON_LIGHTNING_FILL);
    assert(orb_charging_host_update(production, &s_initial));
    assert_state(orb_app_host_current(production), "—", "Battery unavailable", ORB_ICON_BATTERY_WARNING);
    assert(!orb_app_host_rim(production));
    orb_app_host_destroy(production);
    lv_obj_delete(parent);
    quiescent();
}

static size_t reserve_remaining(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    size_t reserve = stats.budget - stats.used;
    assert(orb_memory_reserve(ORB_MEMORY_WORKSPACE, reserve));
    return reserve;
}

static void admission(void)
{
    size_t reserve = reserve_remaining();
    assert(!orb_charging_host_create(false));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    orb_app_host_t *host = orb_charging_host_create(false);
    lv_obj_t *parent = page();
    assert(host);
    reserve = reserve_remaining();
    assert(!orb_charging_host_attach(host, parent, &s_initial));
    assert(!orb_app_host_current(host));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_charging_host_attach(host, parent, &s_initial));
    assert_state(orb_app_host_current(host), "—", "Battery unavailable", ORB_ICON_BATTERY_WARNING);
    orb_app_host_destroy(host);
    lv_obj_delete(parent);
    quiescent();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    assert(sim_display_create());
    orb_ui_init(NULL);
    const orb_screen_descriptor_t *descriptor = orb_charging_descriptor();
    assert(orb_screen_descriptor_valid(descriptor, NULL) && descriptor->id == ORB_SHELL_CHARGING);
    const orb_screen_descriptor_t *screens[] = {descriptor};
    orb_app_manifest_t manifest = {.id = descriptor->id, .name = "Battery",
        .screens = screens, .screen_count = 1, .entry = descriptor->id};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &manifest, NULL));
    assert(orb_screen_registry_find(&registry, descriptor->id) == descriptor);
    orb_app_host_t *warm = orb_charging_host_create(false);
    lv_obj_t *parent = page();
    assert(warm && orb_charging_host_attach(warm, parent, &s_initial));
    orb_app_host_destroy(warm);
    lv_obj_delete(parent);
    settle();
    s_workspace_baseline = used();
    s_timer_baseline = timers();
    lifecycle();
    presentation();
    preview();
    admission();
    puts("system_charging: lifecycle, live projection, empty context, preview and admission passed");
    return 0;
}
