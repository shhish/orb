/* Measured Design 2.0 rim limits and the stopwatch's shared tick geometry. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
extern size_t sim_memory_allocation_calls(void);
static uint32_t tick(void) { return s_now; }

static void require(bool valid, const char *message)
{
    ++s_checks;
    if (valid) return;
    fprintf(stderr, "kit rim2: %s\n", message);
    exit(EXIT_FAILURE);
}

static unsigned timers(void)
{
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) ++count;
    return count;
}

static void prepare(void)
{
    ++s_now;
    lv_timer_handler();
}

static void ready(lv_obj_t *rim, bool expected)
{
    for (unsigned slice = 0; slice < 2; ++slice)
        require(lv_obj_raster_ready(lv_obj_get_child(rim, slice)) == expected,
                "rim source readiness differs from deferred preparation state");
}

static lv_draw_buf_t *snapshot(lv_obj_t *root)
{
    lv_draw_buf_t *pixels = lv_snapshot_take(root, LV_COLOR_FORMAT_ARGB8888);
    require(pixels != NULL, "rim pixel oracle snapshot failed");
    return pixels;
}

static void same_pixels(const lv_draw_buf_t *before, const lv_draw_buf_t *after)
{
    require(before->header.w == after->header.w && before->header.h == after->header.h,
            "deferred preparation changed snapshot geometry");
    for (unsigned y = 0; y < before->header.h; ++y)
        require(!memcmp(before->data + y * before->header.stride,
                        after->data + y * after->header.stride,
                        before->header.w * lv_color_format_get_size(before->header.cf)),
                "deferred rim pixels differ from immediate native output");
}

static void deferred_lifecycle(lv_obj_t *root)
{
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    unsigned timer_count = timers();
    lv_obj_t *rim = orb_kit_rim_title_create(root, "Before", ORB_KIT_HOME);
    require(rim != NULL, "deferred rim fixture unavailable");
    lv_obj_raster_get_stats(&after);
    require(after.builds == before.builds + 2, "rim creation did not prepare its capture sources");
    require(timers() == timer_count, "rim creation queued work after its capture boundary");
    ready(rim, true);
    before = after;
    orb_kit_rim_text_set(rim, "Intermediate", " · OFF");
    orb_kit_rim_text_set(rim, "Latest", " · ON");
    require(timers() == timer_count + 1, "pending text changes queued duplicate preparations");
    lv_obj_raster_get_stats(&after);
    require(before.builds == after.builds, "rim setter still captures below its caller");
    lv_draw_buf_t *native = snapshot(root);
    lv_draw_buf_t *native565 = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB565);
    require(native565 != NULL, "immediate RGB565 rim snapshot failed");
    prepare();
    ready(rim, true);
    require(timers() == timer_count, "serviced rim retained its deferred job");
    lv_draw_buf_t *cached = snapshot(root);
    lv_draw_buf_t *cached565 = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB565);
    require(cached565 != NULL, "prepared RGB565 rim snapshot failed");
    same_pixels(native, cached);
    same_pixels(native565, cached565);
    lv_draw_buf_destroy(native565);
    lv_draw_buf_destroy(cached565);
    lv_draw_buf_destroy(native);
    orb_kit_rim_text_set(rim, "Replaced newest", " · OFF");
    require(orb_kit_rim_title_set_inset(rim, true), "deferred inset change unavailable");
    ready(rim, false);
    native = snapshot(root);
    bool changed = false;
    for (unsigned y = 0; y < cached->header.h; ++y)
        changed |= memcmp(native->data + y * native->header.stride,
                          cached->data + y * cached->header.stride,
                          cached->header.w * sizeof(lv_color32_t)) != 0;
    require(changed, "warm content/inset change showed the old cached pixels");
    lv_draw_buf_destroy(cached);
    prepare();
    ready(rim, true);
    cached = snapshot(root);
    same_pixels(native, cached);
    lv_draw_buf_destroy(native);
    lv_draw_buf_destroy(cached);

    /* Font observers may run while the owning place is hidden. The deferred
     * source must still contain the newest glyphs when the place is revealed. */
    lv_obj_set_hidden(rim, true);
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK,
            "deferred AX3 font change unavailable");
    require(orb_fonts_set_bold_text(true) == ORB_FONTS_OK,
            "deferred Bold font change unavailable");
    orb_kit_rim_text_set(rim, "Updated", NULL);
    ready(rim, false);
    prepare();
    ready(rim, true);
    lv_obj_set_hidden(rim, false);
    cached = snapshot(root);
    for (unsigned i = 0; i < 2; ++i) lv_obj_set_raster(lv_obj_get_child(rim, i), false);
    native = snapshot(root);
    same_pixels(native, cached);
    lv_draw_buf_destroy(native);
    lv_draw_buf_destroy(cached);
    lv_obj_delete(rim);

    /* Deletion owns cancellation before freeing borrowed strings or children.
     * Servicing timers afterwards must neither touch retired state nor build. */
    timer_count = timers();
    rim = orb_kit_rim_caption_create(root, "Delete before service");
    require(timers() == timer_count, "caption creation queued preparation");
    orb_kit_rim_text_set(rim, "Retired newest text", NULL);
    require(timers() == timer_count + 1, "caption update did not queue preparation");
    lv_obj_delete(rim);
    require(timers() == timer_count, "deleted rim left a callback owning freed state");
    lv_obj_raster_get_stats(&before);
    prepare();
    lv_obj_raster_get_stats(&after);
    require(before.count == after.count && before.builds == after.builds,
            "retired rim was prepared after deletion");
    require(orb_fonts_set_bold_text(false) == ORB_FONTS_OK, "default weight restore unavailable");
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
            "default size restore unavailable");
}

