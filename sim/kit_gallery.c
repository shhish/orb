/* Design 2.0 primitive gallery. Every scene is composed through the public kit
 * API and rendered through the firmware-equivalent RGB565 band pipeline. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static lv_display_t *s_display;
static const char *s_output;
static const char *s_mode;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
extern size_t sim_memory_allocation_calls(void);

static uint32_t tick(void) { return s_now; }

static void require(bool valid, const char *message)
{
    if (valid) return;
    fprintf(stderr, "kit gallery: %s\n", message);
    exit(EXIT_FAILURE);
}

static void settle(unsigned milliseconds)
{
    for (unsigned i = 0; i < milliseconds; i += 16) {
        s_now += 16;
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}

static lv_obj_t *screen(void)
{
    lv_obj_t *old = lv_screen_active();
    lv_obj_t *obj = lv_obj_create(NULL);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, SIM_D, SIM_D);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(obj, false);
    lv_screen_load(obj);
    lv_obj_delete(old);
    return obj;
}

static void center_at(lv_obj_t *obj, int x, int y)
{
    lv_obj_update_layout(obj);
    lv_obj_set_pos(obj, x - lv_obj_get_width(obj) / 2, y - lv_obj_get_height(obj) / 2);
}

static unsigned primaries(lv_obj_t *obj)
{
    if (!lv_obj_is_visible(obj)) return 0;
    unsigned count = orb_kit_is_primary(obj);
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        count += primaries(lv_obj_get_child(obj, i));
    return count;
}

static void save(const char *name)
{
    char path[1024];
    require(snprintf(path, sizeof(path), "%s/%s-%s.png", s_output, name, s_mode) < (int)sizeof(path),
            "output path too long");
    settle(48);
    require(primaries(lv_screen_active()) <= 1, "more than one primary in a gallery screen");
    require(sim_display_save_png(path), "PNG write failed");
    printf("PNG %s\n", path);
}

static float glyph_material_luminance(void)
{
    sim_display_rgb(s_pixels);
    unsigned sum = 0, count = 0;
    /* Inside the right-hand Timer disc, away from its pause glyph and rim.
     * The physical patch stays within both the resting and contracted disc. */
    for (int y = 350; y <= 362; ++y) {
        for (int x = 334; x <= 340; ++x) {
            const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
            sum += 54 * pixel[0] + 183 * pixel[1] + 19 * pixel[2];
            count++;
        }
    }
    return sum / (256.f * count);
}

static void discs(void)
{
    lv_obj_t *root = screen();
    orb_kit_rim_title_create(root, "DISC SIZES", ORB_KIT_GREY);
    lv_obj_t *disc = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL,
                                        ORB_KIT_HOME, ORB_ICON_HOUSE);
    center_at(disc, 143, 143);
    orb_kit_disc_set_focus(disc, true);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_KIND_MINI, ORB_KIT_ACCENT_FILL,
                                ORB_KIT_CODEX, ORB_ICON_GEAR), 283, 143);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_KIND_ROW, ORB_KIT_ACCENT_FILL,
                                ORB_KIT_SOUND, ORB_ICON_SPEAKER_HIGH), 353, 143);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_ART, ORB_KIT_TRACK,
                                ORB_KIT_DOT, ORB_ICON_PLAY_FILL), 233, 297);
    save("discs");

    root = screen();
    orb_kit_rim_title_create(root, "DISC MATERIALS", ORB_KIT_GREY);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_NEUTRAL_GLASS,
                                ORB_KIT_HOME, ORB_ICON_SUN), 143, 177);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_TINTED_GLASS,
                                ORB_KIT_TIME, ORB_ICON_PAUSE_FILL), 323, 177);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_TRACK,
                                ORB_KIT_TIME, ORB_ICON_PLAY_FILL), 233, 301);
    save("disc-materials");
}

