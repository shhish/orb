/* Session-source Settings keeps its existing input quirks during extraction.
 * Semantic action APIs/bounds avoid control activation through child indices. */
#include "orb_app_settings_sources.h"
#include "orb_app_settings_registry.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static orb_intent_t s_last_intent;
static unsigned s_intents, s_timer_baseline, s_scroll_begins;
static size_t s_workspace_baseline;
static int32_t s_saved_scroll;
static const orb_screen_update_t *s_projection;
static uint32_t s_now = 10000;
static lv_indev_data_t s_input;

static uint32_t tick(void)
{
    return s_now;
}

static void advance(unsigned milliseconds)
{
    while (milliseconds) {
        unsigned step = LV_MIN(milliseconds, 16u);
        s_now += step;
        milliseconds -= step;
        lv_timer_handler();
    }
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

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    s_intents++;
    s_last_intent = *value;
    return true;
}

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    (void)context;
    assert(screen == ORB_SETTINGS_SOURCES_SCREEN_ID);
    *update = *s_projection;
    return true;
}

static void quiescent(void)
{
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

static bool has_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (has_text(lv_obj_get_child(root, i), text)) return true;
    return false;
}

static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_intents;
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    assert(orb_app_settings_sources_changed(instance) == -1);
    assert(!orb_app_settings_sources_toggle(instance, 0, ORB_INPUT_PROGRAMMATIC, true));
    assert(!orb_app_settings_sources_toggle(instance, 0, ORB_INPUT_CROWN, true));
    assert(!orb_app_settings_sources_toggle(instance, ORB_SETTINGS_SOURCE_COUNT, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(instance) == -1);
    for (unsigned source = 0; source < ORB_SETTINGS_SOURCE_COUNT; source++) {
        assert(orb_app_settings_sources_toggle(instance, source, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTING_SOURCE_CLAUDE + source);
        assert(s_last_intent.value.setting.value == (source == 0));
        assert(s_last_intent.origin == ORB_INPUT_TOUCH && s_last_intent.physical);
        assert(orb_app_settings_sources_changed(instance) == (int)source);
    }
    assert(orb_app_settings_sources_cycle_recent(instance, ORB_INPUT_TOUCH, true));
    assert(s_last_intent.value.setting.key == ORB_SETTING_SOURCE_RECENT_MINUTES);
    assert(s_last_intent.value.setting.value == 0);
    assert(orb_app_settings_sources_changed(instance) == 3);
    assert(s_intents == before + 4);
    lv_obj_t *list = orb_app_settings_sources_scroll(instance);
    lv_obj_update_layout(orb_screen_root(instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    s_saved_scroll = LV_MIN(24, maximum);
    if (!orb_round_list_scroll_to(list, s_saved_scroll))
        lv_obj_scroll_to_y(list, s_saved_scroll, LV_ANIM_OFF);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    lv_obj_t *list = orb_app_settings_sources_scroll(instance);
    assert(list == orb_screen_page_viewport(instance));
    assert(orb_screen_page_title(instance));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Sessions"));
    assert(orb_app_settings_sources_requests_enabled(instance) == (phase <= ORB_LIFECYCLE_RESTORED));
    if (phase <= ORB_LIFECYCLE_IDEMPOTENT) {
        assert(orb_app_settings_sources_switch_checked(instance, 0));
        assert(orb_app_settings_sources_switch_checked(instance, 1));
        assert(!orb_app_settings_sources_switch_checked(instance, 2));
        assert(has_text(orb_screen_root(instance), "10 min"));
    } else if (phase <= ORB_LIFECYCLE_RESTORED) {
        assert(!orb_app_settings_sources_switch_checked(instance, 0));
        assert(orb_app_settings_sources_switch_checked(instance, 1));
        assert(orb_app_settings_sources_switch_checked(instance, 2));
    }
    if (phase >= ORB_LIFECYCLE_RESTORED) {
        assert(orb_app_settings_sources_changed(instance) == 3);
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_saved_scroll, maximum));
        if (phase == ORB_LIFECYCLE_RESTORED) assert(has_text(orb_screen_root(instance), ORB_ICON_WARNING " Not saved"));
        if (phase == ORB_LIFECYCLE_REMOVED_ID) assert(has_text(orb_screen_root(instance), "Saving…"));
        if (phase >= ORB_LIFECYCLE_REMOVED_ID) {
            unsigned before = s_intents;
            assert(!orb_app_settings_sources_toggle(instance, 0, ORB_INPUT_TOUCH, true));
            assert(!orb_app_settings_sources_cycle_recent(instance, ORB_INPUT_TOUCH, true));
            assert(s_intents == before);
            assert(orb_app_settings_sources_changed(instance) == 3);
        }
    }
}

static void scroll_began(lv_event_t *event)
{
    (void)event;
    s_scroll_begins++;
}

static void create_projected(orb_screen_instance_t *instance, orb_screen_host_t *host,
    const orb_screen_update_t *projection)
{
    s_projection = projection;
    assert(orb_screen_create(instance, host, orb_app_settings_sources_descriptor(), lv_screen_active(), NULL, false));
}

static void initial_projection(orb_screen_host_t *host, const orb_screen_update_t *projection)
{
    orb_screen_instance_t instance = {0};
    create_projected(&instance, host, projection);
    assert(orb_app_settings_sources_switch_checked(&instance, 0));
    assert(has_text(orb_screen_root(&instance), "10 min"));
    lv_obj_t *list = orb_app_settings_sources_scroll(&instance);
    lv_obj_add_event_cb(list, scroll_began, LV_EVENT_SCROLL_BEGIN, NULL);
    s_scroll_begins = 0;
    assert(orb_screen_update(&instance, projection));
    assert(orb_app_settings_sources_scroll(&instance) == list);
    assert(s_scroll_begins == 0);
    orb_screen_destroy(&instance);
    quiescent();
}

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_input;
}

