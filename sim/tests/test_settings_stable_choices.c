#include "orb_app_settings_accessibility.h"
#include "orb_app_settings_root.h"
#include "orb_app_settings_units.h"
#include "orb_fonts.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static orb_screen_model_t s_model;
static uint64_t s_revision = 1;
static unsigned s_allocations;

static void *counted_alloc(size_t size) { s_allocations++; return lv_malloc(size); }
static void counted_free(void *pointer) { lv_free(pointer); }

static void refresh(void)
{
    for (unsigned i = 0; i < 10; i++) { lv_tick_inc(20); lv_timer_handler(); }
    lv_refr_now(NULL);
}

static bool project(void *context, orb_screen_id_t id, orb_screen_update_t *update)
{
    (void)context; (void)id;
    *update = (orb_screen_update_t){.model = &s_model,
        .revisions = {.model = s_revision, .settings = s_revision, .device = s_revision}};
    return true;
}

static bool intent(void *context, const orb_intent_t *value)
{
    (void)context;
    assert(value->kind == ORB_INTENT_SETTING_SET);
    return true;
}

static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static void publish(orb_screen_instance_t *instance)
{
    orb_screen_update_t update;
    s_revision++;
    assert(project(NULL, 0, &update));
    assert(orb_screen_update(instance, &update));
    lv_obj_update_layout(orb_screen_root(instance));
}

static unsigned object_count(lv_obj_t *root)
{
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) count += object_count(lv_obj_get_child(root, i));
    return count;
}

static lv_obj_t *visible_check(lv_obj_t *root)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) && lv_obj_get_style_text_opa(root, 0) &&
        !strcmp(lv_label_get_text(root), ORB_ICON_CHECK)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = visible_check(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}

static unsigned check_count(lv_obj_t *root)
{
    if (lv_obj_is_hidden(root)) return 0;
    unsigned count = lv_obj_check_type(root, &lv_label_class) && lv_obj_get_style_text_opa(root, 0) &&
        !strcmp(lv_label_get_text(root), ORB_ICON_CHECK);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) count += check_count(lv_obj_get_child(root, i));
    return count;
}