static void buttons(void)
{
    lv_obj_t *root = screen();
    orb_kit_rim_title_create(root, "BUTTONS", ORB_KIT_PLAY);
    center_at(orb_kit_button_secondary_create(root, "Done"), 233, 220);
    orb_kit_place_bottom_arc(orb_kit_button_primary_create(root, "Play", ORB_KIT_PLAY));
    save("buttons");

    root = screen();
    lv_obj_t *ring = orb_kit_ring_create(root, 213, 12, ORB_KIT_TIME);
    orb_kit_ring_set_value(ring, 90, false);
    lv_obj_move_to_index(ring, 0);
    orb_kit_rim_title_inset_create(root, "TIMER", ORB_KIT_TIME);
    lv_obj_t *a = orb_kit_glyph_button_create(root, ORB_ICON_X, false, ORB_KIT_TIME);
    lv_obj_t *b = orb_kit_glyph_button_create(root, ORB_ICON_PAUSE_FILL, true, ORB_KIT_TIME);
    orb_kit_place_arc_pair(a, b);
    orb_kit_page_dots_set_active(orb_kit_page_dots_create(root, 4), 0, ORB_KIT_TIME);
    save("glyph-pair");
    float resting = glyph_material_luminance();
    lv_obj_send_event(b, LV_EVENT_PRESSED, NULL);
    settle(96);
    save("glyph-pressed");
    float pressed = glyph_material_luminance();
    require(pressed > resting + 1.f,
            "pressed glass lost its backdrop or failed to brighten");
    orb_kit_press_cancel(b);
    save("glyph-cancelled");
    float cancelled = glyph_material_luminance();
    require(cancelled >= resting - 1.f && cancelled <= resting + 1.f,
            "cancelled glass retained a material flash");
    printf("PRESS %s luminance resting=%.2f pressed=%.2f cancelled=%.2f\n",
           s_mode, resting, pressed, cancelled);
}

static void rim_text(void)
{
    lv_obj_t *root = screen();
    orb_kit_rim_title_two_colour_create(root, "GUEST SUITE BATH", ORB_KIT_HOME, " · 3 ON");
    orb_kit_rim_caption_create(root, "Hold for scenes");
    save("rim-two-colour");
    root = screen();
    orb_kit_rim_date_create(root, "WEDNESDAY", "28");
    lv_obj_t *focused = orb_kit_rim_caption_create(root, "HOME");
    require(orb_kit_rim_caption_set_style(focused, ORB_TEXT_CAPTION2, ORB_TEXT_EMPHASIZED,
                                         lv_color_white(), LV_OPA_COVER),
            "focused caption style unavailable");
    save("rim-date");
    root = screen();
    orb_kit_rim_title_create(root, "THIS TITLE IS LONG ENOUGH TO NEED A SMALLER FONT AND AN ELLIPSIS", ORB_KIT_CODEX);
    orb_kit_rim_caption_create(root, "A longer caption along the bottom rim");
    save("rim-overflow");
    root = screen();
    lv_obj_t *ring = orb_kit_ring_create(root, 213, 14, ORB_KIT_SOUND);
    orb_kit_ring_set_value(ring, 75, false);
    orb_kit_rim_title_inset_create(root, "ACCESSIBILITY", ORB_KIT_SOUND);
    save("rim-inset");
}

static void indicators(void)
{
    lv_obj_t *root = screen();
    lv_obj_t *outer = orb_kit_ring_create(root, 218, 10, ORB_KIT_CLAUDE);
    lv_obj_t *inner = orb_kit_ring_create(root, 194, 6, ORB_KIT_CODEX);
    orb_kit_ring_set_value(outer, 62, false);
    orb_kit_ring_set_value(inner, 38, false);
    lv_obj_move_to_index(outer, 0);
    lv_obj_move_to_index(inner, 0);
    orb_kit_page_dots_set_active(orb_kit_page_dots_create(root, 5), 2, ORB_KIT_CLAUDE);
    save("rings-dots");
    root = screen();
    lv_obj_t *slider = orb_kit_rim_slider_create(root, 213, 14, ORB_KIT_HOME,
                                               0, 100, 5, NULL, NULL);
    /* HIG readability wins over the reference's merged same-colour ring and
     * title ink. Dedicated rim pages validate text; this isolates the slider. */
    lv_obj_move_to_index(slider, 0);
    orb_kit_rim_slider_set_value(slider, 80, false);
    center_at(orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD, ORB_KIT_ACCENT_FILL,
                                ORB_KIT_HOME, ORB_ICON_POWER), 233, 152);
    save("rim-slider");
    lv_obj_send_event(slider, LV_EVENT_PRESSED, NULL);
    save("rim-slider-touched");
    lv_obj_send_event(slider, LV_EVENT_PRESS_LOST, NULL);
    root = screen();
    slider = orb_kit_rim_slider_create(root, 213, 12, ORB_KIT_HOME,
                                      0, 100, 5, NULL, NULL);
    lv_obj_move_to_index(slider, 0);
    orb_kit_rim_slider_set_angles(slider, 145, -110);
    orb_kit_rim_slider_set_value(slider, 70, false);
    save("control-slider");
}

