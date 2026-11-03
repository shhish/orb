/* HIG §§1/7: exercise the real Stopwatch viewport, rather than an imitation
 * list. Only lap rows may enter its source cache. Kit source admission is a
 * preparation-timer job; generic LVGL renderer bookkeeping is not measured
 * as an application allocation or forbidden by these contracts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "orb_memory.h"
#include "orb_model.h"
#include "orb_time.h"
#include "orb_time_view.h"
#include "sim_display.h"

static unsigned s_checks, s_component_allocations;
static uint32_t s_now = 1000;
static lv_display_t *s_display;
static orb_time_view_t *s_view;
static uint8_t s_partial[SIM_D * SIM_D * 3], s_full[sizeof(s_partial)];

#define CHECK(value) do { \
    ++s_checks; \
    if (!(value)) { \
        fprintf(stderr, "Time cache2:%d: %s\n", __LINE__, #value); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

typedef struct {
    size_t retained;
    orb_round_list_cache_stats_t cache;
    unsigned visible;
} receipt_t;

static uint32_t tick(void) { return s_now; }
static int64_t now_us(void) { return (int64_t)s_now * 1000; }
static void *counted_alloc(size_t bytes)
{
    ++s_component_allocations;
    return lv_malloc(bytes);
}
static void counted_free(void *pointer) { lv_free(pointer); }
static size_t retained(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor.total_size;
}
static void settle(void)
{
    for (unsigned i = 0; i < 12; ++i) {
        s_now += 16;
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}

static orb_round_list_cache_stats_t cache(lv_obj_t *list)
{
    orb_round_list_cache_stats_t result;
    CHECK(orb_round_list_cache_stats(list, &result));
    return result;
}
static unsigned visible_rows(lv_obj_t *list)
{
    unsigned count = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_area_t bounds;
        lv_obj_t *row = lv_obj_get_child(list, i);
        count += orb_round_list_row_bounds(row, &bounds);
    }
    return count;
}
static lv_obj_t *row_named(lv_obj_t *list, const char *name)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(row)) continue;
        lv_obj_t *label = lv_obj_get_child(row, 1);
        if (label && lv_obj_check_type(label, &lv_label_class) &&
            !strcmp(lv_label_get_text(label), name)) return row;
    }
    return NULL;
}

static void row_paints(lv_obj_t *list, const char *name)
{
    lv_obj_t *row = row_named(list, name);
    CHECK(row != NULL && lv_obj_get_parent(row) == list);
    lv_area_t label, viewport;
    CHECK(orb_round_list_child_bounds(row, lv_obj_get_child(row, 1), &label));
    lv_obj_get_coords(list, &viewport);
    label.x1 = LV_MAX(label.x1, 0);
    label.x2 = LV_MIN(label.x2, SIM_D - 1);
    label.y1 = LV_MAX(label.y1, viewport.y1);
    label.y2 = LV_MIN(label.y2, viewport.y2);
    CHECK(label.x1 <= label.x2 && label.y1 <= label.y2);
    sim_display_rgb(s_partial);
    unsigned ink = 0;
    for (int y = label.y1; y <= label.y2; ++y)
        for (int x = label.x1; x <= label.x2; ++x) {
            const uint8_t *pixel = s_partial + (y * SIM_D + x) * 3;
            ink += pixel[0] > 70 && pixel[1] > 70 && pixel[2] > 70;
        }
    CHECK(ink > 12);
}

static void draw_reuses_sources(lv_obj_t *list)
{
    orb_round_list_cache_stats_t before = cache(list);
    unsigned allocations = s_component_allocations;
    sim_display_rgb(s_partial);
    lv_obj_invalidate(orb_time_view_root(s_view));
    /* No timer runs here: this isolates the actual draw path from the kit's
     * documented preparation timer, which is permitted to allocate. */
    lv_refr_now(s_display);
    sim_display_rgb(s_full);
    orb_round_list_cache_stats_t after = cache(list);
    CHECK(after.source_builds == before.source_builds);
    CHECK(after.source_bytes == before.source_bytes);
    CHECK(s_component_allocations == allocations);
    CHECK(!memcmp(s_partial, s_full, sizeof(s_partial)));
}

static void scroll_reuses_sources(lv_obj_t *list, int y)
{
    orb_round_list_cache_stats_t before = cache(list);
    unsigned allocations = s_component_allocations;
    /* Call the app's real rim path. A synchronous SCROLL callback may update
     * geometry, chrome and visibility; it must not admit or rebuild rows. */
    orb_time_view_rotate(s_view, (float)y);
    orb_round_list_cache_stats_t immediate = cache(list);
    CHECK(s_component_allocations == allocations);
    CHECK(immediate.source_builds == before.source_builds);
    CHECK(immediate.source_bytes == before.source_bytes);
    settle();
    CHECK(visible_rows(list) > 0 && visible_rows(list) <= 4);
    draw_reuses_sources(list);
}

