/* Render-only design-gate fixtures. These synthetic surfaces exercise orb_ui,
 * not product shell routes, sessions or agent actions. See hig.md sections 1-9. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "orb_fonts.h"
#include "orb_ui.h"
#include "sim_display.h"

#define CONTROL 72 /* hig.md section 3: round the physical 44 pt target up. */
#define MAX_LABELS 24

static uint8_t s_band[SIM_D * 84 * 2] __attribute__((aligned(64)));
static lv_obj_t *s_labels[MAX_LABELS];
static unsigned s_label_count, s_checks, s_glass_count, s_tinted_count;
static uint32_t s_now;

static uint32_t tick(void)
{
    return s_now;
}

static void require(bool ok, const char *why)
{
    s_checks++;
    if (ok) return;
    fprintf(stderr, "glass design fixture: %s\n", why);
    exit(EXIT_FAILURE);
}

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t width, int32_t height)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, orb_text_style_t style,
                       int32_t x, int32_t y, int32_t width)
{
    require(s_label_count < MAX_LABELS, "too many labels");
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    orb_text_apply(obj, style, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(obj, ORB_LABEL, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, width);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_WRAP);
    s_labels[s_label_count++] = obj;
    return obj;
}

static lv_obj_t *glass(lv_obj_t *screen, const char *text, int32_t cx, int32_t cy,
                       int32_t width, bool icon, bool tinted)
{
    /* hig.md sections 1/2/7: glass exists only on controls, at the diagonal
     * toolbar positions and six o'clock. A message Reply is the only tint. */
    lv_obj_t *obj = lv_button_create(screen);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, cx - width / 2, cy - CONTROL / 2);
    lv_obj_set_size(obj, width, CONTROL);
    lv_obj_set_scrollable(obj, false);
    lv_obj_t *caption = label(obj, text, ORB_TEXT_HEADLINE, 0, 0, width - 24);
    if (icon) {
        orb_icon_apply(caption, ORB_TEXT_HEADLINE, ORB_TEXT_REGULAR, 0);
        lv_obj_set_width(caption, LV_SIZE_CONTENT);
    }
    lv_obj_center(caption);
    lv_obj_set_raster(caption, true);
    orb_glass_apply(obj, tinted ? ORB_GLASS_TINTED : ORB_GLASS_REGULAR, ORB_BLUE);
    s_glass_count++;
    s_tinted_count += tinted;
    require(width >= CONTROL && lv_obj_get_height(obj) >= CONTROL, "control is smaller than 44 pt");
    return obj;
}

static void top_controls(lv_obj_t *screen, const char *leading, const char *trailing, const char *context)
{
    glass(screen, leading, 101, 101, CONTROL, true, false);
    glass(screen, trailing, 365, 101, CONTROL, true, false);
    if (context) label(screen, context, ORB_TEXT_HEADLINE, 158, 48, 150);
}

static void face(lv_obj_t *screen)
{
    /* This fixture demonstrates existing clock/font/material primitives; it
     * does not define a replacement product Face or fabricate live data. */
    top_controls(screen, ORB_ICON_TERMINAL, ORB_ICON_GEAR, NULL);
    lv_obj_t *clock = label(screen, "10:09", ORB_TEXT_TITLE1, 53, 158, 360);
    orb_clock_apply(clock, ORB_CLOCK_MEDIUM, 0);
    label(screen, "Tue 27 " "\xC2\xB7" " Online", ORB_TEXT_FOOTNOTE1, 73, 286, 320);
    glass(screen, "Sessions", 233, 392, 220, false, false);
}

static void session_row(lv_obj_t *list, const char *title, const char *state,
                        int32_t y, int32_t height, lv_color_t state_color)
{
    lv_obj_t *row = box(list, 63, y, 340, height);
    lv_obj_set_style_bg_color(row, ORB_GRAY6, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, ORB_PT(20), 0);
    lv_obj_t *name = label(row, title, ORB_TEXT_HEADLINE, 24, 7, 292);
    lv_obj_update_layout(name);
    int32_t detail_y = 7 + lv_obj_get_height(name) + 2;
    lv_obj_t *detail = label(row, state, ORB_TEXT_FOOTNOTE1, 24, detail_y, 292);
    lv_obj_set_style_text_color(detail, state_color, 0);
    lv_obj_update_layout(detail);
    require(detail_y + lv_obj_get_height(detail) <= height - 6, "session text does not fit its row");
}

