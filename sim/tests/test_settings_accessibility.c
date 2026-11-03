/* Production Accessibility uses the shared lifetime/restore harness. No test
 * depends on widget child order, geometry or source-function spelling. */
#include "orb_app_settings_accessibility.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "orb_screen_lifecycle.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_intents, s_timer_baseline;
static size_t s_workspace_baseline;
static orb_intent_t s_last_intent;
static uint32_t s_page;
static int32_t s_saved_scroll;
static orb_screen_update_t s_projection;

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
    assert(orb_screen_back(instance) == ORB_BACK_LEAVE_SCREEN);
    unsigned before = s_intents;
    lv_obj_t *list = orb_app_settings_accessibility_scroll(instance);
    s_saved_scroll = 0;
    if (list) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        s_saved_scroll = LV_MIN(24, maximum);
        orb_page_restore(list, &(orb_page_context_t){.scroll = s_saved_scroll});
    }
    assert(!orb_app_settings_accessibility_open(instance, 0, ORB_INPUT_PROGRAMMATIC, false, NULL));
    if (!s_page) {
        for (unsigned index = 0; index < 5; index++) {
            assert(orb_app_settings_accessibility_open(instance, index, ORB_INPUT_TOUCH, false, NULL));
            assert(s_last_intent.kind == ORB_INTENT_NAVIGATE);
            assert(s_last_intent.value.navigate.screen == ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID);
            assert(s_last_intent.value.navigate.args.page == index + 1);
        }
        assert(!orb_app_settings_accessibility_open(instance, 5, ORB_INPUT_TOUCH, true, NULL));
        assert(!orb_app_settings_accessibility_choose_size(instance, 1, ORB_INPUT_TOUCH, true));
        assert(!orb_app_settings_accessibility_toggle(instance, ORB_INPUT_TOUCH, true));
        assert(s_intents == before + 5);
    } else if (s_page == ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE) {
        assert(!orb_app_settings_accessibility_open(instance, 0, ORB_INPUT_TOUCH, true, NULL));
        assert(orb_app_settings_accessibility_choose_size(instance, 0, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTING_ACCESS_TEXT_SIZE);
        assert(s_last_intent.value.setting.value == 0);
        assert(orb_app_settings_accessibility_choose_size(instance, 2, ORB_INPUT_BUTTON, true));
        assert(s_last_intent.value.setting.value == 2);
        assert(!orb_app_settings_accessibility_choose_size(instance, ORB_CONTENT_SIZE_COUNT, ORB_INPUT_TOUCH, true));
        assert(!orb_app_settings_accessibility_choose_size(instance, 1, ORB_INPUT_CROWN, true));
        assert(!orb_app_settings_accessibility_toggle(instance, ORB_INPUT_TOUCH, true));
        assert(s_intents == before + 2);
    } else {
        assert(!orb_screen_rim_acquire(instance));
        assert(orb_app_settings_accessibility_toggle(instance, ORB_INPUT_TOUCH, true));
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTING_ACCESS_TEXT_SIZE + s_page - 1);
        assert(s_last_intent.value.setting.value == 0);
        assert(!orb_app_settings_accessibility_toggle(instance, ORB_INPUT_CROWN, true));
        assert(!orb_app_settings_accessibility_choose_size(instance, 1, ORB_INPUT_TOUCH, true));
        assert(s_intents == before + 1);
    }
}

static void verify(orb_screen_instance_t *instance, orb_lifecycle_phase_t phase)
{
    assert(orb_screen_alive(instance));
    assert(orb_app_settings_accessibility_page(instance) == s_page);
    assert(!lv_obj_is_clickable(orb_screen_root(instance)));
    assert(lv_obj_is_event_bubble(orb_screen_root(instance)));
    static const char *s_titles[] = {
        "Accessibility", "Text size", "Bold text", "Reduce motion", "Reduce transparency", "Increase contrast",
    };
    orb_screen_chrome_t chrome;
    orb_screen_chrome(instance, &chrome);
    assert(!strcmp(chrome.title, s_titles[s_page]));
    bool changed = phase == ORB_LIFECYCLE_CHANGED || phase == ORB_LIFECYCLE_RESTORED;
    assert(!strcmp(orb_app_settings_accessibility_value(instance, 0), changed ? "Largest" : "Default"));
    for (unsigned i = 1; i < 5; i++)
        assert(!strcmp(orb_app_settings_accessibility_value(instance, i), changed ? "On" : "Off"));
    lv_obj_t *list = orb_app_settings_accessibility_scroll(instance);
    assert(orb_screen_page_viewport(instance) == list);
    assert(orb_screen_page_title(instance));
    assert((list != NULL) == (s_page <= ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE));
    if (list && phase >= ORB_LIFECYCLE_RESTORED) {
        lv_obj_update_layout(orb_screen_root(instance));
        int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
        assert(lv_obj_get_scroll_y(list) == orb_screen_clamp_scroll(s_saved_scroll, maximum));
    }
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context;
    assert(id == ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID);
    *update = s_projection;
    return true;
}

