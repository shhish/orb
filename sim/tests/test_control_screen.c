/* Control's primary panel owns retained widgets, never service state. The
 * compatibility shell keeps selection, crown cadence and brightness flushing. */
#include "control_screen.h"
#include "orb_app.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_kit_motion.h"
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
static unsigned s_commands, s_cues, s_timer_baseline;
static size_t s_workspace_baseline;
static lv_display_t *s_display;
static lv_indev_data_t s_pointer_data;
static orb_intent_t s_last;
static orb_control_model_t s_initial = {.brightness = 68, .battery_pct = 87, .volume = 41};
static orb_control_model_t s_changed = {.brightness = 32, .battery_pct = 9, .volume = 77};
static orb_control_model_t s_removed = {.brightness = 8, .battery_pct = -1, .volume = 5};
static orb_screen_model_t s_initial_view = {&s_initial, sizeof(s_initial)};

static uint32_t tick(void) { return s_now; }

static void advance(unsigned milliseconds)
{
    for (unsigned elapsed = 0; elapsed < milliseconds; elapsed += 16) {
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
    unsigned before = s_commands;
    advance(1600);
    assert(used() == s_workspace_baseline && timers() == s_timer_baseline);
    assert(s_commands == before && !s_cues);
}

static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    /* The level keeps its numeric label hidden because the chip supplies the
     * value. An empty-model battery also says 0%; inspect the visible reader. */
    if (lv_obj_is_hidden(root)) return NULL;
    const char *button = orb_kit_button_get_text(root);
    if ((button && !strcmp(button, text)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_action(lv_obj_t *root, unsigned action)
{
    if (lv_obj_is_clickable(root) && (uintptr_t)lv_obj_get_user_data(root) == action) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_action(lv_obj_get_child(root, i), action);
        if (found) return found;
    }
    return NULL;
}

static bool record(void *context, const orb_intent_t *intent)
{
    (void)context;
    s_commands++;
    s_last = *intent;
    return true;
}

static bool command(void *context, orb_screen_instance_t *instance, const orb_intent_t *intent)
{
    assert(context == &s_commands);
    assert(orb_screen_alive(instance));
    return record(NULL, intent);
}

static bool allow_synthetic(void *context, const orb_intent_t *intent)
{
    (void)context; (void)intent;
    return true;
}

static void cue(void *context, orb_feedback_role_t role)
{
    (void)context; (void)role;
    s_cues++;
}

static bool project(void *context, orb_screen_id_t screen, orb_screen_update_t *update)
{
    (void)context;
    assert(screen == ORB_SHELL_CONTROL_CENTER);
    *update = (orb_screen_update_t){.revisions.device = 1, .revisions.time = 1, .model = &s_initial_view};
    return true;
}

static void assert_values(orb_screen_instance_t *instance, const char *battery,
    lv_color_t color, int brightness, unsigned volume, int level)
{
    lv_obj_t *root = orb_screen_root(instance);
    assert(root && orb_control_level(instance));
    lv_obj_t *number = find_text(root, battery);
    assert(number && lv_obj_check_type(number, &lv_label_class));
    assert(lv_color_eq(lv_obj_get_style_text_color(number, 0), color));
    char text[48];
    snprintf(text, sizeof(text), ORB_ICON_SUN " %d%%", brightness);
    assert(find_text(root, text));
    snprintf(text, sizeof(text), ORB_ICON_SPEAKER_LOW " %u%%", volume);
    assert(find_text(root, text));
    assert(orb_kit_rim_slider_get_value(orb_control_level(instance)) == level);
    assert(!orb_screen_page_viewport(instance) && orb_screen_page_title(instance));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Control Center"));
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    if (phase == ORB_LIFECYCLE_INITIAL || phase == ORB_LIFECYCLE_IDEMPOTENT)
        assert_values(instance, ORB_ICON_BATTERY_HIGH " 87%", ORB_LABEL, 68, 41, 68);
    else if (phase == ORB_LIFECYCLE_CHANGED || phase == ORB_LIFECYCLE_RESTORED)
        assert_values(instance, "9%", ORB_RED, 32, 77, 32);
    else if (phase == ORB_LIFECYCLE_REMOVED_ID)
        assert_values(instance, "—", ORB_KIT_PLAY, 8, 5, 8);
    else
        assert_values(instance, "0%", ORB_RED, 0, 0, 0);
}

static void exercise(orb_screen_instance_t *instance)
{
    unsigned before = s_commands;
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    orb_rim_binding_t binding = {0};
    assert(orb_control_descriptor()->ops->rim_binding(instance, &binding));
    assert(binding.property == ORB_CONTROL_BRIGHTNESS && binding.kind == ORB_RIM_VALUE);
    assert(binding.minimum == 0 && binding.maximum == 100 && binding.value == 32);
    assert(binding.detent == 1 && binding.units_per_turn == 100);
    orb_screen_context_t context = {0};
    assert(orb_control_descriptor()->context_bytes == sizeof(context));
    assert(orb_screen_save(instance, &context, sizeof(context)));
    assert(context.version == 1 && context.size == sizeof(context));
    context.version++;
    assert(!orb_screen_restore(instance, &context, sizeof(context)));
    context.version--;
    context.size++;
    assert(!orb_screen_restore(instance, &context, sizeof(context)));
    assert(s_commands == before);
}

static void lifecycle(void)
{
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project,
        .intent = record, .allow_synthetic = allow_synthetic, .cue = cue, .defer_rim = true});
    orb_screen_model_t changed_view = {&s_changed, sizeof(s_changed)};
    orb_screen_model_t removed_view = {&s_removed, sizeof(s_removed)};
    orb_screen_update_t initial = {.revisions.device = 1, .revisions.time = 1, .model = &s_initial_view};
    orb_screen_update_t changed = {.revisions.device = 2, .revisions.time = 2, .model = &changed_view};
    orb_screen_update_t removed = {.revisions.device = 3, .revisions.time = 3, .model = &removed_view};
    orb_screen_update_t empty = {.revisions.device = 4, .revisions.time = 4};
    orb_lifecycle_case_t test = {.descriptor = orb_control_descriptor(),
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
    unsigned before = s_commands;
    orb_app_host_t *host = orb_control_host_create(false, command, &s_commands);
    lv_obj_t *parent = page();
    orb_control_model_t model = s_initial;
    assert(host && !orb_control_host_update(host, &model));
    assert(!orb_control_host_attach(host, parent, NULL));
    assert(orb_control_host_attach(host, parent, &model));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    lv_obj_t *root = orb_screen_root(instance);
    assert(lv_obj_get_parent(orb_screen_page_title(instance)) == parent);
    assert_values(instance, ORB_ICON_BATTERY_HIGH " 87%", ORB_LABEL, 68, 41, 68);
    assert(!orb_control_host_update(host, NULL));
    lv_obj_t *number = find_text(root, ORB_ICON_BATTERY_HIGH " 87%");
    size_t reservation = used();
    assert(orb_control_host_update(host, &model));
    assert_values(instance, "87%", ORB_KIT_PLAY, 68, 41, 68);
    assert(find_text(root, "87%") == number && used() == reservation);
    assert(orb_control_host_update(host, &model));
    assert(find_text(root, "87%") == number && used() == reservation);

    model = s_changed;
    model.control_volume = true;
    model.silent = true;
    assert(orb_control_host_update(host, &model));
    assert_values(instance, "9%", ORB_RED, 32, 77, 77);
    /* A real Silent change now updates its retained disc and lights it up. */
    assert(!find_text(root, ORB_ICON_BELL) && find_text(root, ORB_ICON_BELL_SLASH));
    orb_rim_binding_t binding = {0};
    assert(orb_control_descriptor()->ops->rim_binding(instance, &binding));
    assert(binding.property == ORB_CONTROL_VOLUME && binding.value == 77);
    model.battery_pct = 100;
    model.volume = 99;
    orb_app_host_poll(host);
    assert_values(instance, "9%", ORB_RED, 32, 77, 77);

    orb_screen_context_t context = {0};
    assert(orb_screen_save(instance, &context, sizeof(context)));
    /* A selector can change before its queued rebuild. The shell's current
     * property must update the old arc immediately, regardless of its tint. */
    orb_control_value(instance, false, 12);
    assert_values(instance, "9%", ORB_RED, 12, 77, 12);
    assert(orb_control_descriptor()->ops->rim_binding(instance, &binding));
    assert(binding.property == ORB_CONTROL_BRIGHTNESS && binding.value == 12);
    orb_control_value(instance, true, 53);
    assert_values(instance, "9%", ORB_RED, 12, 53, 53);
    assert(orb_screen_restore(instance, &context, sizeof(context)));
    assert_values(instance, "9%", ORB_RED, 12, 53, 53);
    assert(orb_screen_root(instance) == root && s_commands == before);
    orb_app_host_detach_page(host, parent);
    assert(!orb_app_host_current(host));
    lv_obj_delete(parent);
    parent = page();
    model = (orb_control_model_t){.battery_pct = -1, .brightness = 10, .volume = 20, .charging = true};
    assert(orb_control_host_attach(host, parent, &model));
    instance = orb_app_host_current(host);
    assert_values(instance, ORB_ICON_BATTERY_CHARGING " —", ORB_KIT_PLAY, 10, 20, 10);
    assert(orb_control_host_update(host, &model));
    assert_values(instance, "—", ORB_KIT_PLAY, 10, 20, 10);
    lv_obj_delete(parent);
    assert(!orb_app_host_current(host));
    orb_app_host_destroy(host);
    assert(!orb_control_level(NULL));
    orb_control_value(NULL, false, 10);
    orb_control_prepare_launch(NULL, ORB_SHELL_VOICE);
    assert(s_commands == before);
    quiescent();
}

static void actions(bool preview)
{
    orb_app_host_t *host = orb_control_host_create(preview, command, &s_commands);
    lv_obj_t *parent = page();
    assert(host && orb_control_host_attach(host, parent, &s_initial));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    lv_obj_t *root = orb_screen_root(instance);
    const unsigned actions[] = {ORB_CONTROL_SILENT, ORB_CONTROL_QUIET, ORB_CONTROL_FLASHLIGHT,
        ORB_CONTROL_SLEEP, ORB_SHELL_VOICE, ORB_SHELL_SETTINGS, ORB_CONTROL_BRIGHTNESS, ORB_CONTROL_VOLUME};
    for (unsigned i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        lv_obj_t *control = find_action(root, actions[i]);
        assert(control);
        lv_obj_update_layout(root);
        lv_area_t bounds;
        lv_obj_get_coords(control, &bounds);
        unsigned before = s_commands;
        lv_obj_send_event(control, LV_EVENT_CLICKED, NULL);
        assert(s_commands == before + (preview ? 0 : 1));
        if (preview) continue;
        assert(s_last.kind == ORB_INTENT_APP_COMMAND && s_last.origin == ORB_INPUT_TOUCH && !s_last.physical);
        assert(s_last.value.command.target == ORB_SHELL_CONTROL_CENTER && s_last.value.command.action == actions[i]);
        assert(s_last.launch.valid && s_last.launch.rect.x == bounds.x1 && s_last.launch.rect.y == bounds.y1);
        assert(s_last.launch.rect.width == bounds.x2 - bounds.x1 + 1 &&
            s_last.launch.rect.height == bounds.y2 - bounds.y1 + 1);
    }
    unsigned before = s_commands;
    lv_obj_t *voice = find_action(root, ORB_SHELL_VOICE);
    if (!preview) {
        orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
        lv_obj_send_event(voice, LV_EVENT_CLICKED, NULL);
        assert(s_commands == before);
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
        lv_obj_add_state(voice, LV_STATE_PRESSED);
        orb_control_prepare_launch(instance, ORB_SHELL_VOICE);
        assert(!lv_obj_has_state(voice, LV_STATE_PRESSED) && s_commands == before);
    } else {
        orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
        assert(orb_screen_visibility_get(instance) == ORB_VISIBILITY_SUSPENDED);
        assert(orb_screen_back(instance) == ORB_BACK_CONSUMED && !orb_app_host_rim(host));
        lv_obj_send_event(voice, LV_EVENT_CLICKED, NULL);
        assert(s_commands == before);
    }
    orb_app_host_destroy(host);
    lv_obj_delete(parent);
    quiescent();
}

static void pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_pointer_data;
}