static void sessions(lv_obj_t *screen, bool ax3)
{
    /* hig.md sections 2/3/7: content is standard dark material on black. The
     * public content compositor owns fades and cached round-safe transforms. */
    lv_obj_t *viewport = box(screen, 0, 0, SIM_D, SIM_D);
    lv_obj_t *list = box(viewport, 0, 0, SIM_D, SIM_D);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    session_row(list, "Orb", "Building glass", ax3 ? 172 : 164, ax3 ? 112 : 80, ORB_ORANGE);
    session_row(list, "Recipes", "Ready", ax3 ? 500 : 256, ax3 ? 112 : 80, ORB_GREEN);
    session_row(list, "Media", "Waiting for you", ax3 ? 628 : 500, ax3 ? 112 : 80, ORB_ORANGE);
    require(orb_round_list_apply(list, &(orb_round_list_config_t){.margin = ORB_RIM_MARGIN_TEXT, .fade = ORB_PT(32)}),
            "round-list helper unavailable");
    require(orb_scroll_edge_apply(viewport, 150, 124), "scroll-edge helper unavailable");
    /* The two visible arrow controls are alternatives to either scroll
     * direction (hig.md sections 7/8); the bottom action returns Home. */
    top_controls(screen, ORB_ICON_CHEVRON_UP, ORB_ICON_CHEVRON_DOWN, "10:09");
    glass(screen, "Home", 233, 392, 196, false, false);
    lv_obj_raster_prepare(screen);
}

static void message(lv_obj_t *screen)
{
    /* hig.md sections 1/5/7: an informational message stays black, with no
     * approval/default action. The only tinted control is its explicit Reply. */
    label(screen, "Build complete", ORB_TEXT_HEADLINE, 63, 167, 340);
    label(screen, "Tests passed.\nReady to review.", ORB_TEXT_BODY, 73, 221, 320);
    top_controls(screen, ORB_ICON_X, ORB_ICON_MIC, "Codex");
    glass(screen, "Reply", 233, 392, 196, false, true);
}

static void check_labels(void)
{
    for (unsigned i = 0; i < s_label_count; i++) {
        lv_obj_t *obj = s_labels[i];
        if (!lv_obj_is_visible(obj)) continue;
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        int32_t chord = orb_chord_width(a.y1, a.y2, ORB_RIM_MARGIN_TEXT);
        int32_t safe_left = (SIM_D - chord) / 2;
        require(chord > 0 && a.x1 >= safe_left && a.x2 < SIM_D - safe_left,
                "visible text escaped its circle chord");
        require(lv_obj_get_height(obj) > 0, "visible label has no line height");
        const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
        require(font && font->line_height >= 20, "text is below the HIG minimum");
    }
}

