/* Compatibility-host contracts exercise public actions and value context;
 * no private page fields or kit child topology are inspected. */
#include "orb_app_settings.h"
#include "orb_app_settings_sound.h"
#include "orb_app_settings_display.h"
#include "settings_host_fixture.h"
#include "orb_kit.h"
#include "orb_ui.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>

static bool accept(void *context, const orb_intent_t *intent)
{
    (void)context;
    (void)intent;
    return true;
}

static size_t workspace_used(void)
{
    orb_memory_stats_t memory;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &memory);
    return memory.used;
}

static void sound(orb_settings_host_t *host, lv_obj_t *page)
{
    assert(orb_settings_host_stage_sound(host, lv_screen_active(), false, "", ""));
    assert(orb_settings_host_commit_staged(host, page));
}

static void open_page(orb_settings_host_t *host, lv_obj_t *page, orb_settings_sound_page_t child)
{
    assert(orb_app_settings_sound_open(orb_settings_host_current(host), child, ORB_INPUT_TOUCH, true, NULL));
    sound(host, page);
    assert(orb_app_settings_sound_page(orb_settings_host_current(host)) == child);
}

static void local_back(orb_settings_host_t *host, lv_obj_t *page, orb_settings_sound_page_t parent)
{
    assert(orb_settings_host_back_for(host, ORB_SETTINGS_SOUND_SCREEN_ID));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_SOUND_SCREEN_ID) == parent);
    sound(host, page);
    assert(orb_app_settings_sound_page(orb_settings_host_current(host)) == parent);
}

static void scroll_to(orb_settings_host_t *host, int32_t value)
{
    lv_obj_t *list = orb_settings_host_list(host);
    assert(list);
    lv_obj_update_layout(list);
    int32_t maximum = LV_MAX(0, lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list));
    assert(maximum >= value);
    lv_obj_scroll_to_y(list, value, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(list) == value);
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    size_t baseline = workspace_used();
    lv_obj_t *page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, 466, 466);
    lv_obj_set_scrollable(page, false);
    orb_settings_host_t *host = orb_settings_host_create(false, NULL, accept, NULL);
    assert(host);
    sound(host, page);
    scroll_to(host, 96);
    open_page(host, page, ORB_SETTINGS_SOUND_QUIET_PAGE);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 0);
    open_page(host, page, ORB_SETTINGS_SOUND_START_PAGE);
    assert(orb_app_settings_sound_step_hour(orb_settings_host_current(host), 1, ORB_INPUT_TOUCH, true));
    assert(orb_app_settings_sound_changed(orb_settings_host_current(host)) == ORB_SETTINGS_SOUND_START_PAGE);
    local_back(host, page, ORB_SETTINGS_SOUND_QUIET_PAGE);
    assert(orb_app_settings_sound_changed(orb_settings_host_current(host)) == ORB_SETTINGS_SOUND_START_PAGE);
    local_back(host, page, ORB_SETTINGS_SOUND_PAGE);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 96);
    open_page(host, page, ORB_SETTINGS_SOUND_PALETTE_PAGE);
    scroll_to(host, 288);

    orb_settings_host_setting_queue(host, ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS, 7);
    orb_shell_snapshot_t status = {.hub_name = "Test Mac"};
    assert(settings_show_about(host, page, &status, "Connected", true));
    sound(host, page);
    assert(orb_app_settings_sound_page(orb_settings_host_current(host)) == ORB_SETTINGS_SOUND_PALETTE_PAGE);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 288);
    assert(orb_app_settings_sound_changed(orb_settings_host_current(host)) == ORB_SETTINGS_SOUND_START_PAGE);
    local_back(host, page, ORB_SETTINGS_SOUND_PAGE);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 96);
    open_page(host, page, ORB_SETTINGS_SOUND_PALETTE_PAGE);
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 0);

    /* A copied preview must never acquire the live frame's pending write. */
    orb_settings_host_t *preview = NULL;
    assert(orb_settings_host_clone(&preview, host, true));
    int32_t value;
    assert(!orb_settings_host_setting_take(preview, ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS, &value));
    assert(orb_settings_host_setting_take(host, ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS, &value) && value == 7);
    assert(!orb_settings_host_setting_take(host, ORB_SETTING_DISPLAY_ALWAYS_BRIGHTNESS, &value));
    orb_settings_host_destroy(preview);

    local_back(host, page, ORB_SETTINGS_SOUND_PAGE);
    open_page(host, page, ORB_SETTINGS_SOUND_QUIET_PAGE);
    open_page(host, page, ORB_SETTINGS_SOUND_START_PAGE);
    assert(orb_settings_host_back_for(host, ORB_SETTINGS_SOUND_SCREEN_ID));
    assert(settings_show_about(host, page, &status, "Connected", true));
    sound(host, page);
    assert(orb_app_settings_sound_page(orb_settings_host_current(host)) == ORB_SETTINGS_SOUND_QUIET_PAGE);
    assert(orb_app_settings_sound_changed(orb_settings_host_current(host)) == ORB_SETTINGS_SOUND_START_PAGE);
    orb_settings_host_destroy(host);
    lv_obj_delete(page);
    assert(workspace_used() == baseline);
    puts("Sound host: group Back, fresh child entry, outer return and no pending-write replay passed");
    return 0;
}