static void level_input(bool synthetic, bool volume, bool preview)
{
    orb_control_model_t model = s_initial;
    model.control_volume = volume;
    orb_app_host_t *host = orb_control_host_create(preview, command, &s_commands);
    lv_obj_t *parent = page();
    assert(host && orb_control_host_attach(host, parent, &model));
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, s_display);
    lv_indev_set_read_cb(input, pointer_read);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    if (synthetic) orb_input_mark_synthetic(input);
    lv_obj_update_layout(parent);
    unsigned before = s_commands;
    /* This is the published bottom arc at approximately 20%, not a private
     * knob child or a callback pulled out of the implementation. */
    s_pointer_data = (lv_indev_data_t){.point = {117, 412}, .state = LV_INDEV_STATE_PRESSED};
    lv_indev_read(input);
    assert(s_commands == before + (preview ? 0 : 1));
    if (!preview) {
        assert(s_last.kind == ORB_INTENT_SETTING_SET && s_last.origin == ORB_INPUT_TOUCH);
        assert(s_last.physical == !synthetic);
        assert(s_last.value.setting.key == (volume ? ORB_CONTROL_VOLUME : ORB_CONTROL_BRIGHTNESS));
        assert(abs(s_last.value.setting.value - 20) <= 1);
    }
    s_pointer_data.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    lv_indev_reset(input, NULL);
    lv_indev_delete(input);
    orb_app_host_destroy(host);
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
    assert(!orb_control_host_create(false, command, &s_commands));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    orb_app_host_t *host = orb_control_host_create(false, command, &s_commands);
    lv_obj_t *parent = page();
    assert(host);
    reserve = reserve_remaining();
    assert(!orb_control_host_attach(host, parent, &s_initial));
    assert(!orb_app_host_current(host));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_control_host_attach(host, parent, &s_initial));
    assert_values(orb_app_host_current(host), ORB_ICON_BATTERY_HIGH " 87%", ORB_LABEL, 68, 41, 68);
    orb_app_host_destroy(host);
    lv_obj_delete(parent);
    quiescent();
}