static void check_tint_opacity(lv_obj_t *screen, bool opaque)
{
    orb_glass_cache_prewarm();
    orb_ui_set_reduce_transparency(false);
    lv_obj_t *tinted = box(screen, 197, 197, CONTROL, CONTROL);
    lv_obj_t *regular = box(screen, 197, 197, CONTROL, CONTROL);
    orb_glass_apply(tinted, ORB_GLASS_TINTED, ORB_YELLOW);
    orb_glass_apply(regular, ORB_GLASS_REGULAR, ORB_GRAY);
    require(lv_obj_get_style_bg_opa(tinted, 0) == LV_OPA_70, "default tinted opacity changed");
    require(!orb_glass_set_tint_opacity(screen, LV_OPA_90), "plain object accepted tint opacity");
    require(!orb_glass_set_tint_opacity(regular, LV_OPA_90), "regular glass accepted tint opacity");
    require(!orb_glass_set_tint_opacity(tinted, LV_OPA_20), "tint opacity below bounded range accepted");
    require(!orb_glass_set_tint_opacity(tinted, LV_OPA_COVER), "opaque custom fill accepted");
    require(orb_glass_set_tint_opacity(tinted, LV_OPA_90), "stronger action tint unavailable");
    require(lv_obj_get_style_bg_opa(tinted, 0) == LV_OPA_90, "resting action tint is not 90 percent");
    require(lv_obj_get_style_blur_radius(tinted, 0) > 0, "stronger tint removed backdrop blur");
    lv_obj_send_event(tinted, LV_EVENT_PRESSED, NULL);
    for (unsigned i = 0; i < 10; ++i) { s_now += 16; lv_timer_handler(); }
    require(lv_obj_get_style_bg_opa(tinted, 0) == LV_OPA_COVER * 95 / 100, "pressed action tint is not 95 percent");
    lv_obj_send_event(tinted, LV_EVENT_RELEASED, NULL);
    for (unsigned i = 0; i < 18; ++i) { s_now += 16; lv_timer_handler(); }
    require(lv_obj_get_style_bg_opa(tinted, 0) == LV_OPA_90, "released action tint failed to settle");
    unsigned handlers = lv_obj_get_event_count(tinted);
    orb_glass_apply(tinted, ORB_GLASS_REGULAR, ORB_GRAY6);
    orb_glass_apply(tinted, ORB_GLASS_TINTED, ORB_ORANGE);
    require(lv_obj_get_event_count(tinted) == handlers, "restyling stacked glass handlers");
    require(orb_glass_set_tint_opacity(tinted, 66), "Design 1.0 low tint unavailable");
    require(lv_obj_get_style_bg_opa(tinted, 0) == 66, "low tint is not 26 percent at rest");
    require(lv_obj_get_style_blur_radius(tinted, 0) > 0, "low tint removed glass material");
    lv_obj_send_event(tinted, LV_EVENT_PRESSED, NULL);
    for (unsigned i = 0; i < 10; ++i) { s_now += 16; lv_timer_handler(); }
    require(lv_obj_get_style_bg_opa(tinted, 0) == 66 + LV_OPA_20,
            "pressed low tint jumped to an opaque accent");
    lv_obj_send_event(tinted, LV_EVENT_RELEASED, NULL);
    for (unsigned i = 0; i < 18; ++i) { s_now += 16; lv_timer_handler(); }
    require(lv_obj_get_style_bg_opa(tinted, 0) == 66, "released low tint failed to settle");
    orb_ui_set_reduce_transparency(true);
    require(orb_glass_set_tint_opacity(tinted, 61), "opaque accessibility tint update failed");
    require(lv_obj_get_style_bg_opa(tinted, 0) == LV_OPA_COVER, "Reduce Transparency lost opaque tint");
    require(lv_color_eq(lv_obj_get_style_bg_color(tinted, 0), ORB_GRAY6),
            "Reduce Transparency turned a low tint into a bright solid accent");
    lv_obj_delete(tinted);
    lv_obj_delete(regular);
    orb_ui_set_reduce_transparency(opaque);
}

int main(int argc, char **argv)
{
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s face|list|card OUTPUT.png default|ax3|opaque|ax3-opaque [bold]\n", argv[0]);
        return EXIT_FAILURE;
    }
    bool ax3 = !strcmp(argv[3], "ax3") || !strcmp(argv[3], "ax3-opaque");
    bool opaque = !strcmp(argv[3], "opaque") || !strcmp(argv[3], "ax3-opaque");
    if (strcmp(argv[3], "default") && !ax3 && !opaque) return EXIT_FAILURE;
    if (argc == 5 && strcmp(argv[4], "bold")) return EXIT_FAILURE;

    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *display = sim_display_create();
    require(display != NULL, "display unavailable");
    lv_display_set_buffers(display, s_band, NULL, sizeof(s_band), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_lock();
    orb_ui_init(NULL);
    orb_ui_set_reduce_motion(true);
    orb_ui_set_reduce_transparency(opaque);
    require(orb_fonts_set_content_size(ax3 ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
            "font pack unavailable");
    require(orb_fonts_set_bold_text(argc == 5) == ORB_FONTS_OK, "bold font pack unavailable");
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    check_tint_opacity(screen, opaque);
    if (!strcmp(argv[1], "face")) face(screen);
    else if (!strcmp(argv[1], "list")) sessions(screen, ax3);
    else if (!strcmp(argv[1], "card")) message(screen);
    else return EXIT_FAILURE;
    require(s_glass_count <= 3 && s_tinted_count <= 1, "too many glass or tinted elements");
    lv_obj_update_layout(screen);
    for (unsigned frame = 0; frame < 3; ++frame) {
        s_now += 16;
        lv_timer_handler();
        lv_refr_now(display);
    }
    check_labels();
    require(sim_display_save_png(argv[2]), "could not save PNG");
    lv_unlock();
    printf("glass design %s %s%s: %u checks passed\n", argv[1], argv[3], argc == 5 ? " bold" : "", s_checks);
    return EXIT_SUCCESS;
}
