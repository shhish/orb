/* The handles below deliberately outlive their selection and are never
 * registered as font observers. This is the ownership edge missed by the
 * old test's walk of only currently styled objects. */
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/core/lv_refr_private.h"
#include "../../../orb_ui/orb_round_source.h"
#include "../../../orb_ui/orb_kit_value.h"

typedef struct {
    const lv_font_t *font;
    uint32_t letter;
    lv_font_glyph_dsc_t glyph;
} retained_font_t;

#define RETAINED_FONT_COUNT (ORB_TEXT_STYLE_COUNT * 8 + ORB_CLOCK_COUNT)
static retained_font_t s_retained[RETAINED_FONT_COUNT];
static unsigned s_retained_count;
static lv_obj_t *s_retained_screen;
static lv_layer_t s_queued_layer;
static lv_layer_t *s_old_layers;
static lv_display_t *s_queue_display;
static lv_obj_t *s_dots_label;
static lv_draw_buf_t *s_old_dots;
static lv_obj_t *s_value_label;
static const lv_font_t *s_value_font;

static void retain_font(const lv_font_t *font, uint32_t letter)
{
    assert(s_retained_count < RETAINED_FONT_COUNT);
    retained_font_t *held = &s_retained[s_retained_count++];
    held->font = font; held->letter = letter;
    assert(lv_font_get_glyph_dsc(font, &held->glyph, letter, 0));
    lv_obj_t *label = lv_label_create(s_retained_screen);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, letter == '8' ? "88" : letter == 'H' ? "Held" : ORB_ICON_CHECK);
    lv_obj_set_pos(label, 50, 50 + (s_retained_count % 4) * 75);
}

static void retain_before_switch(void)
{
    assert(!s_retained_screen);
    s_retained_screen = lv_obj_create(NULL);
    s_retained_count = 0;
    s_dots_label = lv_label_create(s_retained_screen);
    lv_obj_set_style_text_font(s_dots_label, orb_font(ORB_TEXT_BODY), 0);
    lv_label_set_text(s_dots_label, "Retained dots");
    s_old_dots = orb_round_text_dots(s_dots_label);
    assert(s_old_dots);
    s_value_label = lv_label_create(s_retained_screen);
    s_value_font = orb_font_variant(ORB_TEXT_BODY, ORB_TEXT_TABULAR);
    lv_obj_set_style_text_font(s_value_label, s_value_font, 0);
    orb_kit_value_set(s_value_label, "Elapsed 12:34 minutes", "Elapsed 88:88 minutes");
    orb_kit_value_fit(s_value_label, 80);
    assert(orb_kit_value_find(s_value_label)->fit_font == s_value_font);
    for (int style = 0; style < ORB_TEXT_STYLE_COUNT; style++) {
        for (unsigned variant = 0; variant < 4; variant++) {
            retain_font(orb_font_variant(style, variant), variant & ORB_TEXT_TABULAR ? '8' : 'H');
            retain_font(orb_icon_font_variant(style, variant), 0xe000);
        }
    }
    for (int clock = 0; clock < ORB_CLOCK_COUNT; clock++) retain_font(orb_clock_font(clock), '8');
    orb_kit_rim_title_create(s_retained_screen, "Retained rim", lv_color_white());
    lv_obj_t *list = lv_obj_create(s_retained_screen);
    lv_obj_set_size(list, 418, 300);
    lv_obj_t *row = orb_kit_row_create(list, &(orb_kit_row_config_t){.title="Retained row", .value="01:23"});
    orb_kit_row_set_live_value(row, "01:23", "88:88");
    /* The detached screen's raster must rebuild after its source payload was
     * genuinely freed, rather than hiding the stale pointer in old pixels. */
    lv_obj_set_raster(list, true);
    lv_obj_raster_prepare(list);
}

