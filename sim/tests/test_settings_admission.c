/* Covers the introduced WORKSPACE admission boundary through production host
 * APIs. The outer workspace rollback still needs the integration test below. */
#include "orb_app_settings.h"
#include "orb_app_settings_units.h"
#include "orb_app_settings_root.h"
#include "orb_kit.h"
#include "orb_page.h"
#include "settings_host_fixture.h"
#include "sim_display.h"

#include <assert.h>
#include <stdio.h>

static unsigned s_accepted;

static bool accept(void *context, const orb_intent_t *value)
{
    (void)context;
    (void)value;
    s_accepted++;
    return true;
}

static size_t used(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    return stats.used;
}

static size_t reserve_remaining(void)
{
    orb_memory_stats_t stats;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &stats);
    /* Telemetry includes tracked host allocations, while admission does not.
     * Fill the actual remaining admission through the public reserve seam. */
    size_t chunk = 1, held = 0;
    while (chunk <= stats.budget / 2) chunk *= 2;
    for (; chunk; chunk /= 2)
        if (orb_memory_reserve(ORB_MEMORY_WORKSPACE, chunk)) held += chunk;
    return held;
}

static lv_obj_t *page(void)
{
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 466, 466);
    lv_obj_set_scrollable(parent, false);
    lv_obj_set_clickable(parent, false);
    return parent;
}

static orb_shell_snapshot_t status(void)
{
    return (orb_shell_snapshot_t){.firmware_version = "0.5.2", .device_id = "orb-test", .hub_name = "Test Mac"};
}

static orb_model_snapshot_t model(void)
{
    return (orb_model_snapshot_t){.online = true, .synced = true};
}

static void host_rejection(void)
{
    lv_obj_t *legacy = page();
    lv_obj_t *label = lv_label_create(legacy);
    lv_label_set_text(label, "Legacy source remains available");
    size_t baseline = used();
    size_t reservation = reserve_remaining();
    assert(!orb_settings_host_create(false, NULL, accept, NULL));
    assert(lv_obj_get_parent(label) == legacy);
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);
    assert(used() == baseline);
    lv_obj_delete(legacy);
}

static void retained_source(void)
{
    size_t baseline = used();
    lv_obj_t *old_page = page();
    orb_settings_host_t *host = orb_settings_host_create(false, NULL, accept, NULL);
    orb_shell_snapshot_t first = status();
    orb_model_snapshot_t values = model();
    assert(host && settings_show_about(host, old_page, &first, "Connected", true));
    orb_screen_instance_t *source = orb_settings_host_current(host);
    lv_obj_t *source_root = orb_screen_root(source);
    lv_obj_t *source_title = orb_settings_host_title(host);
    lv_obj_t *source_list = orb_settings_host_list(host);
    lv_obj_scroll_to_y(source_list, 24, LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(source_list) == 24);
    size_t attached = used();
    size_t reservation = reserve_remaining();
    assert(!orb_settings_host_stage_units(host, lv_screen_active(), ORB_SETTINGS_UNITS_SCREEN_ID, &values, ""));
    assert(orb_settings_host_current(host) == source && orb_screen_alive(source));
    assert(orb_screen_root(source) == source_root && orb_settings_host_title(host) == source_title);
    assert(lv_obj_get_parent(source_root) == old_page && lv_obj_get_parent(source_title) == old_page);
    assert(lv_obj_get_scroll_y(source_list) == 24);
    assert(!orb_settings_host_staged(host) && !orb_settings_host_snapshot_begin(host));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);
    assert(used() == attached);

    assert(orb_settings_host_stage_units(host, lv_screen_active(), ORB_SETTINGS_UNITS_SCREEN_ID, &values, ""));
    orb_screen_instance_t *candidate = orb_settings_host_staged(host);
    lv_obj_t *candidate_root = orb_screen_root(candidate);
    lv_obj_t *candidate_title = orb_screen_page_title(candidate);
    assert(candidate != source && orb_screen_visibility_get(candidate) == ORB_VISIBILITY_COVERED);
    assert(orb_settings_host_current(host) == source && orb_screen_alive(source));
    /* Drawing the hidden candidate never grants active-screen action rights. */
    lv_obj_t *snapshot = orb_settings_host_snapshot_begin(host);
    assert(snapshot && lv_obj_get_parent(candidate_root) == snapshot);
    assert(lv_obj_get_parent(candidate_title) == snapshot);
    assert(!orb_app_settings_units_open_temperature(candidate, ORB_INPUT_TOUCH, false, NULL));
    orb_settings_host_snapshot_end(host);
    assert(lv_obj_is_hidden(snapshot));
    assert(orb_settings_host_stage_units(host, lv_screen_active(), ORB_SETTINGS_UNITS_SCREEN_ID, &values, ""));
    assert(orb_settings_host_staged(host) == candidate && orb_screen_root(candidate) == candidate_root);
    orb_settings_host_cancel_staged(host);
    assert(!orb_screen_alive(candidate) && orb_settings_host_current(host) == source);
    assert(lv_obj_get_scroll_y(source_list) == 24 && used() == attached);

    assert(orb_settings_host_stage_units(host, lv_screen_active(), ORB_SETTINGS_UNITS_SCREEN_ID, &values, ""));
    candidate = orb_settings_host_staged(host);
    candidate_root = orb_screen_root(candidate);
    candidate_title = orb_screen_page_title(candidate);
    lv_obj_t *new_page = page();
    lv_obj_set_hidden(new_page, true);
    reservation = reserve_remaining();
    /* Once admission succeeds, commit transfers source context rather than
     * allocating a copy; it succeeds with zero WORKSPACE headroom. */
    assert(orb_settings_host_commit_staged(host, new_page));
    assert(!orb_screen_alive(source) && orb_settings_host_current(host) == candidate);
    assert(orb_screen_root(candidate) == candidate_root && orb_settings_host_title(host) == candidate_title);
    assert(lv_obj_get_parent(candidate_root) == new_page && lv_obj_get_parent(candidate_title) == new_page);
    assert(!orb_settings_host_staged(host));
    assert(orb_settings_host_detach_page(host, old_page));
    lv_obj_delete(old_page);
    assert(orb_screen_alive(candidate));
    orb_memory_release(ORB_MEMORY_WORKSPACE, reservation);
    lv_obj_set_hidden(new_page, false);
    assert(orb_settings_host_stage_about(host, lv_screen_active(), &first, "Connected", true));
    assert(orb_settings_host_commit_staged(host, new_page));
    assert(lv_obj_get_scroll_y(orb_settings_host_list(host)) == 24);
    orb_settings_host_destroy(host);
    lv_obj_delete(new_page);
    assert(used() == baseline);
}

