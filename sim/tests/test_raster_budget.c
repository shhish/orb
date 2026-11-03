/* Exercise real raster admission and lifetime rules through LVGL. The custom
 * allocator catches full-screen ARGB masters and repeated failed requests. */
#include <stdio.h>
#include <stdlib.h>

#include "lvgl.h"
#include "orb_memory.h"
#include "sim_display.h"

/* Exercise the real admission predicate without starting another preparation
 * epoch, which would correctly repin visible sources before cache pressure. */
#include "../../firmware/third_party/lvgl__lvgl/src/core/lv_obj_raster.c"

static unsigned s_checks, s_large_attempts;
static size_t s_largest, s_fail_at;
#define CHECK(c) do { s_checks++; if(!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while(0)
static void * allocate(size_t bytes, lv_color_format_t cf)
{
    LV_UNUSED(cf);
    if(bytes > s_largest) s_largest = bytes;
    if(s_fail_at && bytes >= s_fail_at) { s_large_attempts++; return NULL; }
    return malloc(bytes);
}
static lv_obj_t * raster(lv_obj_t * parent, int w, int h, bool matte)
{
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x376AA4), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    if(matte) lv_obj_set_raster_matte(obj, lv_color_black());
    else lv_obj_set_raster(obj, true);
    return obj;
}
int main(void)
{
    lv_init();
    lv_obj_raster_set_allocator(allocate, free);
    lv_display_t * display = sim_display_create();
    lv_obj_t * screen = lv_screen_active();
    lv_obj_raster_stats_t before, after;
    lv_obj_t * page = raster(screen, 466, 466, true);
    CHECK(!lv_obj_raster_ready(page));
    lv_obj_raster_prepare(page);
    CHECK(lv_obj_raster_ready(page));
    lv_obj_raster_get_stats(&after);
    CHECK(after.page_bytes >= 466u * 466u * 2u);
    CHECK(after.page_bytes <= after.page_budget);
    CHECK(s_largest < 466u * 466u * 4u);
    lv_obj_delete(page);

    /* The largest adjacent supported square-like transparent build measures
     * 261480 B including row/split scratch, just inside the 256 KiB envelope.
     * This is transient raster ownership, independent of shell workspaces. */
    orb_memory_stats_t workspace_before, workspace_after, build;
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace_before);
    page = raster(screen, 253, 252, false);
    lv_obj_raster_prepare(page);
    CHECK(lv_obj_raster_ready(page));
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(build.used == 0 && build.peak == 261480);
    CHECK(build.peak < build.budget && build.rejections == 0);
    orb_memory_get(ORB_MEMORY_WORKSPACE, &workspace_after);
    CHECK(workspace_after.used == workspace_before.used && workspace_after.peak == workspace_before.peak);
    lv_obj_delete(page);
    page = raster(screen, 254, 252, false);
    lv_obj_raster_prepare(page);
    CHECK(!lv_obj_raster_ready(page));
    lv_obj_delete(page);

    /* An allocator failure must retire the scratch charge too; cache retry
     * permits a subsequent build without leaking transient owner bytes. */
    page = raster(screen, 200, 100, false);
    s_fail_at = 40000;
    lv_obj_raster_cache_retry();
    lv_obj_raster_prepare(page);
    CHECK(!lv_obj_raster_ready(page));
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(build.used == 0);
    s_fail_at = 0;
    s_large_attempts = 0;
    lv_obj_raster_cache_retry();
    lv_obj_raster_prepare(page);
    CHECK(lv_obj_raster_ready(page));
    orb_memory_get(ORB_MEMORY_RASTER_BUILD, &build);
    CHECK(build.used == 0);
    lv_obj_delete(page);

    /* An unproven transparent whole-page source is rejected before allocating
     * 868624 bytes. Repeated refreshes stay on the generic renderer. */
    page = raster(screen, 466, 466, false);
    lv_obj_raster_get_stats(&before);
    lv_obj_raster_prepare(page);
    lv_obj_raster_prepare(page);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);
    CHECK(after.rejections == before.rejections + 1);
    CHECK(after.negative_hits > before.negative_hits);
    CHECK(s_largest < 466u * 466u * 4u);
    lv_obj_delete(page);

    /* Hidden retained sources may be evicted, using last use rather than tree
     * order. The newest source stays warm while the oldest source rebuilds. */
    lv_obj_t * rows[8];
    for(unsigned i = 0; i < 8; i++) {
        rows[i] = raster(screen, 200, 300, true);
        lv_obj_set_hidden(rows[i], true);
        lv_obj_raster_prepare(rows[i]);
        lv_obj_raster_get_stats(&after);
        CHECK(after.source_bytes <= after.source_budget);
    }
    CHECK(after.evictions > 0);
    lv_obj_raster_get_stats(&before);
    lv_obj_raster_prepare(rows[7]);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);
    lv_obj_raster_prepare(rows[0]);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds + 1);
    for(unsigned i = 0; i < 8; i++) lv_obj_delete(rows[i]);

    /* Both the same owner and a second owner stop requesting a size that just
     * failed. A new scroll admission boundary permits one fresh attempt. */
    rows[0] = raster(screen, 200, 100, true);
    rows[1] = raster(screen, 200, 100, true);
    s_fail_at = 40000;
    lv_obj_raster_cache_retry();
    for(unsigned i = 0; i < 20; i++) lv_obj_raster_prepare(rows[i % 2]);
    CHECK(s_large_attempts == 1);
    s_fail_at = 0;
    lv_obj_raster_cache_retry();
    lv_obj_raster_prepare(rows[0]);
    lv_obj_raster_get_stats(&before);
    lv_obj_raster_prepare(rows[0]);
    lv_obj_raster_get_stats(&after);
    CHECK(after.builds == before.builds);
    CHECK(after.source_bytes >= 40000);
    CHECK(lv_obj_raster_ready(rows[0]));
    lv_obj_set_pos(rows[0], 2, 3);
    CHECK(lv_obj_raster_ready(rows[0]));
    lv_obj_invalidate_raster(rows[0]);
    CHECK(!lv_obj_raster_ready(rows[0]));
    lv_obj_delete(rows[0]); lv_obj_delete(rows[1]);

    /* Font observers may invalidate transparent ink while two sets coexist.
     * A locked transaction defers its cold workspace until old fonts retire. */
    page = raster(screen, 200, 100, false);
    lv_obj_raster_get_stats(&before);
    lv_obj_raster_cache_set_paused(true);
    lv_obj_raster_prepare(page);
    lv_obj_raster_get_stats(&after);
    CHECK(!lv_obj_raster_ready(page) && after.builds == before.builds);
    lv_obj_raster_cache_set_paused(false);
    lv_obj_raster_prepare(page);
    lv_obj_raster_get_stats(&after);
    CHECK(lv_obj_raster_ready(page) && after.builds == before.builds + 1);
    lv_obj_delete(page);

    /* A completed frame keeps its sources pinned until the locked caller has
     * drained drawing and explicitly releases that frame for a memory trim. */
    rows[0] = raster(screen, 200, 100, true);
    lv_obj_raster_prepare(rows[0]);
    lv_refr_now(display);
    rows[1] = raster(screen, 200, 100, true);
    CHECK(lv_obj_raster_ready(rows[0]));
    CHECK(!admit(get_raster(rows[1]), RASTER_BUDGET));
    CHECK(lv_obj_raster_ready(rows[0]));
    lv_draw_wait_for_finish();
    lv_obj_raster_cache_release_frame();
    CHECK(admit(get_raster(rows[1]), RASTER_BUDGET));
    CHECK(!lv_obj_raster_ready(rows[0]));
    lv_obj_raster_prepare(rows[0]);
    CHECK(lv_obj_raster_ready(rows[0]));
    CHECK(!admit(get_raster(rows[1]), RASTER_BUDGET)); /* next preparation repins */
    lv_obj_delete(rows[0]); lv_obj_delete(rows[1]);
    lv_obj_raster_get_stats(&after);
    CHECK(after.source_bytes == 0 && after.page_bytes == 0);
    lv_display_delete(display);
    lv_deinit();
    printf("raster budget: %u checks passed; transparent build peak=%zu B / %zu B\n",
           s_checks, build.peak, build.budget);
    return 0;
}
