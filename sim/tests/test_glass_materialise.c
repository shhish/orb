/* Material transitions through the public API and actual band renderer. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_ui.h"
#include "sim_display.h"
#include "../../firmware/components/orb_ui/orb_ui_private.h"

static uint32_t s_now;
static unsigned s_checks, s_allocations;
static bool s_fail_allocations;
static uint8_t s_band[SIM_D * 84 * 2] __attribute__((aligned(64)));
static uint8_t s_rgb[SIM_D * SIM_D * 3], s_absent[SIM_D * SIM_D * 3], s_mid[SIM_D * SIM_D * 3];

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }

static void *tracked_alloc(size_t size)
{
    if (s_fail_allocations) return NULL;
    void *p = malloc(size);
    if (p) s_allocations++;
    return p;
}

static void tracked_free(void *p)
{
    if (p) s_allocations--;
    free(p);
}

static void advance(lv_display_t *display, uint32_t ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(10, end - s_now);
        lv_timer_handler();
    }
    lv_refr_now(display);
    sim_display_rgb(s_rgb);
}

static void save(lv_display_t *display, const char *prefix, const char *name)
{
    if (!prefix) return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s-%s.png", prefix, name);
    CHECK(n > 0 && n < sizeof(path));
    lv_refr_now(display);
    CHECK(sim_display_save_png(path));
}

static lv_obj_t *control(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 123, 260);
    lv_obj_set_size(obj, 220, 72);
    lv_obj_set_scrollable(obj, false);
    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, "Reply");
    orb_text_apply(label, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(label, ORB_LABEL, 0);
    lv_obj_center(label);
    orb_glass_apply(obj, ORB_GLASS_REGULAR, ORB_BLUE);
    return obj;
}

static lv_obj_t *shape(int w, int h)
{
    lv_obj_t *obj = lv_button_create(lv_screen_active());
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, 150, 200);
    lv_obj_set_size(obj, w, h);
    orb_glass_apply(obj, ORB_GLASS_REGULAR, ORB_BLUE);
    return obj;
}

static void mask_cache_contract(lv_display_t *display)
{
    orb_ui_set_reduce_motion(false);
    orb_ui_set_reduce_transparency(false);
    orb_glass_cache_prewarm();
    orb_glass_build_stats_t before, after;
    orb_glass_build_stats(&before);
    CHECK(before.bytes <= 640u * 1024u && before.entries <= 32);
    lv_obj_t *a = shape(ORB_CONTROL, ORB_CONTROL);
    lv_obj_t *b = shape(ORB_PT(120), ORB_CONTROL);
    CHECK(lv_obj_get_style_blur_radius(a, 0) == ORB_PT(16));
    CHECK(lv_obj_get_style_blur_radius(b, 0) == ORB_PT(16));
    advance(display, 40);
    lv_obj_delete(a); lv_obj_delete(b);
    a = shape(72, 72); b = shape(72, 72);
    orb_glass_build_stats(&after);
    CHECK(after.builds == before.builds); /* construction never prepares */
    CHECK(lv_obj_get_style_blur_radius(a, 0) == ORB_PT(16));
    CHECK(lv_obj_get_style_blur_radius(b, 0) == ORB_PT(16));
    advance(display, 50);
    orb_glass_build_stats(&after);
    CHECK(after.builds == before.builds); /* identical controls share a lease */
    lv_obj_delete(a); lv_obj_delete(b);
    for (int cycle = 0; cycle < 20; cycle++) {
        a = shape(72, 72);
        advance(display, 20);
        lv_obj_delete(a);
    }
    orb_glass_build_stats(&after);
    CHECK(after.builds == before.builds); /* retained across navigation */
    a = shape(173, 69);
    orb_glass_build_stats(&before);
    CHECK(lv_obj_get_style_blur_radius(a, 0) == 0);
    CHECK(lv_obj_get_style_bg_opa(a, 0) == LV_OPA_90);
    lv_refr_now(display);
    orb_glass_build_stats(&after);
    CHECK(after.builds == before.builds); /* DRAW_MAIN never prepares */
    advance(display, 40);
    CHECK(lv_obj_get_style_blur_radius(a, 0) == ORB_PT(16));
    orb_glass_build_stats(&after);
    CHECK(after.builds == before.builds + 1);
    lv_obj_delete(a);
    /* Pressure evicts only unleased entries and stays bounded. Keep one
     * standard shape live while enough other keys cycle to fill every slot. */
    a = shape(72, 72);
    for (int w = 121; w < 281; w += 4) {
        b = shape(w, 65);
        advance(display, 40);
        lv_obj_delete(b);
        orb_glass_build_stats(&after);
        CHECK(after.bytes <= 640u * 1024u && after.entries <= 32);
        CHECK(lv_obj_get_style_blur_radius(a, 0) == ORB_PT(16));
    }
    lv_obj_delete(a);
    /* Hidden setup screens must not build masks or evict boot shapes. The
     * first visible frame requests preparation while retaining its fallback. */
    a = lv_button_create(lv_screen_active());
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, 177, 63);
    lv_obj_set_hidden(a, true);
    orb_glass_apply(a, ORB_GLASS_REGULAR, ORB_BLUE);
    orb_glass_build_stats(&before);
    advance(display, 80);
    orb_glass_build_stats(&after);
    CHECK(after.builds == before.builds);
    lv_obj_set_hidden(a, false);
    lv_refr_now(display);
    CHECK(lv_obj_get_style_blur_radius(a, 0) == 0);
    advance(display, 40);
    CHECK(lv_obj_get_style_blur_radius(a, 0) == ORB_PT(16));
    lv_obj_delete(a);
    a = shape(159, 67);
    s_fail_allocations = true;
    orb_glass_build_stats(&before);
    advance(display, 80);
    orb_glass_build_stats(&after);
    CHECK(after.failures == before.failures + 1);
    CHECK(lv_obj_get_style_blur_radius(a, 0) == 0);
    advance(display, 80);
    orb_glass_build_stats(&before);
    CHECK(before.failures == after.failures); /* OOM never becomes a retry loop */
    s_fail_allocations = false;
    lv_obj_delete(a);
    orb_glass_cache_clear();
    CHECK(s_allocations == 0);
    orb_glass_build_stats(&after);
    CHECK(after.bytes == 0 && after.entries == 0);
    printf("mask cache: peak=%zu budget=%u largest_host_build_us=%llu\n",
           after.peak_bytes, 640u * 1024u, (unsigned long long)after.max_build_us);
}