static orb_control_model_t home_model(void)
{
    orb_home_snapshot_t *home = calloc(1, sizeof(*home));
    assert(home);
    home->online = home->synced = home->available = true;
    home->scene_count = 2;
    home->scenes[0] = (orb_home_scene_t){.id = "unsafe", .name = "Good morning", .contains_unlock = true};
    home->scenes[1] = (orb_home_scene_t){.id = "morning", .name = "Good morning"};
    home->accessory_count = 3;
    home->accessories[0] = (orb_home_accessory_t){.id = "lock", .name = "Lock", .favourite = true,
        .kind = ORB_HOME_LOCK, .writable = ORB_HOME_ON};
    home->accessories[1] = (orb_home_accessory_t){.id = "lamp", .name = "East lamp", .favourite = true,
        .kind = ORB_HOME_LIGHT, .reachable = true, .writable = ORB_HOME_ON,
        .state = {.present = ORB_HOME_ON, .on = true}};
    home->accessories[2] = (orb_home_accessory_t){.id = "outlet", .name = "West outlet", .favourite = true,
        .kind = ORB_HOME_OUTLET, .reachable = true, .writable = ORB_HOME_ON,
        .state = {.present = ORB_HOME_ON}};
    assert(orb_control_home_scene(home, 10) == &home->scenes[1]);
    assert(!orb_control_home_scene(home, 18));
    assert(orb_control_home_favourite(home, 0) == &home->accessories[1]);
    assert(orb_control_home_favourite(home, 1) == &home->accessories[2]);
    assert(!orb_control_home_favourite(home, 2));
    orb_control_model_t model = s_initial;
    orb_control_home_project(&model, home, 10);
    assert(model.home_panel && model.home.ready && model.home.has_scene);
    assert(!strcmp(model.home.scene.id, "morning"));
    assert(model.home.present[0] && model.home.present[1]);
    /* Projections outlive the borrowed generation, without another snapshot. */
    memset(home, 0, sizeof(*home));
    free(home);
    return model;
}

