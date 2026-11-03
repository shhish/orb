/* Owner payload receipts for the real Home open and private scroll workload.
 * Run each mode in a fresh process: orb_memory peaks are lifetime counters.
 * Desktop payload accounting does not establish board heap/PSRAM headroom. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_home.h"
#include "orb_devtools_scroll_fixture.h"
#include "orb_memory.h"
#include "orb_model.h"
#include "orb_scroll_fixture.h"
#include "orb_ui.h"
#include "orb_workspace.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_mode;
static lv_display_t *s_display;
static orb_workspace_t *s_live;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "workspace peak %s:%d: %s\n", s_mode, __LINE__, #value); \
    exit(EXIT_FAILURE); \
} } while (0)

static uint32_t tick(void) { return s_now; }

static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 4) {
        s_now += 4;
        if (s_live) orb_workspace_poll(s_live);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}

static void report(const char *phase)
{
    for (orb_memory_owner_t owner = 0; owner < ORB_MEMORY_COUNT; owner++) {
        orb_memory_stats_t stats;
        orb_memory_get(owner, &stats);
        printf("WSPEAK mode=%s phase=%s owner=%s used=%zu peak=%zu budget=%zu rejections=%u\n",
            s_mode, phase, orb_memory_name(owner), stats.used, stats.peak,
            stats.budget, stats.rejections);
    }
}

static void home_open(void)
{
    orb_workspace_navigate(s_live, ORB_SHELL_HOME);
    advance(1600);
    CHECK(orb_workspace_surface(s_live) == ORB_SHELL_HOME);
    orb_shell_open_metrics_t metrics;
    orb_workspace_open_metrics(s_live, &metrics);
    CHECK(metrics.snapshot_captures > 0);
    CHECK(!metrics.active);
    printf("WSPEAK mode=%s captures=%llu cache_bytes=%zu\n", s_mode,
        (unsigned long long)metrics.snapshot_captures, metrics.cache_bytes);
    report("home-open");
}

static void scroll_bench(orb_scroll_surface_t surface)
{
    /* The developer benchmark keeps the owner's workspace alive but hidden.
     * Its synthetic snapshots intentionally differ from the live models. */
    orb_workspace_show(s_live, false);
    advance(32);
    lv_obj_t *saved = lv_screen_active();
    lv_obj_t *screen = lv_obj_create(NULL);
    CHECK(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, 466, 466);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, 255, 0);
    orb_memory_stats_t retained;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &retained);
    orb_scroll_fixture_t *fixture = orb_devtools_scroll_fixture_create(screen, surface);
    CHECK(fixture);
    lv_screen_load(screen);
    advance(1000);
    bool prepared = false;
    for (unsigned attempt = 0; attempt < 5 && !prepared; attempt++) {
        prepared = orb_scroll_fixture_prepare(fixture);
        advance(100);
    }
    CHECK(prepared);
    CHECK(orb_scroll_fixture_viewport_pixels(fixture) > 0);
    report("scroll-ready");
    bool moved = false;
    int32_t initial = orb_scroll_fixture_position(fixture);
    for (unsigned elapsed = 0; elapsed <= 1200; elapsed += 4) {
        orb_scroll_fixture_tick(fixture, elapsed);
        advance(4);
        moved |= orb_scroll_fixture_position(fixture) != initial;
    }
    orb_scroll_fixture_diagnostics_t diagnostics;
    orb_scroll_fixture_diagnostics(fixture, &diagnostics);
    CHECK(orb_scroll_fixture_gestures(fixture) == 1);
    CHECK(diagnostics.reports == 15 && diagnostics.releases == 1);
    CHECK(moved);
    CHECK(diagnostics.offset_changes > 0);
    printf("WSPEAK mode=%s surface=%s reports=%u releases=%u offsets=%u position=%ld\n",
        s_mode, orb_scroll_surface_name(surface), diagnostics.reports,
        diagnostics.releases, diagnostics.offset_changes,
        (long)orb_scroll_fixture_position(fixture));
    report("scroll-bench");
    orb_scroll_fixture_destroy(fixture);
    lv_screen_load(saved);
    lv_obj_delete(screen);
    advance(32);
    orb_memory_stats_t released;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &released);
    CHECK(released.used == retained.used);
    report("scroll-destroyed");
}

int main(int argc, char **argv)
{
    s_mode = argc > 1 ? argv[1] : "home";
    CHECK(argc <= 3);
    bool home = !strcmp(s_mode, "home");
    orb_scroll_surface_t surface = ORB_SCROLL_ROOMS;
    if (!home && strcmp(s_mode, "scroll"))
        CHECK(!strncmp(s_mode, "scroll-", 7) && orb_scroll_surface_parse(s_mode + 7, &surface));
    size_t ceiling = home ? 192u * 1024u : 240u * 1024u;
    if (argc > 2) {
        char *end = NULL;
        unsigned long long parsed = strtoull(argv[2], &end, 10);
        CHECK(end && !*end && parsed > 0 && parsed <= SIZE_MAX);
        ceiling = (size_t)parsed;
    }
    bool baseline = getenv("ORB_WORKSPACE_PEAK_BASELINE") != NULL;
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL);
    CHECK(orb_model_init() && orb_home_init());
    orb_model_reset(); orb_home_reset();
    orb_model_load_demo(); orb_home_load_demo();
    orb_model_set_connection(true, true);
    orb_home_set_connection(true, true);
    advance(32);
    orb_memory_stats_t start;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &start);
    report("start");
    s_live = orb_workspace_create(lv_screen_active(), NULL, NULL, NULL);
    CHECK(s_live);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED,
        .link = ORB_SHELL_LINK_ONLINE, .battery_pct = 80, .brightness = 70,
        .rssi = -50, .clock_time = "10:09", .clock_date = "Thursday 29"};
    orb_workspace_update(s_live, &status);
    advance(1600);
    CHECK(orb_workspace_surface(s_live) == ORB_SHELL_FACE);
    report("live-face");
    if (home) home_open();
    else scroll_bench(surface);
    orb_workspace_destroy(s_live); s_live = NULL;
    advance(32);
    orb_memory_stats_t finish;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &finish);
    CHECK(finish.used == start.used);
    CHECK(finish.peak > start.used + 100u * 1024u);
    if (!baseline) {
        CHECK(finish.peak <= ceiling);
        CHECK(finish.rejections == start.rejections);
        orb_memory_stats_t build;
        orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
        CHECK(build.used == 0 && build.peak > 0 && build.peak <= build.budget);
        CHECK(build.rejections == 0);
    }
    report("destroyed");
    printf("WSPEAK mode=%s checks=%u workspace_peak=%zu ceiling=%zu baseline=%u\n",
        s_mode, s_checks, finish.peak, ceiling, baseline);
    return EXIT_SUCCESS;
}
