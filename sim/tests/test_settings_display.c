/* Display's production descriptor uses the common lifecycle driver. Semantic
 * actions verify intents; parity remains responsible for exact page geometry. */
#include "orb_app_settings_display.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static orb_intent_t s_last_intent;
static unsigned s_intents, s_timer_baseline, s_scroll_begins;
static const orb_screen_update_t *s_projection;
static size_t s_workspace_baseline;
static uint32_t s_page;
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

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    (void)context;
    assert(screen == ORB_SETTINGS_DISPLAY_SCREEN_ID);
    *update = *s_projection;
    return true;
}

static void quiescent(void)
{
    assert(workspace_used() == s_workspace_baseline);
    assert(timer_count() == s_timer_baseline);
}

static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_intents;
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    assert(!orb_app_settings_display_open(instance, ORB_SETTINGS_BRIGHTNESS_PAGE,
        ORB_INPUT_PROGRAMMATIC, false, NULL));
    if (!s_page) {
        lv_obj_t *list = orb_app_settings_display_scroll(instance);
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        s_saved_scroll = LV_MIN(24, maximum);
        if (!orb_round_list_scroll_to(list, s_saved_scroll))
            lv_obj_scroll_to_y(list, s_saved_scroll, LV_ANIM_OFF);
        assert(orb_app_settings_display_open(instance, ORB_SETTINGS_BRIGHTNESS_PAGE,
            ORB_INPUT_TOUCH, true, NULL));
        assert(s_last_intent.kind == ORB_INTENT_NAVIGATE);
        assert(s_last_intent.value.navigate.screen == ORB_SETTINGS_DISPLAY_SCREEN_ID);
        assert(s_last_intent.value.navigate.args.page == ORB_SETTINGS_BRIGHTNESS_PAGE);
        assert(orb_app_settings_display_open(instance, ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE,
            ORB_INPUT_TOUCH, false, NULL));
        assert(s_last_intent.value.navigate.args.page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE);
        assert(orb_app_settings_display_toggle(instance, ORB_SETTING_DISPLAY_ALWAYS_ON,
            ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTING_DISPLAY_ALWAYS_ON);
        assert(s_last_intent.value.setting.value == 1);
        assert(orb_app_settings_display_toggle(instance, ORB_SETTING_DISPLAY_PICKUP,
            ORB_INPUT_BUTTON, true));
        assert(s_last_intent.value.setting.key == ORB_SETTING_DISPLAY_PICKUP);
        assert(s_last_intent.value.setting.value == 1);
        assert(orb_app_settings_display_toggle(instance, ORB_SETTING_DISPLAY_QUIET,
            ORB_INPUT_TOUCH, true));
        assert(s_last_intent.value.setting.key == ORB_SETTING_DISPLAY_QUIET);
        assert(s_last_intent.value.setting.value == 0);
        assert(s_intents == before + 5);
        assert(!orb_app_settings_display_set_level(instance, 50, ORB_INPUT_TOUCH, true));
        return;
    }
    s_saved_scroll = 0;
    assert(!orb_app_settings_display_open(instance, ORB_SETTINGS_BRIGHTNESS_PAGE,
        ORB_INPUT_TOUCH, true, NULL));
    assert(!orb_screen_rim_acquire(instance));
    assert(orb_app_settings_display_set_level(instance, 200, ORB_INPUT_TOUCH, true));
    assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
    assert(s_last_intent.value.setting.key == (s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ?
        ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS : ORB_SETTING_DISPLAY_BRIGHTNESS));
    assert(s_last_intent.value.setting.value == (s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? 15 : 100));
    assert(orb_app_settings_display_level(instance) == s_last_intent.value.setting.value);
    assert(orb_app_settings_display_set_level(instance, -5, ORB_INPUT_CROWN, true));
    assert(s_last_intent.value.setting.value == (s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? 1 : 0));
    assert(s_last_intent.origin == ORB_INPUT_CROWN && s_last_intent.physical);
    assert(!orb_app_settings_display_set_level(instance, 5, ORB_INPUT_PROGRAMMATIC, true));
    assert(s_intents == before + 2);
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(orb_app_settings_display_page(instance) == s_page);
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ?
        "Always On brightness" : s_page ? "Brightness" : "Display"));
    if (s_page) {
        assert(!orb_app_settings_display_scroll(instance));
        assert(orb_app_settings_display_level_control(instance));
        assert(!orb_screen_page_viewport(instance));
        assert(!orb_screen_page_title(instance));
        int32_t expected = phase == ORB_LIFECYCLE_INITIAL || phase == ORB_LIFECYCLE_IDEMPOTENT ?
            (s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? 3 : 70) :
            phase <= ORB_LIFECYCLE_RESTORED ?
            (s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? 4 : 22) :
            (s_page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? 1 : 0);
        assert(orb_app_settings_display_level(instance) == expected);
    } else {
        lv_obj_t *list = orb_app_settings_display_scroll(instance);
        assert(list && lv_obj_is_scrollable(list));
        assert(orb_screen_page_viewport(instance) == list);
        assert(orb_screen_page_title(instance));
        if (phase >= ORB_LIFECYCLE_RESTORED) {
            lv_obj_update_layout(orb_screen_root(instance));
            int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
            assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_saved_scroll, maximum));
        }
    }
}

static void scroll_began(lv_event_t *event)
{
    (void)event;
    s_scroll_begins++;
}

static bool has_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (has_text(lv_obj_get_child(root, i), text)) return true;
    return false;
}

