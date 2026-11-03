/* Public round-list and content-edge helpers through the band renderer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_ui.h"
#include "sim_display.h"

#define D ORB_SCREEN_D
static uint8_t s_rgb[D * D * 3], s_before[D * D * 3];
static int s_checks;
static int s_allocations;
static size_t s_bytes;
static const char *s_shot_prefix;
static unsigned s_frames;

static void count_frame(lv_event_t *event)
{
    LV_UNUSED(event);
    s_frames++;
}

static void require(bool ok, const char *why)
{
    s_checks++;
    if (!ok) {
        fprintf(stderr, "round helpers: %s\n", why);
        exit(1);
    }
}

static void *tracked_alloc(size_t size)
{
    size_t *p = malloc(size + sizeof(*p));
    if (p) {
        *p = size;
        s_allocations++;
        s_bytes += size;
        return p + 1;
    }
    return NULL;
}

static void tracked_free(void *ptr)
{
    size_t *p = (size_t *)ptr - 1;
    s_allocations--;
    s_bytes -= *p;
    free(p);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static int red(int x, int y)
{
    return s_rgb[(y * D + x) * 3];
}

static void render(lv_display_t *display)
{
    lv_refr_now(display);
    sim_display_rgb(s_rgb);
}

static void save_frame(const char *name)
{
    if (s_shot_prefix) {
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s-%s.png", s_shot_prefix, name);
        require(n > 0 && (size_t)n < sizeof(path), "screenshot path too long");
        require(sim_display_save_png(path), "could not save helper screenshot");
    }
}

static void edges(lv_display_t *display)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *viewport = box(screen, 0, 0, D, D, lv_color_white());
    require(orb_scroll_edge_apply(viewport, 120, 100), "edge allocation failed");
    uint32_t callbacks = lv_obj_get_event_count(viewport);
    require(orb_scroll_edge_apply(viewport, 120, 100), "edge reapply failed");
    require(lv_obj_get_event_count(viewport) == callbacks, "edge reapply duplicated callbacks");
    /* A later control sibling must remain sharp above the content fade. */
    box(screen, 210, 30, 46, 46, lv_color_white());
    render(display);
    require(red(233, 0) <= 8, "top edge did not dissolve to black");
    require(red(233, 119) >= 240, "top fade did not recover full content opacity");
    require(red(233, 233) >= 248, "edge effect dimmed the center");
    require(red(233, 465) <= 8, "bottom edge did not dissolve to black");
    require(red(233, 367) >= 240, "bottom fade did not begin transparently");
    require(red(233, 45) >= 248, "edge effect dimmed floating controls");
    for (int y = 80; y < 119; y++) {
        require(red(180, y) <= red(180, y + 1) + 8, "top ramp is not monotonic across bands");
    }
    memcpy(s_before, s_rgb, sizeof(s_rgb));
    lv_area_t small = {170, 82, 292, 101};
    lv_obj_invalidate_area(viewport, &small);
    render(display);
    require(!memcmp(s_before, s_rgb, sizeof(s_rgb)), "partial edge redraw changed pixels");
    save_frame("scroll-edges");
    require(orb_scroll_edge_apply(viewport, 0, 0), "edge disable failed");
    render(display);
    require(red(233, 0) >= 248 && red(233, 465) >= 248, "zero extent did not disable edges");
    lv_obj_clean(screen);
    require(s_allocations == 0, "edge state leaked on delete");
}

