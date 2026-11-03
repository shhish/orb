/* Stage 1's oracle is the unchanged compatibility renderer, never a second
 * call to the retained path. Every frozen frame is compared byte for byte. */
#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "src/core/lv_refr_private.h"
#include "orb_ui.h"
#include "orb_demo.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "sim_display.h"
#include "lvgl/draw/lv_draw_sw_profile.h"

#define FRAME_BYTES (SIM_D * SIM_D * 3)
static uint8_t s_band[SIM_D * 84 * 2] __attribute__((aligned(64)));
static uint8_t s_retained[FRAME_BYTES], s_reference[FRAME_BYTES];
static uint32_t s_now;
static unsigned s_checks;

static void require(bool condition, const char *why)
{
    s_checks++;
    if(condition) return;
    fprintf(stderr, "retained material: %s\n", why);
    exit(EXIT_FAILURE);
}
static uint32_t tick(void) { return s_now; }
static void refresh(lv_display_t *display) { lv_obj_update_layout(lv_screen_active()); lv_refr_now(display); }
static void full(lv_display_t *display) { lv_obj_invalidate(lv_screen_active()); refresh(display); }
static void prepare(lv_display_t *display)
{
    /* Unknown shapes use one opaque frame while the shared-mask preparation
     * timer runs; retention assertions start after that documented fallback. */
    for(unsigned i = 0; i < 4; i++) { s_now += 20; lv_timer_handler(); }
    full(display);
}
static void compare(lv_display_t *display)
{
    refresh(display);
    sim_display_rgb(s_retained);
    lv_obj_material_set_enabled(false);
    full(display);
    sim_display_rgb(s_reference);
    require(!memcmp(s_retained, s_reference, sizeof(s_reference)), "retained/compatibility pixels differ");
    lv_obj_material_set_enabled(true);
    full(display);
}
static lv_obj_t *scene(lv_obj_t **source, lv_obj_t **text)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    *source = lv_obj_create(screen);
    lv_obj_remove_style_all(*source);
    lv_obj_set_size(*source, 300, 230);
    lv_obj_set_pos(*source, 83, 110);
    lv_obj_set_style_bg_opa(*source, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(*source, lv_color_hex(0x16508A), 0);
    *text = lv_label_create(*source);
    lv_label_set_text(*text, "Retained material\nExact RGB565 pixels");
    orb_text_apply(*text, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_obj_set_pos(*text, 8, 30);
    lv_obj_set_raster_matte(*source, lv_color_black());
    lv_obj_set_raster_scale(*source, 211);
    lv_obj_set_raster_opa(*source, 219);
    lv_obj_t *control = lv_button_create(screen);
    lv_obj_remove_style_all(control);
    lv_obj_set_pos(control, 136, 174);
    lv_obj_set_size(control, 194, 71);
    orb_glass_apply(control, ORB_GLASS_REGULAR, ORB_GRAY);
    lv_obj_t *label = lv_label_create(control);
    lv_label_set_text(label, "Reply");
    orb_text_apply(label, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
    lv_obj_center(label);
    lv_obj_raster_prepare(screen);
    return control;
}
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
static uint64_t transformed(const lv_draw_sw_profile_snapshot_t *p)
{
    uint64_t n = 0;
    for(unsigned i = 0; i < LV_DRAW_SW_PROFILE_SLOTS; i++) {
        if(lv_draw_sw_profile_transformed(i)) n += p->rows[i].calls;
    }
    return n;
}
#endif
static void unmanaged(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(0x00DAC3);
    lv_area_t area = {130, 180, 330, 230};
    lv_draw_rect(lv_event_get_layer(event), &d, &area);
}
static void * fail_allocate(size_t bytes) { LV_UNUSED(bytes); return NULL; }
int main(int argc, char **argv)
{
    bool native = argc == 2 && !strcmp(argv[1], "--native");
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    if(native) lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, s_band, NULL, sizeof(s_band), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_lock();
    orb_ui_init(NULL);
    orb_glass_cache_prewarm();
    lv_obj_t *source, *text;
    lv_obj_t *control = scene(&source, &text);
    prepare(display);
    lv_draw_sw_material_stats_t before, after;
    lv_draw_sw_material_get_stats(&before);
    require(before.entries == 1 && before.bytes <= before.budget_bytes, "material admission/budget failed");
    lv_obj_raster_stats_t raster_before, raster_after;
    lv_obj_raster_get_stats(&raster_before);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot_t profile_before, profile_after;
    lv_draw_sw_profile_snapshot(&profile_before);
#endif
    for(unsigned i = 0; i < 12; i++) {
        orb_glass_set_light(180 + i * 9, 0.6f);
        refresh(display);
    }
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&profile_after);
#endif
    lv_obj_raster_get_stats(&raster_after);
    lv_draw_sw_material_get_stats(&after);
    require(after.base_hits > before.base_hits, "light ticks did not restore retained bases");
    require(after.blur_kernels == before.blur_kernels, "light ticks ran blur kernels");
    require(after.source_recaptures == before.source_recaptures, "light ticks recaptured sources");
    require(raster_after.builds == raster_before.builds, "light ticks rebuilt source rasters");
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    require(transformed(&profile_after) == transformed(&profile_before), "light ticks traversed transformed rows");
#endif
    compare(display);
#if defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY
    lv_draw_sw_material_get_stats(&before);
    for(unsigned i = 0; i < 12; i++) {
        lv_draw_sw_blur_note_object_motion(source);
        lv_obj_set_style_translate_x(source, i + 1, 0);
        lv_obj_set_raster_opa(source, 190 + i);
        /* Content may change during motion too; skipped revision-registry
         * updates must not allow an old base to survive the settle boundary. */
        if(i == 6) lv_label_set_text(text, "Content changed while moving");
        compare(display);
    }
    lv_draw_sw_material_get_stats(&after);
    require(after.moving_bypasses > before.moving_bypasses, "moving backdrop did not bypass retention");
    require(after.map_builds == before.map_builds, "moving backdrop rebuilt retained maps");
    require(after.base_builds == before.base_builds, "moving backdrop copied retained bases");
    s_now += 160;
    lv_timer_handler();
    require(!lv_draw_sw_blur_motion_active(display), "retained motion did not settle");
    prepare(display);
    compare(display);
    orb_glass_set_light(20, 0.6f);
    refresh(display);
    lv_draw_sw_material_get_stats(&before);
    orb_glass_set_light(70, 0.6f);
    refresh(display);
    lv_draw_sw_material_get_stats(&after);
    require(after.base_hits > before.base_hits, "settled backdrop did not recover base reuse");
    require(after.blur_kernels == before.blur_kernels, "settled light-only update blurred again");
#endif
    for(unsigned tint = 0; tint <= 255; tint += 17) {
        lv_obj_set_style_bg_color(control, lv_color_hex(0x0091FF), 0);
        lv_obj_set_style_bg_opa(control, tint, 0);
        compare(display);
    }
    /* A full-width partial invalidation forces a shadow-only first band and
     * then the control interior. Publishing a new base key in the first band
     * would incorrectly skip that interior and retain the preceding tint. */
    for(unsigned tint = 3; tint < 255; tint += 23) {
        lv_obj_set_style_bg_opa(control, tint, 0);
        lv_area_t bands = {0, 142, SIM_D - 1, 280};
        lv_inv_area(display, &bands);
        compare(display);
    }
        for(int size = 0; size <= 5; size++) {
        lv_obj_set_style_transform_width(control, size, 0);
        lv_obj_set_style_transform_height(control, size / 2, 0);
        compare(display);
    }
    lv_obj_raster_get_stats(&raster_before);
    lv_obj_send_event(control, LV_EVENT_PRESSED, NULL);
    for(unsigned i = 0; i < 40; i++) {
        s_now += 20;
        if(i == 10) lv_obj_send_event(control, LV_EVENT_RELEASED, NULL);
        lv_timer_handler();
        compare(display);
    }
    lv_obj_raster_get_stats(&raster_after);
    require(raster_after.builds == raster_before.builds, "press spring rebuilt source rasters");
    lv_obj_material_unregister(control);
    require(!lv_obj_material_register(control, 204, 75, 18, fail_allocate, free), "OOM admission succeeded");
    require(!lv_obj_material_register(control, INT_MAX, 77, 18, malloc, free), "oversized width admitted");
    require(!lv_obj_material_register(control, 208, INT_MAX, 18, malloc, free), "oversized height admitted");
    require(!lv_obj_material_register(control, 208, 77, INT_MAX, malloc, free), "oversized padding admitted");
    orb_glass_set_light(80, 0.4f);
    compare(display);
    /* Orb's draw hook lazily re-admits the control after the explicit failed
     * admission; returning from another screen uses the same retry path. */
    lv_draw_sw_material_get_stats(&after);
    require(after.entries == 1, "draw did not retry after OOM");
    full(display);
        lv_label_set_text(text, "Changed descendant");
    compare(display);
    lv_obj_set_raster_scale(source, 169);
    lv_obj_set_raster_opa(source, 121);
    lv_obj_set_pos(source, 95, 102);
    compare(display);
    lv_obj_set_style_clip_corner(source, true, 0);
    compare(display);
    lv_obj_set_hidden(control, true);
    compare(display);
    lv_obj_set_hidden(control, false);
    compare(display);
    lv_obj_t *custom = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(custom);
    lv_obj_set_size(custom, 300, 200);
    lv_obj_set_pos(custom, 80, 120);
    lv_obj_add_event_cb(custom, unmanaged, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_move_to_index(custom, 1);
    full(display);
    orb_glass_set_light(30, 0.8f);
    compare(display);
    lv_obj_delete(custom);
    lv_obj_delete(source);
    source = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(source);
    lv_obj_set_size(source, 300, 200);
    lv_obj_set_pos(source, 80, 120);
    lv_obj_set_style_bg_color(source, lv_color_hex(0x30D158), 0);
    lv_obj_set_style_bg_opa(source, LV_OPA_COVER, 0);
    lv_obj_move_to_index(source, 0);
    compare(display);
    lv_obj_t *extra[3];
    for(unsigned i = 0; i < 3; i++) {
        extra[i] = lv_button_create(lv_screen_active());
        lv_obj_remove_style_all(extra[i]);
        lv_obj_set_size(extra[i], 71, 71);
        lv_obj_set_pos(extra[i], 65 + i * 130, 65);
        orb_glass_apply(extra[i], ORB_GLASS_REGULAR, ORB_GRAY);
    }
    full(display);
    lv_draw_sw_material_get_stats(&after);
    require(after.entries == 3, "bounded three-owner admission failed");
    require(after.bytes <= after.budget_bytes, "three owners exceeded material budget");
    compare(display);
    for(unsigned i = 0; i < 3; i++) lv_obj_delete(extra[i]);
    lv_obj_delete(control);
    lv_draw_sw_material_get_stats(&after);
    require(after.entries == 0 && after.bytes == 0, "retired material leaked");
    lv_obj_clean(lv_screen_active());
    size_t plateau = 0;
    for(unsigned cycle = 0; cycle < 20; cycle++) {
        control = scene(&source, &text);
        full(display);
        lv_draw_sw_material_get_stats(&after);
        if(cycle == 0) plateau = after.bytes;
        require(after.bytes == plateau, "20-cycle retained plateau grew");
        lv_obj_clean(lv_screen_active());
        lv_draw_sw_material_get_stats(&after);
        require(after.bytes == 0 && after.entries == 0, "cycle retained leases survived deletion");
    }
    lv_obj_t *initial = lv_screen_active();
    lv_obj_t *demo = lv_obj_create(NULL);
    orb_demo_build(demo);
    lv_screen_load(demo);
    uint32_t demo_end = s_now + 8600;
    while(s_now < demo_end) { s_now += 20; lv_timer_handler(); }
    require(!strcmp(orb_demo_phase(), "glass"), "actual demo did not reach settled glass phase");
    lv_anim_delete_all();
    full(display);
    /* The full-screen round bands may omit invisible shadow corners; warm one
     * actual control invalidation before testing the retained presentation. */
    orb_glass_set_light(100, 0.6f);
    refresh(display);
    lv_draw_sw_material_get_stats(&before);
    require(before.entries == 3, "actual demo did not admit all three controls");
    lv_obj_raster_get_stats(&raster_before);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&profile_before);
#endif
    for(unsigned i = 0; i < 12; i++) {
        orb_glass_set_light(130 + i * 9, 0.6f);
        refresh(display);
    }
    lv_draw_sw_material_get_stats(&after);
    lv_obj_raster_get_stats(&raster_after);
#if defined(CONFIG_ORB_DISPLAY_DRAW_PROFILE) && CONFIG_ORB_DISPLAY_DRAW_PROFILE
    lv_draw_sw_profile_snapshot(&profile_after);
    require(transformed(&profile_after) == transformed(&profile_before), "actual demo light ticks transformed rows");
#endif
    require(after.base_hits - before.base_hits >= 36, "actual demo did not restore all controls each light tick");
    require(after.blur_kernels == before.blur_kernels, "actual demo light ticks ran kernels");
    require(after.source_recaptures == before.source_recaptures, "actual demo light ticks recaptured sources");
    require(raster_after.builds == raster_before.builds, "actual demo light ticks rebuilt sources");
    compare(display);
    lv_screen_load(initial);
    lv_obj_delete(demo);
    /* Match benchmark_start: preserve a real live shell, construct a second
     * screen while it is inactive, load the fixture, then return to the shell.
     * Empty-root tests missed all three material slots being pinned offscreen. */
    require(orb_model_init(), "model init failed");
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    lv_obj_t *saved = lv_screen_active();
    orb_shell_t *shell = orb_shell_create(saved, NULL, NULL);
    require(shell != NULL, "preserved shell construction failed");
    orb_shell_snapshot_t status = {
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70,
    };
    orb_shell_update(shell, &status);
    orb_shell_navigate(shell, ORB_SHELL_APPS);
    for(unsigned i = 0; i < 40; i++) { s_now += 20; lv_timer_handler(); }
    lv_draw_sw_material_get_stats(&before);
    require(before.entries == 3, "preserved shell did not occupy all material slots");
    size_t plateau_bytes = 0;
    for(unsigned cycle = 0; cycle < 3; cycle++) {
        lv_obj_t *fixture = lv_obj_create(NULL);
        orb_demo_build(fixture);
        lv_screen_load(fixture);
#if defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY
        /* The ease-in begins with a stationary value; admit its cold first
         * frame before measuring the genuinely moving part of the phase. */
        for(unsigned i = 0; i < 5; i++) { s_now += 20; lv_timer_handler(); }
        lv_draw_sw_material_get_stats(&before);
        for(unsigned i = 5; i < 200; i++) { s_now += 20; lv_timer_handler(); }
        lv_draw_sw_material_get_stats(&after);
        require(after.map_builds == before.map_builds, "live scrolling fixture rebuilt retained maps");
        require(after.base_builds == before.base_builds, "live scrolling fixture copied retained bases");
        for(unsigned i = 200; i < 420; i++) { s_now += 20; lv_timer_handler(); }
#else
        for(unsigned i = 0; i < 420; i++) { s_now += 20; lv_timer_handler(); }
#endif
        require(!strcmp(orb_demo_phase(), "glass"), "loaded fixture did not reach glass");
        lv_draw_sw_material_get_stats(&before);
        for(unsigned i = 0; i < 100; i++) { s_now += 20; lv_timer_handler(); }
        lv_draw_sw_material_get_stats(&after);
        require(after.base_hits > before.base_hits + 100, "saved shell starved active fixture bases");
        require(after.map_hits > before.map_hits, "saved shell starved active fixture maps");
        require(after.entries == 3 && after.bytes <= after.budget_bytes, "cross-screen material budget failed");
        if(cycle == 0) plateau_bytes = after.bytes;
        require(after.bytes == plateau_bytes, "cross-screen material payload grew");
        lv_screen_load(saved);
        lv_obj_delete(fixture);
        for(unsigned i = 0; i < 40; i++) { s_now += 20; lv_timer_handler(); }
        prepare(display);
        lv_draw_sw_material_get_stats(&after);
        require(after.entries > 0 && after.bytes <= after.budget_bytes,
                "preserved shell did not re-admit its visible controls");
        orb_glass_set_light(10 + cycle * 30, 0.6f);
        refresh(display);
        compare(display);
    }
    orb_shell_destroy(shell);
    lv_draw_sw_material_get_stats(&after);
    require(after.bytes == 0 && after.entries == 0, "cross-screen material leases leaked");
    /* Apps may present its cached body when restored. Separately prove that a
     * re-admitted, directly drawn glass control regains the same light-only
     * reuse guarantee, rather than merely reacquiring unused payload bytes. */
    control = scene(&source, &text);
    prepare(display);
    lv_obj_t *fixture = lv_obj_create(NULL);
    orb_demo_build(fixture);
    lv_screen_load(fixture);
    prepare(display);
    lv_screen_load(saved);
    lv_obj_delete(fixture);
    prepare(display);
    orb_glass_set_light(110, 0.6f);
    refresh(display);
    lv_draw_sw_material_get_stats(&before);
    orb_glass_set_light(150, 0.6f);
    refresh(display);
    lv_draw_sw_material_get_stats(&after);
    require(after.base_hits > before.base_hits, "returned direct control did not retain its base");
    require(after.blur_kernels == before.blur_kernels, "returned direct light tick ran a blur kernel");
    require(after.source_recaptures == before.source_recaptures, "returned direct light tick recaptured its source");
    compare(display);
    lv_obj_clean(saved);
    lv_draw_sw_material_get_stats(&after);
    require(after.bytes == 0 && after.entries == 0, "returned direct control lease leaked");
    lv_unlock();
    printf("retained material: %u checks; exact frozen reference, 12 light frames, 20 lifecycle cycles\n", s_checks);
    return EXIT_SUCCESS;
}