static void seeded_initial_projection(orb_screen_host_t *host, const orb_screen_update_t *initial)
{
    s_projection = initial;
    for (unsigned page = ORB_SETTINGS_DISPLAY_PAGE; page <= ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE; page++) {
        orb_screen_instance_t instance = {0};
        assert(orb_screen_create(&instance, host, orb_app_settings_display_descriptor(),
            lv_screen_active(), &(orb_screen_args_t){.page = page}, false));
        lv_obj_t *list = orb_screen_page_viewport(&instance);
        lv_obj_t *level = orb_app_settings_display_level_control(&instance);
        if (page) {
            assert(!list && !orb_screen_page_title(&instance));
            assert(level);
            assert(orb_app_settings_display_level(&instance) ==
                (page == ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE ? 3 : 70));
        } else {
            assert(list && !level);
            assert(!strcmp(orb_kit_rim_text_get(orb_screen_page_title(&instance)), "DISPLAY"));
            assert(has_text(orb_screen_root(&instance), "70%"));
            assert(has_text(orb_screen_root(&instance), "3%"));
            lv_obj_add_event_cb(list, scroll_began, LV_EVENT_SCROLL_BEGIN, NULL);
        }
        unsigned before = s_intents;
        s_scroll_begins = 0;
        assert(orb_screen_update(&instance, initial));
        /* A changed framework revision with an equal value projection still
         * skips app widget setters and cannot start a zero-distance scroll. */
        orb_screen_update_t equal = *initial;
        equal.revisions.device++;
        assert(orb_screen_update(&instance, &equal));
        assert(orb_screen_page_viewport(&instance) == list);
        assert(orb_app_settings_display_level_control(&instance) == level);
        assert(s_scroll_begins == 0 && s_intents == before);
        orb_screen_destroy(&instance);
        quiescent();
    }
}

static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static void scroll_end_switches(orb_screen_host_t *host)
{
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_settings_display_descriptor(), lv_screen_active(), NULL, false));
    lv_obj_t *quiet = find_label(orb_screen_root(&instance), "Quiet mode");
    assert(quiet);
    for (unsigned i = 0; i < 80; i++) { lv_tick_inc(16); lv_timer_handler(); }
    lv_obj_scroll_to_view_recursive(quiet, LV_ANIM_OFF);
    for (unsigned i = 0; i < 10; i++) { lv_tick_inc(16); lv_timer_handler(); }
    lv_refr_now(lv_obj_get_display(quiet));
    const char *names[] = {"Wake on pickup", "Quiet mode"};
    for (unsigned i = 0; i < 2; i++) {
        lv_obj_t *name = find_label(orb_screen_root(&instance), names[i]);
        assert(name);
        lv_obj_t *row = lv_obj_get_parent(name), *toggle = orb_kit_row_switch(row);
        assert(toggle && !lv_obj_is_hidden(toggle));
        lv_area_t row_bounds, toggle_bounds;
        assert(orb_round_list_row_bounds(row, &row_bounds));
        lv_obj_get_coords(toggle, &toggle_bounds);
        assert(toggle_bounds.x1 >= row_bounds.x1 && toggle_bounds.x2 <= row_bounds.x2 &&
            toggle_bounds.y1 >= row_bounds.y1 && toggle_bounds.y2 <= row_bounds.y2);
    }
    orb_screen_destroy(&instance);
    quiescent();
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_DISPLAY_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent, .project = project});
    orb_settings_display_model_t first = {.brightness = 70, .always_on_brightness = 3,
        .always_on = true, .motion_wake = true, .has_host = true, .interactive = true};
    orb_settings_display_model_t changed = {.brightness = 22, .always_on_brightness = 4,
        .quiet = true, .has_host = true, .interactive = true};
    orb_settings_display_model_t empty = {0};
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t empty_model = {&empty, sizeof(empty)};
    orb_screen_update_t initial = {.revisions.device = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.device = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.device = 3, .model = &empty_model};
    orb_screen_update_t missing = {.revisions.device = 4};
    s_projection = &initial;
    for (unsigned page = ORB_SETTINGS_DISPLAY_PAGE; page <= ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE; page++) {
        orb_screen_instance_t warm = {0};
        assert(orb_screen_create(&warm, &host, orb_app_settings_display_descriptor(), lv_screen_active(),
            &(orb_screen_args_t){.page = page}, false));
        assert(orb_screen_update(&warm, &initial));
        orb_screen_destroy(&warm);
    }
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();
    seeded_initial_projection(&host, &initial);
    s_projection = &initial;
    for (s_page = ORB_SETTINGS_DISPLAY_PAGE; s_page <= ORB_SETTINGS_ALWAYS_BRIGHTNESS_PAGE; s_page++) {
        orb_screen_args_t args = {.page = s_page};
        orb_lifecycle_case_t test = {.descriptor = orb_app_settings_display_descriptor(), .args = &args,
            .initial = &initial, .changed = &next, .removed = &removed, .empty = &missing,
            .exercise = exercise, .verify = verify, .quiescent = quiescent};
        orb_test_screen_lifecycle(&host, &test);
    }
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_settings_display_descriptor(), lv_screen_active(), NULL, true));
    assert(orb_screen_update(&preview, &initial));
    assert(!orb_app_settings_display_toggle(&preview, ORB_SETTING_DISPLAY_ALWAYS_ON, ORB_INPUT_TOUCH, true));
    assert(!orb_app_settings_display_open(&preview, ORB_SETTINGS_BRIGHTNESS_PAGE, ORB_INPUT_TOUCH, true, NULL));
    orb_screen_destroy(&preview);
    quiescent();
    scroll_end_switches(&host);
    puts("Settings Display: lifecycle, value context, safe routes and typed level intents passed");
    return 0;
}