static void rows(void)
{
    lv_obj_t *root = screen();
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    const orb_kit_row_config_t config[] = {
        {.title="Display", .glyph=ORB_ICON_SUN, .accent=ORB_KIT_CODEX, .value="70%"},
        {.title="Sounds", .glyph=ORB_ICON_SPEAKER_HIGH, .accent=ORB_KIT_SOUND, .value="On"},
        {.title="Quiet", .glyph=ORB_ICON_MOON, .accent=ORB_KIT_DOT, .show_switch=true, .checked=true},
        {.title="Orb", .subtitle="Connected", .glyph=ORB_ICON_CHECK, .accent=ORB_KIT_PLAY},
        {.title="About", .glyph=ORB_ICON_GEAR, .accent=ORB_KIT_GREY},
        {.title="Updates", .glyph=ORB_ICON_ARROW_UP, .accent=ORB_KIT_CODEX},
    };
    lv_obj_t *row_switch = NULL;
    for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); ++i) {
        lv_obj_t *row = orb_kit_row_create(list, &config[i]);
        lv_obj_set_y(row, 94 + 84 * i);
        if (config[i].show_switch) row_switch = orb_kit_row_switch(row);
    }
    require(orb_kit_list_apply(list), "chord list preparation failed");
    orb_kit_scroll_arc_attach(list);
    orb_kit_rim_title_create(root, "SETTINGS", ORB_KIT_GREY);
    save("rows-scroll-start");
    require(row_switch != NULL, "row switch unavailable");
    orb_kit_switch_set_checked(row_switch, false, true);
    settle(80);
    save("rows-switch-moving");
    lv_obj_send_event(row_switch, LV_EVENT_PRESSED, NULL);
    settle(96);
    save("rows-switch-touched");
    orb_kit_press_cancel(row_switch);
    orb_round_list_scroll_to(list, 190);
    orb_round_list_scroll_finish(list);
    save("rows-scroll-middle");

    root = screen();
    list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    const orb_kit_row_config_t long_rows[] = {
        {.title="Desk charge limit", .subtitle="Keep battery at 80%", .glyph=ORB_ICON_GEAR,
            .accent=ORB_KIT_GREY, .show_switch=true, .checked=true},
        {.title="Spoken replies", .subtitle="Read replies aloud", .glyph=ORB_ICON_SPEAKER_HIGH,
            .accent=ORB_KIT_SOUND, .show_switch=true, .checked=true},
        {.title="Alex's Mac mini", .subtitle="Disconnected from hub", .glyph=ORB_ICON_GEAR,
            .accent=ORB_KIT_GREY},
    };
    for (unsigned i = 0; i < sizeof(long_rows) / sizeof(long_rows[0]); i++) {
        lv_obj_t *row = orb_kit_row_create(list, &long_rows[i]);
        lv_obj_set_y(row, 140 + i * 84);
    }
    require(orb_kit_list_apply(list), "long-string row preparation failed");
    orb_kit_scroll_arc_attach(list);
    orb_kit_rim_title_create(root, "REAL SETTINGS", ORB_KIT_SECONDARY);
    save("rows-real-strings");

    root = screen();
    orb_kit_rim_title_create(root, "SWITCH", ORB_KIT_PLAY);
    lv_obj_t *off = orb_kit_switch_create(root, false, ORB_KIT_PLAY);
    lv_obj_t *on = orb_kit_switch_create(root, true, ORB_KIT_PLAY);
    center_at(off, 153, 233);
    center_at(on, 313, 233);
    save("switches");
    lv_obj_send_event(on, LV_EVENT_PRESSED, NULL);
    settle(96);
    save("switch-touched");
    orb_kit_press_cancel(on);
}

