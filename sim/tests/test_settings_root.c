#include "orb_app.h"
#include "orb_app_settings_root.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static uint32_t s_page;
static int32_t s_scroll;
static orb_intent_t s_last_intent;
static orb_screen_update_t s_initial;

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    s_intents++;
    s_last_intent = *value;
    return true;
}

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

static void quiescent(void)
{
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

static void exercise(orb_screen_instance_t *instance)
{
    lv_obj_t *list = orb_app_settings_root_scroll(instance);
    lv_obj_update_layout(orb_screen_root(instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    s_scroll = LV_MIN(48, maximum);
    if (!orb_round_list_scroll_to(list, s_scroll)) lv_obj_scroll_to_y(list, s_scroll, LV_ANIM_OFF);
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    if (s_page == ORB_SETTINGS_GLASS_PAGE) {
        assert(!orb_app_settings_glass_select(instance, 1, ORB_INPUT_CROWN, true));
        assert(!orb_app_settings_glass_select(instance, 1, ORB_INPUT_TOUCH, false));
        assert(!orb_app_settings_glass_select(instance, 3, ORB_INPUT_TOUCH, true));
        for (unsigned i = 0; i < 3; i++) {
            assert(orb_app_settings_glass_select(instance, i, ORB_INPUT_TOUCH, true));
            assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
            assert(s_last_intent.value.setting.key == ORB_SETTINGS_SETTING_GLASS);
            assert(s_last_intent.value.setting.value == (int32_t)i);
        }
        return;
    }
    static const orb_screen_id_t s_routes[] = {9, 21, 10, ORB_SCREEN_INVALID_ID, 11, 17, 15, 19, 7, 29, 8};
    assert(!orb_app_settings_root_activate(instance, ORB_SETTINGS_ITEM_MAC, ORB_INPUT_TOUCH, false, NULL));
    for (unsigned i = 0; i < ORB_SETTINGS_ITEM_COUNT; i++) {
        bool physical = i == ORB_SETTINGS_ITEM_MAC;
        assert(orb_app_settings_root_activate(instance, i, ORB_INPUT_TOUCH, physical, NULL));
        if (i == ORB_SETTINGS_ITEM_MAC) {
            assert(s_last_intent.kind == ORB_INTENT_APP_COMMAND);
            assert(s_last_intent.value.command.action == ORB_SETTINGS_COMMAND_CONNECTION);
        } else {
            assert(s_last_intent.kind == ORB_INTENT_NAVIGATE);
            assert(s_last_intent.value.navigate.screen == s_routes[i]);
            assert(s_last_intent.value.navigate.args.page == (uint32_t)(i == ORB_SETTINGS_ITEM_GLASS));
        }
    }
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(orb_app_settings_root_page(instance) == s_page);
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, s_page == ORB_SETTINGS_GLASS_PAGE ? "Glass" : "Settings"));
    uint32_t expected = phase == ORB_LIFECYCLE_INITIAL || phase == ORB_LIFECYCLE_IDEMPOTENT ? 1 :
        phase == ORB_LIFECYCLE_CHANGED || phase == ORB_LIFECYCLE_RESTORED ? 2 : 0;
    assert(orb_app_settings_glass_preference(instance) == expected);
    lv_obj_t *list = orb_app_settings_root_scroll(instance);
    assert(list && lv_obj_is_scrollable(list));
    if (phase >= ORB_LIFECYCLE_RESTORED) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_scroll, maximum));
    }
}

static void disabled_and_preview(orb_screen_host_t *host)
{
    orb_settings_root_model_t value = {.glass_preference = 1};
    orb_screen_model_t projection = {&value, sizeof(value)};
    orb_screen_update_t update = {.model = &projection, .revisions.settings = 1};
    orb_screen_instance_t instance = {0};
    unsigned before = s_intents;
    for (unsigned page = 0; page < 2; page++) {
        orb_screen_args_t args = {.page = page};
        assert(orb_screen_create(&instance, host, orb_app_settings_root_descriptor(), lv_screen_active(), &args, false));
        assert(orb_screen_update(&instance, &update));
        if (page) assert(!orb_app_settings_glass_select(&instance, 0, ORB_INPUT_TOUCH, true));
        else assert(!orb_app_settings_root_activate(&instance, ORB_SETTINGS_ITEM_UNITS, ORB_INPUT_TOUCH, false, NULL));
        orb_screen_destroy(&instance);
        quiescent();
    }
    value.interactive = true;
    assert(orb_screen_create(&instance, host, orb_app_settings_root_descriptor(), lv_screen_active(), NULL, true));
    assert(orb_screen_update(&instance, &update));
    assert(!orb_app_settings_root_activate(&instance, ORB_SETTINGS_ITEM_UNITS, ORB_INPUT_TOUCH, true, NULL));
    orb_screen_destroy(&instance);
    assert(s_intents == before);
    quiescent();
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_SETTINGS_ROOT_SCREEN_ID);
    *update = s_initial;
    return true;
}

