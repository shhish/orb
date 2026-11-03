#include "orb_app.h"
#include "orb_app_settings_units.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>

static unsigned s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static bool s_temperature;
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
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    lv_obj_t *list = orb_app_settings_units_scroll(instance);
    lv_obj_update_layout(orb_screen_root(instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    s_scroll = LV_MIN(24, maximum);
    if (!orb_round_list_scroll_to(list, s_scroll)) lv_obj_scroll_to_y(list, s_scroll, LV_ANIM_OFF);
    if (!s_temperature) {
        assert(orb_app_settings_units_open_temperature(instance, ORB_INPUT_TOUCH, false, NULL));
        assert(s_last_intent.kind == ORB_INTENT_NAVIGATE);
        assert(s_last_intent.value.navigate.screen == ORB_SETTINGS_TEMPERATURE_SCREEN_ID);
        assert(!orb_app_settings_temperature_select(instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, true));
        return;
    }
    assert(!orb_app_settings_temperature_select(instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, false));
    assert(!orb_app_settings_temperature_select(instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_CROWN, true));
    /* A crown can adjust numeric settings through the framework, but cannot
     * activate a radio row. The semantic action uses a deliberate tap/button. */
    for (unsigned i = 0; i < 3; i++) {
        assert(orb_app_settings_temperature_select(instance, i, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTINGS_SETTING_TEMPERATURE);
        assert(s_last_intent.value.setting.value == (int32_t)i);
        assert(s_last_intent.physical && s_last_intent.origin == ORB_INPUT_TOUCH);
    }
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    lv_obj_t *list = orb_app_settings_units_scroll(instance);
    assert(list && lv_obj_is_scrollable(list));
    if (s_temperature) {
        assert(orb_app_settings_temperature_enabled(instance) == (phase != ORB_LIFECYCLE_EMPTY));
        assert(orb_app_settings_temperature_selection(instance) ==
            (phase == ORB_LIFECYCLE_CHANGED || phase == ORB_LIFECYCLE_RESTORED ?
                ORB_SETTINGS_TEMPERATURE_C : ORB_SETTINGS_TEMPERATURE_AUTOMATIC));
    }
    if (phase >= ORB_LIFECYCLE_RESTORED) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_scroll, maximum));
    }
}

static void pending_offline_and_results(orb_screen_host_t *host)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_settings_temperature_descriptor(), lv_screen_active(), NULL, false));
    orb_settings_units_model_t value = {.temperature = ORB_TEMPERATURE_F,
        .temperature_owner = true, .ready = true, .prefs_pending = true};
    orb_screen_model_t projection = {&value, sizeof(value)};
    orb_screen_update_t update = {.model = &projection, .revisions.model = 1};
    unsigned before = s_intents;
    assert(orb_screen_update(&instance, &update));
    assert(!orb_app_settings_temperature_enabled(&instance));
    assert(!orb_app_settings_temperature_select(&instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, true));
    value.prefs_pending = false; value.ready = false; update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(!orb_app_settings_temperature_enabled(&instance));
    assert(!orb_app_settings_temperature_select(&instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, true));
    value.ready = true; value.prefs_error[0] = 'x'; update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_temperature_enabled(&instance));
    assert(orb_app_settings_temperature_selection(&instance) == ORB_SETTINGS_TEMPERATURE_F);
    assert(s_intents == before); /* Results and revision updates never resubmit. */
    assert(orb_app_settings_temperature_select(&instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_BUTTON, true));
    assert(s_intents == before + 1);
    orb_screen_visibility(&instance, ORB_VISIBILITY_COVERED);
    assert(!orb_app_settings_temperature_select(&instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, true));
    orb_screen_destroy(&instance);
    quiescent();

    assert(orb_screen_create(&instance, host, orb_app_settings_temperature_descriptor(), lv_screen_active(), NULL, true));
    update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(!orb_app_settings_temperature_select(&instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, true));
    orb_screen_destroy(&instance);
    quiescent();
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_SETTINGS_UNITS_SCREEN_ID || id == ORB_SETTINGS_TEMPERATURE_SCREEN_ID);
    *update = s_initial;
    return true;
}

static void router_context(orb_screen_host_t *host)
{
    const orb_screen_descriptor_t *screens[] = {
        orb_app_settings_units_descriptor(), orb_app_settings_temperature_descriptor(),
    };
    const orb_app_manifest_t app = {.id = 8, .name = "Settings Units", .screens = screens,
        .screen_count = 2, .entry = ORB_SETTINGS_UNITS_SCREEN_ID};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &app, NULL));
    orb_router_t router;
    host->config.project = project;
    assert(orb_router_init(&router, &registry, host, lv_screen_active(), ORB_SETTINGS_UNITS_SCREEN_ID, NULL));
    s_temperature = false;
    exercise(router.current);
    assert(orb_router_open(&router, ORB_SETTINGS_TEMPERATURE_SCREEN_ID, NULL, NULL, true));
    assert(orb_router_back(&router, true) == ORB_BACK_LEAVE_SCREEN);
    assert(router.route.id == ORB_SETTINGS_UNITS_SCREEN_ID);
    verify(router.current, ORB_LIFECYCLE_RESTORED);
    orb_router_destroy(&router);
    host->config.project = NULL;
    quiescent();
}