static void regressions(void)
{
    lv_obj_t *root = screen();
    orb_kit_rim_title_inset_create(root, "STOPWATCH", ORB_KIT_TIME);
    lv_obj_t *ticks = orb_kit_tick_ring_create(root, 219, 12, ORB_KIT_TIME);
    orb_kit_tick_ring_set_position(ticks, 82400, 60000);
    lv_obj_t *a = orb_kit_glyph_button_create(root, ORB_ICON_X, false, ORB_KIT_TIME);
    lv_obj_t *b = orb_kit_glyph_button_create(root, ORB_ICON_PLAY_FILL, true, ORB_KIT_TIME);
    orb_kit_place_arc_pair(a, b);
    save("stopwatch-minute-ticks");
    root = screen();
    ticks = orb_kit_tick_ring_create(root, 219, 60, ORB_KIT_TIME);
    orb_kit_tick_ring_set_position(ticks, 15000, 60000);
    orb_kit_rim_title_inset_create(root, "SECONDS", ORB_KIT_TIME);
    save("stopwatch-second-ticks");

    const char *names[] = {"Reading Room & Family Library", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW"};
    for (unsigned i = 0; i < 2; i++) {
        root = screen();
        orb_kit_rim_title_create(root, "DEMO HOME", ORB_KIT_HOME);
        lv_obj_t *label = lv_label_create(root);
        lv_obj_set_style_text_color(label, ORB_LABEL, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        orb_kit_text_fit_info_t info;
        require(orb_kit_hero_fit(label, names[i], i ? 406 : 220, 3, &info),
                "hero text failed its declared centre budget");
        center_at(label, 233, 225);
        save(i ? "hero-32-byte-wide" : "hero-room-name");
    }

    root = screen();
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    const orb_kit_row_config_t fixture[] = {
        {.title="Reading Room & Family Library", .subtitle="Last known 12 minutes ago",
            .value="No Response", .glyph=ORB_ICON_HOUSE, .accent=ORB_KIT_HOME},
        {.title="Repeat", .value="Weekdays", .glyph=ORB_ICON_TIMER, .accent=ORB_KIT_TIME},
        {.title="Accessory state", .value="Last known", .glyph=ORB_ICON_POWER, .accent=ORB_KIT_HOME},
        {.title="Lap 16", .subtitle="Split 12:59.99", .value="12:59.99",
            .glyph=ORB_ICON_TIMER, .accent=ORB_KIT_TIME},
    };
    for (unsigned i = 0; i < 4; i++) {
        lv_obj_t *row = orb_kit_row_create(list, &fixture[i]);
        lv_obj_set_y(row, 96 + i * 84);
        if (i == 1) orb_kit_disc_set_focus(lv_obj_get_child(row, 0), true);
        if (i == 3) {
            orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_SUBTITLE, ORB_TEXT_TABULAR);
            orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_VALUE, ORB_TEXT_TABULAR);
        }
    }
    lv_obj_t *end = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title="End of list", .glyph=ORB_ICON_CHECK, .accent=ORB_KIT_TIME});
    lv_obj_set_y(end, 700);
    require(orb_kit_list_apply(list), "regression row preparation failed");
    orb_kit_rim_title_create(root, "ROW FIT", ORB_KIT_TIME);
    save("rows-fit-focus");
    orb_round_list_scroll_to(list, 84);
    orb_round_list_scroll_finish(list);
    require(lv_obj_get_scroll_y(list) == 84, "regression scrolled state did not move its content");
    save("rows-fit-scrolled");

    root = screen();
    list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 466, 466);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (unsigned i = 0; i < 16; i++) {
        char title[24], subtitle[32];
        snprintf(title, sizeof(title), "Lap %u", i + 1);
        snprintf(subtitle, sizeof(subtitle), "Split 00:%02u.42", i + 1);
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
            .title=title, .subtitle=subtitle, .value="12:59.99",
            .glyph=ORB_ICON_TIMER, .accent=ORB_KIT_TIME});
        lv_obj_set_y(row, 96 + i * 84);
        orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_SUBTITLE, ORB_TEXT_TABULAR);
        orb_kit_row_set_text_role(row, ORB_KIT_ROW_TEXT_VALUE, ORB_TEXT_TABULAR);
    }
    require(orb_kit_list_apply(list), "lap gallery list preparation failed");
    orb_kit_scroll_arc_attach(list);
    orb_kit_rim_title_create(root, "LAPS", ORB_KIT_TIME);
    save("laps16-start");
    orb_round_list_scroll_to(list, 1000);
    orb_round_list_scroll_finish(list);
    save("laps16-end");
}

static double clock_us(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec * 1e6 + time.tv_nsec / 1e3;
}