static void floating_switch_quirk(orb_screen_host_t *host, const orb_screen_update_t *projection)
{
    orb_screen_instance_t instance = {0};
    create_projected(&instance, host, projection);
    assert(orb_screen_update(&instance, projection));
    lv_obj_t *list = orb_app_settings_sources_scroll(&instance);
    lv_obj_scroll_to_y(list, 84, LV_ANIM_OFF);
    lv_obj_update_layout(orb_screen_root(&instance));
    orb_round_list_update(list);
    lv_area_t bounds;
    assert(orb_app_settings_sources_action_bounds(&instance, 2, true, &bounds));
    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, lv_obj_get_display(orb_screen_root(&instance)));
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    unsigned before = s_intents;
    assert(!orb_app_settings_sources_switch_checked(&instance, 2));
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    advance(20);
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    advance(400);
    /* This intentionally preserves the old floating-target hit-test refusal:
     * local switch movement alone does not enqueue a preference request. */
    assert(s_intents == before);
    assert(orb_app_settings_sources_switch_checked(&instance, 2));
    assert(orb_app_settings_sources_changed(&instance) == -1);
    orb_settings_sources_model_t refreshed;
    memcpy(&refreshed, projection->model->data, sizeof(refreshed));
    refreshed.render_epoch++;
    orb_screen_model_t view = {&refreshed, sizeof(refreshed)};
    orb_screen_update_t refresh = {.revisions.model = 100, .model = &view};
    assert(orb_screen_update(&instance, &refresh));
    assert(!orb_app_settings_sources_switch_checked(&instance, 2));
    assert(s_intents == before);
    lv_indev_delete(pointer);
    orb_screen_destroy(&instance);
    quiescent();
}

static void preference_attribution(orb_screen_host_t *host)
{
    size_t size = orb_app_settings_sources_descriptor()->context_bytes;
    orb_screen_context_t *context = calloc(1, size);
    assert(context);
    orb_settings_sources_model_t model = {.sources = {true, true, false}, .idle_minutes = 10,
        .ready = true, .interactive = true, .error = "Request failed",
        .hub_pref_operation = 10, .hub_pref_key = ORB_SETTING_SOURCE_RECENT_MINUTES};
    orb_screen_model_t view = {&model, sizeof(model)};
    orb_screen_update_t update = {.revisions.model = 200, .model = &view};
    orb_screen_instance_t instance = {0};
    create_projected(&instance, host, &update);
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == 3);
    assert(has_text(orb_screen_root(&instance), ORB_ICON_WARNING " Not saved"));
    assert(orb_screen_save(&instance, context, size));
    orb_screen_destroy(&instance);
    quiescent();

    /* Sources is hidden while Temperature accepts one new UI preference.
     * Returning restores old scroll/context, but must reset the old row marker. */
    model.hub_pref_operation = 11;
    model.hub_pref_key = 1;
    update.revisions.model++;
    unsigned before = s_intents;
    create_projected(&instance, host, &update);
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == -1);
    assert(orb_screen_restore(&instance, context, size));
    assert(orb_app_settings_sources_changed(&instance) == -1);
    assert(!has_text(orb_screen_root(&instance), ORB_ICON_WARNING " Not saved"));
    assert(s_intents == before);

    /* A newer accepted source intent maps to its row; the same remembered
     * operation must not overwrite a later local attribution on remote update. */
    model.hub_pref_operation = 12;
    model.hub_pref_key = ORB_SETTING_SOURCE_CODEX;
    update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == 1);
    assert(orb_app_settings_sources_toggle(&instance, 2, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(&instance) == 2);
    model.idle_minutes = 30;
    update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == 2);
    assert(orb_screen_save(&instance, context, size));
    orb_screen_destroy(&instance);
    quiescent();

    /* observed_operation is value context: this already-seen key must not be
     * re-applied during reconstruction, and restoring must never emit it. */
    before = s_intents;
    create_projected(&instance, host, &update);
    assert(orb_screen_update(&instance, &update));
    assert(orb_screen_restore(&instance, context, size));
    assert(orb_app_settings_sources_changed(&instance) == 2);
    assert(s_intents == before);
    model.hub_pref_operation = 13;
    model.hub_pref_key = 999;
    update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == 2);
    model.sources[0] = false;
    update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == 2);
    model.hub_pref_operation = 14;
    model.hub_pref_key = 1;
    update.revisions.model++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sources_changed(&instance) == -1);
    assert(s_intents == before);
    orb_screen_destroy(&instance);
    free(context);
    quiescent();
}

