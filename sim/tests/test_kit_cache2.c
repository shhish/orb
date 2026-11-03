/* HIG §7: retained rows keep the complete focus outline and only the current
 * viewport's sources. Source admission is a timer job, never a scroll/draw job. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_kit.h"
#include "sim_display.h"

static unsigned s_checks, s_component_allocations;
static uint32_t s_now;
static lv_display_t *s_display;
static uint8_t s_partial[SIM_D * SIM_D * 3], s_full[SIM_D * SIM_D * 3];
static uint32_t tick(void) { return s_now; }
static void *counted_alloc(size_t size) { s_component_allocations++; return lv_malloc(size); }
static void counted_free(void *pointer) { lv_free(pointer); }
#define CHECK(value) do { s_checks++; if (!(value)) { fprintf(stderr, "kit cache2:%d: %s\n", __LINE__, #value); exit(1); } } while (0)

static void refresh(void)
{
    s_now += 20;
    lv_timer_handler();
    lv_refr_now(s_display);
}

static lv_obj_t *list_create(lv_obj_t *root, int y, int height)
{
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, y);
    lv_obj_set_size(list, SIM_D, height);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    return list;
}

static bool white_near(int x, int y)
{
    for (int py = y - 2; py <= y + 2; py++)
        for (int px = x - 2; px <= x + 2; px++) {
            if (px < 0 || py < 0 || px >= SIM_D || py >= SIM_D) continue;
            const uint8_t *pixel = s_partial + (py * SIM_D + px) * 3;
            if (pixel[0] > 220 && pixel[1] > 220 && pixel[2] > 220) return true;
        }
    return false;
}

static void focus(lv_obj_t *root)
{
    lv_obj_t *list = list_create(root, 0, SIM_D);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Wednesday", .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_TIME});
    lv_obj_set_y(row, 197);
    lv_obj_t *disc = lv_obj_get_child(row, 0);
    orb_kit_disc_set_focus(disc, true);
    CHECK(orb_kit_list_apply(list));
    refresh();
    lv_area_t bounds;
    CHECK(orb_round_list_child_bounds(row, disc, &bounds));
    CHECK(lv_area_get_width(&bounds) == 40 && lv_area_get_height(&bounds) == 40);
    CHECK(lv_obj_get_style_transform_scale_x(disc, 0) == 256);
    sim_display_rgb(s_partial);
    int cx = (bounds.x1 + bounds.x2 + 1) / 2, cy = (bounds.y1 + bounds.y2 + 1) / 2;
    CHECK(white_near(cx - 26, cy));
    CHECK(white_near(cx + 26, cy));
    CHECK(white_near(cx, cy - 26));
    CHECK(white_near(cx, cy + 26));
    orb_round_list_cache_stats_t focused, unfocused;
    CHECK(orb_round_list_cache_stats(list, &focused));
    orb_kit_disc_set_focus(disc, false);
    refresh();
    sim_display_rgb(s_partial);
    CHECK(!white_near(cx - 26, cy) && !white_near(cx + 26, cy));
    CHECK(!white_near(cx, cy - 26) && !white_near(cx, cy + 26));
    CHECK(orb_round_list_cache_stats(list, &unfocused));
    CHECK(unfocused.source_builds == focused.source_builds + 1);
    orb_kit_disc_set_focus(disc, true);
    refresh();
    sim_display_rgb(s_partial);
    CHECK(white_near(cx - 26, cy) && white_near(cx + 26, cy));
    CHECK(white_near(cx, cy - 26) && white_near(cx, cy + 26));
    CHECK(lv_obj_get_width(disc) == 40 && lv_obj_get_height(disc) == 40);
    lv_obj_delete(list);
}

static bool channel_ink(const uint8_t *pixel, unsigned channel)
{
    return pixel[channel] > 45 && pixel[channel] > pixel[(channel + 1) % 3] * 2 &&
        pixel[channel] > pixel[(channel + 2) % 3] * 2;
}

static void lane_pixels(lv_obj_t *root)
{
    lv_obj_t *list = list_create(root, 0, SIM_D);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Reading Room Library", .subtitle = "Last updated yesterday",
        .value = "Disconnected from hub", .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_GREY});
    lv_obj_t *labels[] = {lv_obj_get_child(row, 1), lv_obj_get_child(row, 2), lv_obj_get_child(row, 3)};
    const lv_color_t colors[] = {lv_color_hex(0xff0000), lv_color_hex(0x00ff00), lv_color_hex(0x0000ff)};
    for (unsigned c = 0; c < 3; c++) lv_obj_set_style_text_color(labels[c], colors[c], 0);
    lv_obj_set_y(row, 197);
    CHECK(orb_kit_list_apply(list));
    for (int y = 105; y <= 321; y += 24) {
        lv_obj_set_y(row, y);
        for (unsigned c = 0; c < 3; c++) lv_obj_set_hidden(labels[c], false);
        orb_round_list_update(list);
        refresh();
        sim_display_rgb(s_full);
        for (unsigned own = 0; own < 3; own++) {
            /* Oracle: each lane painted alone, with unchanged native geometry.
             * Whole-row snapshot fragments from neighboring lanes must never
             * add colored pixels beyond that lane's isolated presentation. */
            for (unsigned c = 0; c < 3; c++) lv_obj_set_hidden(labels[c], c != own);
            orb_round_list_update(list);
            refresh();
            sim_display_rgb(s_partial);
            for (unsigned pixel = 0; pixel < SIM_D * SIM_D; pixel++)
                if (channel_ink(s_full + pixel * 3, own))
                    CHECK(channel_ink(s_partial + pixel * 3, own));
        }
    }
    lv_obj_delete(list);
}

