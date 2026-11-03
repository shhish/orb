/* Every Sound page uses the common production descriptor/lifecycle driver.
 * Semantic actions select controls; no test activates a kit child by topology. */
#include "orb_app_settings_sound.h"
#include "orb_app_settings_registry.h"
#include "orb_kit.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static orb_intent_t s_last_intent;
static unsigned s_intents, s_timer_baseline, s_page, s_changed;
static size_t s_workspace_baseline;
static int32_t s_saved_scroll;

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

static bool has_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (has_text(lv_obj_get_child(root, i), text)) return true;
    return false;
}

static void set_test_scroll(orb_screen_instance_t *instance)
{
    lv_obj_t *list = orb_app_settings_sound_scroll(instance);
    s_saved_scroll = 0;
    if (!list) return;
    lv_obj_update_layout(orb_screen_root(instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    s_saved_scroll = LV_MIN(24, maximum);
    if (!orb_round_list_scroll_to(list, s_saved_scroll))
        lv_obj_scroll_to_y(list, s_saved_scroll, LV_ANIM_OFF);
}

static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_intents;
    s_changed = 0;
    set_test_scroll(instance);
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    assert(!orb_app_settings_sound_open(instance, ORB_SETTINGS_SOUND_VOLUME_PAGE,
        ORB_INPUT_PROGRAMMATIC, false, NULL));
    if (s_page == ORB_SETTINGS_SOUND_PAGE) {
        assert(orb_app_settings_sound_open(instance, ORB_SETTINGS_SOUND_VOLUME_PAGE, ORB_INPUT_TOUCH, true, NULL));
        assert(s_last_intent.kind == ORB_INTENT_NAVIGATE);
        assert(s_last_intent.value.navigate.screen == ORB_SETTINGS_SOUND_SCREEN_ID);
        assert(s_last_intent.value.navigate.args.page == ORB_SETTINGS_SOUND_VOLUME_PAGE);
        assert(orb_app_settings_sound_open(instance, ORB_SETTINGS_SOUND_QUIET_PAGE, ORB_INPUT_TOUCH, false, NULL));
        assert(s_last_intent.origin == ORB_INPUT_TOUCH && !s_last_intent.physical);
        assert(orb_app_settings_sound_open(instance, ORB_SETTINGS_SOUND_PALETTE_PAGE, ORB_INPUT_TOUCH, true, NULL));
        assert(orb_app_settings_sound_open_display(instance, ORB_INPUT_TOUCH, true, NULL));
        assert(s_last_intent.value.navigate.screen == 9);
        assert(orb_app_settings_sound_toggle(instance, ORB_SETTING_SOUND_SPEECH, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTING_SOUND_SPEECH);
        assert(s_last_intent.value.setting.value == 1);
        assert(orb_app_settings_sound_toggle(instance, ORB_SETTING_SOUND_SILENT, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.value.setting.key == ORB_SETTING_SOUND_SILENT);
        assert(s_last_intent.value.setting.value == 0);
        assert(s_intents == before + 6);
    } else if (s_page == ORB_SETTINGS_SOUND_QUIET_PAGE) {
        assert(orb_app_settings_sound_open(instance, ORB_SETTINGS_SOUND_START_PAGE, ORB_INPUT_TOUCH, true, NULL));
        assert(orb_app_settings_sound_open(instance, ORB_SETTINGS_SOUND_END_PAGE, ORB_INPUT_TOUCH, true, NULL));
        assert(orb_app_settings_sound_toggle(instance, ORB_SETTING_SOUND_QUIET_ENABLED, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.value.setting.key == ORB_SETTING_SOUND_QUIET_ENABLED);
        assert(s_last_intent.value.setting.value == 0);
        s_changed = ORB_SETTINGS_SOUND_QUIET_PAGE;
        assert(s_intents == before + 3);
    } else if (s_page == ORB_SETTINGS_SOUND_PALETTE_PAGE) {
        assert(orb_app_settings_sound_preview(instance, ORB_SOUND_ALARM, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_APP_COMMAND);
        assert(s_last_intent.value.command.target == ORB_SETTINGS_SOUND_SCREEN_ID);
        assert(s_last_intent.value.command.action == ORB_SETTINGS_SOUND_COMMAND_PREVIEW);
        assert(s_last_intent.value.command.value == ORB_SOUND_ALARM);
        assert(!orb_app_settings_sound_preview(instance, ORB_SOUND_CUE_COUNT, ORB_INPUT_TOUCH, true));
        assert(s_intents == before + 1);
    } else if (s_page == ORB_SETTINGS_SOUND_VOLUME_PAGE) {
        assert(orb_app_settings_sound_set_volume(instance, 200, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.value.setting.key == ORB_SETTING_SOUND_VOLUME);
        assert(s_last_intent.value.setting.value == 100);
        assert(orb_app_settings_sound_set_volume(instance, -1, ORB_INPUT_CROWN, true));
        assert(s_last_intent.value.setting.value == 0);
        assert(s_last_intent.origin == ORB_INPUT_CROWN && s_last_intent.physical);
        assert(s_intents == before + 2);
        assert(orb_screen_rim_acquire(instance));
        assert(orb_screen_rim_begin(instance, ORB_INPUT_CROWN, true));
        assert(orb_screen_rim_set(instance, 7, ORB_INPUT_CROWN));
        assert(s_intents == before + 2);
        assert(orb_screen_rim_finish(instance, false));
        assert(orb_app_settings_sound_value(instance) == 0);
        assert(s_intents == before + 2);
        orb_screen_rim_release(orb_screen_host(instance));
    } else {
        bool start = s_page == ORB_SETTINGS_SOUND_START_PAGE;
        assert(orb_app_settings_sound_step_hour(instance, 1, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.value.setting.key == (start ? ORB_SETTING_SOUND_QUIET_START : ORB_SETTING_SOUND_QUIET_END));
        assert(s_last_intent.value.setting.value == (start ? 0 : 1));
        assert(orb_app_settings_sound_step_hour(instance, -1, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.value.setting.value == (start ? 23 : 0));
        s_changed = s_page;
        unsigned after_buttons = s_intents;
        assert(orb_app_settings_sound_set_hour(instance, 99, ORB_INPUT_CROWN, true));
        assert(orb_app_settings_sound_value(instance) == 23);
        assert(s_intents == after_buttons + !start);
        assert(orb_app_settings_sound_set_hour(instance, -1, ORB_INPUT_CROWN, true));
        assert(orb_app_settings_sound_value(instance) == 0);
        assert(orb_app_settings_sound_changed(instance) == s_changed);
        assert(!orb_app_settings_sound_set_hour(instance, 2, ORB_INPUT_PROGRAMMATIC, true));
    }
    assert(orb_app_settings_sound_changed(instance) == s_changed);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(orb_app_settings_sound_page(instance) == s_page);
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    static const char *s_titles[] = {"Sound", "Volume", "Quiet hours", "Starts", "Ends", "System sounds"};
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, s_titles[s_page]));
    bool scalar = s_page == ORB_SETTINGS_SOUND_VOLUME_PAGE || s_page == ORB_SETTINGS_SOUND_START_PAGE || s_page == ORB_SETTINGS_SOUND_END_PAGE;
    lv_obj_t *list = orb_app_settings_sound_scroll(instance);
    assert((list != NULL) == !scalar);
    if (s_page == ORB_SETTINGS_SOUND_PAGE) {
        assert(has_text(orb_screen_root(instance), "Volume"));
        assert(has_text(orb_screen_root(instance), "Test voice") == (phase <= ORB_LIFECYCLE_IDEMPOTENT));
    } else if (s_page == ORB_SETTINGS_SOUND_PALETTE_PAGE) {
        assert(has_text(orb_screen_root(instance), "Alarm"));
    }
    if (s_page == ORB_SETTINGS_SOUND_VOLUME_PAGE) {
        assert(orb_app_settings_sound_level_control(instance));
        assert(orb_app_settings_sound_value(instance) == (phase <= ORB_LIFECYCLE_IDEMPOTENT ? 70 :
            phase <= ORB_LIFECYCLE_RESTORED ? 22 : 0));
    } else if (s_page == ORB_SETTINGS_SOUND_START_PAGE || s_page == ORB_SETTINGS_SOUND_END_PAGE) {
        int value = phase <= ORB_LIFECYCLE_IDEMPOTENT ? (s_page == ORB_SETTINGS_SOUND_START_PAGE ? 22 : 7) :
            phase <= ORB_LIFECYCLE_RESTORED ? (s_page == ORB_SETTINGS_SOUND_START_PAGE ? 23 : 0) : 0;
        assert(orb_app_settings_sound_value(instance) == value);
    } else if (phase >= ORB_LIFECYCLE_RESTORED) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_saved_scroll, maximum));
    }
    if (phase >= ORB_LIFECYCLE_RESTORED) assert(orb_app_settings_sound_changed(instance) == s_changed);
}

static void create_projected(orb_screen_instance_t *instance, orb_screen_host_t *host,
    unsigned page, const orb_screen_update_t *update)
{
    assert(orb_screen_create(instance, host, orb_app_settings_sound_descriptor(), lv_screen_active(),
        &(orb_screen_args_t){.page = page}, false));
    assert(orb_screen_update(instance, update));
}

static void retained_volume(orb_screen_host_t *host, orb_settings_sound_model_t model)
{
    orb_screen_instance_t instance = {0};
    orb_screen_model_t projection = {&model, sizeof(model)};
    orb_screen_update_t update = {.revisions.settings = 10, .model = &projection};
    create_projected(&instance, host, ORB_SETTINGS_SOUND_VOLUME_PAGE, &update);
    lv_obj_t *level = orb_app_settings_sound_level_control(&instance);
    lv_obj_add_state(level, LV_STATE_PRESSED);
    model.prefs.saving = true;
    model.prefs.volume = 44;
    update.revisions.settings++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sound_level_control(&instance) == level);
    assert(lv_obj_has_state(level, LV_STATE_PRESSED));
    assert(orb_app_settings_sound_value(&instance) == 44);
    model.prefs.saving = false;
    update.revisions.settings++;
    assert(orb_screen_update(&instance, &update));
    assert(orb_app_settings_sound_level_control(&instance) == level);
    lv_obj_remove_state(level, LV_STATE_PRESSED);
    orb_screen_destroy(&instance);
    quiescent();
}

static void group_context(orb_screen_host_t *host, const orb_screen_update_t *update)
{
    size_t size = orb_app_settings_sound_descriptor()->context_bytes;
    orb_screen_context_t *context = calloc(1, size);
    assert(context);
    orb_screen_instance_t instance = {0};
    create_projected(&instance, host, ORB_SETTINGS_SOUND_PAGE, update);
    s_saved_scroll = 0;
    set_test_scroll(&instance);
    int32_t root_scroll = s_saved_scroll;
    assert(orb_screen_save(&instance, context, size));
    orb_screen_destroy(&instance);
    create_projected(&instance, host, ORB_SETTINGS_SOUND_QUIET_PAGE, update);
    assert(orb_app_settings_sound_restore_group(&instance, context, size));
    assert(orb_app_settings_sound_page(&instance) == ORB_SETTINGS_SOUND_QUIET_PAGE);
    assert(orb_app_settings_sound_toggle(&instance, ORB_SETTING_SOUND_QUIET_ENABLED, ORB_INPUT_TOUCH, true));
    assert(orb_screen_save(&instance, context, size));
    orb_screen_destroy(&instance);
    create_projected(&instance, host, ORB_SETTINGS_SOUND_START_PAGE, update);
    assert(orb_app_settings_sound_restore_group(&instance, context, size));
    assert(orb_app_settings_sound_changed(&instance) == ORB_SETTINGS_SOUND_QUIET_PAGE);
    assert(orb_app_settings_sound_step_hour(&instance, 1, ORB_INPUT_TOUCH, true));
    assert(orb_screen_save(&instance, context, size));
    orb_screen_destroy(&instance);
    create_projected(&instance, host, ORB_SETTINGS_SOUND_QUIET_PAGE, update);
    assert(orb_app_settings_sound_restore_group(&instance, context, size));
    assert(orb_app_settings_sound_page(&instance) == ORB_SETTINGS_SOUND_QUIET_PAGE);
    assert(orb_app_settings_sound_changed(&instance) == ORB_SETTINGS_SOUND_START_PAGE);
    assert(orb_screen_save(&instance, context, size));
    orb_screen_destroy(&instance);
    create_projected(&instance, host, ORB_SETTINGS_SOUND_PAGE, update);
    assert(orb_app_settings_sound_restore_group(&instance, context, size));
    lv_obj_t *list = orb_app_settings_sound_scroll(&instance);
    lv_obj_update_layout(orb_screen_root(&instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(root_scroll, maximum));
    assert(orb_app_settings_sound_changed(&instance) == ORB_SETTINGS_SOUND_START_PAGE);
    orb_screen_destroy(&instance);
    free(context);
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_SOUND_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});
    orb_settings_sound_model_t first = {.prefs = {.volume = 70, .spoken_replies = true,
        .quiet_start = 22, .quiet_end = 7}, .speech_voice = "Test voice", .interactive = true};
    orb_settings_sound_model_t changed = {.prefs = {.volume = 22, .quiet_enabled = true,
        .quiet_start = 23, .quiet_end = 0}, .silent = true, .interactive = true, .render_epoch = 1};
    orb_settings_sound_model_t empty = {0};
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t empty_model = {&empty, sizeof(empty)};
    orb_screen_update_t initial = {.revisions.settings = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.settings = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.settings = 3, .model = &empty_model};
    orb_screen_update_t missing = {.revisions.settings = 4};
    orb_sound_preferences_t service_before;
    orb_sound_preferences(&service_before);
    for (unsigned page = 0; page <= ORB_SETTINGS_SOUND_PALETTE_PAGE; page++) {
        orb_screen_instance_t warm = {0};
        create_projected(&warm, &host, page, &initial);
        orb_screen_destroy(&warm);
    }
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();
    for (s_page = 0; s_page <= ORB_SETTINGS_SOUND_PALETTE_PAGE; s_page++) {
        orb_screen_args_t args = {.page = s_page};
        orb_lifecycle_case_t test = {.descriptor = orb_app_settings_sound_descriptor(), .args = &args,
            .initial = &initial, .changed = &next, .removed = &removed, .empty = &missing,
            .exercise = exercise, .verify = verify, .quiescent = quiescent};
        orb_test_screen_lifecycle(&host, &test);
    }
    retained_volume(&host, first);
    group_context(&host, &initial);
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_settings_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_SETTINGS_SOUND_SCREEN_ID) == orb_app_settings_sound_descriptor());
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_settings_sound_descriptor(), lv_screen_active(),
        &(orb_screen_args_t){.page = ORB_SETTINGS_SOUND_PALETTE_PAGE}, true));
    assert(orb_screen_update(&preview, &initial));
    unsigned before = s_intents;
    assert(!orb_app_settings_sound_preview(&preview, ORB_SOUND_ALARM, ORB_INPUT_TOUCH, true));
    assert(s_intents == before);
    orb_screen_destroy(&preview);
    quiescent();
    orb_sound_preferences_t service_after;
    orb_sound_preferences(&service_after);
    assert(service_after.volume == service_before.volume);
    assert(service_after.quiet_start == service_before.quiet_start && service_after.quiet_end == service_before.quiet_end);
    assert(service_after.quiet_enabled == service_before.quiet_enabled);
    assert(service_after.spoken_replies == service_before.spoken_replies);
    assert(service_after.silent == service_before.silent);
    puts("Settings Sound: six-page lifecycle, typed intents, held volume and opaque group context passed");
    return 0;
}