static bool refuse_intent(void *context, const orb_intent_t *value)
{
    (void)context;
    (void)value;
    return false;
}

static void refused_attribution(orb_screen_host_t *host, const orb_screen_update_t *projection)
{
    orb_screen_instance_t instance = {0};
    create_projected(&instance, host, projection);
    assert(orb_screen_update(&instance, projection));
    assert(orb_app_settings_sources_requests_enabled(&instance));
    assert(orb_app_settings_sources_toggle(&instance, 0, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(&instance) == 0);
    unsigned before = s_intents;
    orb_screen_intent_cb_t original = host->config.intent;
    /* The copied screen projection can still say ready/not-pending while the
     * live host gate already refuses this contact. Attribution must stay on
     * the previously accepted row, without rolling the optimistic switch back. */
    host->config.intent = refuse_intent;
    assert(!orb_app_settings_sources_toggle(&instance, 1, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(&instance) == 0);
    assert(!orb_app_settings_sources_cycle_recent(&instance, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(&instance) == 0);
    assert(s_intents == before);
    host->config.intent = original;
    assert(orb_app_settings_sources_cycle_recent(&instance, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(&instance) == 3);
    before = s_intents;
    host->config.intent = refuse_intent;
    assert(!orb_app_settings_sources_toggle(&instance, 2, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sources_changed(&instance) == 3);
    assert(s_intents == before);
    host->config.intent = original;
    orb_screen_destroy(&instance);
    quiescent();
}

static unsigned text_count(lv_obj_t *root, const char *text)
{
    unsigned count = lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        count += text_count(lv_obj_get_child(root, i), text);
    return count;
}

static void unknown_projection(orb_screen_host_t *host)
{
    orb_settings_sources_model_t value = {.interactive = true};
    orb_screen_model_t model = {&value, sizeof(value)};
    orb_screen_update_t update = {.revisions.model = 1, .model = &model};
    orb_screen_instance_t instance = {0};
    create_projected(&instance, host, &update);
    assert(orb_screen_update(&instance, &update));
    lv_obj_t *root = orb_screen_root(&instance);
    /* AX row growth can put Recent activity below the initial viewport.
     * Every retained setting must still say Unknown before hub confirmation. */
    assert(text_count(root, "Unknown") == ORB_SETTINGS_SOURCE_COUNT + 1);
    assert(!has_text(root, "On") && !has_text(root, "Off"));
    assert(!orb_app_settings_sources_requests_enabled(&instance));
    orb_screen_destroy(&instance);
    quiescent();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_SOURCES_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent, .project = project});
    orb_settings_sources_model_t first = {.sources = {true, true, false}, .idle_minutes = 10,
        .ready = true, .interactive = true};
    orb_settings_sources_model_t changed = {.sources = {false, true, true}, .idle_minutes = 30,
        .ready = true, .interactive = true, .error = "Request failed"};
    orb_settings_sources_model_t pending = {.sources = {false, true, true}, .idle_minutes = 30,
        .ready = true, .interactive = true, .pending = true};
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t pending_model = {&pending, sizeof(pending)};
    orb_screen_update_t initial = {.revisions.model = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.model = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.model = 3, .model = &pending_model};
    orb_screen_update_t missing = {.revisions.model = 4};
    orb_screen_instance_t warm = {0};
    create_projected(&warm, &host, &initial);
    assert(orb_screen_update(&warm, &initial));
    orb_screen_destroy(&warm);
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();
    initial_projection(&host, &initial);
    orb_lifecycle_case_t test = {.descriptor = orb_app_settings_sources_descriptor(),
        .initial = &initial, .changed = &next, .removed = &removed, .empty = &missing,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    s_projection = &initial;
    orb_test_screen_lifecycle(&host, &test);
    floating_switch_quirk(&host, &initial);
    preference_attribution(&host);
    refused_attribution(&host, &initial);
    unknown_projection(&host);
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_settings_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_SETTINGS_SOURCES_SCREEN_ID) == orb_app_settings_sources_descriptor());
    orb_screen_instance_t preview = {0};
    s_projection = &initial;
    assert(orb_screen_create(&preview, &host, orb_app_settings_sources_descriptor(), lv_screen_active(), NULL, true));
    assert(orb_screen_update(&preview, &initial));
    unsigned before = s_intents;
    assert(!orb_app_settings_sources_toggle(&preview, 0, ORB_INPUT_TOUCH, true));
    assert(s_intents == before);
    orb_screen_destroy(&preview);
    quiescent();
    puts("Settings Sources: route17, lifecycle, immutable projection, attribution and existing switch behavior passed");
    return 0;
}