static void choice(orb_screen_host_t *host, const char *name, const char *shot)
{
    orb_settings_root_model_t glass = {.glass_preference = 1, .interactive = true};
    orb_settings_units_model_t temperature = {.temperature = ORB_TEMPERATURE_F, .ready = true, .known = true};
    orb_settings_accessibility_model_t text = {.interactive = true};
    const orb_screen_descriptor_t *descriptor;
    orb_screen_args_t args = {0};
    const char *title;
    bool is_glass = !strcmp(name, "glass"), is_temperature = !strcmp(name, "temperature");
    bool reflow = !strcmp(name, "text-size-reflow");
    if (is_glass) {
        descriptor = orb_app_settings_root_descriptor();
        args.page = ORB_SETTINGS_GLASS_PAGE;
        s_model = (orb_screen_model_t){&glass, sizeof(glass)};
        title = "Tinted";
    } else if (is_temperature) {
        descriptor = orb_app_settings_temperature_descriptor();
        s_model = (orb_screen_model_t){&temperature, sizeof(temperature)};
        title = "°C";
    } else {
        assert(!strcmp(name, "text-size") || reflow);
        descriptor = orb_app_settings_accessibility_descriptor();
        args.page = ORB_SETTINGS_ACCESS_TEXT_SIZE_PAGE;
        s_model = (orb_screen_model_t){&text, sizeof(text)};
        title = reflow ? "Largest" : "Larger";
    }
    orb_screen_instance_t instance = {0};
    assert(orb_screen_create(&instance, host, descriptor, lv_screen_active(), &args, false));
    publish(&instance);
    lv_obj_t *list = orb_screen_page_viewport(&instance);
    assert(list);
    /* Put a different choice below the centre, as a person sees it before
     * tapping. An applied preference must not move a new target under them. */
    orb_page_restore(list, &(orb_page_context_t){.scroll = is_glass ? 72 : is_temperature ? 100 : 24});
    lv_obj_update_layout(list);
    refresh();
    int32_t scroll = lv_obj_get_scroll_y(list);
    lv_obj_t *label = find_text(list, title);
    assert(label);
    lv_area_t before, before_row;
    lv_obj_get_coords(label, &before);
    lv_obj_get_coords(lv_obj_get_parent(label), &before_row);
    unsigned allocations = s_allocations, objects = object_count(orb_screen_root(&instance));
    if (is_glass) {
        assert(orb_app_settings_glass_select(&instance, 2, ORB_INPUT_TOUCH, true));
        glass.glass_preference = 2;
    } else if (is_temperature) {
        assert(orb_app_settings_temperature_select(&instance, ORB_SETTINGS_TEMPERATURE_C, ORB_INPUT_TOUCH, true));
        temperature.temperature_owner = true;
        temperature.temperature = ORB_TEMPERATURE_C;
    } else {
        text.content_size = reflow ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_LARGER;
        assert(orb_app_settings_accessibility_choose_size(&instance, text.content_size, ORB_INPUT_TOUCH, true));
        if (reflow) assert(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    }
    publish(&instance);
    lv_obj_t *updated_list = orb_screen_page_viewport(&instance);
    if (!reflow) assert(lv_obj_get_scroll_y(updated_list) == scroll);
    label = find_text(updated_list, title);
    assert(label);
    lv_area_t after, after_row;
    lv_obj_get_coords(label, &after);
    lv_obj_get_coords(lv_obj_get_parent(label), &after_row);
    if (!reflow) assert(before_row.x1 == after_row.x1 && before_row.y1 == after_row.y1 &&
        before_row.x2 == after_row.x2 && before_row.y2 == after_row.y2);
    else {
        lv_area_t viewport;
        lv_obj_get_coords(updated_list, &viewport);
        assert(after.y1 >= viewport.y1 && after.y2 <= viewport.y2);
    }
    refresh();
    lv_obj_t *check = visible_check(updated_list);
    assert(check);
    lv_area_t checked;
    lv_obj_get_coords(check, &checked);
    lv_area_t row_bounds;
    assert(orb_round_list_row_bounds(lv_obj_get_parent(label), &row_bounds));
    assert(checked.y1 >= row_bounds.y1 && checked.y2 <= row_bounds.y2);
    if (is_glass || is_temperature) {
        assert(updated_list == list);
        assert(s_allocations == allocations && object_count(orb_screen_root(&instance)) == objects);
        for (unsigned enabled = 0; enabled < 2; enabled++) {
            if (is_glass) glass.interactive = enabled;
            else temperature.prefs_pending = !enabled;
            allocations = s_allocations;
            publish(&instance);
            assert(s_allocations == allocations && object_count(orb_screen_root(&instance)) == objects);
            updated_list = orb_screen_page_viewport(&instance);
            refresh();
            assert(lv_obj_get_scroll_y(updated_list) == scroll);
            assert(check_count(updated_list) == 1);
        }
        if (is_temperature) {
            strcpy(temperature.prefs_error, "rejected");
            publish(&instance);
            assert(orb_screen_page_viewport(&instance) == list && object_count(orb_screen_root(&instance)) == objects);
            assert(lv_obj_get_scroll_y(list) == scroll);
            memset(temperature.notice, 'W', sizeof(temperature.notice) - 1);
            allocations = s_allocations;
            publish(&instance);
            assert(s_allocations == allocations && object_count(orb_screen_root(&instance)) == objects);
            temperature.prefs_error[0] = temperature.notice[0] = '\0';
            publish(&instance);
            refresh();
            assert(check_count(list) == 1 && lv_obj_get_scroll_y(list) == scroll);
        }
    }
    if (shot) {
        lv_refr_now(NULL);
        assert(sim_display_save_png(shot));
    }
    orb_screen_destroy(&instance);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    lv_init();
    assert(sim_display_create());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_scrollable(lv_screen_active(), false);
    orb_ui_init(&(orb_ui_config_t){.alloc = counted_alloc, .free = counted_free});
    orb_nav_t nav;
    orb_nav_init(&nav, ORB_SETTINGS_ROOT_SCREEN_ID);
    orb_screen_host_t host;
    orb_screen_host_init(&host, &(orb_screen_host_config_t){.nav = &nav, .intent = intent, .project = project});
    choice(&host, argv[1], argc == 3 ? argv[2] : NULL);
    printf("Settings %s choice retains its place\n", argv[1]);
    return 0;
}