static void mask_transaction_contract(lv_display_t *display)
{
    orb_glass_cache_prewarm();
    lv_obj_t *a = shape(72, 72), *b = shape(72, 72), *hidden = shape(173, 69);
    advance(display, 40);
    lv_obj_set_hidden(hidden, true);
    advance(display, 20);
    memcpy(s_mid, s_rgb, sizeof(s_mid));
    orb_glass_cache_clear();
    orb_glass_build_stats_t leased, suspended, restored;
    lv_draw_sw_material_stats_t material_before, material_after;
    lv_draw_sw_material_get_stats(&material_before);
#if defined(CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) && CONFIG_ORB_DISPLAY_RETAINED_MATERIAL
    CHECK(material_before.entries > 0 && material_before.bytes > 0);
#endif
    orb_glass_build_stats(&leased);
    printf("mask transaction retained: entries=%u bytes=%zu\n", leased.entries, leased.bytes);
    CHECK(leased.entries >= 2 && leased.bytes > 30000);
    unsigned allocations = s_allocations;
    /* The caller has drained all rendering and retains LVGL ownership until
     * resume. No refresh, input callback or preparation timer can run here. */
    orb_glass_cache_suspend();
    orb_glass_cache_suspend();
    orb_glass_build_stats(&suspended);
    lv_draw_sw_material_get_stats(&material_after);
    CHECK(material_after.entries == 0 && material_after.bytes == 0);
    CHECK(suspended.entries == 0 && suspended.bytes < 8192);
    CHECK(s_allocations < allocations);
    s_fail_allocations = true;
    orb_glass_cache_prewarm();
    orb_glass_build_stats(&restored);
    CHECK(restored.entries == 0 && restored.failures == suspended.failures);
    s_fail_allocations = false;
    orb_glass_cache_resume();
    orb_glass_cache_resume();
    orb_glass_build_stats(&restored);
    CHECK(restored.entries == leased.entries && restored.bytes == leased.bytes);
    CHECK(s_allocations <= allocations);
    lv_draw_sw_material_get_stats(&material_after);
    CHECK(material_after.entries <= material_before.entries && material_after.bytes <= material_before.bytes);
#if defined(CONFIG_ORB_DISPLAY_RETAINED_MATERIAL) && CONFIG_ORB_DISPLAY_RETAINED_MATERIAL
    CHECK(material_after.entries > 0 && material_after.bytes > 0);
#endif
    CHECK(lv_obj_get_style_blur_radius(a, 0) == ORB_PT(16));
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
    sim_display_rgb(s_rgb);
    CHECK(!memcmp(s_mid, s_rgb, sizeof(s_rgb)));
    /* Font observers may replace a control during the same locked commit.
     * Saved lease bits belong to each object, never a retired masks pointer. */
    orb_glass_cache_suspend();
    lv_obj_delete(b);
    b = shape(96, 96);
    lv_obj_set_size(a, 73, 72);
    orb_glass_kit_resize(a);
    lv_draw_sw_material_get_stats(&material_after);
    CHECK(material_after.entries == 0 && material_after.bytes == 0);
    orb_glass_cache_resume();
    CHECK(lv_obj_get_style_blur_radius(b, 0) == ORB_PT(16));
    lv_obj_delete(a); lv_obj_delete(b); lv_obj_delete(hidden);
    orb_glass_cache_clear();
    CHECK(s_allocations == 0);
    puts("mask transaction: live/hidden leases reclaimed, exact pixels restored, replacements safe");
}