static orb_screen_update_t s_seed;
static unsigned s_scroll_events;

static bool seed_project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_SETTINGS_UNITS_SCREEN_ID || id == ORB_SETTINGS_TEMPERATURE_SCREEN_ID);
    *update = s_seed;
    return true;
}

static void scrolled(lv_event_t *event)
{
    (void)event;
    s_scroll_events++;
}

static void initial_projection(orb_screen_host_t *host)
{
    orb_settings_units_model_t seed = {.temperature = ORB_TEMPERATURE_C,
        .temperature_owner = true, .ready = true};
    orb_screen_model_t projection = {&seed, sizeof(seed)};
    s_seed = (orb_screen_update_t){.model = &projection, .revisions.model = 100};
    const orb_screen_descriptor_t *descriptors[] = {
        orb_app_settings_units_descriptor(), orb_app_settings_temperature_descriptor(),
    };
    host->config.project = seed_project;
    unsigned before = s_intents;
    for (unsigned i = 0; i < 2; i++) {
        seed.temperature = ORB_TEMPERATURE_C;
        s_seed.revisions.model++;
        orb_screen_instance_t instance = {0};
        assert(orb_screen_create(&instance, host, descriptors[i], lv_screen_active(), NULL, false));
        /* The first prepared tree already owns the host's copied value. */
        assert(orb_app_settings_temperature_selection(&instance) == ORB_SETTINGS_TEMPERATURE_C);
        assert(orb_app_settings_temperature_enabled(&instance) == (i == 1));
        lv_obj_t *viewport = orb_screen_page_viewport(&instance);
        lv_obj_t *title = orb_screen_page_title(&instance);
        assert(viewport == orb_app_settings_units_scroll(&instance) && title);
        lv_obj_add_event_cb(viewport, scrolled, LV_EVENT_SCROLL, NULL);
        s_scroll_events = 0;
        assert(orb_screen_update(&instance, &s_seed));
        s_seed.revisions.model++;
        assert(orb_screen_update(&instance, &s_seed));
        /* Different revision, same model: retained semantic page handles and
         * zero spurious scroll events prove update did not reconstruct it. */
        assert(orb_screen_page_viewport(&instance) == viewport);
        assert(orb_screen_page_title(&instance) == title);
        assert(s_scroll_events == 0);
        seed.temperature = ORB_TEMPERATURE_F;
        s_seed.revisions.model++;
        assert(orb_screen_update(&instance, &s_seed));
        assert(orb_app_settings_temperature_selection(&instance) == ORB_SETTINGS_TEMPERATURE_F);
        assert(s_intents == before);
        orb_screen_destroy(&instance);
        quiescent();
    }
    host->config.project = NULL;
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_UNITS_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});
    orb_settings_units_model_t first = {.temperature = ORB_TEMPERATURE_F, .ready = true};
    orb_settings_units_model_t changed = {.temperature = ORB_TEMPERATURE_C, .temperature_owner = true, .ready = true};
    orb_settings_units_model_t removed = {.ready = true};
    orb_screen_model_t models[] = {{&first, sizeof(first)}, {&changed, sizeof(changed)}, {&removed, sizeof(removed)}};
    s_initial = (orb_screen_update_t){.model = &models[0], .revisions.model = 1};
    orb_screen_update_t next = {.model = &models[1], .revisions.model = 2};
    orb_screen_update_t missing = {.model = &models[2], .revisions.model = 3};
    orb_screen_update_t empty = {.revisions.model = 4};
    const orb_screen_descriptor_t *descriptors[] = {
        orb_app_settings_units_descriptor(), orb_app_settings_temperature_descriptor(),
    };
    for (unsigned i = 0; i < 2; i++) {
        orb_screen_instance_t warm = {0};
        assert(orb_screen_create(&warm, &host, descriptors[i], lv_screen_active(), NULL, false));
        assert(orb_screen_update(&warm, &s_initial));
        orb_screen_destroy(&warm);
    }
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();
    for (unsigned i = 0; i < 2; i++) {
        s_temperature = i == 1;
        orb_lifecycle_case_t test = {.descriptor = descriptors[i], .initial = &s_initial, .changed = &next,
            .removed = &missing, .empty = &empty, .exercise = exercise, .verify = verify, .quiescent = quiescent};
        unsigned before = s_intents;
        orb_test_screen_lifecycle(&host, &test);
        assert(s_intents == before + (s_temperature ? 3u : 1u));
    }
    pending_offline_and_results(&host);
    initial_projection(&host);
    router_context(&host);
    puts("Settings Units/Temperature: lifecycle, projection, disabled intents and routing passed");
    return 0;
}