static void bench(void)
{
    for (unsigned curved = 0; curved < 2; ++curved) {
        lv_obj_t *root = screen();
        lv_obj_t *obj;
        if (curved) obj = orb_kit_rim_title_create(root, "GUEST SUITE BATH", ORB_KIT_HOME);
        else {
            obj = lv_label_create(root);
            lv_label_set_text(obj, "GUEST SUITE BATH");
            orb_text_apply(obj, ORB_TEXT_FOOTNOTE1, ORB_TEXT_EMPHASIZED, 0);
            lv_obj_set_style_text_color(obj, ORB_KIT_HOME, 0);
            center_at(obj, 233, 35);
        }
        settle(96);
        const unsigned samples = 120;
        lv_area_t damage;
        lv_obj_get_coords(obj, &damage);
        lv_obj_raster_stats_t raster_before, raster_after;
        lv_obj_raster_get_stats(&raster_before);
        size_t allocations = sim_memory_allocation_calls();
        double begin = clock_us();
        for (unsigned i = 0; i < samples; ++i) {
            /* Moving/damage invalidation redraws the prepared source. Content
             * invalidation deliberately rebuilds it and measures creation. */
            lv_obj_invalidate_area(root, &damage);
            lv_refr_now(s_display);
        }
        double elapsed = clock_us() - begin;
        lv_obj_raster_get_stats(&raster_after);
        printf("BENCH %s %s: %.2f us/refresh, %zu pipeline allocator calls/%u refreshes\n",
               s_mode, curved ? "rim" : "plain", elapsed / samples,
               sim_memory_allocation_calls() - allocations, samples);
        printf("BENCH %s %s: %u raster source builds, %u source rejections\n", s_mode,
               curved ? "rim" : "plain", raster_after.builds - raster_before.builds,
               raster_after.rejections - raster_before.rejections);
        require(!curved || (raster_after.builds == raster_before.builds &&
                            raster_after.rejections == raster_before.rejections),
                "prepared rim source rebuilt during drawing");
    }
}

static size_t retained_bytes(void)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    return monitor.total_size;
}

