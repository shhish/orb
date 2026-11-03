/* HIG §§3/7 and ROUND-FIRST §1: retained capsules follow their current chord,
 * including the pointer target and trailing accessory's spatial anchor. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_ui.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "sim_display.h"

static unsigned s_checks;
static unsigned s_allocations;
static uint32_t s_now;
static lv_display_t *s_display;
static lv_obj_t *s_saved_screen;
static uint8_t s_partial[466 * 466 * 3], s_full[466 * 466 * 3];

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "chord list: %s:%d: %s\n", __FILE__, __LINE__, #value); \
    exit(1); \
} } while (0)

static uint32_t tick(void) { return s_now; }
static void *counted_alloc(size_t size) { ++s_allocations; return lv_malloc(size); }
static void counted_free(void *ptr) { lv_free(ptr); }

static void refresh(void)
{
    s_now += 20;
    lv_timer_handler();
    lv_refr_now(s_display);
}

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static lv_obj_t *caption(lv_obj_t *row, const char *text, int x, int width)
{
    lv_obj_t *label = lv_label_create(row);
    orb_text_apply(label, ORB_TEXT_CAPTION2, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(label, ORB_LABEL, 0);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, lv_obj_get_style_text_font(label, 0)->line_height);
    lv_obj_set_x(label, x);
    lv_obj_set_y(label, (72 - lv_obj_get_style_text_font(label, 0)->line_height) / 2);
    return label;
}

static void consistent_pixels(lv_obj_t *screen)
{
    refresh();
    sim_display_rgb(s_partial);
    lv_obj_invalidate(screen);
    lv_refr_now(s_display);
    sim_display_rgb(s_full);
    CHECK(!memcmp(s_partial, s_full, sizeof(s_full)));
}

static void snapshot_formats(lv_obj_t *list)
{
    const lv_color_format_t formats[] = {LV_COLOR_FORMAT_ARGB8888, LV_COLOR_FORMAT_XRGB8888};
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        lv_draw_buf_t *snapshot = lv_snapshot_take(list, formats[i]);
        CHECK(snapshot && snapshot->header.cf == formats[i]);
        const lv_color32_t *edge = (const lv_color32_t *)(snapshot->data + 233 * snapshot->header.stride) + 35;
        CHECK(edge->red || edge->green || edge->blue);
        unsigned ink = 0;
        for (unsigned y = 210; y < 255; ++y) {
            const lv_color32_t *line = (const lv_color32_t *)(snapshot->data + y * snapshot->header.stride);
            for (unsigned x = 40; x < 430; ++x)
                if (line[x].red > 100 && line[x].green > 100 && line[x].blue > 100) ++ink;
        }
        CHECK(ink > 20); /* The matte capsule and its label survive alpha snapshots. */
        lv_draw_buf_destroy(snapshot);
    }
}

static lv_color32_t snapshot_pixel(const lv_draw_buf_t *image, int x, int y)
{
    return ((const lv_color32_t *)(image->data + y * image->header.stride))[x];
}

/* The unchanged ARGB compositor is the scalar oracle. Exercise both the
 * native RGB565 snapshot path and the swapped565 real display path against
 * it, including byte order, sparse gaps, opacity and glyph ellipses. */
static void native_pixel_oracle(lv_obj_t *screen)
{
    refresh();
    sim_display_rgb(s_partial);
    lv_draw_buf_t *reference = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    lv_draw_buf_t *native = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    CHECK(reference && native);
    CHECK(reference->header.w == 466 && reference->header.h == 466);
    for (int y = 0; y < 466; ++y) {
        const uint16_t *line = (const uint16_t *)(native->data + y * native->header.stride);
        for (int x = 0; x < 466; ++x) {
            lv_color32_t pixel = snapshot_pixel(reference, x, y);
            uint16_t expected = lv_color_to_u16(lv_color_make(pixel.red, pixel.green, pixel.blue));
            CHECK(line[x] == expected);
            /* Exclude only the simulator's physical bezel coverage ramp. */
            int dx = x - 233, dy = y - 233;
            if (dx * dx + dy * dy < 220 * 220) {
                const uint8_t *shown = s_partial + ((size_t)y * 466 + x) * 3;
                CHECK(shown[0] == ((expected >> 11) << 3 | (expected >> 11) >> 2));
                CHECK(shown[1] == (((expected >> 5) & 63) << 2 | ((expected >> 5) & 63) >> 4));
                CHECK(shown[2] == ((expected & 31) << 3 | (expected & 31) >> 2));
            }
        }
    }
    lv_draw_buf_destroy(native);
    lv_draw_buf_destroy(reference);
}