static void decode_retained(lv_font_glyph_dsc_t *glyph, bool required)
{
    if (!glyph->box_w || !glyph->box_h) return;
    lv_draw_buf_t *buffer = lv_draw_buf_create(glyph->box_w, glyph->box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    assert(buffer);
    const void *pixels = lv_font_get_glyph_bitmap(glyph, buffer);
    if (required) assert(pixels);
    lv_font_glyph_release_draw_data(glyph);
    lv_draw_buf_destroy(buffer);
}

static void fallback_pixels(const lv_font_t *font, uint32_t letter)
{
    lv_font_glyph_dsc_t actual, reference;
    const lv_font_t *source = orb_fonts_handle_source(font);
    if (!lv_font_get_glyph_dsc(source, &reference, letter, 0)) return;
    assert(lv_font_get_glyph_dsc(font, &actual, letter, 0));
    assert(actual.box_w == reference.box_w && actual.box_h == reference.box_h);
    assert(actual.ofs_x == reference.ofs_x && actual.ofs_y == reference.ofs_y);
    assert(actual.resolved_font->line_height == reference.resolved_font->line_height);
    assert(actual.resolved_font->base_line == reference.resolved_font->base_line);
    if (!actual.box_w || !actual.box_h) return;
    lv_draw_buf_t *a = lv_draw_buf_create(actual.box_w, actual.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    lv_draw_buf_t *b = lv_draw_buf_create(actual.box_w, actual.box_h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    assert(a && b);
    assert(lv_font_get_glyph_bitmap(&actual, a) && lv_font_get_glyph_bitmap(&reference, b));
    for (unsigned row = 0; row < actual.box_h; row++)
        assert(!memcmp(lv_draw_buf_goto_xy(a, 0, row), lv_draw_buf_goto_xy(b, 0, row), actual.box_w));
    lv_draw_buf_destroy(a); lv_draw_buf_destroy(b);
}

static void retained_dots(void)
{
    lv_draw_buf_t *held = orb_round_text_dots(s_dots_label);
    lv_obj_t *fresh_label = lv_label_create(s_retained_screen);
    lv_obj_set_style_text_font(fresh_label, orb_font(ORB_TEXT_BODY), 0);
    lv_draw_buf_t *fresh = orb_round_text_dots(fresh_label);
    assert(held && fresh);
    assert(held->header.w == fresh->header.w && held->header.h == fresh->header.h);
    for (unsigned row = 0; row < held->header.h; row++)
        assert(!memcmp(lv_draw_buf_goto_xy(held, 0, row), lv_draw_buf_goto_xy(fresh, 0, row), held->header.w * 4u));
    orb_round_text_dots_release(fresh); orb_round_text_dots_release(held);
    orb_round_text_dots_release(s_old_dots); s_old_dots = NULL;
    lv_obj_delete(fresh_label);
}

static void retained_value_fit(void)
{
    const char *semantic = "Elapsed 12:34 minutes";
    assert(lv_obj_get_style_text_font(s_value_label, 0) == s_value_font);
    orb_kit_value_set(s_value_label, semantic, NULL);
    kit_value_t *held = orb_kit_value_find(s_value_label);
    lv_obj_t *fresh_label = lv_label_create(s_retained_screen);
    lv_obj_set_style_text_font(fresh_label, orb_font_variant(ORB_TEXT_BODY, ORB_TEXT_TABULAR), 0);
    orb_kit_value_set(fresh_label, semantic, "Elapsed 88:88 minutes");
    orb_kit_value_fit(fresh_label, 80);
    kit_value_t *fresh = orb_kit_value_find(fresh_label);
    assert(held->fit_full_width == fresh->fit_full_width);
    assert(held->fit_ellipsis_width == fresh->fit_ellipsis_width);
    assert(!memcmp(held->fit_prefix, fresh->fit_prefix, (strlen(semantic) + 1) * sizeof(*held->fit_prefix)));
    assert(!strcmp(lv_label_get_text(s_value_label), lv_label_get_text(fresh_label)));
    lv_obj_delete(fresh_label);
}

static void visit_retained_after_switch(void)
{
    for (unsigned i = 0; i < s_retained_count; i++) {
        retained_font_t *held = &s_retained[i];
        /* A held pre-switch glyph may be refused if its old geometry no longer
         * fits. It must never decode a new larger bitmap into an old buffer. */
        decode_retained(&held->glyph, false);
        lv_font_glyph_dsc_t fresh;
        assert(lv_font_get_glyph_dsc(held->font, &fresh, held->letter, 0));
        decode_retained(&fresh, true);
        fallback_pixels(held->font, held->letter);
        fallback_pixels(held->font, 0xe000); /* inline icon fallback uses its own baseline */
        fallback_pixels(held->font, 'H'); /* tabular records fall back to ordinary text */
    }
    retained_dots();
    retained_value_fit();
    lv_obj_t *previous = lv_screen_active();
    lv_screen_load(s_retained_screen);
    lv_obj_invalidate_raster(s_retained_screen);
    lv_obj_invalidate(s_retained_screen);
    lv_refr_now(lv_display_get_default());
    lv_screen_load(previous);
    lv_obj_delete(s_retained_screen); s_retained_screen = NULL;
}

static void queue_before_switch(lv_display_t *display)
{
    lv_layer_init(&s_queued_layer);
    s_queue_display = display;
    s_queued_layer.display = display;
    s_queued_layer.color_format = LV_COLOR_FORMAT_ARGB8888;
    s_queued_layer.draw_buf = lv_draw_buf_create(300, 180, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    assert(s_queued_layer.draw_buf);
    s_queued_layer.buf_area = (lv_area_t){0, 0, 299, 179};
    s_queued_layer._clip_area = s_queued_layer.buf_area;
    s_queued_layer.phy_clip_area = s_queued_layer.buf_area;
    s_queued_layer.all_tasks_added = true;
    s_old_layers = display->layer_head;
    s_queued_layer.next = s_old_layers;
    display->layer_head = &s_queued_layer;
    lv_display_t *refreshing = lv_refr_get_disp_refreshing();
    lv_refr_set_disp_refreshing(NULL);
    lv_draw_label_dsc_t label; lv_draw_label_dsc_init(&label);
    label.font = orb_font(ORB_TEXT_BODY); label.text = "Queued before switching " ORB_ICON_CHECK;
    lv_draw_label(&s_queued_layer, &label, &s_queued_layer.buf_area);
    lv_refr_set_disp_refreshing(refreshing);
    assert(s_queued_layer.draw_task_head);
    assert(s_queued_layer.draw_task_head->state == LV_DRAW_TASK_STATE_WAITING);
}

static void finish_queued_after_switch(bool switched)
{
    bool drained = s_queued_layer.draw_task_head == NULL;
    /* Also execute the old implementation's forgotten queue, so ASan reports
     * the concrete bad font read instead of just an ownership assertion. */
    while (s_queued_layer.draw_task_head) {
        lv_draw_dispatch_layer(s_queue_display, &s_queued_layer);
        lv_draw_wait_for_finish();
    }
    if (switched) assert(drained);
    s_queue_display->layer_head = s_old_layers;
    lv_draw_buf_destroy(s_queued_layer.draw_buf);
}
