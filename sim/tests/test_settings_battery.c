/* Battery uses the same lifecycle driver as every production descriptor.
 * Pointer fixtures obtain semantic action bounds, never kit child indices. */
#include "orb_app_settings_battery.h"
#include "orb_app_settings_registry.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static int32_t s_saved_scroll;
static orb_intent_t s_last_intent;
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

static void quiescent(void)
{
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

/* Content assertions search semantic label values; traversal never activates
 * a control or assumes a particular title/row/switch child topology. */
static bool has_text(lv_obj_t *root, const char *value)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), value)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (has_text(lv_obj_get_child(root, i), value)) return true;
    return false;
}

static void exercise(orb_screen_instance_t *instance)
{
    lv_obj_t *list = orb_app_settings_battery_scroll(instance);
    lv_obj_update_layout(orb_screen_root(instance));
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    s_saved_scroll = LV_MIN(24, maximum);
    if (!orb_round_list_scroll_to(list, s_saved_scroll))
        lv_obj_scroll_to_y(list, s_saved_scroll, LV_ANIM_OFF);
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    unsigned before = s_intents;
    assert(!orb_app_settings_battery_toggle_desk_limit(instance, ORB_INPUT_PROGRAMMATIC, true));
    assert(!orb_app_settings_battery_toggle_desk_limit(instance, ORB_INPUT_TOUCH, false));
    assert(!orb_app_settings_battery_toggle_desk_limit(instance, ORB_INPUT_CROWN, true));
    assert(s_intents == before);
    assert(orb_app_settings_battery_desk_checked(instance));
    assert(orb_app_settings_battery_toggle_desk_limit(instance, ORB_INPUT_TOUCH, true));
    assert(s_intents == before + 1);
    assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
    assert(s_last_intent.value.setting.key == ORB_SETTINGS_KEY_DESK_LIMIT);
    assert(s_last_intent.value.setting.value == 0);
    assert(s_last_intent.origin == ORB_INPUT_TOUCH && s_last_intent.physical);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Battery"));
    lv_obj_t *list = orb_app_settings_battery_scroll(instance);
    assert(list && lv_obj_is_scrollable(list));
    if (phase <= ORB_LIFECYCLE_IDEMPOTENT) {
        assert(has_text(orb_screen_root(instance), "77%"));
        assert(has_text(orb_screen_root(instance), "94%"));
        assert(has_text(orb_screen_root(instance), "77°F"));
        assert(has_text(orb_screen_root(instance), "Charging"));
        assert(!orb_app_settings_battery_desk_checked(instance));
    } else if (phase <= ORB_LIFECYCLE_RESTORED) {
        assert(has_text(orb_screen_root(instance), "41%"));
        assert(has_text(orb_screen_root(instance), "88%"));
        assert(has_text(orb_screen_root(instance), "18°C"));
        assert(has_text(orb_screen_root(instance), "On battery"));
        assert(has_text(orb_screen_root(instance), ORB_ICON_WARNING " Not saved"));
        assert(orb_app_settings_battery_desk_checked(instance));
    } else {
        assert(has_text(orb_screen_root(instance), "—"));
        assert(has_text(orb_screen_root(instance), "Unavailable"));
        assert(has_text(orb_screen_root(instance), "Battery unavailable"));
        assert(!orb_app_settings_battery_desk_checked(instance));
    }
    if (phase >= ORB_LIFECYCLE_RESTORED) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_saved_scroll, maximum));
    }
}

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_input;
}

static void pointer_toggle(orb_screen_instance_t *instance, lv_indev_t *pointer)
{
    lv_obj_t *list = orb_app_settings_battery_scroll(instance);
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    lv_obj_update_layout(orb_screen_root(instance));
    orb_round_list_update(list);
    lv_area_t bounds;
    assert(orb_app_settings_battery_desk_bounds(instance, &bounds));
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    advance(20);
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    advance(400);
}

static void physical_and_synthetic(orb_screen_host_t *host, const orb_screen_update_t *initial)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_settings_battery_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&instance, initial));
    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, lv_obj_get_display(orb_screen_root(&instance)));
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    unsigned before = s_intents;
    orb_input_mark_synthetic(pointer);
    pointer_toggle(&instance, pointer);
    assert(s_intents == before);
    assert(!orb_app_settings_battery_desk_checked(&instance));
    /* Retire the existing global synthetic-input hint before checking cleanup. */
    advance(1500);
    lv_indev_set_user_data(pointer, NULL);
    pointer_toggle(&instance, pointer);
    assert(s_intents == before + 1);
    assert(orb_app_settings_battery_desk_checked(&instance));
    assert(s_last_intent.value.setting.value == 1);
    lv_indev_delete(pointer);
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
    orb_nav_init(&nav, ORB_SETTINGS_BATTERY_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});
    orb_settings_battery_model_t first = {.present = true, .usb = true, .charging = true,
        .health_valid = true, .temperature_valid = true, .battery_pct = 77, .health_pct = 94,
        .temperature_c = 25.f, .fahrenheit = true};
    orb_settings_battery_model_t changed = {.present = true, .health_valid = true,
        .temperature_valid = true, .battery_pct = 41, .health_pct = 88,
        .temperature_c = 18.3f, .desk_charge_limit = true, .battery_setting_error = true};
    orb_settings_battery_model_t removed = {.present = false, .health_valid = true,
        .temperature_valid = true, .battery_pct = 60, .health_pct = 94, .temperature_c = 50.f};
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t removed_model = {&removed, sizeof(removed)};
    orb_screen_update_t initial = {.revisions.device = 1, .revisions.model = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.device = 2, .revisions.model = 2, .model = &changed_model};
    orb_screen_update_t gone = {.revisions.device = 3, .revisions.model = 3, .model = &removed_model};
    orb_screen_update_t empty = {.revisions.device = 4, .revisions.model = 4};
    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_settings_battery_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&warm, &initial));
    orb_screen_destroy(&warm);
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();

    orb_lifecycle_case_t test = {.descriptor = orb_app_settings_battery_descriptor(),
        .initial = &initial, .changed = &next, .removed = &gone, .empty = &empty,
        .exercise = exercise, .verify = verify, .quiescent = quiescent};
    orb_test_screen_lifecycle(&host, &test);
    physical_and_synthetic(&host, &initial);
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, orb_app_settings_manifest(), NULL));
    assert(orb_screen_registry_find(&registry, ORB_SETTINGS_BATTERY_SCREEN_ID) == orb_app_settings_battery_descriptor());
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_settings_battery_descriptor(), lv_screen_active(), NULL, true));
    assert(orb_screen_update(&preview, &initial));
    unsigned before = s_intents;
    assert(!orb_app_settings_battery_toggle_desk_limit(&preview, ORB_INPUT_TOUCH, true));
    assert(s_intents == before);
    orb_screen_destroy(&preview);
    quiescent();
    puts("Settings Battery: lifecycle, telemetry, scroll context, physical intent and synthetic rollback passed");
    return 0;
}
