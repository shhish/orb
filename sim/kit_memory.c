/* Same fixtures link against entry and final kit archives for comparable host
 * payload receipts. Native fonts/allocator headers and board budgets excluded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_time.h"
#include "sim_display.h"

static uint32_t s_now;
static uint32_t tick(void) { return s_now; }
static void settle(orb_shell_t *shell)
{
    for (unsigned i = 0; i < 120; i++) {
        s_now += 16;
        if (shell) orb_shell_poll(shell);
        lv_timer_handler();
    }
    lv_refr_now(lv_display_get_default());
}
static size_t bytes(void)
{
    lv_mem_monitor_t m; lv_mem_monitor(&m); return m.total_size;
}
static void report(const char *mode, const char *name, size_t start)
{
    orb_glass_build_stats_t glass; orb_glass_build_stats(&glass);
    printf("MEMORY2 %s %s retained=%zu delta=%lld shared-glass=%zu\n", mode, name,
        bytes(), (long long)bytes() - (long long)start, glass.bytes);
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    bool ax = strcmp(mode, "default"), bold = !strcmp(mode, "ax3-bold-rt");
    lv_init(); lv_tick_set_cb(tick);
    if (!sim_display_create()) return EXIT_FAILURE;
    orb_ui_init(NULL);
    if (orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) != ORB_FONTS_OK ||
        orb_fonts_set_bold_text(bold) != ORB_FONTS_OK) return EXIT_FAILURE;
    orb_ui_set_reduce_transparency(bold);
    orb_glass_cache_prewarm(); settle(NULL);
    size_t start = bytes();
    lv_obj_t *root = lv_screen_active();
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list); lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (unsigned i = 0; i < 16; i++) {
        char title[32], subtitle[40];
        snprintf(title, sizeof(title), "Lap %u", i + 1);
        snprintf(subtitle, sizeof(subtitle), "Split 00:%02u.42", i + 1);
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
            .title=title, .subtitle=subtitle, .value="12:59.99",
            .glyph=ORB_ICON_TIMER, .accent=ORB_KIT_TIME});
        lv_obj_set_y(row, 96 + 84 * i);
    }
    if (!orb_kit_list_apply(list)) return EXIT_FAILURE;
    lv_obj_t *title = orb_kit_rim_title_create(root, "LAPS", ORB_KIT_TIME);
    settle(NULL); report(mode, "laps16", start);
    orb_round_list_cache_stats_t cache;
    if (orb_round_list_cache_stats(list, &cache)) printf(
        "CACHE2 %s source=%zu tables=%zu metadata=%zu builds=%u\n", mode,
        cache.source_bytes, cache.width_table_bytes, cache.metadata_bytes, cache.source_builds);
    orb_round_list_scroll_to(list, 1000); orb_round_list_scroll_finish(list);
    settle(NULL); report(mode, "laps16-end", start);
    lv_obj_delete(title); lv_obj_delete(list); settle(NULL);
    if (!orb_model_init() || !orb_home_init()) return EXIT_FAILURE;
    orb_time_init();
    orb_shell_t *shell = orb_shell_create(root, NULL, NULL);
    if (!shell) return EXIT_FAILURE;
    orb_shell_snapshot_t snapshot = {.net=ORB_SHELL_NET_CONNECTED,
        .link=ORB_SHELL_LINK_ONLINE, .battery_pct=80, .brightness=70,
        .rssi=-50, .clock_time="17:28", .clock_date="Wednesday 28",
        .hub_name="Alex's Mac mini", .device_name="Orb", .firmware_version="kit2"};
    orb_shell_update(shell, &snapshot);
    settle(shell);
    start = bytes();
    orb_shell_navigate(shell, ORB_SHELL_SETTINGS); settle(shell);
    report(mode, "settings-root-vs-face", start);
    orb_shell_navigate(shell, ORB_SHELL_FACE); settle(shell);
    report(mode, "returned-face", start);
    orb_shell_destroy(shell);
    return EXIT_SUCCESS;
}