static void press_and_ancestor_opacity(lv_obj_t *screen, lv_obj_t *list, lv_obj_t *row, lv_obj_t *title)
{
    lv_draw_buf_t *normal = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    CHECK(normal);
    lv_area_t title_bounds;
    CHECK(orb_round_list_child_bounds(row, title, &title_bounds));
    lv_color32_t normal_bg = snapshot_pixel(normal, 233, 205);
    int empty_x = -1, empty_y = -1;
    for (int y = title_bounds.y1; y <= title_bounds.y2 && empty_x < 0; ++y)
        for (int x = title_bounds.x1; x <= title_bounds.x2; ++x) {
            lv_color32_t pixel = snapshot_pixel(normal, x, y);
            if (pixel.red == normal_bg.red && pixel.green == normal_bg.green && pixel.blue == normal_bg.blue) {
                empty_x = x; empty_y = y; break;
            }
        }
    CHECK(empty_x >= 0);
    lv_obj_add_state(row, LV_STATE_PRESSED);
    refresh();
    native_pixel_oracle(screen); /* Changed live matte and effective opacity invalidate keyed tables. */
    lv_draw_buf_t *held = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    CHECK(held);
    lv_color32_t held_bg = snapshot_pixel(held, 233, 205);
    lv_color32_t label_bg = snapshot_pixel(held, empty_x, empty_y);
    CHECK(held_bg.red != normal_bg.red || held_bg.green != normal_bg.green || held_bg.blue != normal_bg.blue);
    CHECK(label_bg.red == held_bg.red && label_bg.green == held_bg.green && label_bg.blue == held_bg.blue);
    lv_draw_buf_destroy(held);
    lv_draw_buf_destroy(normal);
    lv_obj_remove_state(row, LV_STATE_PRESSED);
    for (unsigned layered = 0; layered < 2; ++layered) {
        if (layered) lv_obj_set_style_opa_layered(list, 128, 0);
        else lv_obj_set_style_opa(list, 128, 0);
        refresh();
        lv_draw_buf_t *dimmed = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
        CHECK(dimmed);
        lv_color32_t dimmed_bg = snapshot_pixel(dimmed, 233, 205);
        CHECK(dimmed_bg.red > 0 && dimmed_bg.green > 0 && dimmed_bg.blue > 0);
        CHECK(dimmed_bg.red < normal_bg.red && dimmed_bg.green < normal_bg.green && dimmed_bg.blue < normal_bg.blue);
        lv_draw_buf_destroy(dimmed);
        if (layered) lv_obj_set_style_opa_layered(list, LV_OPA_COVER, 0);
        else lv_obj_set_style_opa(list, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_opa(list, LV_OPA_TRANSP, 0);
    refresh();
    lv_draw_buf_t *hidden = lv_snapshot_take(screen, LV_COLOR_FORMAT_ARGB8888);
    CHECK(hidden);
    lv_color32_t hidden_pixel = snapshot_pixel(hidden, 233, 233);
    CHECK(!hidden_pixel.red && !hidden_pixel.green && !hidden_pixel.blue);
    lv_draw_buf_destroy(hidden);
    lv_obj_set_style_opa(list, LV_OPA_COVER, 0);
    refresh();
}

static void translated_viewport(lv_obj_t *screen, lv_obj_t *list, lv_obj_t *row,
                                lv_obj_t *title, lv_obj_t *value,
                                const orb_round_list_cache_stats_t *before)
{
    const lv_point_t moves[] = {{480, 0}, {0, 0}, {32, -52}, {-24, 24}, {0, 0}};
    unsigned allocations = s_allocations;
    for (unsigned i = 0; i < sizeof(moves) / sizeof(moves[0]); ++i) {
        lv_obj_set_pos(list, moves[i].x, moves[i].y);
        refresh(); /* Ancestor translation emits no list SCROLL event. */
        lv_area_t bounds, leading, trailing;
        CHECK(orb_round_list_row_bounds(row, &bounds));
        int center_x = 233 + moves[i].x, center_y = 233 + moves[i].y;
        CHECK(abs(bounds.x1 + bounds.x2 - (center_x * 2 - 1)) <= 1);
        CHECK(abs(bounds.y1 + bounds.y2 - (center_y * 2 - 1)) <= 1);
        if (moves[i].x == 480) {
            lv_point_t offscreen = {center_x, center_y};
            CHECK(!orb_round_list_hit_test(row, &offscreen));
            CHECK(!orb_round_list_child_bounds(row, value, &trailing));
        } else {
            double dy = abs(center_y - 233);
            int exact = 72 + (int)(2 * sqrt(173 * 173 - dy * dy));
            int width = lv_area_get_width(&bounds);
            CHECK(width <= exact && exact - width <= 16);
            CHECK(orb_round_list_child_bounds(row, title, &leading));
            CHECK(orb_round_list_child_bounds(row, value, &trailing));
            CHECK(abs(leading.x1 - bounds.x1 - 16) <= 1);
            CHECK(abs(bounds.x2 - trailing.x2 - 16) <= 1);
            lv_point_t centre = {center_x, center_y};
            CHECK(orb_round_list_hit_test(row, &centre));
            if (moves[i].x == 32) {
                lv_point_t previous_left_edge = {25, center_y};
                CHECK(!orb_round_list_hit_test(row, &previous_left_edge));
            }
        }
        consistent_pixels(screen);
        orb_round_list_cache_stats_t after;
        CHECK(orb_round_list_cache_stats(list, &after));
        CHECK(after.source_builds == before->source_builds);
        CHECK(after.source_bytes == before->source_bytes);
        CHECK(after.width_table_bytes == before->width_table_bytes);
        CHECK(s_allocations == allocations);
    }
}

static void chord_geometry(bool ax)
{
    CHECK(orb_fonts_set_content_size(ax ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    orb_fonts_set_bold_text(ax);
    orb_ui_set_reduce_transparency(ax);
    lv_obj_t *screen = plain(NULL, 0, 0, 466, 466);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *list = plain(screen, 0, 0, 466, 466);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *row = NULL, *title = NULL, *value = NULL;
    for (unsigned i = 0; i < 7; ++i) {
        lv_obj_t *r = plain(list, 24, 197 + (int)i * 96, 418, 72);
        lv_obj_set_style_bg_color(r, ORB_GRAY6, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 36, 0);
        lv_obj_set_style_bg_color(r, ORB_GRAY4, LV_STATE_PRESSED);
        lv_obj_set_style_opa(r, 179, LV_STATE_PRESSED);
        lv_obj_set_clickable(r, true);
        lv_obj_t *t = caption(r, "Title gets the wider middle band before truncating", 16, 300);
        lv_obj_t *v = caption(r, "On", 342, 60);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -16, 0);
        if (!i) { row = r; title = t; value = v; }
    }
    lv_screen_load(screen);
    CHECK(orb_round_list_apply(list, &(orb_round_list_config_t){
        .margin = 24, .fade = 38, .stepped = true, .chord_fit = true,
    }));
    refresh();
    orb_round_list_cache_stats_t before;
    orb_round_list_cache_stats(list, &before);
    CHECK(before.width_table_bytes > 0 && before.width_table_bytes < 1024);
    CHECK(before.source_bytes > 0 && before.source_builds > 0);
    CHECK(before.source_bytes < 7u * 418 * 72); /* Less than half of matte RGB565 rows. */
    snapshot_formats(list);
    press_and_ancestor_opacity(screen, list, row, title);
    native_pixel_oracle(screen);
    CHECK(orb_round_list_cache_stats(list, &before));
    unsigned prepared_allocations = s_allocations;
    translated_viewport(screen, list, row, title, value, &before);

    lv_area_t logical;
    lv_obj_get_coords(row, &logical);
    const int original_width = lv_area_get_width(&logical);
    const unsigned offsets[] = {0, 24, 52, 88, 120, 160, 120, 52, 0};
    int previous_width = 1000;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        CHECK(orb_round_list_scroll_to(list, (int32_t)offsets[i]));
        lv_area_t bounds, trailing, leading;
        CHECK(orb_round_list_row_bounds(row, &bounds));
        CHECK(orb_round_list_child_bounds(row, value, &trailing));
        CHECK(orb_round_list_child_bounds(row, title, &leading));
        int center = 233 - (int)offsets[i];
        int scale = center < 140 ? 211 : 256;
        int width = lv_area_get_width(&bounds);
        int native = width * 256 / scale;
        double cap_radius = 209 - 36;
        double dy = abs(center - 233);
        int exact = 72 + (int)(2 * sqrt(cap_radius * cap_radius - dy * dy));
        CHECK(native <= exact + 2);
        CHECK(exact - native <= 28); /* Conservative 4px position / 8px width steps. */
        CHECK(abs(bounds.x1 + bounds.x2 - 465) <= 1);
        CHECK(abs(bounds.y1 + bounds.y2 - (center * 2 - 1)) <= 1);
        CHECK(abs(bounds.x2 - trailing.x2 - 16 * scale / 256) <= 2);
        CHECK(abs(leading.x1 - bounds.x1 - 16 * scale / 256) <= 2);
        if (i < 6) { CHECK(width <= previous_width); previous_width = width; }
        if (!offsets[i]) {
            CHECK(width >= 410 && width <= 418);
            lv_point_t widened_edge = {bounds.x1 + 2, center};
            CHECK(widened_edge.x < (466 - 340) / 2);
            CHECK(orb_round_list_hit_test(row, &widened_edge));
        }
        lv_point_t edge = {bounds.x2 - 1, center};
        CHECK(orb_round_list_hit_test(row, &edge));
        edge.x = bounds.x2 + 1;
        CHECK(!orb_round_list_hit_test(row, &edge));
        lv_obj_get_coords(row, &logical);
        CHECK(lv_area_get_width(&logical) == original_width);
        consistent_pixels(screen);
        native_pixel_oracle(screen);
        size_t inside = ((size_t)center * 466 + bounds.x1 + 2) * 3;
        CHECK(s_full[inside] || s_full[inside + 1] || s_full[inside + 2]);
        size_t outside = ((size_t)center * 466 + bounds.x1 - 2) * 3;
        CHECK(!s_full[outside] && !s_full[outside + 1] && !s_full[outside + 2]);
        orb_round_list_cache_stats_t after;
        orb_round_list_cache_stats(list, &after);
        CHECK(after.width_table_bytes == before.width_table_bytes);
        CHECK(after.source_bytes == before.source_bytes);
        CHECK(after.source_builds == before.source_builds);
        CHECK(s_allocations == prepared_allocations);
    }
    orb_round_list_scroll_finish(list);
    /* The last input direction may settle; it must still reuse its sources. */
    refresh();
    orb_round_list_cache_stats_t after;
    orb_round_list_cache_stats(list, &after);
    CHECK(after.source_builds == before.source_builds);
    /* A changed row height/text during rim ownership must keep the old
     * source and shape, then admit exactly one replacement after settling.
     * This catches table allocation from nested LAYOUT_CHANGED/SCROLL and
     * anchor drift while an aligned accessory's logical y has changed. */
    CHECK(orb_round_list_scroll_to(list, 48));
    unsigned motion_allocations = s_allocations;
    lv_area_t old_trailing, changed_trailing;
    CHECK(orb_round_list_child_bounds(row, value, &old_trailing));
    lv_label_set_text(title, "Model update received during a fling");
    lv_obj_set_height(row, 96);
    orb_round_list_update(list);
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == before.source_builds);
    CHECK(after.width_table_bytes == before.width_table_bytes);
    CHECK(after.source_bytes == before.source_bytes);
    CHECK(s_allocations == motion_allocations);
    CHECK(orb_round_list_child_bounds(row, value, &changed_trailing));
    CHECK(changed_trailing.y1 == old_trailing.y1 + 12);
    CHECK(lv_area_get_height(&changed_trailing) == lv_area_get_height(&old_trailing));
    consistent_pixels(screen);
    CHECK(s_allocations == motion_allocations);
    lv_obj_set_height(row, 72);
    orb_round_list_update(list);
    CHECK(orb_round_list_child_bounds(row, value, &changed_trailing));
    CHECK(changed_trailing.y1 == old_trailing.y1);
    CHECK(orb_round_list_scroll_to(list, 0));
    orb_round_list_scroll_finish(list);
    refresh();
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == before.source_builds + 1);
    before.source_builds = after.source_builds;
    /* The public update contract still refreshes explicit content edits.
     * Admission happens here, outside the scroll/draw workload above. */
    lv_label_set_text(title, "Changed retained title");
    orb_round_list_update(list);
    refresh();
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == before.source_builds + 1);
    lv_obj_delete(value);
    orb_round_list_update(list);
    refresh();
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == before.source_builds + 2);
    CHECK(after.width_table_bytes == before.width_table_bytes);
    lv_obj_t *added = caption(row, "Added", 342, 60);
    lv_obj_align(added, LV_ALIGN_RIGHT_MID, -16, 0);
    orb_round_list_update(list);
    refresh();
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == before.source_builds + 3);
    lv_area_t added_bounds, current_bounds;
    CHECK(orb_round_list_child_bounds(row, added, &added_bounds));
    CHECK(orb_round_list_row_bounds(row, &current_bounds));
    CHECK(abs(current_bounds.x2 - added_bounds.x2 - 16) <= 1);
    printf("chord list %s: tables=%u source=%u builds=%u\n", ax ? "ax3" : "default",
           (unsigned)before.width_table_bytes, (unsigned)before.source_bytes, (unsigned)before.source_builds);
    lv_screen_load(s_saved_screen);
    lv_obj_delete(screen);
}