static void tiny_ellipsis(lv_obj_t *root)
{
    lv_obj_t *list = list_create(root, 0, SIM_D);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
        .title = "Reading Room & Family Library", .subtitle = "Last updated yesterday",
        .value = "No Response", .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_GREY});
    lv_obj_set_y(row, 197);
    CHECK(orb_kit_list_apply(list));
    lv_obj_t *title = lv_obj_get_child(row, 1), *disc = lv_obj_get_child(row, 0);
    const lv_font_t *font = lv_obj_get_style_text_font(title, 0);
    int ellipsis = lv_font_get_glyph_width(font, 0x2026, 0);
    unsigned hidden = 0, hidden_rows = 0;
    /* Include entry/exit at the bezel. Template-based rails can now fit the
     * whole old central range; hiding there is no longer a valid requirement. */
    for (int y = -ORB_KIT_ROW_H; y <= SIM_D; y += 2) {
        lv_obj_set_y(row, y);
        lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
        refresh();
        lv_area_t title_bounds, disc_bounds;
        if (!orb_round_list_child_bounds(row, title, &title_bounds) ||
            !orb_round_list_child_bounds(row, disc, &disc_bounds)) {
            hidden_rows++;
            continue;
        }
        int width = lv_area_get_width(&title_bounds);
        int terminal = ellipsis * lv_area_get_width(&disc_bounds) / ORB_KIT_DISC_ROW;
        CHECK(width <= 1 || width >= terminal - 2);
        hidden += width <= 1;
    }
    /* Caption2 is the shared source floor, so Default now remains readable
     * through the82% shoulder too. Narrow title lanes hide before painting a
     * partial ellipsis instead of making the complete row disappear. */
    CHECK(hidden > 0 || hidden_rows > 0);
    orb_kit_row_set_value(row, "4");
    lv_obj_t *value = lv_obj_get_child(row, 3);
    lv_obj_set_style_text_color(value, lv_color_hex(0x0000ff), 0);
    lv_obj_set_y(row, 197);
    orb_round_list_update(list);
    refresh();
    lv_area_t value_bounds;
    CHECK(orb_round_list_child_bounds(row, value, &value_bounds));
    CHECK(lv_area_get_width(&value_bounds) > 1);
    /* A trailing "4" is narrower than an ellipsis; at AX3 this long title
     * stacks the value under it on a full-width lane instead. */
    if (lv_obj_get_style_text_align(value, 0) != LV_TEXT_ALIGN_LEFT)
        CHECK(lv_area_get_width(&value_bounds) < lv_font_get_glyph_width(lv_obj_get_style_text_font(value, 0), 0x2026, 0));
    sim_display_rgb(s_partial);
    unsigned ink = 0;
    for (int y = value_bounds.y1; y <= value_bounds.y2; y++)
        for (int x = value_bounds.x1; x <= value_bounds.x2; x++)
            if (x >= 0 && x < SIM_D && y >= 0 && y < SIM_D)
                ink += channel_ink(s_partial + (y * SIM_D + x) * 3, 2);
    CHECK(ink > 0);
    lv_obj_delete(list);
}