static void scroll_began(lv_event_t *event)
{
    unsigned *begins = lv_event_get_user_data(event);
    (*begins)++;
}

static void initial_projection_no_scroll(orb_screen_host_t *host)
{
    unsigned begins = 0;
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466);
    lv_obj_set_scrollable(parent, false);
    lv_obj_add_event_cb(parent, scroll_began, LV_EVENT_SCROLL_BEGIN, &begins);
    host->config.project = project;
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_settings_root_descriptor(), parent, NULL, false));
    /* Create prepared the projected preference before the first update. */
    assert(orb_app_settings_glass_preference(&instance) == 1);
    lv_obj_t *list = orb_app_settings_root_scroll(&instance);
    assert(orb_screen_page_viewport(&instance) == list);
    assert(orb_screen_page_title(&instance));
    assert(orb_screen_update(&instance, &s_initial));
    assert(orb_app_settings_root_scroll(&instance) == list);
    orb_screen_update_t equal = s_initial;
    equal.revisions.settings++;
    assert(orb_screen_update(&instance, &equal));
    assert(orb_app_settings_root_scroll(&instance) == list);
    assert(lv_obj_get_scroll_y(list) == 0 && begins == 0);
    orb_screen_destroy(&instance);
    lv_obj_delete(parent);
    host->config.project = NULL;
    quiescent();
}

static void router_context(orb_screen_host_t *host)
{
    const orb_screen_descriptor_t *screens[] = {orb_app_settings_root_descriptor()};
    const orb_app_manifest_t app = {.id = 8, .name = "Settings", .screens = screens,
        .screen_count = 1, .entry = ORB_SETTINGS_ROOT_SCREEN_ID};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &app, NULL));
    orb_router_t router;
    host->config.project = project;
    assert(orb_router_init(&router, &registry, host, lv_screen_active(), ORB_SETTINGS_ROOT_SCREEN_ID, NULL));
    s_page = ORB_SETTINGS_ROOT_PAGE;
    exercise(router.current);
    assert(orb_router_open(&router, ORB_SETTINGS_ROOT_SCREEN_ID,
        &(orb_screen_args_t){.page = ORB_SETTINGS_GLASS_PAGE}, NULL, true));
    assert(orb_app_settings_root_page(router.current) == ORB_SETTINGS_GLASS_PAGE);
    assert(orb_router_back(&router, true) == ORB_BACK_LEAVE_SCREEN);
    assert(orb_app_settings_root_page(router.current) == ORB_SETTINGS_ROOT_PAGE);
    assert(lv_obj_get_scroll_y(orb_app_settings_root_scroll(router.current)) == s_scroll);
    orb_router_destroy(&router);
    host->config.project = NULL;
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_ROOT_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});
    orb_settings_root_model_t first = {.hub_name = "Test Mac", .firmware_version = "0.5.2",
        .glass_preference = 1, .sound_on = true, .interactive = true};
    orb_settings_root_model_t changed = {.hub_name = "Another Mac", .firmware_version = "0.5.3",
        .glass_preference = 2, .interactive = true};
    orb_settings_root_model_t removed = {.interactive = true};
    orb_screen_model_t models[] = {{&first, sizeof(first)}, {&changed, sizeof(changed)}, {&removed, sizeof(removed)}};
    s_initial = (orb_screen_update_t){.model = &models[0], .revisions.settings = 1};
    orb_screen_update_t next = {.model = &models[1], .revisions.settings = 2};
    orb_screen_update_t missing = {.model = &models[2], .revisions.settings = 3};
    orb_screen_update_t empty = {.revisions.settings = 4};
    orb_screen_instance_t warm = {0};
    for (unsigned page = 0; page < 2; page++) {
        orb_screen_args_t args = {.page = page};
        assert(orb_screen_create(&warm, &host, orb_app_settings_root_descriptor(), lv_screen_active(), &args, false));
        assert(orb_screen_update(&warm, &s_initial));
        orb_screen_destroy(&warm);
    }
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();
    for (s_page = 0; s_page < 2; s_page++) {
        orb_screen_args_t args = {.page = s_page};
        orb_lifecycle_case_t test = {.descriptor = orb_app_settings_root_descriptor(), .args = &args,
            .initial = &s_initial, .changed = &next, .removed = &missing, .empty = &empty,
            .exercise = exercise, .verify = verify, .quiescent = quiescent};
        unsigned before = s_intents;
        orb_test_screen_lifecycle(&host, &test);
        assert(s_intents == before + (s_page ? 3u : ORB_SETTINGS_ITEM_COUNT));
    }
    disabled_and_preview(&host);
    initial_projection_no_scroll(&host);
    router_context(&host);
    puts("Settings root/Glass: lifecycle, context, typed routes/commands/settings and cleanup passed");
    return 0;
}