static void legacy_geometry(void)
{
    lv_obj_t *screen = plain(NULL, 0, 0, 466, 466);
    lv_obj_t *list = plain(screen, 0, 0, 466, 466);
    lv_obj_t *row = plain(list, 63, 197, 340, 72);
    caption(row, "Legacy fixed width", 16, 308);
    lv_screen_load(screen);
    CHECK(orb_round_list_apply(list, NULL));
    refresh();
    lv_area_t bounds;
    CHECK(orb_round_list_child_bounds(row, lv_obj_get_child(row, 0), &bounds));
    CHECK(lv_area_get_width(&bounds) == 308);
    lv_point_t beyond_old_edge = {35, 233};
    CHECK(!orb_round_list_hit_test(row, &beyond_old_edge));
    lv_screen_load(s_saved_screen);
    lv_obj_delete(screen);
}

static bool s_held_pointer;
static void held_pointer_read(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = (lv_point_t){233, 233};
    data->state = s_held_pointer ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void source_formats(bool dense)
{
    /* More than 256 colours exercises the sparse RGB565 fallback; filling
     * the complete source exercises the uncompressed dense fallback too. */
    static uint16_t pixels[418 * 72];
    int width = dense ? 418 : 320, height = dense ? 72 : 36;
    for (int i = 0; i < width * height; i++) pixels[i] = (uint16_t)(i * 17 + 1234);
    lv_image_dsc_t image = {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = width, .h = height, .stride = width * 2},
        .data_size = width * height * 2, .data = (const uint8_t *)pixels,
    };
    lv_obj_t *screen = plain(NULL, 0, 0, 466, 466);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *list = plain(screen, 0, 0, 466, 466);
    lv_obj_t *row = plain(list, 24, 197, 418, 72);
    lv_obj_set_style_bg_color(row, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 36, 0);
    lv_obj_t *art = lv_image_create(row);
    lv_image_set_src(art, &image);
    lv_obj_set_pos(art, dense ? 0 : 16, dense ? 0 : 18);
    lv_screen_load(screen);
    refresh(); sim_display_rgb(s_partial);
    /* Preparing a new navigation destination is explicit caller work, even
     * while a pointer is still held on the page it is leaving. */
    lv_indev_t *input = lv_indev_create(); CHECK(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, s_display);
    lv_indev_set_read_cb(input, held_pointer_read);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    s_held_pointer = true; lv_indev_read(input);
    CHECK(orb_round_list_apply(list, &(orb_round_list_config_t){
        .margin = 24, .fade = 38, .stepped = true, .chord_fit = true,
    }));
    s_held_pointer = false; lv_indev_read(input); lv_indev_delete(input);
    refresh(); sim_display_rgb(s_full);
    for (int y = 220; y < 245; y++)
        CHECK(!memcmp(s_partial + ((size_t)y * 466 + 130) * 3,
                      s_full + ((size_t)y * 466 + 130) * 3, 150 * 3));
    orb_round_list_cache_stats_t stats;
    CHECK(orb_round_list_cache_stats(list, &stats));
    if (dense) CHECK(stats.source_bytes == 418u * 72 * 2);
    else CHECK(stats.source_bytes < 418u * 72 * 2);
    native_pixel_oracle(screen);
    lv_obj_set_style_opa(row, 137, 0);
    native_pixel_oracle(screen);
    lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
    unsigned allocations = s_allocations;
    consistent_pixels(screen);
    CHECK(s_allocations == allocations);
    lv_screen_load(s_saved_screen); lv_obj_delete(screen);
}