static void rows(lv_display_t *display)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *list = box(screen, 0, 0, D, D, lv_color_black());
    lv_obj_t *row = box(list, 63, 193, 340, 80, lv_color_white());
    lv_obj_t *icon = box(row, 8, 4, 72, 72, lv_color_hex(0xFF0000));
    lv_obj_t *other = box(list, 63, 361, 340, 80, lv_color_white());
    box(list, 63, 800, 340, 80, lv_color_white());
    require(orb_round_list_apply(list, NULL), "list allocation failed");
    uint32_t callbacks = lv_obj_get_event_count(list);
    require(orb_round_list_apply(list, NULL), "list reapply failed");
    require(lv_obj_get_event_count(list) == callbacks, "list reapply duplicated callbacks");
    require(s_allocations == 2, "same-sized rows did not share a transform table");
    require(s_bytes < 4000, "transform table unexpectedly large");
    lv_obj_raster_prepare(list);
    render(display);
    require(red(70, 233) >= 248, "center row was needlessly shrunk");
    lv_point_t center = {233, 233}, corner = {64, 194};
    require(orb_round_list_hit_test(row, &center), "center row rejected its center");
    require(orb_round_list_hit_test(row, &corner), "unscaled row rejected an inside point");
    lv_area_t token;
    require(orb_round_list_child_bounds(row, icon, &token), "native icon bounds were not available");
    require(token.x1 == 71 && token.y1 == 197 && token.x2 == 142 && token.y2 == 268,
            "native icon bounds moved away from the actual child");
    require(!orb_round_list_child_bounds(row, other, &token), "unrelated row became an icon anchor");
    require(!orb_round_list_child_bounds(NULL, icon, &token), "missing row became an icon anchor");
    require(!orb_round_list_child_bounds(row, NULL, &token), "missing icon became an anchor");
    require(!orb_round_list_child_bounds(row, icon, NULL), "missing output bounds were accepted");
    lv_obj_set_hidden(icon, true);
    require(!orb_round_list_child_bounds(row, icon, &token), "hidden icon became an anchor");
    lv_obj_set_hidden(icon, false);
    lv_obj_set_style_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(row, 179, LV_STATE_PRESSED);
    lv_obj_add_state(row, LV_STATE_PRESSED);
    orb_round_list_update(list);
    render(display);
    require(red(233, 233) == 0, "pressed feedback revived an unreadable row");
    require(!orb_round_list_hit_test(row, &center), "unreadable held row accepted a tap");
    require(!orb_round_list_child_bounds(row, icon, &token), "unreadable held row exposed an icon anchor");
    lv_obj_remove_state(row, LV_STATE_PRESSED);
    lv_obj_set_style_opa(row, LV_OPA_COVER, 0);
    orb_round_list_update(list);
    render(display);
    lv_obj_raster_stats_t before, after;
    lv_obj_raster_get_stats(&before);
    int allocations = s_allocations;
    lv_obj_scroll_to_y(list, 168, LV_ANIM_OFF);
    orb_round_list_update(list);
    render(display);
    require(red(70, 65) == 0, "edge row escaped the circle chord");
    require(red(233, 65) > 0 && red(233, 65) < 248, "bezel row did not fade");
    save_frame("round-list");
    corner = (lv_point_t){64, 26};
    center = (lv_point_t){233, 65};
    require(!orb_round_list_hit_test(row, &corner), "scaled row accepted an invisible corner");
    require(orb_round_list_hit_test(row, &center), "scaled row rejected its center");
    require(orb_round_list_child_bounds(row, icon, &token), "scaled icon bounds were not available");
    /* This fixture's 340x80 row at y=25 has Q8 chord scale 152. The icon
     * must move with that row, rather than stay in its logical text rail. */
    require(token.x1 == 136 && token.y1 == 43 && token.x2 == 177 && token.y2 == 84,
            "scaled icon anchor did not follow the row presentation");
    int token_x = (token.x1 + token.x2) / 2, token_y = (token.y1 + token.y2) / 2;
    require(s_rgb[(token_y * D + token_x) * 3] > 0 &&
            s_rgb[(token_y * D + token_x) * 3 + 1] == 0,
            "scaled icon anchor sampled its white row/title instead of its red icon");
    lv_obj_scroll_to_y(list, 273, LV_ANIM_OFF);
    orb_round_list_update(list);
    render(display);
    center = (lv_point_t){233, -40};
    require(!orb_round_list_hit_test(row, &center), "offscreen row accepted a tap");
    require(!orb_round_list_child_bounds(row, icon, &token), "offscreen icon became an anchor");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
    orb_round_list_update(list);
    render(display);
    lv_obj_raster_get_stats(&after);
    require(after.builds == before.builds, "row movement rebuilt source bitmaps");
    require(s_allocations == allocations, "row movement allocated another table");
    lv_obj_scroll_to_y(list, 168, LV_ANIM_OFF);
    orb_round_list_update(list);
    lv_obj_set_floating(row, true);
    lv_obj_set_y(row, 25);
    orb_round_list_update(list);
    render(display);
    require(red(110, 65) >= 248, "floating row retained its scaled/faded raster transform");
    require(!orb_round_list_child_bounds(row, icon, &token), "floating row kept a round icon anchor");
    lv_obj_set_floating(row, false);
    lv_obj_set_y(row, 193);
    orb_round_list_update(list);
    render(display);
    require(red(110, 65) == 0, "row did not resume its bezel transform after floating");
    lv_obj_set_size(other, 300, 60);
    lv_obj_update_layout(list);
    orb_round_list_update(list);
    require(s_allocations == 3, "new row size did not acquire its own table");
    lv_obj_delete(other);
    orb_round_list_update(list);
    require(s_allocations == 2, "obsolete row-size table was retained");
    lv_obj_clean(screen);
    require(s_allocations == 0 && s_bytes == 0, "round-list state leaked on delete");
}