static receipt_t fixture(unsigned laps, bool sweep)
{
    orb_time_init();
    orb_time_poll(now_us(), 0);
    CHECK(orb_time_stopwatch_start(now_us()));
    for (unsigned i = 0; i < laps; ++i) {
        s_now += 10500 + i * 3100;
        CHECK(orb_time_stopwatch_lap(now_us()));
    }
    orb_time_stopwatch_pause(now_us());
    orb_memory_stats_t workspace_before, workspace_open, workspace_closed;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace_before);
    size_t baseline = retained();
    s_view = orb_time_view_create(lv_screen_active(), NULL);
    CHECK(s_view != NULL);
    orb_time_view_select(s_view, ORB_TIME_PAGE_STOPWATCH);
    settle();
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace_open);
    CHECK(workspace_open.used == workspace_before.used + orb_time_view_state_size());
    CHECK(workspace_open.used <= workspace_open.budget);
    CHECK(workspace_open.rejections == workspace_before.rejections);
    lv_obj_t *list = orb_time_view_scroll(s_view);
    CHECK(list != NULL && lv_obj_get_width(list) == SIM_D && lv_obj_get_height(list) < 300);
    unsigned actual_rows = 0, floating = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); ++i) {
        lv_obj_t *child = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(child)) { ++floating; continue; }
        CHECK(lv_obj_get_height(child) >= ORB_KIT_ROW_H); /* AX3 rows may grow. */
        CHECK(lv_obj_get_child_count(child) >= 3);
        ++actual_rows;
    }
    CHECK(actual_rows == laps && floating == 1);
    /* The summary is a floating content layer. Capturing it as a full-height
     * row was the bug: it retained every lap and hid the real viewport rows. */
    orb_round_list_cache_stats_t summary = cache(list);
    /* Idle lookahead may already hold nearby rows outside the viewport; the
     * floating summary itself is never captured. Lap rows begin at 276 since
     * polish review 23 removed the empty band under the pinned header. */
    CHECK(summary.source_builds == 0 && (summary.lookahead_builds > 0 || summary.source_bytes == 0));
    scroll_reuses_sources(list, 276);
    receipt_t receipt = {.retained = retained() - baseline,
        .cache = cache(list), .visible = visible_rows(list)};
    /* Taller AX rows may all be captured by idle lookahead before entering;
     * a zero synchronous admission count then proves reuse, not missing ink. */
    CHECK(receipt.cache.source_bytes > 0);
    CHECK(receipt.cache.source_builds + receipt.cache.lookahead_builds > 0);
    CHECK(receipt.cache.source_builds <= 4);
    CHECK(receipt.cache.metadata_bytes < 16000);
    char first[24];
    snprintf(first, sizeof(first), "Lap %u", laps);
    row_paints(list, first);
    if (sweep) {
        int maximum = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
        CHECK(maximum > 1100);
        uint32_t builds = receipt.cache.source_builds + receipt.cache.lookahead_builds;
        for (int y = 370; y < maximum; y += 54) {
            scroll_reuses_sources(list, y);
            orb_round_list_cache_stats_t current = cache(list);
            CHECK(current.metadata_bytes < 16000);
            CHECK(current.source_bytes <= receipt.cache.source_bytes * 2);
        }
        scroll_reuses_sources(list, maximum);
        row_paints(list, "Lap 1");
        orb_round_list_cache_stats_t end = cache(list);
        CHECK(end.source_builds + end.lookahead_builds > builds);
        CHECK(end.source_bytes <= receipt.cache.source_bytes * 2);
        scroll_reuses_sources(list, 276);
        row_paints(list, first);
        printf("Time cache2 laps=%u start_source=%zu end_source=%zu start_builds=%u end_builds=%u visible=%u\n",
            laps, receipt.cache.source_bytes, end.source_bytes,
            (unsigned)receipt.cache.source_builds, (unsigned)end.source_builds, receipt.visible);
    }
    orb_time_view_destroy(s_view);
    s_view = NULL;
    settle();
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace_closed);
    CHECK(workspace_closed.used == workspace_before.used);
    CHECK(workspace_closed.rejections == workspace_before.rejections);
    return receipt;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "default";
    bool ax3 = !strcmp(mode, "ax3") || !strcmp(mode, "ax3-bold-rt");
    bool bold = !strcmp(mode, "ax3-bold-rt");
    if (argc > 2 || (!ax3 && strcmp(mode, "default"))) return 2;
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display != NULL);
    orb_ui_init(&(orb_ui_config_t){.alloc = counted_alloc, .free = counted_free});
    CHECK(orb_model_init());
    CHECK(orb_fonts_set_content_size(ax3 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(bold);
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    /* Warm shared material first so the comparison concerns Time's retained
     * tree and viewport sources rather than one-time glass cache admission. */
    orb_glass_cache_prewarm();
    settle();
    (void)fixture(4, false);
    receipt_t few = fixture(4, false), many = fixture(16, true);
    CHECK(many.visible <= 4 && few.visible <= 4);
    CHECK(many.cache.source_bytes <= few.cache.source_bytes * 2);
    CHECK(many.cache.metadata_bytes <= few.cache.metadata_bytes * 2);
    /* The extra twelve semantic rows need children/styles, but must not add
     * twelve full row rasters. This permits 8 KiB per added logical row. */
    CHECK(many.retained <= few.retained + 12u * 8192u);
    printf("Time cache2 mode=%s few_retained=%zu sixteen_retained=%zu few_source=%zu sixteen_source=%zu state=%zu checks=%u\n",
        mode, few.retained, many.retained, few.cache.source_bytes,
        many.cache.source_bytes, orb_time_view_state_size(), s_checks);
    return EXIT_SUCCESS;
}