static void scroll_began(lv_event_t *event)
{
    unsigned *begins = lv_event_get_user_data(event);
    (*begins)++;
}

static void initial_projection(orb_screen_host_t *host)
{
    unsigned before = s_intents;
    orb_settings_accessibility_model_t seeded = {.content_size = ORB_CONTENT_SIZE_AX3,
        .bold = true, .reduce_motion = true, .reduce_transparency = true, .contrast = true, .interactive = true};
    orb_screen_model_t model = {&seeded, sizeof(seeded)};
    s_projection = (orb_screen_update_t){.revisions.settings = 1, .model = &model};
    host->config.project = project;
    for (unsigned page = 0; page <= ORB_SETTINGS_ACCESS_CONTRAST_PAGE; page++) {
        unsigned begins = 0;
        lv_obj_t *parent = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(parent);
        lv_obj_set_size(parent, 466, 466);
        lv_obj_set_scrollable(parent, false);
        lv_obj_add_event_cb(parent, scroll_began, LV_EVENT_SCROLL_BEGIN, &begins);
        orb_screen_instance_t instance = {0};
        orb_screen_args_t args = {.page = page};
        assert(orb_screen_create(&instance, host, orb_app_settings_accessibility_descriptor(), parent, &args, false));
        assert(!strcmp(orb_app_settings_accessibility_value(&instance, 0), "Largest"));
        for (unsigned i = 1; i < 5; i++)
            assert(!strcmp(orb_app_settings_accessibility_value(&instance, i), "On"));
        lv_obj_t *list = orb_app_settings_accessibility_scroll(&instance);
        lv_obj_t *title = orb_screen_page_title(&instance);
        int32_t initial_scroll = list ? lv_obj_get_scroll_y(list) : 0;
        unsigned initial_begins = begins;
        assert(orb_screen_update(&instance, &s_projection));
        orb_screen_update_t equal = s_projection;
        equal.revisions.settings++;
        assert(orb_screen_update(&instance, &equal));
        assert(orb_app_settings_accessibility_scroll(&instance) == list);
        assert(orb_screen_page_title(&instance) == title);
        assert(!list || lv_obj_get_scroll_y(list) == initial_scroll);
        assert(begins == initial_begins);
        /* Activation availability does not alter the legacy visible controls. */
        orb_settings_accessibility_model_t unavailable = seeded;
        unavailable.interactive = false;
        orb_screen_model_t unavailable_model = {&unavailable, sizeof(unavailable)};
        equal.model = &unavailable_model;
        equal.revisions.settings++;
        assert(orb_screen_update(&instance, &equal));
        assert(orb_app_settings_accessibility_scroll(&instance) == list);
        assert(orb_screen_page_title(&instance) == title && begins == initial_begins);
        orb_screen_destroy(&instance);
        lv_obj_delete(parent);
        quiescent();
    }
    host->config.project = NULL;
    assert(s_intents == before);
}

static lv_obj_t *find_label(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *shown = lv_label_get_text(root);
        size_t length = strlen(shown), ellipsis = sizeof("…") - 1;
        /* The common row engine keeps at most two lines, then one terminal
         * U+2026. Match that displayed prefix to target its retained row. */
        if (!strcmp(shown, text) || (length > ellipsis &&
            !strcmp(shown + length - ellipsis, "…") && !strncmp(text, shown, length - ellipsis))) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_indev_data_t s_pointer_input;

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    *data = s_pointer_input;
}

