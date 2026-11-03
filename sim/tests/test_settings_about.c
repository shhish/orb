/* Production Settings descriptors exercised through the shared lifetime driver.
 * Interactions address semantic actions and the published scroll viewport. */
#include "orb_app_settings.h"
#include "orb_app_settings_units.h"
#include "orb_app_settings_root.h"
#include "orb_app_settings_registry.h"
#include "orb_kit.h"
#include "settings_host_fixture.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static orb_intent_t s_last_intent;
static uint32_t s_page;
static int32_t s_saved_scroll;
static orb_screen_update_t s_initial;

static unsigned timer_count(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) count++;
    return count;
}

static size_t workspace_used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    s_intents++;
    s_last_intent = *value;
    return true;
}

static void quiescent(void)
{
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

static void exercise(orb_screen_instance_t *instance)
{
    lv_obj_t *list = orb_app_settings_about_scroll(instance);
    lv_obj_update_layout(orb_screen_root(instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    s_saved_scroll = LV_MIN(24, maximum);
    if (!orb_round_list_scroll_to(list, s_saved_scroll))
        lv_obj_scroll_to_y(list, s_saved_scroll, LV_ANIM_OFF);
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    unsigned before = s_intents;
    assert(!orb_app_settings_about_open_licences(instance, ORB_INPUT_PROGRAMMATIC, false, NULL));
    bool opened = orb_app_settings_about_open_licences(instance, ORB_INPUT_TOUCH, false, NULL);
    assert(opened == (s_page == ORB_SETTINGS_ABOUT_PAGE));
    assert(s_intents == before + opened);
    if (opened) {
        assert(s_last_intent.kind == ORB_INTENT_NAVIGATE);
        assert(s_last_intent.value.navigate.screen == ORB_SETTINGS_ABOUT_SCREEN_ID);
        assert(s_last_intent.value.navigate.args.page == ORB_SETTINGS_LICENCES_PAGE);
    }
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(orb_app_settings_about_page(instance) == s_page);
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, s_page == ORB_SETTINGS_LICENCES_PAGE ? "Licenses" : "About"));
    lv_obj_t *list = orb_app_settings_about_scroll(instance);
    assert(list && lv_obj_is_scrollable(list));
    if (phase >= ORB_LIFECYCLE_RESTORED) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_saved_scroll, maximum));
    }
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_SETTINGS_ABOUT_SCREEN_ID);
    *update = s_initial;
    return true;
}

static void router_context(orb_screen_host_t *host)
{
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_settings_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_SETTINGS_ABOUT_SCREEN_ID) == orb_app_settings_about_descriptor());
    orb_router_t router;
    host->config.project = project;
    assert(orb_router_init(&router, &registry, host, lv_screen_active(), ORB_SETTINGS_ABOUT_SCREEN_ID, NULL));
    s_page = ORB_SETTINGS_ABOUT_PAGE;
    exercise(router.current);
    assert(orb_router_open(&router, ORB_SETTINGS_ABOUT_SCREEN_ID,
        &(orb_screen_args_t){.page = ORB_SETTINGS_LICENCES_PAGE}, NULL, true));
    assert(orb_app_settings_about_page(router.current) == ORB_SETTINGS_LICENCES_PAGE);
    assert(orb_router_back(&router, true) == ORB_BACK_LEAVE_SCREEN);
    verify(router.current, ORB_LIFECYCLE_RESTORED);
    orb_router_destroy(&router);
    host->config.project = NULL;
    quiescent();
}

static void host_changed(void *context)
{
    unsigned *changes = context;
    (*changes)++;
}

static lv_obj_t *host_parent(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466);
    return parent;
}