static void compressed_rails(lv_obj_t *root)
{
    const char *values[] = {"Weekdays", "No Response", "Last known", "02:59:59.99"};
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        lv_obj_t *list = list_create(root, 0, SIM_D);
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){
            .title = "Reading Room & Family Library", .subtitle = "Last updated yesterday",
            .value = values[i], .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_TIME});
        lv_obj_set_y(row, 197);
        CHECK(orb_kit_list_apply(list));
        for (int y = 73; y <= 321; y += 8) {
            unsigned allocations = s_component_allocations;
            lv_obj_set_y(row, y);
            lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
            CHECK(s_component_allocations == allocations);
            refresh();
            lv_area_t disc, title, subtitle, value, row_bounds;
            if (!orb_round_list_row_bounds(row, &row_bounds)) continue;
            CHECK(orb_round_list_child_bounds(row, lv_obj_get_child(row, 0), &disc));
            CHECK(orb_round_list_child_bounds(row, lv_obj_get_child(row, 1), &title));
            CHECK(orb_round_list_child_bounds(row, lv_obj_get_child(row, 2), &subtitle));
            CHECK(orb_round_list_child_bounds(row, lv_obj_get_child(row, 3), &value));
            CHECK(disc.x2 < value.x1);
            bool stacked = lv_obj_get_style_text_align(lv_obj_get_child(row, 3), 0) == LV_TEXT_ALIGN_LEFT;
            /* Inter line boxes include unused leading; stacked lanes follow
             * their ink rather than requiring disjoint font rectangles. */
            if (lv_area_get_width(&title) > 1) CHECK(stacked ? title.y1 < value.y1 : title.x2 < value.x1);
            if (lv_area_get_width(&subtitle) > 1) CHECK(stacked ? subtitle.y1 < value.y1 : subtitle.x2 < value.x1);
            CHECK(value.x2 <= row_bounds.x2);
            sim_display_rgb(s_partial);
            lv_obj_invalidate(root);
            lv_refr_now(s_display);
            sim_display_rgb(s_full);
            CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
        }
        lv_obj_delete(list);
    }
}

static void visible_sources(lv_obj_t *root)
{
    lv_obj_t *list = list_create(root, 80, 320);
    lv_obj_t *rows[16];
    for (unsigned i = 0; i < 16; i++) {
        char title[40], subtitle[40], value[24];
        snprintf(title, sizeof(title), "Lap %02u", i + 1);
        snprintf(subtitle, sizeof(subtitle), "Split 00:%02u.42", i + 12);
        snprintf(value, sizeof(value), "12:%02u.93", i + 10);
        rows[i] = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = title,
            .subtitle = subtitle, .value = value, .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_TIME});
        lv_obj_set_y(rows[i], 8 + i * 82);
    }
    CHECK(orb_kit_list_apply(list));
    refresh();
    orb_round_list_cache_stats_t initial, before, after;
    CHECK(orb_round_list_cache_stats(list, &initial));
    CHECK(initial.source_builds <= 5 && initial.source_builds > 0);
    /* Preserve the source metadata ceiling. Optional worker poses have their
     * own globally enforced PSRAM cap and are reported separately. */
    CHECK(initial.metadata_bytes < 16000);
    CHECK(initial.draw_pose_bytes > 0 && initial.draw_pose_bytes <= ORB_ROUND_LIST_DRAW_POSE_BUDGET);
    size_t peak_metadata = initial.metadata_bytes, peak_pose = initial.draw_pose_bytes;
    int limit = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    for (int y = 0; y <= limit; y += 54) {
        CHECK(orb_round_list_cache_stats(list, &before));
        unsigned allocations = s_component_allocations;
        CHECK(orb_round_list_scroll_to(list, y));
        CHECK(s_component_allocations == allocations);
        CHECK(orb_round_list_cache_stats(list, &after));
        CHECK(after.source_builds == before.source_builds);
        CHECK(after.draw_pose_bytes == before.draw_pose_bytes);
        refresh(); /* Admission happens here, before the display refresh. */
        CHECK(orb_round_list_cache_stats(list, &after));
        /* Departed rows within one viewport stay retained under their own
         * bounded budget; on-screen metadata keeps its original ceiling. */
        CHECK(after.retained_bytes <= ORB_ROUND_LIST_LOOKAHEAD_BUDGET + 16000);
        CHECK(after.metadata_bytes < 16000 + after.retained_bytes);
        CHECK(after.draw_pose_bytes <= ORB_ROUND_LIST_DRAW_POSE_BUDGET);
        peak_metadata = LV_MAX(peak_metadata, after.metadata_bytes);
        peak_pose = LV_MAX(peak_pose, after.draw_pose_bytes);
        for (unsigned i = 0; i < 16; i++) {
            lv_area_t logical; lv_obj_get_coords(rows[i], &logical);
            int centre = (logical.y1 + logical.y2 + 1) / 2;
            if (centre < 170 || centre > 290) continue;
            lv_area_t bounds;
            CHECK(orb_round_list_row_bounds(rows[i], &bounds));
            CHECK(bounds.y2 >= 80 && bounds.y1 <= 399);
        }
        sim_display_rgb(s_partial);
        lv_obj_invalidate(root);
        lv_refr_now(s_display);
        sim_display_rgb(s_full);
        CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
    }
    orb_round_list_scroll_finish(list);
    refresh();
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds > initial.source_builds);
    printf("kit cache2 mode=%d bold=%d initial source=%zu metadata=%zu poses=%zu builds=%u final source=%zu metadata=%zu poses=%zu builds=%u peak metadata=%zu poses=%zu\n",
        orb_fonts_content_size(), orb_fonts_bold_text(), initial.source_bytes, initial.metadata_bytes,
        initial.draw_pose_bytes, (unsigned)initial.source_builds, after.source_bytes, after.metadata_bytes,
        after.draw_pose_bytes, (unsigned)after.source_builds, peak_metadata, peak_pose);
    lv_obj_delete(list);
}