static void following_edges(lv_display_t *display)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *viewport = box(screen, 0, 0, D, D, lv_color_white());
    box(viewport, 0, 0, D, D * 2, lv_color_white());
    lv_obj_set_scroll_dir(viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(viewport, LV_SCROLLBAR_MODE_OFF);
    require(orb_scroll_edge_follow(viewport, 64, 48), "following edge allocation failed");
    render(display);
    require(red(233, 0) >= 248, "following edge dimmed first resting content");
    unsigned frames = s_frames;
    int allocations = s_allocations;
    for (int i = 0; i < 10; i++)
        require(orb_scroll_edge_follow(viewport, 64, 48), "following reapply failed");
    render(display);
    require(s_frames == frames, "unchanged edge policy redrew an idle viewport");
    require(s_allocations == allocations, "following reapply allocated callback state");
    lv_obj_scroll_to_y(viewport, 1, LV_ANIM_OFF);
    render(display);
    require(red(233, 2) >= 248 && red(233, 32) >= 248,
            "first pixel of scrolling abruptly dimmed a full line");
    lv_obj_scroll_to_y(viewport, 80, LV_ANIM_OFF);
    render(display);
    require(red(233, 0) <= 8, "scrolled top did not dissolve to black");
    require(red(233, 63) >= 240, "scrolled top did not recover full contrast");
    require(red(233, 233) >= 248, "following edge dimmed central content");
    for (int y = 1; y < 62; y++)
        require(red(233, y) <= red(233, y + 1) + 8, "following edge clipped instead of dissolving");
    memcpy(s_before, s_rgb, sizeof(s_rgb));
    lv_area_t partial = {190, 24, 270, 90};
    lv_obj_invalidate_area(viewport, &partial);
    render(display);
    require(!memcmp(s_before, s_rgb, sizeof(s_rgb)), "following partial redraw changed pixels");
    save_frame("following-edges");
    lv_obj_scroll_to_y(viewport, D - 1, LV_ANIM_OFF);
    render(display);
    require(red(233, 463) >= 248 && red(233, 433) >= 248,
            "last pixel of scrolling abruptly dimmed a full line");
    lv_obj_scroll_to_y(viewport, D, LV_ANIM_OFF);
    render(display);
    require(red(233, 465) >= 248, "following edge dimmed last settled content");
    lv_obj_scroll_to_y(viewport, 0, LV_ANIM_OFF);
    render(display);
    require(red(233, 0) >= 248, "following edge did not restore resting row");
    require(orb_scroll_edge_apply(viewport, 64, 48), "switch to fixed edge failed");
    render(display);
    require(red(233, 0) <= 8, "fixed edge retained follow-scroll semantics");
    lv_obj_clean(screen);
    require(s_allocations == 0 && s_bytes == 0, "following edge state leaked on delete");
}

static void clipped_hits(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *viewport = box(screen, 0, 133, D, 200, lv_color_black());
    lv_obj_t *list = box(viewport, 0, 0, D, D, lv_color_black());
    lv_obj_t *row = box(list, 133, 160, 200, 80, lv_color_white());
    lv_obj_t *icon = box(row, 20, 20, 160, 40, lv_color_hex(0xFF0000));
    require(orb_round_list_apply(list, NULL), "clipped list allocation failed");
    lv_point_t visible = {233, 320}, clipped = {233, 350};
    require(orb_round_list_hit_test(row, &visible), "ancestor-clipped row rejected its visible part");
    require(!orb_round_list_hit_test(row, &clipped), "row accepted a point outside its ancestor viewport");
    lv_area_t token;
    require(orb_round_list_child_bounds(row, icon, &token), "partly clipped icon lost its anchor");
    require(token.y1 == 313 && token.y2 == 352, "clipping changed the complete icon anchor geometry");
    lv_obj_set_y(icon, 40);
    lv_obj_update_layout(list);
    require(!orb_round_list_child_bounds(row, icon, &token), "entirely clipped icon became an anchor");
    lv_obj_set_hidden(viewport, true);
    require(!orb_round_list_hit_test(row, &visible), "row accepted a point under a hidden ancestor");
    require(!orb_round_list_child_bounds(row, icon, &token), "hidden ancestor exposed an icon anchor");
    lv_obj_clean(screen);
    require(s_allocations == 0 && s_bytes == 0, "clipped-list state leaked on delete");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        s_shot_prefix = argv[1];
    }
    lv_init();
    lv_display_t *display = sim_display_create();
    orb_ui_init(&(orb_ui_config_t){tracked_alloc, tracked_free});
    lv_display_add_event_cb(display, count_frame, LV_EVENT_RENDER_START, NULL);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    edges(display);
    rows(display);
    following_edges(display);
    clipped_hits();
    printf("round helpers: %d checks passed\n", s_checks);
    return 0;
}