static void compatibility_host(void)
{
    unsigned changes = 0;
    orb_settings_host_t *host = orb_settings_host_create(false, host_changed, NULL, &changes);
    assert(host);
    orb_shell_snapshot_t status = {0};
    snprintf(status.firmware_version, sizeof(status.firmware_version), "0.5.2");
    lv_obj_t *parent = host_parent();
    assert(settings_show_about(host, parent, &status, "Connected", true));
    orb_screen_instance_t *instance = orb_settings_host_current(host);
    lv_obj_t *list = orb_app_settings_about_scroll(instance);
    lv_obj_scroll_to_y(list, 24, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(list) == 24);

    /* BOOT can arrive before the deferred route has built its destination.
     * It unwinds that local intent rather than leaving the outer About route. */
    assert(orb_app_settings_about_open_licences(instance, ORB_INPUT_TOUCH, false, NULL));
    assert(orb_settings_host_back_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID));
    assert(!orb_settings_host_back_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID));
    assert(changes == 2);
    assert(settings_show_about(host, parent, &status, "Connected", true));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == 0);
    instance = orb_settings_host_current(host);
    assert(lv_obj_get_scroll_y(orb_app_settings_about_scroll(instance)) == 24);

    orb_settings_host_t *preview = NULL;
    assert(orb_settings_host_clone(&preview, host, true));
    lv_obj_t *preview_parent = host_parent();
    assert(settings_show_about(preview, preview_parent, &status, "Connected", true));
    assert(!orb_app_settings_about_open_licences(orb_settings_host_current(preview), ORB_INPUT_TOUCH, true, NULL));
    assert(lv_obj_get_scroll_y(orb_app_settings_about_scroll(instance)) == 24);
    orb_settings_host_destroy(preview);
    lv_obj_delete(preview_parent);

    assert(orb_app_settings_about_open_licences(instance, ORB_INPUT_TOUCH, false, NULL));
    assert(settings_show_about(host, parent, &status, "Connected", true));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == 1);
    lv_obj_scroll_to_y(orb_settings_host_list(host), 48, LV_ANIM_OFF);
    assert(orb_settings_host_detach(host));
    lv_obj_delete(parent);
    parent = host_parent();
    assert(settings_show_about(host, parent, &status, "Connected", true));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == 1);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 48);
    assert(orb_settings_host_back_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID));
    assert(settings_show_about(host, parent, &status, "Connected", true));
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 24);
    instance = orb_settings_host_current(host);
    assert(orb_app_settings_about_open_licences(instance, ORB_INPUT_TOUCH, false, NULL));
    assert(settings_show_about(host, parent, &status, "Connected", true));
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 0);
    /* Each outer route retains its own local args after another migrated
     * screen uses the same host. About must return to licences, not root. */
    orb_model_snapshot_t units = {.online = true, .synced = true};
    assert(settings_show_units(host, parent, ORB_SETTINGS_UNITS_SCREEN_ID, &units, ""));
    assert(orb_screen_descriptor(orb_settings_host_current(host))->id == ORB_SETTINGS_UNITS_SCREEN_ID);
    assert(settings_show_units(host, parent, ORB_SETTINGS_TEMPERATURE_SCREEN_ID, &units, ""));
    assert(settings_show_about(host, parent, &status, "Connected", true));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == ORB_SETTINGS_LICENCES_PAGE);
    /* Parent deletion disposes both the moving body and its external chrome. */
    lv_obj_delete(parent);
    assert(!orb_screen_alive(orb_settings_host_current(host)));
    orb_settings_host_destroy(host);
    quiescent();
}

typedef struct { unsigned count; orb_intent_t value; } setting_sink_t;

static bool accept_setting(void *context, const orb_intent_t *value)
{
    setting_sink_t *sink = context;
    sink->count++;
    sink->value = *value;
    return true;
}

static void pending_back_cross_screen_commit(void)
{
    setting_sink_t sink = {0};
    orb_settings_host_t *host = orb_settings_host_create(false, NULL, accept_setting, &sink);
    assert(host);
    orb_shell_snapshot_t status = {.firmware_version = "0.5.2", .device_id = "orb-test", .hub_name = "Test Mac"};
    lv_obj_t *old_parent = host_parent();
    assert(settings_show_about(host, old_parent, &status, "Connected", true));
    lv_obj_scroll_to_y(orb_settings_host_list(host), 24, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 24);
    assert(orb_app_settings_about_open_licences(orb_settings_host_current(host), ORB_INPUT_TOUCH, true, NULL));
    assert(settings_show_about(host, old_parent, &status, "Connected", true));
    assert(orb_app_settings_about_page(orb_settings_host_current(host)) == ORB_SETTINGS_LICENCES_PAGE);
    lv_obj_scroll_to_y(orb_settings_host_list(host), 48, LV_ANIM_OFF);

    /* Back is queued, with the Licences presentation still alive. Before its
     * local replacement happens, an outer route transaction commits Settings. */
    assert(orb_settings_host_back_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == ORB_SETTINGS_ABOUT_PAGE);
    assert(orb_settings_host_stage_root(host, lv_screen_active(), &status, false));
    lv_obj_t *new_parent = host_parent();
    assert(orb_settings_host_commit_staged(host, new_parent));
    orb_screen_instance_t *instance = orb_settings_host_current(host);
    assert(orb_screen_descriptor(instance)->id == ORB_SETTINGS_ROOT_SCREEN_ID);
    assert(orb_app_settings_root_page(instance) == ORB_SETTINGS_ROOT_PAGE);
    assert(orb_settings_host_detach_page(host, old_parent));
    lv_obj_delete(old_parent);
    assert(orb_screen_alive(instance));

    bool remembered_about_root = orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == ORB_SETTINGS_ABOUT_PAGE;
    bool glass_usable = orb_app_settings_root_activate(instance, ORB_SETTINGS_ITEM_GLASS, ORB_INPUT_TOUCH, true, NULL);
    if (!remembered_about_root || !glass_usable)
        fprintf(stderr, "pending About Back/cross commit: About root=%d, Glass accepted=%d\n",
            remembered_about_root, glass_usable);
    assert(remembered_about_root && glass_usable);
    assert(settings_show_root(host, new_parent, &status, false));
    instance = orb_settings_host_current(host);
    assert(orb_app_settings_root_page(instance) == ORB_SETTINGS_GLASS_PAGE);
    assert(orb_app_settings_glass_select(instance, 2, ORB_INPUT_TOUCH, true));
    assert(sink.value.kind == ORB_INTENT_SETTING_SET);
    assert(sink.value.value.setting.key == ORB_SETTINGS_SETTING_GLASS && sink.value.value.setting.value == 2);
    assert(sink.count == 3); /* Licences, Glass, explicit preference request. */

    assert(orb_settings_host_back_for(host, ORB_SETTINGS_ROOT_SCREEN_ID));
    assert(settings_show_root(host, new_parent, &status, false));
    assert(settings_show_about(host, new_parent, &status, "Connected", true));
    instance = orb_settings_host_current(host);
    assert(orb_app_settings_about_page(instance) == ORB_SETTINGS_ABOUT_PAGE);
    /* Saving the outgoing Licences tree under queued page0 would incorrectly
     * resurrect its page/48px scroll here. Preserve the root's original record. */
    assert(lv_obj_get_scroll_y(orb_app_settings_about_scroll(instance)) == 24);
    orb_settings_host_destroy(host);
    lv_obj_delete(new_parent);
    quiescent();
}