/* A late copy lane must skip hundreds of tiny earlier runs without losing
 * its first occupied run. ARGB deliberately uses the unindexed scalar path,
 * providing an independent pixel oracle for both palette and raw RGB565 runs. */
static void fragmented_sources(bool indexed)
{
    static uint16_t early_pixels[306 * 58], late_pixels[72 * 40];
    uint16_t matte = lv_color_to_u16(ORB_GRAY6);
    for (unsigned i = 0; i < sizeof(early_pixels) / sizeof(early_pixels[0]); ++i)
        early_pixels[i] = i % 3 ? matte : indexed ? (uint16_t)(0xf800u + i % 8) : (uint16_t)(i * 17 + 1234);
    for (unsigned i = 0; i < sizeof(late_pixels) / sizeof(late_pixels[0]); ++i)
        late_pixels[i] = i % 5 ? indexed ? (uint16_t)(0x07e0u + i % 8) : (uint16_t)(i * 31 + 4321) : matte;
    lv_image_dsc_t images[] = {
        {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = 306, .h = 58, .stride = 306 * 2},
         .data_size = sizeof(early_pixels), .data = (const uint8_t *)early_pixels},
        {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                    .w = 72, .h = 40, .stride = 72 * 2},
         .data_size = sizeof(late_pixels), .data = (const uint8_t *)late_pixels},
    };
    lv_obj_t *screen = plain(NULL, 0, 0, 466, 466);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t *list = plain(screen, 0, 0, 466, 466);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *row = plain(list, 24, 197, 418, 72);
    lv_obj_set_style_bg_color(row, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 36, 0);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *image = lv_image_create(row);
        lv_image_set_src(image, &images[i]);
        lv_obj_set_pos(image, i ? 330 : 8, i ? 17 : 7);
    }
    plain(list, 24, 197 + 4 * 96, 418, 72);
    lv_screen_load(screen);
    CHECK(orb_round_list_apply(list, &(orb_round_list_config_t){
        .margin = 24, .fade = 38, .stepped = true, .chord_fit = true,
    }));
    refresh();
    orb_round_list_cache_stats_t before;
    CHECK(orb_round_list_cache_stats(list, &before));
    CHECK(before.source_bytes < 418u * 72 * 2);
    const int offsets[] = {0, 52, 120, 160};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        CHECK(orb_round_list_scroll_to(list, offsets[i]));
        refresh();
        unsigned allocations = s_allocations;
        native_pixel_oracle(screen);
        lv_obj_set_style_opa(row, 137, 0);
        native_pixel_oracle(screen);
        lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
        consistent_pixels(screen);
        CHECK(s_allocations == allocations);
        orb_round_list_cache_stats_t after;
        CHECK(orb_round_list_cache_stats(list, &after));
        CHECK(after.source_builds == before.source_builds);
        CHECK(after.source_bytes == before.source_bytes);
        CHECK(after.metadata_bytes == before.metadata_bytes);
    }
    lv_screen_load(s_saved_screen);
    lv_obj_delete(screen);
}