static void provisional_parent_delete(void)
{
    size_t baseline = used();
    lv_obj_t *old_page = page(), *scene = page();
    orb_settings_host_t *host = orb_settings_host_create(false, NULL, accept, NULL);
    orb_shell_snapshot_t first = status();
    orb_model_snapshot_t values = model();
    assert(host && settings_show_about(host, old_page, &first, "Connected", true));
    orb_screen_instance_t *source = orb_settings_host_current(host);
    assert(orb_settings_host_stage_units(host, scene, ORB_SETTINGS_UNITS_SCREEN_ID, &values, ""));
    lv_obj_delete(scene);
    assert(orb_screen_alive(source) && !orb_settings_host_staged(host));
    assert(!orb_settings_host_commit_staged(host, old_page));
    orb_settings_host_cancel_staged(host);
    orb_settings_host_destroy(host);
    lv_obj_delete(old_page);
    assert(used() == baseline);
}

/* Add to the existing settings-admission test, which already owns page(),
 * status(), used() and accept(). Include orb_app_settings_root.h there. */
static void pending_source_departure(void)
{
    size_t baseline = used();
    lv_obj_t *source_page = page(), *target_page = page();
    orb_shell_snapshot_t first = status();
    orb_settings_host_t *host = orb_settings_host_create(false, NULL, accept, NULL);
    assert(host && orb_settings_host_can_depart(host));
    assert(settings_show_about(host, source_page, &first, "Connected", true));
    assert(orb_app_settings_about_open_licences(orb_settings_host_current(host), ORB_INPUT_TOUCH, false, NULL));
    assert(settings_show_about(host, source_page, &first, "Connected", true));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == 1);
    assert(orb_settings_host_back_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == 0);
    assert(orb_settings_host_can_depart(host));
    assert(orb_settings_host_stage_root(host, lv_screen_active(), &first, false));
    assert(orb_settings_host_commit_staged(host, target_page));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ABOUT_SCREEN_ID) == 0);
    assert(orb_app_settings_root_activate(orb_settings_host_current(host),
        ORB_SETTINGS_ITEM_GLASS, ORB_INPUT_TOUCH, false, NULL));
    assert(orb_settings_host_page_for(host, ORB_SETTINGS_ROOT_SCREEN_ID) == 1);
    orb_settings_host_destroy(host);
    lv_obj_delete(source_page);
    lv_obj_delete(target_page);
    assert(used() == baseline);
}

int main(void)
{
    lv_init();
    assert(sim_display_create());
    orb_ui_init(NULL);
    host_rejection();
    retained_source();
    provisional_parent_delete();
    assert(s_accepted == 0);
    pending_source_departure();
    assert(s_accepted == 2);
    puts("settings admission tests passed");
    return 0;
}