static void synthetic_setting_policy(void)
{
    setting_sink_t sink = {0};
    orb_settings_host_t *host = orb_settings_host_create(false, NULL, accept_setting, &sink);
    lv_obj_t *parent = host_parent();
    orb_model_snapshot_t model = {.online = true, .synced = true};
    assert(settings_show_units(host, parent, ORB_SETTINGS_TEMPERATURE_SCREEN_ID, &model, ""));
    orb_screen_instance_t *instance = orb_settings_host_current(host);
    assert(orb_app_settings_temperature_select(instance, ORB_SETTINGS_TEMPERATURE_F, ORB_INPUT_TOUCH, false));
    assert(sink.count == 1 && sink.value.kind == ORB_INTENT_SETTING_SET && !sink.value.physical);
    assert(sink.value.value.setting.key == ORB_SETTINGS_SETTING_TEMPERATURE);
    assert(!orb_screen_emit(instance, &(orb_intent_t){.kind = ORB_INTENT_SETTING_SET,
        .origin = ORB_INPUT_TOUCH, .value.setting.key = 590}));
    assert(!orb_screen_emit(instance, &(orb_intent_t){.kind = ORB_INTENT_APPROVE, .origin = ORB_INPUT_TOUCH}));
    assert(sink.count == 1);
    orb_settings_host_t *preview = NULL;
    assert(orb_settings_host_clone(&preview, host, true));
    lv_obj_t *preview_parent = host_parent();
    assert(settings_show_units(preview, preview_parent, ORB_SETTINGS_TEMPERATURE_SCREEN_ID, &model, ""));
    assert(!orb_app_settings_temperature_select(orb_settings_host_current(preview),
        ORB_SETTINGS_TEMPERATURE_F, ORB_INPUT_TOUCH, true));
    orb_settings_host_destroy(preview);
    lv_obj_delete(preview_parent);
    orb_settings_host_destroy(host);
    lv_obj_delete(parent);
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_ABOUT_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});
    orb_settings_about_model_t first = {.firmware_version = "0.5.2", .device_id = "orb-test",
        .hub_name = "Test Mac", .connection_text = "Connected", .ready = true};
    orb_settings_about_model_t changed = {.firmware_version = "0.5.3", .device_id = "orb-test",
        .connection_text = "Mac offline"};
    orb_settings_about_model_t empty = {0};
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t empty_model = {&empty, sizeof(empty)};
    s_initial = (orb_screen_update_t){.revisions.device = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.device = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.device = 3, .model = &empty_model};
    orb_screen_update_t missing = {.revisions.device = 4};
    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_settings_about_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&warm, &s_initial));
    orb_screen_destroy(&warm);
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();

    for (s_page = ORB_SETTINGS_ABOUT_PAGE; s_page <= ORB_SETTINGS_LICENCES_PAGE; s_page++) {
        orb_screen_args_t args = {.page = s_page};
        orb_lifecycle_case_t test = {.descriptor = orb_app_settings_about_descriptor(), .args = &args,
            .initial = &s_initial, .changed = &next, .removed = &removed, .empty = &missing,
            .exercise = exercise, .verify = verify, .quiescent = quiescent};
        unsigned before = s_intents;
        orb_test_screen_lifecycle(&host, &test);
        assert(s_intents == before + (s_page == ORB_SETTINGS_ABOUT_PAGE));
    }
    router_context(&host);
    compatibility_host();
    pending_back_cross_screen_commit();
    synthetic_setting_policy();
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_settings_about_descriptor(), lv_screen_active(), NULL, true));
    assert(!orb_app_settings_about_open_licences(&preview, ORB_INPUT_TOUCH, true, NULL));
    orb_screen_destroy(&preview);
    quiescent();
    puts("Settings About/licences: lifecycle, value context, routing and typed intents passed");
    return 0;
}