static void verify_home(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    (void)phase;
    lv_obj_t *root = orb_screen_root(instance);
    assert(!orb_control_level(instance) && !orb_screen_page_viewport(instance));
    assert(orb_screen_page_title(instance));
    assert(find_text(root, "Good morning") && find_text(root, "East lamp") && find_text(root, "West outlet"));
    lv_obj_t *on = find_text(root, "On"), *off = find_text(root, "Off");
    assert(on && off && lv_color_eq(lv_obj_get_style_text_color(on, 0), ORB_LABEL_LEGACY_SECONDARY));
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, "Home"));
}

static void exercise_home(orb_screen_instance_t *instance)
{
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    orb_rim_binding_t binding = {0};
    assert(!orb_control_descriptor()->ops->rim_binding(instance, &binding));
    orb_screen_context_t context = {0};
    assert(orb_screen_save(instance, &context, sizeof(context)));
    assert(context.version == 1 && context.size == sizeof(context));
    context.version++;
    assert(!orb_screen_restore(instance, &context, sizeof(context)));
    context.version--;
    context.size++;
    assert(!orb_screen_restore(instance, &context, sizeof(context)));
}

static void home_lifecycle(void)
{
    orb_control_model_t initial_model = home_model();
    orb_screen_model_t view = {&initial_model, sizeof(initial_model)};
    orb_screen_model_t original = s_initial_view;
    s_initial_view = view;
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.project = project,
        .intent = record, .allow_synthetic = allow_synthetic, .cue = cue, .defer_rim = true});
    orb_control_model_t changed_model = initial_model;
    strcpy(changed_model.home.scene.name, "Changed scene");
    orb_screen_model_t changed_view = {&changed_model, sizeof(changed_model)};
    orb_screen_update_t initial = {.revisions.device = 1, .revisions.time = 1, .model = &view};
    orb_screen_update_t changed = {.revisions.device = 2, .revisions.time = 2, .model = &changed_view};
    orb_screen_update_t removed = {.revisions.device = 3, .revisions.time = 3};
    orb_screen_update_t empty = {.revisions.device = 4, .revisions.time = 4};
    /* The shell rebuilds this composition atomically. An update, including
     * router staging, must never refit a subset of its old controls. */
    orb_lifecycle_case_t test = {.descriptor = orb_control_descriptor(), .initial = &initial,
        .changed = &changed, .removed = &removed, .empty = &empty,
        .exercise = exercise_home, .verify = verify_home, .quiescent = quiescent};
    orb_test_screen_lifecycle(&host, &test);
    s_initial_view = original;
}