static void titles(lv_obj_t *root, const char *mode)
{
    const char *names[] = {
        "Reading Room & Family Library", "Guest Suite Bath", "Kitchen",
        "Conservatory & Breakfast Room", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
        "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii", "Living Room and Downstairs Study",
    };
    unsigned longest = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        require(strlen(names[i]) <= 32, "fixture exceeds legal room-name bytes");
        lv_obj_t *title = orb_kit_rim_title_create(root, names[i], ORB_KIT_HOME);
        orb_kit_rim_text_info_t info;
        require(orb_kit_rim_text_info(title, &info), "rim metadata unavailable");
        require(info.angle_degrees <= 120.01f, "rim exceeds 120 degree budget");
        if (info.ellipsised) {
            require(info.style == ORB_TEXT_FOOTNOTE2, "ellipsis preceded Footnote2 step");
            longest = LV_MAX(longest, strlen(names[i]));
        }
        printf("RIM %s bytes=%zu angle=%.2f style=%d ellipsis=%d %s\n",
               mode, strlen(names[i]), info.angle_degrees, info.style,
               info.ellipsised, names[i]);
        size_t allocations = sim_memory_allocation_calls();
        lv_obj_raster_stats_t before, after;
        lv_obj_raster_get_stats(&before);
        orb_kit_rim_text_set(title, names[i], NULL);
        lv_obj_raster_get_stats(&after);
        require(sim_memory_allocation_calls() == allocations && before.builds == after.builds,
                "unchanged rim set allocated or rebuilt prepared text");
        ready(title, true);
        for (unsigned slice = 0; slice < 2; ++slice) {
            require(lv_obj_raster_ready(lv_obj_get_child(title, slice)),
                    "title retained source was not prepared");
        }
        /* Exercise suffix creation/removal while the fitted text is borrowed
         * by both slices; subsequent draws must retain valid source data. */
        orb_kit_rim_text_set(title, names[i], " · ON");
        lv_refr_now(lv_display_get_default());
        orb_kit_rim_text_set(title, names[i], NULL);
        lv_refr_now(lv_display_get_default());
        lv_obj_delete(title);
    }
    printf("RIM %s longest-ellipsised-fixture=%u bytes\n", mode, longest);
    /* Find exact real-font boundaries for the widest and narrowest ASCII caps.
     * Lowercase names become uppercase before fitting, so audit uppercase. */
    const char *letters = "MWI";
    for (unsigned glyph = 0; letters[glyph]; ++glyph) {
        unsigned fits = 0;
        char name[33];
        for (unsigned n = 1; n <= 32; ++n) {
            memset(name, letters[glyph], n); name[n] = 0;
            float angle = orb_kit_rim_text_measure(name, ORB_TEXT_FOOTNOTE2, 200);
            if (angle <= 120.f) fits = n;
        }
        printf("RIM_BOUNDARY %s glyph=%c full=%u first-ellipsis=%u\n",
               mode, letters[glyph], fits, fits < 32 ? fits + 1 : 0);
    }
}

