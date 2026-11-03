/* Round-first visual evidence. Every session, usage window and notification is
 * process-local synthetic data; no board, network or persistent owner state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "round shell at %u ms: %s:%d: %s\n", s_now, __FILE__, __LINE__, #value); exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input; *data = s_input;
}
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20; orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}

static void next_provider(void)
{
    s_input.point = (lv_point_t){233, 340}; s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(40);
    for (unsigned i = 1; i <= 8; ++i) {
        s_input.point.y = 340 - 22 * (int)i;
        lv_indev_read(s_pointer); advance(20);
    }
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(1000);
}

static orb_usage_snapshot_t usage(bool stale, bool missing, bool long_values)
{
    orb_usage_snapshot_t result = {.count = missing ? 2 : 4};
    const orb_agent_t providers[] = {ORB_AGENT_CLAUDE, ORB_AGENT_CLAUDE, ORB_AGENT_CODEX, ORB_AGENT_CODEX};
    const double percentages[] = {62, 85, 38, 71};
    for (unsigned i = 0; i < result.count; ++i) {
        result.windows[i] = (orb_usage_window_t){.provider = providers[i],
            .used_percent = long_values ? 100 : percentages[i], .stale = stale,
            .resets_at = i % 2 ? UINT64_C(1793228370) : UINT64_C(1793216940),
            .observed_at = UINT64_C(1793194860), .source = ORB_USAGE_USAGE};
        snprintf(result.windows[i].label, sizeof(result.windows[i].label), "%s", i % 2 ? "week" : "5 hours");
    }
    result.unknown[1] = missing;
    return result;
}

static void fresh(orb_shell_surface_t surface, bool stale, bool missing, bool long_values)
{
    if (s_shell) { orb_shell_destroy(s_shell); s_shell = NULL; }
    orb_model_reset(); orb_model_set_connection(true, true);
    const orb_session_t sessions[] = {
        {.id = "firmware", .agent = ORB_AGENT_CLAUDE, .title = "esp32s3-firmware", .project = "Orb",
            .state = ORB_SESSION_BUSY, .reason = "building", .updated = 1793196300},
        {.id = "media", .agent = ORB_AGENT_CODEX, .title = "demo-project", .project = "Media",
            .state = ORB_SESSION_DONE, .reason = "638 tests passed", .updated = 1793195760},
        {.id = "dot", .agent = ORB_AGENT_DOT, .title = "Dot", .state = ORB_SESSION_IDLE, .updated = 1793196300},
        {.id = "site", .agent = ORB_AGENT_CLAUDE, .title = "alex-site", .state = ORB_SESSION_IDLE, .updated = 1793192700},
    };
    CHECK(orb_model_sessions_chunk(sessions, 4, true, false));
    orb_usage_snapshot_t windows = usage(stale, missing, long_values);
    CHECK(orb_model_set_usage(&windows));
    const char *titles[] = {"Build completed", "Changes ready to review", "Diagnostics finished", "Session needs attention"};
    for (unsigned i = 0; i < 4; ++i) {
        orb_card_t card = {.kind = ORB_CARD_ATTENTION, .priority = ORB_PRIORITY_NORMAL};
        snprintf(card.id, sizeof(card.id), "round-notice-%u", i);
        snprintf(card.session, sizeof(card.session), "%s", sessions[i].id);
        snprintf(card.title, sizeof(card.title), "%s", titles[i]);
        snprintf(card.body, sizeof(card.body), "The latest synthetic changes are ready.");
        CHECK(orb_model_upsert_card(&card));
    }
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true, .charging = true,
        .desk_charge_limit = true, .clock_time = "10:09", .hub_name = "Synthetic Mac",
        .battery_detail = {.present = true, .usb = true, .charging = true, .battery_pct = 80,
            .health_valid = true, .health_pct = 96, .temperature_valid = true, .temperature_c = 25}};
    orb_shell_update(s_shell, &status); orb_shell_navigate(s_shell, surface); advance(1400);
    CHECK(orb_shell_surface(s_shell) == surface);
    for (unsigned i = 0; i < 4; ++i) {
        char id[32]; snprintf(id, sizeof(id), "round-notice-%u", i);
        orb_shell_fixture_notification_age(s_shell, id, 3600000);
    }
    advance(100);
}

static void shot(const char *directory, const char *name)
{
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.png", directory, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
}

static unsigned memory(lv_obj_t *root, const char *surface, const char *mode)
{
    orb_round_list_cache_stats_t stats;
    unsigned lists = orb_round_list_cache_stats(root, &stats);
    if (lists) {
        printf("round_memory surface=%s mode=%s width_tables=%zu source=%zu metadata=%zu source_builds=%u\n",
               surface, mode, stats.width_table_bytes, stats.source_bytes, stats.metadata_bytes,
               (unsigned)stats.source_builds);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        lists += memory(lv_obj_get_child(root, i), surface, mode);
    return lists;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER); lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer); lv_indev_set_mode(s_pointer, LV_INDEV_MODE_EVENT);
    bool ax = !strncmp(argv[2], "ax3", 3);
    CHECK(orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(!strcmp(argv[2], "ax3-bold-rt")) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(!strcmp(argv[2], "ax3-bold-rt"));
    const struct { const char *name; orb_shell_surface_t surface; } lists[] = {
        {"Settings", ORB_SHELL_SETTINGS}, {"Sound", ORB_SHELL_SOUND}, {"Battery", ORB_SHELL_BATTERY},
        {"Sessions", ORB_SHELL_SESSIONS}, {"Notifications", ORB_SHELL_NOTIFICATIONS},
    };
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        fresh(lists[i].surface, false, false, false); shot(argv[1], lists[i].name);
        CHECK(memory(orb_shell_root(s_shell), lists[i].name, argv[2]) > 0);
    }
    fresh(ORB_SHELL_USAGE, false, false, false); shot(argv[1], "Usage-claude");
    next_provider(); shot(argv[1], "Usage-codex");
    fresh(ORB_SHELL_USAGE, true, false, false); shot(argv[1], "Usage-stale");
    fresh(ORB_SHELL_USAGE, false, true, false);
    next_provider(); shot(argv[1], "Usage-missing");
    fresh(ORB_SHELL_USAGE, true, false, true); shot(argv[1], "Usage-long");
    orb_shell_destroy(s_shell);
    printf("round shell captures %s: %u checks passed\n", argv[2], s_checks);
    lv_deinit(); return 0;
}
