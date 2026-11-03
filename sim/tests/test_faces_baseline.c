/* The same process-local shell fixture is used before and after migration.
 * Reports requested LVGL payload only, not ESP heap or host font allocations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "orb_faces.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_memory.h"
#include "orb_shell.h"
#include "sim_display.h"

static uint32_t s_now;
static uint32_t tick(void) { return s_now; }
static size_t retained(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor.total_size;
}

static unsigned timers_for(const void *owner)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer))
        count += lv_timer_get_user_data(timer) == owner;
    return count;
}

static unsigned running_content_timers(void)
{
    unsigned count = 0;
    lv_timer_t *refresh = lv_display_get_refr_timer(lv_display_get_default());
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer))
        count += timer != refresh && !lv_timer_get_paused(timer);
    return count;
}

static bool visible_text(lv_obj_t *object, const char *value)
{
    if (!lv_obj_is_visible(object)) return false;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), value)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (visible_text(lv_obj_get_child(object, i), value)) return true;
    return false;
}

static unsigned visible_lines(lv_obj_t *object)
{
    if (!lv_obj_is_visible(object)) return 0;
    unsigned count = lv_obj_check_type(object, &lv_line_class);
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        count += visible_lines(lv_obj_get_child(object, i));
    return count;
}

/* Design 2.0 Lock deliberately retains numeric public aggregates. It must
 * remain inert and minute-based, with no ticking seconds or live private text
 * fields in its data contract. */
static void public_face_contracts(void)
{
    orb_faces_snapshot_t data = {.now = 1793208527, .clock = "17:28", .weekday = "Wednesday", .day = "28",
        .usage_percent = {62, 38}, .battery_percent = 80, .home_on = 3, .home_total = 8,
        .working = 7, .online = true, .next_seconds = 480, .next_total_seconds = 900,
        .weather_override = true, .weather = {.configured = true, .available = true,
            .temperature = 72, .unit = ORB_TEMPERATURE_F, .condition = ORB_WEATHER_CLEAR, .is_day = true}};
    for (unsigned kind = 0; kind < ORB_FACE_COUNT; ++kind) {
        orb_faces_t *faces = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true});
        assert(faces && timers_for(faces) == 1);
        orb_faces_select(faces, kind);
        orb_faces_update(faces, &data);
        orb_faces_set_always_on(faces, true);
        orb_face_complication_t value;
        assert(orb_faces_complication(faces, ORB_FACE_CLAUDE, &value) && value.percent == 62);
        assert(orb_faces_complication(faces, ORB_FACE_HOME, &value) && !strcmp(value.text, "3 on"));
        assert(!orb_faces_rotary(faces, NULL, NULL));
        orb_faces_hold(faces);
        assert(orb_faces_mode(faces) == ORB_FACES_FACE);
        if (kind == ORB_FACE_ORB) assert(visible_text(orb_faces_root(faces), "62"));
        if (kind == ORB_FACE_MERIDIAN) assert(visible_lines(orb_faces_root(faces)) == 62);
        for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer))
            if (lv_timer_get_user_data(timer) == faces) assert(lv_timer_get_paused(timer));
        const void *owner = faces;
        orb_faces_destroy(faces);
        assert(timers_for(owner) == 0);
    }
}

static void inactive_animation_contract(void)
{
    orb_faces_t *faces = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true});
    assert(faces);
    orb_faces_snapshot_t data = {.now = 1793208527, .clock = "17:28", .usage_percent = {10, 20},
        .battery_percent = 80, .online = true, .weather_override = true};
    orb_faces_update(faces, &data);
    orb_faces_set_active(faces, false);
    for (unsigned i = 0; i < 90; ++i) { s_now += 16; lv_timer_handler(); }
    unsigned before = running_content_timers();
    orb_faces_set_active(faces, true);
    data.usage_percent[0] = 80;
    orb_faces_update(faces, &data);
    assert(running_content_timers() > before);
    orb_faces_set_active(faces, false);
    assert(running_content_timers() == before);
    data.usage_percent[0] = 40;
    orb_faces_update(faces, &data);
    assert(running_content_timers() == before);
    orb_faces_destroy(faces);
}