static void inline_toggles(orb_screen_host_t *host)
{
    orb_settings_accessibility_model_t value = {.interactive = true};
    orb_screen_model_t model = {&value, sizeof(value)};
    orb_screen_update_t update = {.revisions.settings = 1, .model = &model};
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, orb_app_settings_accessibility_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&instance, &update));
    lv_indev_t *pointer = lv_indev_create();
    assert(pointer);
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, lv_obj_get_display(orb_screen_root(&instance)));
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    static const char *s_titles[] = {"Bold text", "Reduce motion", "Reduce transparency", "Increase contrast"};
    for (unsigned i = 0; i < 4; i++) {
        lv_obj_t *title = find_label(orb_screen_root(&instance), s_titles[i]);
        assert(title);
        lv_obj_t *row = lv_obj_get_parent(title);
        assert(orb_kit_row_switch(row));
        assert(!orb_kit_switch_get_checked(orb_kit_row_switch(row)));
        lv_obj_t *list = orb_app_settings_accessibility_scroll(&instance);
        int32_t scroll = lv_obj_get_y(row) + lv_obj_get_height(row) / 2 - 105;
        orb_round_list_scroll_to(list, LV_MAX(0, scroll));
        orb_round_list_update(list);
        lv_area_t bounds;
        assert(orb_round_list_row_bounds(row, &bounds));
        s_pointer_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
        unsigned before = s_intents;
        s_pointer_input.state = LV_INDEV_STATE_PRESSED;
        lv_indev_read(pointer);
        lv_tick_inc(32);
        lv_timer_handler();
        s_pointer_input.state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(pointer);
        assert(s_intents == before + 1);
        assert(s_last_intent.kind == ORB_INTENT_SETTING_SET);
        assert(s_last_intent.value.setting.key == ORB_SETTING_ACCESS_BOLD + i);
        assert(s_last_intent.value.setting.value == 1);
        assert(orb_kit_switch_get_checked(orb_kit_row_switch(row)));
        /* A deferred or rejected intent projects the same applied truth. The
         * native switch must roll back even when no model field changed. */
        update.revisions.settings++;
        assert(orb_screen_update(&instance, &update));
        assert(!orb_kit_switch_get_checked(orb_kit_row_switch(row)));
    }
    lv_indev_delete(pointer);
    orb_screen_destroy(&instance);
    quiescent();
}

static void detail_rollback(orb_screen_host_t *host)
{
    orb_settings_accessibility_model_t value = {.interactive = true};
    orb_screen_model_t model = {&value, sizeof(value)};
    orb_screen_update_t update = {.revisions.settings = 1, .model = &model};
    orb_screen_instance_t instance = {0};
    orb_screen_args_t args = {.page = ORB_SETTINGS_ACCESS_BOLD_PAGE};
    assert(orb_screen_create(&instance, host, orb_app_settings_accessibility_descriptor(),
        lv_screen_active(), &args, false));
    assert(orb_screen_update(&instance, &update));
    lv_obj_t *toggle = NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(orb_screen_root(&instance)); i++) {
        lv_obj_t *child = lv_obj_get_child(orb_screen_root(&instance), i);
        if ((uintptr_t)lv_obj_get_user_data(child) == 265) toggle = child;
    }
    assert(toggle && !orb_kit_switch_get_checked(toggle));
    unsigned before = s_intents;
    assert(orb_app_settings_accessibility_toggle(&instance, ORB_INPUT_BUTTON, true));
    orb_kit_switch_set_checked(toggle, true, false);
    assert(s_intents == before + 1 && s_last_intent.value.setting.value == 1);
    assert(orb_kit_switch_get_checked(toggle));
    update.revisions.settings++;
    assert(orb_screen_update(&instance, &update));
    assert(!orb_kit_switch_get_checked(toggle));
    snprintf(value.notice, sizeof(value.notice), "Couldn’t change text. Try again");
    update.revisions.settings++;
    assert(orb_screen_update(&instance, &update));
    assert(find_label(orb_screen_root(&instance), value.notice));
    assert(!strcmp(orb_app_settings_accessibility_value(&instance, 1), "Off"));
    before = s_intents;
    assert(orb_app_settings_accessibility_toggle(&instance, ORB_INPUT_BUTTON, true));
    assert(s_intents == before + 1 && s_last_intent.value.setting.value == 1);
    value.bold = true;
    value.notice[0] = '\0';
    update.revisions.settings++;
    assert(orb_screen_update(&instance, &update));
    assert(!strcmp(orb_app_settings_accessibility_value(&instance, 1), "On"));
    orb_screen_destroy(&instance);
    quiescent();
}

static unsigned label_count(lv_obj_t *root, const char *text)
{
    unsigned count = lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        count += label_count(lv_obj_get_child(root, i), text);
    return count;
}