static lv_obj_t *managed_list(lv_obj_t *obj)
{
    orb_round_list_cache_stats_t stats;
    if (orb_round_list_cache_stats(obj, &stats)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = managed_list(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}

static void maximum_sessions(void)
{
    /* Real shell renderer, maximum-sized synthetic model; no live data. */
    CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_reset(); orb_model_set_connection(true, true);
    orb_session_t sessions[ORB_MODEL_MAX_SESSIONS] = {0};
    for (unsigned i = 0; i < ORB_MODEL_MAX_SESSIONS; i++) {
        snprintf(sessions[i].id, sizeof(sessions[i].id), "max-session-%u", i);
        snprintf(sessions[i].title, sizeof(sessions[i].title), "Firmware review project %02u", i);
        snprintf(sessions[i].project, sizeof(sessions[i].project), "Round shell source memory");
        snprintf(sessions[i].reason, sizeof(sessions[i].reason), "Reviewing cached presentation");
        sessions[i].agent = i & 1 ? ORB_AGENT_CODEX : ORB_AGENT_CLAUDE;
        sessions[i].state = ORB_SESSION_BUSY;
        sessions[i].updated = 1000 + i;
    }
    CHECK(orb_model_sessions_chunk(sessions, ORB_MODEL_MAX_SESSIONS, true, false));
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true);
    lv_obj_t *screen = plain(NULL, 0, 0, 466, 466);
    lv_screen_load(screen);
    orb_shell_t *shell = orb_shell_create(screen, NULL, NULL); CHECK(shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .clock_time = "10:09", .hub_name = "Synthetic Mac"};
    orb_shell_update(shell, &status); orb_shell_navigate(shell, ORB_SHELL_SESSIONS);
    for (unsigned i = 0; i < 70; i++) { orb_shell_poll(shell); refresh(); }
    CHECK(orb_shell_surface(shell) == ORB_SHELL_SESSIONS);
    lv_obj_t *list = managed_list(orb_shell_root(shell)); CHECK(list);
    orb_round_list_cache_stats_t before, after;
    CHECK(orb_round_list_cache_stats(list, &before));
    CHECK(lv_obj_get_child_count(list) == ORB_MODEL_MAX_SESSIONS + 1);
    /* One 120-byte chord table per distinct row height: AX3 rows that wrap
     * their titles grow, so Sessions and its Usage route may differ. */
    int32_t heights[4] = {0};
    unsigned distinct = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        if (lv_obj_is_floating(row)) continue;
        bool seen = false;
        for (unsigned j = 0; j < distinct; j++) seen |= heights[j] == lv_obj_get_height(row);
        if (!seen && distinct < 4) heights[distinct++] = lv_obj_get_height(row);
    }
    CHECK(distinct >= 1 && distinct <= 2);
    CHECK(before.width_table_bytes == 120 * distinct);
    size_t native_bytes = 0, scratch_bytes = 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        size_t bytes = (size_t)lv_obj_get_width(row) * lv_obj_get_height(row) * 2;
        native_bytes += bytes;
        if (bytes > scratch_bytes) scratch_bytes = bytes;
    }
    CHECK(before.source_bytes < native_bytes / 2);
    /* Sessions includes its visible Usage route after the session rows. */
    lv_obj_t *usage_row = lv_obj_get_child(list, ORB_MODEL_MAX_SESSIONS);
    CHECK(lv_obj_check_type(lv_obj_get_child(usage_row, 1), &lv_label_class));
    CHECK(!strcmp(lv_label_get_text(lv_obj_get_child(usage_row, 1)), "Usage"));
    int limit = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list);
    for (int y = 0; y <= limit; y += 120) {
        orb_round_list_cache_stats_t previous;
        CHECK(orb_round_list_cache_stats(list, &previous));
        unsigned allocations = s_allocations;
        CHECK(orb_round_list_scroll_to(list, y));
        CHECK(orb_round_list_cache_stats(list, &after));
        /* K5 admits entering rows on its preparation timer. The synchronous
         * scroll path still cannot build sources or allocate component data. */
        CHECK(after.source_bytes == previous.source_bytes);
        CHECK(after.source_builds == previous.source_builds);
        CHECK(s_allocations == allocations);
        refresh();
        CHECK(orb_round_list_cache_stats(list, &after));
        CHECK(after.source_bytes > 0 && after.source_bytes < native_bytes / 2);
        CHECK(after.width_table_bytes == before.width_table_bytes);
        CHECK(after.source_builds >= previous.source_builds);
        previous = after; allocations = s_allocations;
        lv_obj_invalidate(screen); lv_refr_now(s_display);
        CHECK(orb_round_list_cache_stats(list, &after));
        CHECK(after.source_bytes == previous.source_bytes);
        CHECK(after.source_builds == previous.source_builds);
        CHECK(s_allocations == allocations);
    }
    orb_round_list_cache_stats_t retained = after;
    /* Same-page membership refresh must wait for the active rim owner. */
    CHECK(orb_model_sessions_chunk(sessions, ORB_MODEL_MAX_SESSIONS - 1, true, false));
    for (unsigned i = 0; i < 8; i++) { orb_shell_poll(shell); refresh(); }
    CHECK(managed_list(orb_shell_root(shell)) == list);
    CHECK(lv_obj_get_child_count(list) == ORB_MODEL_MAX_SESSIONS + 1);
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.source_builds == retained.source_builds);
    orb_round_list_scroll_finish(list);
    for (unsigned i = 0; i < 70; i++) { orb_shell_poll(shell); refresh(); }
    list = managed_list(orb_shell_root(shell)); CHECK(list);
    CHECK(lv_obj_get_child_count(list) == ORB_MODEL_MAX_SESSIONS);
    CHECK(orb_round_list_cache_stats(list, &after));
    CHECK(after.width_table_bytes > 0 && after.source_bytes > 0);
    printf("max24 actual Sessions AX3+Bold+RT: tables=%zu source=%zu metadata=%zu poses=%zu native_rgb565=%zu capture_scratch=%zu\n",
           before.width_table_bytes, before.source_bytes, before.metadata_bytes, before.draw_pose_bytes, native_bytes, scratch_bytes);
    orb_shell_destroy(shell);
    lv_screen_load(s_saved_screen); lv_obj_delete(screen);
}

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display);
    s_saved_screen = lv_screen_active();
    orb_ui_init(&(orb_ui_config_t){.alloc = counted_alloc, .free = counted_free});
    legacy_geometry();
    chord_geometry(false);
    chord_geometry(true);
    source_formats(false);
    source_formats(true);
    fragmented_sources(true);
    fragmented_sources(false);
    maximum_sessions();
    printf("chord list: %u checks passed\n", s_checks);
    lv_deinit();
    return 0;
}