static void memory_report(void)
{
    /* A process-local CLIB instrument measures requested LVGL payload. Font
     * pack mallocs and malloc headers are excluded. Shape caches are shared;
     * report them separately rather than charging every control for a copy. */
    orb_glass_cache_prewarm();
    lv_obj_t *root = screen();
    const char *names[] = {"disc80-flat", "disc80-glass", "disc80-tinted", "disc56", "disc40",
        "art180", "primary", "secondary", "glyph", "rim-title", "rim-two-colour", "rim-date",
        "rim-caption", "page-dots5", "ring", "rim-slider", "switch", "row", "row-subtitle",
        "row-switch", "scroll-arc", "rim-title-inset", "paged-scroll-arc"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        lv_obj_t *obj = NULL;
        lv_obj_t *list = NULL;
        if (i >= 17 && i <= 20) {
            list = lv_obj_create(root);
            lv_obj_remove_style_all(list);
            lv_obj_set_size(list, 466, 466);
        }
        size_t before = retained_bytes();
        orb_glass_build_stats_t masks_before, masks_after;
        orb_glass_build_stats(&masks_before);
        switch (i) {
        case 0: case 1: case 2:
            obj = orb_kit_disc_create(root, ORB_KIT_DISC_STANDARD,
                i == 0 ? ORB_KIT_ACCENT_FILL : i == 1 ? ORB_KIT_NEUTRAL_GLASS : ORB_KIT_TINTED_GLASS,
                ORB_KIT_CODEX, ORB_ICON_GEAR); break;
        case 3: obj = orb_kit_disc_create(root, ORB_KIT_DISC_KIND_MINI, ORB_KIT_ACCENT_FILL,
                                         ORB_KIT_CODEX, ORB_ICON_GEAR); break;
        case 4: obj = orb_kit_disc_create(root, ORB_KIT_DISC_KIND_ROW, ORB_KIT_ACCENT_FILL,
                                         ORB_KIT_CODEX, ORB_ICON_GEAR); break;
        case 5: obj = orb_kit_disc_create(root, ORB_KIT_DISC_ART, ORB_KIT_TRACK,
                                         ORB_KIT_CODEX, ORB_ICON_PLAY); break;
        case 6: obj = orb_kit_button_primary_create(root, "Play", ORB_KIT_PLAY); break;
        case 7: obj = orb_kit_button_secondary_create(root, "Done"); break;
        case 8: obj = orb_kit_glyph_button_create(root, ORB_ICON_X, false, ORB_KIT_CODEX); break;
        case 9: obj = orb_kit_rim_title_create(root, "GUEST SUITE BATH", ORB_KIT_HOME); break;
        case 10: obj = orb_kit_rim_title_two_colour_create(root, "GUEST SUITE BATH", ORB_KIT_HOME, " · 3 ON"); break;
        case 11: obj = orb_kit_rim_date_create(root, "WEDNESDAY", "28"); break;
        case 12: obj = orb_kit_rim_caption_create(root, "Hold for scenes"); break;
        case 13: obj = orb_kit_page_dots_create(root, 5); break;
        case 14: obj = orb_kit_ring_create(root, 213, 12, ORB_KIT_TIME); break;
        case 15: obj = orb_kit_rim_slider_create(root, 213, 14, ORB_KIT_HOME, 0, 100, 5, NULL, NULL); break;
        case 16: obj = orb_kit_switch_create(root, true, ORB_KIT_PLAY); break;
        case 17: case 18: case 19:
            obj = orb_kit_row_create(list, &(orb_kit_row_config_t){.title="Sounds", .glyph=ORB_ICON_SPEAKER_HIGH,
                .accent=ORB_KIT_SOUND, .subtitle=i == 18 ? "Connected" : NULL,
                .show_switch=i == 19, .checked=true}); break;
        case 20: obj = orb_kit_scroll_arc_attach(list); break;
        case 21: obj = orb_kit_rim_title_inset_create(root, "GUEST SUITE BATH", ORB_KIT_HOME); break;
        case 22: obj = orb_kit_scroll_arc_create(root, 2, 5); break;
        }
        require(obj != NULL, "memory fixture creation failed");
        lv_obj_update_layout(root);
        size_t after = retained_bytes();
        orb_glass_build_stats(&masks_after);
        int64_t shared_delta = (int64_t)masks_after.bytes - (int64_t)masks_before.bytes;
        int64_t private_delta = (int64_t)after - (int64_t)before - shared_delta;
        printf("MEMORY %s %s private=%lld shared-mask-delta=%lld bytes retained LVGL payload\n",
               s_mode, names[i], (long long)private_delta, (long long)shared_delta);
        if (i >= 17 && i <= 19) {
            lv_obj_set_y(obj, 160);
            require(orb_kit_list_apply(list), "memory row chord preparation failed");
            orb_round_list_cache_stats_t cache;
            require(orb_round_list_cache_stats(list, &cache), "row cache stats unavailable");
            printf("MEMORY %s %s chord-cache: source=%zu tables=%zu metadata=%zu bytes\n",
                   s_mode, names[i], cache.source_bytes, cache.width_table_bytes, cache.metadata_bytes);
        }
        if (list) lv_obj_delete(list);
        else lv_obj_delete(obj);
    }
    orb_glass_build_stats_t stats;
    orb_glass_build_stats(&stats);
    printf("MEMORY %s shared-glass-cache %zu bytes %u entries\n", s_mode, stats.bytes, stats.entries);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUTPUT_DIR default|ax3|ax3-bold-rt\n", argv[0]);
        return EXIT_FAILURE;
    }
    s_output = argv[1]; s_mode = argv[2];
    bool ax = !strcmp(s_mode, "ax3") || !strcmp(s_mode, "ax3-bold-rt");
    bool bold = !strcmp(s_mode, "ax3-bold-rt");
    require(ax || !strcmp(s_mode, "default"), "unknown accessibility variant");
    require(mkdir(s_output, 0755) == 0 || errno == EEXIST, "cannot create output directory");
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    require(s_display != NULL, "display unavailable");
    orb_ui_init(NULL);
    orb_ui_set_reduce_transparency(bold);
    require(orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
            "content size fonts unavailable");
    require(orb_fonts_set_bold_text(bold) == ORB_FONTS_OK, "bold fonts unavailable");
    memory_report(); discs(); buttons(); rim_text(); indicators(); rows(); regressions(); bench();
    orb_glass_build_stats_t glass_stats;
    orb_glass_build_stats(&glass_stats);
    printf("MEMORY %s shared-glass-final %zu bytes, peak=%zu, entries=%u failures=%u\n",
           s_mode, glass_stats.bytes, glass_stats.peak_bytes, glass_stats.entries, glass_stats.failures);
    return EXIT_SUCCESS;
}