static void home_presentation(bool preview, bool synthetic)
{
    orb_control_model_t model = home_model();
    orb_app_host_t *host = orb_control_host_create(preview, command, &s_commands);
    lv_obj_t *parent = page();
    assert(host && orb_control_host_attach(host, parent, &model));
    orb_screen_instance_t *instance = orb_app_host_current(host);
    assert(lv_obj_get_parent(orb_screen_page_title(instance)) == parent);
    memset(&model, 0, sizeof(model));
    verify_home(instance, ORB_LIFECYCLE_INITIAL);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, s_display);
    lv_indev_set_read_cb(input, pointer_read);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    if (synthetic) orb_input_mark_synthetic(input);
    const unsigned actions[] = {ORB_CONTROL_HOME_SCENE, ORB_CONTROL_HOME_FIRST, ORB_CONTROL_HOME_SECOND};
    for (unsigned i = 0; i < 3; i++) {
        lv_obj_t *control = find_action(orb_screen_root(instance), actions[i]);
        assert(control);
        lv_obj_update_layout(parent);
        lv_area_t area; lv_obj_get_coords(control, &area);
        unsigned before = s_commands;
        s_pointer_data = (lv_indev_data_t){.point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2},
            .state = LV_INDEV_STATE_PRESSED};
        lv_indev_read(input);
        s_pointer_data.state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(input);
        assert(s_commands == before + (preview ? 0 : 1));
        if (!preview) {
            assert(s_last.kind == ORB_INTENT_APP_COMMAND && s_last.origin == ORB_INPUT_TOUCH);
            assert(s_last.physical == !synthetic && s_last.value.command.target == ORB_SHELL_CONTROL_CENTER);
            assert(s_last.value.command.action == actions[i] && s_last.launch.valid);
        }
    }
    unsigned before = s_commands;
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    lv_obj_send_event(find_action(orb_screen_root(instance), ORB_CONTROL_HOME_SCENE), LV_EVENT_CLICKED, NULL);
    assert(s_commands == before);
    lv_indev_reset(input, NULL);
    lv_indev_delete(input);
    orb_app_host_detach_page(host, parent);
    assert(!orb_app_host_current(host));
    lv_obj_delete(parent);
    parent = page();
    model = s_initial;
    assert(orb_control_host_attach(host, parent, &model));
    assert(orb_control_level(orb_app_host_current(host)));
    orb_app_host_detach_page(host, parent);
    lv_obj_delete(parent);
    parent = page();
    orb_control_home_project(&model, NULL, 10);
    size_t reserve = reserve_remaining();
    assert(!orb_control_host_attach(host, parent, &model));
    assert(!orb_app_host_current(host));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reserve);
    assert(orb_control_host_attach(host, parent, &model));
    instance = orb_app_host_current(host);
    assert(!orb_control_level(instance));
    lv_obj_t *open = find_action(orb_screen_root(instance), ORB_SHELL_HOME);
    assert(open && find_text(orb_screen_root(instance), "Open Home"));
    assert(!find_action(orb_screen_root(instance), ORB_CONTROL_HOME_FIRST));
    lv_obj_add_state(open, LV_STATE_PRESSED);
    orb_control_prepare_launch(instance, ORB_SHELL_HOME);
    assert(!lv_obj_has_state(open, LV_STATE_PRESSED));
    lv_obj_delete(parent);
    assert(!orb_app_host_current(host));
    orb_app_host_destroy(host);
    assert(s_commands == before);
    quiescent();
}