static void runtime_motion_contract(void)
{
    orb_faces_t *faces = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true});
    assert(faces);
    orb_faces_snapshot_t data = {.now = 1793208527, .clock = "17:28", .usage_percent = {-1, -1},
        .battery_percent = 80, .online = true, .weather_override = true};
    orb_faces_select(faces, ORB_FACE_MERIDIAN);
    orb_faces_update(faces, &data);
    orb_faces_set_active(faces, true);
    lv_timer_t *seconds = NULL;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer))
        if (lv_timer_get_user_data(timer) == faces) seconds = timer;
    assert(seconds && !lv_timer_get_paused(seconds));
    orb_ui_set_reduce_motion(true);
    orb_faces_update(faces, &data);
    assert(lv_timer_get_paused(seconds));
    orb_ui_set_reduce_motion(false);
    orb_faces_update(faces, &data);
    assert(!lv_timer_get_paused(seconds));
    orb_faces_destroy(faces);
}

static void preview_isolation_contract(void)
{
    orb_faces_prefs_t before, after;
    orb_faces_current_preferences(&before);
    orb_faces_t *preview = orb_faces_create(lv_screen_active(), &(orb_faces_config_t){.preview = true});
    assert(preview);
    orb_faces_select(preview, (before.face + 1) % ORB_FACE_COUNT);
    assert(orb_faces_save(preview));
    orb_faces_current_preferences(&after);
    assert(!memcmp(&before, &after, sizeof(before)));
    orb_faces_destroy(preview);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) return EXIT_FAILURE;
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    if (!display) return EXIT_FAILURE;
    orb_ui_init(NULL);
    bool ax = strcmp(argv[2], "default") != 0;
    bool bold = !strcmp(argv[2], "ax3-bold-rt");
    if (orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) return EXIT_FAILURE;
    orb_ui_set_reduce_transparency(bold);
    if (!orb_model_init() || !orb_home_init()) return EXIT_FAILURE;
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    orb_glass_cache_prewarm();
    if (argc == 4) {
        if (!strcmp(argv[3], "public")) public_face_contracts();
        else if (!strcmp(argv[3], "inactive")) inactive_animation_contract();
        else if (!strcmp(argv[3], "motion")) runtime_motion_contract();
        else if (!strcmp(argv[3], "preview")) preview_isolation_contract();
        else return EXIT_FAILURE;
        printf("Faces lifecycle %s/%s passed\n", argv[2], argv[3]);
        return EXIT_SUCCESS;
    }
    size_t before = retained();
    orb_memory_stats_t owners_before[ORB_MEMORY_COUNT];
    for (unsigned i = 0; i < ORB_MEMORY_COUNT; ++i) orb_memory_get(i, &owners_before[i]);
    orb_glass_build_stats_t glass_before, glass_after;
    orb_glass_build_stats(&glass_before);
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    if (!shell) return EXIT_FAILURE;
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .brightness = 70, .clock_time = "17:28", .clock_date = "Wednesday, October 28"};
    orb_shell_update(shell, &status);
    orb_shell_navigate(shell, ORB_SHELL_FACE);
    for (unsigned i = 0; i < 90; ++i) {
        s_now += 16;
        orb_shell_poll(shell);
        lv_timer_handler();
    }
    lv_refr_now(display);
    size_t after = retained();
    orb_glass_build_stats(&glass_after);
    printf("FACE-SHELL %s retained=%lld shared-mask-delta=%lld private=%lld bytes\n", argv[2],
        (long long)after - (long long)before, (long long)glass_after.bytes - (long long)glass_before.bytes,
        (long long)after - (long long)before - ((long long)glass_after.bytes - (long long)glass_before.bytes));
    for (unsigned i = 0; i < ORB_MEMORY_COUNT; ++i) {
        orb_memory_stats_t owner; orb_memory_get(i, &owner);
        printf("FACE-OWNER %s %s before=%zu after=%zu delta=%lld budget=%zu rejections=%u\n", argv[2],
            orb_memory_name(i), owners_before[i].used, owner.used,
            (long long)owner.used - (long long)owners_before[i].used, owner.budget, owner.rejections);
    }
    if (!sim_display_save_png(argv[1])) return EXIT_FAILURE;
    orb_shell_destroy(shell);
    public_face_contracts();
    inactive_animation_contract();
    runtime_motion_contract();
    preview_isolation_contract();
    return EXIT_SUCCESS;
}