static void ticks(lv_obj_t *root, unsigned count)
{
    lv_obj_t *ring = orb_kit_tick_ring_create(root, 219, count, ORB_KIT_TIME);
    require(ring != NULL, "tick ring unavailable");
    lv_obj_update_layout(root);
    require(lv_obj_get_width(ring) == 466 && !lv_obj_is_clickable(ring),
            "tick ring owns input or changed display geometry");
    lv_obj_t *marker = lv_obj_get_child(ring, 0);
    require(marker && lv_obj_get_width(marker) == 14 && lv_obj_get_height(marker) == 14,
            "stopwatch marker differs from 14px design diameter");
    const int centers[][2] = {{233,28}, {438,233}, {233,438}, {28,233}};
    for (unsigned quarter = 0; quarter < 4; ++quarter) {
        orb_kit_tick_ring_set_position(ring, quarter * 15000, 60000);
        lv_obj_update_layout(root);
        require(lv_obj_get_x(marker) + 7 == centers[quarter][0] &&
                lv_obj_get_y(marker) + 7 == centers[quarter][1],
                "marker phase is not clockwise from twelve o'clock");
    }
    orb_kit_tick_ring_set_position(ring, 82400, 60000);
    lv_obj_update_layout(root);
    require(abs(lv_obj_get_x(marker) + 7 - 379) <= 1 &&
            abs(lv_obj_get_y(marker) + 7 - 376) <= 1,
            "fractional second marker differs from stopwatch reference");
    lv_refr_now(lv_display_get_default());
    sim_display_rgb(s_pixels);
    /* Cardinal ticks reach outer tip219 and terminate at inner205. The
     * centre and canvas stay black: no full gauge track or decoration. */
    for (int y = 14; y <= 28; ++y) {
        const uint8_t *pixel = &s_pixels[(y * SIM_D + 233) * 3];
        require(pixel[0] > 25 && pixel[0] < 85, "top tick lost or wrong semantic gray");
    }
    const uint8_t *centre = &s_pixels[(233 * SIM_D + 233) * 3];
    require(!centre[0] && !centre[1] && !centre[2], "tick ring paints content canvas");
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    size_t allocations = sim_memory_allocation_calls();
    for (unsigned i = 0; i < 100; ++i)
        orb_kit_tick_ring_set_position(ring, 82400, 60000);
    lv_obj_raster_get_stats(&after);
    require(allocations == sim_memory_allocation_calls() && before.builds == after.builds,
            "stationary tick marker allocates or rasterizes");
    lv_obj_delete(ring);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    require(sim_display_create() != NULL, "display unavailable");
    orb_ui_init(NULL);
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root, false);
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
            "default fonts unavailable");
    deferred_lifecycle(root);
    titles(root, "default");
    require(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK,
            "AX3 fonts unavailable");
    titles(root, "ax3");
    require(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold fonts unavailable");
    orb_ui_set_reduce_transparency(true);
    titles(root, "ax3-bold-rt");
    ticks(root, 12); ticks(root, 60);
    require(!orb_kit_tick_ring_create(root, 219, 13, ORB_KIT_TIME), "invalid tick count accepted");
    printf("kit rim2: %u checks passed\n", s_checks);
    return EXIT_SUCCESS;
}