/* HIG §6: rows near the viewport are captured only while the display idles,
 * then a throw into them admits nothing and paints exactly like a fresh list. */
static void lookahead_sources(lv_obj_t *root)
{
    lv_obj_t *list = list_create(root, 80, 320);
    for (unsigned i = 0; i < 16; i++) {
        char title[40], subtitle[40], value[24];
        snprintf(title, sizeof(title), "Session %02u", i + 1);
        snprintf(subtitle, sizeof(subtitle), "Reviewing change %u", i + 40);
        snprintf(value, sizeof(value), "%u min", i + 2);
        lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){.title = title,
            .subtitle = subtitle, .value = value, .glyph = ORB_ICON_GEAR, .accent = ORB_KIT_DOT});
        lv_obj_set_y(row, 8 + i * 82);
    }
    CHECK(orb_kit_list_apply(list));
    refresh();
    orb_round_list_cache_stats_t initial, idle, before, after;
    CHECK(orb_round_list_cache_stats(list, &initial));
    CHECK(initial.lookahead_builds == 0);
    /* One capture per slice, only after the display has rendered nothing. */
    unsigned previous = 0;
    for (unsigned i = 0; i < 40; i++) {
        s_now += 20;
        lv_timer_handler();
        CHECK(orb_round_list_cache_stats(list, &idle));
        CHECK(idle.lookahead_builds <= previous + 1);
        previous = idle.lookahead_builds;
    }
    CHECK(idle.lookahead_builds > 0 && idle.source_builds == initial.source_builds);
    /* A settled window stops: further idle slices neither capture nor churn. */
    for (unsigned i = 0; i < 40; i++) { s_now += 20; lv_timer_handler(); }
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.lookahead_builds == idle.lookahead_builds);
    CHECK(idle.retained_bytes > 0 && idle.retained_bytes <= ORB_ROUND_LIST_LOOKAHEAD_BUDGET + 16000);
    CHECK(idle.draw_pose_bytes == initial.draw_pose_bytes);
    /* A two-row throw enters rows that were prepared while idle. */
    CHECK(orb_round_list_cache_stats(list, &before));
    unsigned allocations = s_component_allocations;
    CHECK(orb_round_list_scroll_to(list, 164));
    CHECK(s_component_allocations == allocations);
    refresh();
    orb_round_list_scroll_finish(list);
    refresh();
    /* AX rows have variable heights: releasing at 164 px now snaps forward
     * to a row boundary. Reapply is an idle operation, after that native
     * animation ends, rather than a cache rebuild during the throw. */
    for (unsigned frame = 0; frame < 32 && lv_obj_is_scrolling(list); frame++) refresh();
    CHECK(!lv_obj_is_scrolling(list));
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == before.source_builds);
    lv_obj_invalidate(root);
    lv_refr_now(s_display);
    sim_display_rgb(s_partial);
    /* Reapply discards every retained source and captures the viewport anew. */
    CHECK(orb_kit_list_apply(list));
    refresh();
    lv_obj_invalidate(root);
    lv_refr_now(s_display);
    sim_display_rgb(s_full);
    CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
    lv_obj_delete(list);
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(&(orb_ui_config_t){.alloc = counted_alloc, .free = counted_free});
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_scrollable(root, false);
    for (unsigned mode = 0; mode < 3; mode++) {
        CHECK(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(mode == 2) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(mode == 2);
        focus(root);
        lane_pixels(root);
        tiny_ellipsis(root);
        compressed_rails(root);
        visible_sources(root);
        lookahead_sources(root);
    }
    printf("kit cache2: %u checks passed\n", s_checks);
    return 0;
}