static void home_loading_budget(void)
{
    orb_home_snapshot_t home = {.online = true};
    bool waiting = false; uint32_t started = 0;
    orb_control_model_t model = {0}; orb_control_home_project(&model, &home, 10);
    model.home.wait_started = orb_control_home_wait(&waiting, &started, &home);
    uint32_t original = started;
    orb_app_host_t *host = orb_control_host_create(false, command, &s_commands);
    lv_obj_t *parent = page(); assert(host && orb_control_host_attach(host, parent, &model));
    advance(400); assert(orb_kit_motion_timer_running()); advance(19600);
    orb_app_host_destroy(host); lv_obj_delete(parent);
    model.home.wait_started = orb_control_home_wait(&waiting, &started, &home); assert(started == original);
    host = orb_control_host_create(false, command, &s_commands); parent = page();
    assert(host && orb_control_host_attach(host, parent, &model));
    advance(11000); assert(!orb_kit_motion_timer_running());
    orb_app_host_destroy(host); lv_obj_delete(parent);
    home.synced = true; (void)orb_control_home_wait(&waiting, &started, &home); assert(!waiting);
    home.synced = false; (void)orb_control_home_wait(&waiting, &started, &home);
    assert(waiting && started != original); quiescent();
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    assert(s_display);
    orb_ui_init(NULL);
    const orb_screen_descriptor_t *descriptor = orb_control_descriptor();
    assert(orb_screen_descriptor_valid(descriptor, NULL) && descriptor->id == ORB_SHELL_CONTROL_CENTER);
    const orb_screen_descriptor_t *screens[] = {descriptor};
    orb_app_manifest_t manifest = {.id = descriptor->id, .name = "Control Center",
        .screens = screens, .screen_count = 1, .entry = descriptor->id};
    orb_screen_registry_t registry;
    orb_screen_registry_init(&registry);
    assert(orb_screen_registry_add(&registry, &manifest, NULL));
    assert(orb_screen_registry_find(&registry, descriptor->id) == descriptor);
    orb_app_host_t *warm = orb_control_host_create(false, command, &s_commands);
    lv_obj_t *parent = page();
    assert(warm && orb_control_host_attach(warm, parent, &s_initial));
    orb_app_host_destroy(warm);
    lv_obj_delete(parent);
    advance(1600);
    s_workspace_baseline = used();
    s_timer_baseline = timers();
    lifecycle();
    presentation();
    actions(false);
    actions(true);
    level_input(false, false, false);
    level_input(true, true, false);
    level_input(false, false, true);
    admission();
    home_lifecycle();
    home_presentation(false, false);
    home_presentation(false, true);
    home_presentation(true, false);
    home_loading_budget();
    puts("control_screen: lifecycle, retained values, staging, input, preview and admission passed");
    return 0;
}