static void notice_status(orb_screen_host_t *host)
{
    static const unsigned s_pages[] = {ORB_SETTINGS_ACCESSIBILITY_PAGE,
        ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE, ORB_SETTINGS_ACCESS_BOLD_PAGE};
    const char *directory = getenv("ORB_ACCESSIBILITY_SHOTS");
    for (unsigned profile = 0; profile < 3; profile++) {
        assert(orb_fonts_set_content_size(profile ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        assert(orb_fonts_set_bold_text(profile != 0) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(profile == 2);
        for (unsigned i = 0; i < sizeof(s_pages) / sizeof(s_pages[0]); i++) {
            orb_settings_accessibility_model_t value = {.content_size = orb_fonts_content_size(),
                .bold = orb_fonts_bold_text(), .reduce_transparency = profile == 2, .interactive = true};
            orb_screen_model_t model = {&value, sizeof(value)};
            s_projection = (orb_screen_update_t){.revisions.settings = 1, .model = &model};
            host->config.project = project;
            orb_screen_instance_t instance = {0};
            orb_screen_args_t args = {.page = s_pages[i]};
            assert(orb_screen_create(&instance, host, orb_app_settings_accessibility_descriptor(),
                lv_screen_active(), &args, false));
            snprintf(value.notice, sizeof(value.notice), "Not saved");
            s_projection.revisions.settings++;
            assert(orb_screen_update(&instance, &s_projection));
            lv_obj_t *root = orb_screen_root(&instance);
            assert(label_count(root, value.notice) == 1);
            lv_obj_t *caption = find_label(root, value.notice);
            assert(caption && !strcmp(lv_label_get_text(caption), value.notice));
            lv_obj_update_layout(root);
            const lv_font_t *font = lv_obj_get_style_text_font(caption, 0);
            assert(lv_obj_get_height(caption) == font->line_height);
            lv_area_t box;
            lv_obj_get_coords(caption, &box);
            assert(lv_obj_get_width(caption) <= orb_chord_width(box.y1, box.y2, ORB_RIM_MARGIN_TEXT));
            if (directory) {
                char path[256];
                snprintf(path, sizeof(path), "%s/notice-%u-%u.png", directory, profile, s_pages[i]);
                lv_refr_now(NULL);
                assert(sim_display_save_png(path));
            }
            value.notice[0] = '\0';
            s_projection.revisions.settings++;
            assert(orb_screen_update(&instance, &s_projection));
            assert(!find_label(root, "Not saved"));
            orb_screen_destroy(&instance);
            assert(orb_screen_create(&instance, host, orb_app_settings_accessibility_descriptor(),
                lv_screen_active(), &args, false));
            assert(!find_label(orb_screen_root(&instance), "Not saved"));
            orb_screen_destroy(&instance);
            quiescent();
        }
    }
    host->config.project = NULL;
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    orb_ui_init(NULL);
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_ACCESSIBILITY_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent});
    orb_settings_accessibility_model_t first = {.interactive = true};
    orb_settings_accessibility_model_t changed = {.content_size = ORB_CONTENT_SIZE_AX3,
        .bold = true, .reduce_motion = true, .reduce_transparency = true, .contrast = true,
        .interactive = true, .notice = "Text pack unavailable"};
    orb_settings_accessibility_model_t empty = {0};
    orb_screen_model_t first_model = {&first, sizeof(first)};
    orb_screen_model_t changed_model = {&changed, sizeof(changed)};
    orb_screen_model_t empty_model = {&empty, sizeof(empty)};
    orb_screen_update_t initial = {.revisions.settings = 1, .model = &first_model};
    orb_screen_update_t next = {.revisions.settings = 2, .model = &changed_model};
    orb_screen_update_t removed = {.revisions.settings = 3, .model = &empty_model};
    orb_screen_update_t missing = {.revisions.settings = 4};
    orb_screen_instance_t warm = {0};
    assert(orb_screen_create(&warm, &host, orb_app_settings_accessibility_descriptor(), lv_screen_active(), NULL, false));
    assert(orb_screen_update(&warm, &initial));
    orb_screen_destroy(&warm);
    s_timer_baseline = timer_count();
    s_workspace_baseline = workspace_used();
    for (s_page = ORB_SETTINGS_ACCESSIBILITY_PAGE; s_page <= ORB_SETTINGS_ACCESS_CONTRAST_PAGE; s_page++) {
        orb_screen_args_t args = {.page = s_page};
        orb_lifecycle_case_t test = {.descriptor = orb_app_settings_accessibility_descriptor(), .args = &args,
            .initial = &initial, .changed = &next, .removed = &removed, .empty = &missing,
            .exercise = exercise, .verify = verify, .quiescent = quiescent};
        orb_test_screen_lifecycle(&host, &test);
    }
    orb_screen_instance_t preview = {0};
    assert(orb_screen_create(&preview, &host, orb_app_settings_accessibility_descriptor(), lv_screen_active(),
        &(orb_screen_args_t){.page = ORB_SETTINGS_ACCESS_BOLD_PAGE}, true));
    assert(orb_screen_update(&preview, &initial));
    assert(!orb_app_settings_accessibility_toggle(&preview, ORB_INPUT_TOUCH, true));
    orb_screen_destroy(&preview);
    quiescent();
    initial_projection(&host);
    inline_toggles(&host);
    detail_rollback(&host);
    notice_status(&host);
    puts("Settings Accessibility: lifecycle, page/scroll context, typed actions and preview suppression passed");
    return 0;
}