int main(int argc, char **argv)
{
    CHECK(argc <= 2);
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    CHECK(display != NULL);
    lv_display_set_buffers(display, s_band, NULL, sizeof(s_band), LV_DISPLAY_RENDER_MODE_PARTIAL);
    orb_ui_init(&(orb_ui_config_t){tracked_alloc, tracked_free});
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);
    /* Real contrasting content must be sampled by the developing material. */
    lv_obj_t *content = lv_label_create(screen);
    lv_label_set_text(content, "Readable content\nbehind the control");
    orb_text_apply(content, ORB_TEXT_BODY, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(content, ORB_LABEL, 0);
    lv_obj_set_width(content, 330);
    lv_obj_set_pos(content, 68, 250);
    advance(display, 50);
    unsigned animations = lv_anim_count_running();
    lv_obj_t *obj = control(screen);
    CHECK(!orb_glass_materialise(content, true));
    CHECK(!orb_glass_set_amount(obj, NAN));
    CHECK(orb_glass_get_amount(obj) == 1);
    CHECK(orb_glass_materialise(obj, true));
    CHECK(orb_glass_get_amount(obj) == 0);
    CHECK(lv_obj_is_hidden(obj));
    advance(display, 0);
    memcpy(s_absent, s_rgb, sizeof(s_absent));
    save(display, argc == 2 ? argv[1] : NULL, "absent");
    advance(display, 120);
    float amount = orb_glass_get_amount(obj);
    CHECK(amount > 0 && amount < 1);
    CHECK(!lv_obj_is_hidden(obj));
    CHECK(lv_obj_get_style_opa(obj, 0) == LV_OPA_COVER);
    int32_t blur = lv_obj_get_style_blur_radius(obj, 0);
    CHECK(blur > 0 && blur < ORB_PT(16));
    CHECK(memcmp(s_absent, s_rgb, sizeof(s_rgb)) != 0);
    save(display, argc == 2 ? argv[1] : NULL, "developing");
    /* Reverse twice: neither retarget may jump its material amount. */
    CHECK(orb_glass_materialise(obj, false));
    CHECK(orb_glass_get_amount(obj) == amount);
    advance(display, 100);
    amount = orb_glass_get_amount(obj);
    CHECK(orb_glass_materialise(obj, true));
    CHECK(orb_glass_get_amount(obj) == amount);
    advance(display, 600);
    CHECK(orb_glass_get_amount(obj) == 1);
    CHECK(lv_obj_get_style_blur_radius(obj, 0) == ORB_PT(16));
    CHECK(lv_anim_count_running() == animations);
    save(display, argc == 2 ? argv[1] : NULL, "complete");
    /* Global clarity modifies live and future regular material without new
     * masks or timers. Accessibility always owns the opaque replacement. */
    unsigned preference_allocations = s_allocations;
    lv_opa_t normal_opacity = lv_obj_get_style_bg_opa(obj, 0);
    CHECK(orb_glass_set_preference(ORB_GLASS_PREFERENCE_CLEAR));
    CHECK(lv_obj_get_style_bg_opa(obj, 0) < normal_opacity);
    CHECK(lv_obj_get_style_blur_radius(obj, 0) < ORB_PT(16));
    CHECK(orb_glass_set_preference(ORB_GLASS_PREFERENCE_TINTED));
    CHECK(lv_obj_get_style_bg_opa(obj, 0) > normal_opacity);
    CHECK(lv_obj_get_style_blur_radius(obj, 0) > ORB_PT(16));
    CHECK(!orb_glass_set_preference((orb_glass_preference_t)99));
    CHECK(orb_glass_preference() == ORB_GLASS_PREFERENCE_TINTED);
    CHECK(s_allocations == preference_allocations);
    orb_ui_set_reduce_transparency(true);
    CHECK(orb_glass_set_preference(ORB_GLASS_PREFERENCE_CLEAR));
    CHECK(lv_obj_get_style_bg_opa(obj, 0) == LV_OPA_90);
    CHECK(lv_obj_get_style_blur_radius(obj, 0) == ORB_PT(16));
    orb_ui_set_reduce_transparency(false);
    CHECK(orb_glass_set_preference(ORB_GLASS_PREFERENCE_DEFAULT));
    CHECK(lv_obj_get_style_bg_opa(obj, 0) == normal_opacity);
    CHECK(s_allocations == preference_allocations);

    CHECK(orb_glass_materialise(obj, false));
    advance(display, 600);
    CHECK(orb_glass_get_amount(obj) == 0);
    CHECK(lv_obj_is_hidden(obj));
    CHECK(!memcmp(s_absent, s_rgb, sizeof(s_rgb)));
    CHECK(orb_glass_set_amount(obj, 1));
    CHECK(orb_glass_materialise(obj, false));
    advance(display, 90);
    lv_obj_delete(obj);
    CHECK(lv_anim_count_running() == animations);
    orb_glass_cache_clear();
    CHECK(s_allocations == 0);
    advance(display, 600);

    orb_ui_set_reduce_motion(true);
    orb_ui_set_reduce_transparency(true);
    obj = control(screen);
    CHECK(orb_glass_materialise(obj, true));
    advance(display, 70);
    CHECK(orb_glass_get_amount(obj) > 0 && orb_glass_get_amount(obj) < 1);
    CHECK(lv_obj_get_style_opa(obj, 0) == LV_OPA_COVER);
    /* RM does not animate blur/depth; RT remains the opaque final material. */
    CHECK(lv_obj_get_style_blur_radius(obj, 0) == ORB_PT(16));
    CHECK(lv_obj_get_style_bg_opa(obj, 0) == LV_OPA_90);
    save(display, argc == 2 ? argv[1] : NULL, "reduced-crossfade");
    memcpy(s_mid, s_rgb, sizeof(s_mid));
    advance(display, 100);
    unsigned faded_text = 0;
    for (int y = 266; y < 279; y++) {
        for (int x = 160; x < 300; x++) {
            size_t i = (y * SIM_D + x) * 3;
            if (s_absent[i] > 200 && s_rgb[i] < 90) {
                /* A style-only alpha animation left the backdrop blur fully
                 * visible. The rendered text must actually cross-fade. */
                CHECK(s_mid[i] > s_rgb[i] + 40 && s_mid[i] < s_absent[i] - 25);
                faded_text++;
            }
        }
    }
    CHECK(faded_text > 20);
    CHECK(orb_glass_get_amount(obj) == 1);
    CHECK(lv_obj_get_style_opa(obj, 0) == LV_OPA_COVER);
    CHECK(lv_anim_count_running() == animations);
    unsigned allocations = s_allocations;
    CHECK(orb_glass_materialise(obj, true));
    CHECK(s_allocations == allocations);
    s_fail_allocations = true;
    CHECK(orb_glass_materialise(obj, false));
    CHECK(orb_glass_get_amount(obj) == 0 && lv_obj_is_hidden(obj));
    CHECK(lv_anim_count_running() == animations);
    CHECK(s_allocations == allocations);
    s_fail_allocations = false;
    lv_obj_delete(obj);
    orb_glass_cache_clear();
    CHECK(s_allocations == 0);
    CHECK(lv_anim_count_running() == animations);
    mask_cache_contract(display);
    mask_transaction_contract(display);
    printf("glass materialise: %u checks passed\n", s_checks);
    return 0;
}
