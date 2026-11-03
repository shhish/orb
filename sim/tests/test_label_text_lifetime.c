/* Freeze a queued label while GET_SELF_SIZE temporarily removes its ellipsis.
 * DRAW_TASK_ADDED runs before dispatch: the regression needs no worker timing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/widgets/label/lv_label_private.h"

static unsigned s_checks, s_queued, s_reverted;
static lv_obj_t *s_label;
static lv_font_t s_font;
static const lv_draw_label_dsc_t *s_queued_dsc;
static bool s_probing, s_static;
static char s_frozen[128];

#define CHECK(value) do { s_checks++; if(!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while(0)

static bool glyph(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    (void)font;
    if(s_probing) {
        /* The producer's label buffer really has been reverted to the full
         * text here. A worker borrowing it observes this different string. */
        if(strcmp(lv_label_get_text(s_label), s_frozen)) s_reverted++;
        CHECK(!strcmp(s_queued_dsc->text, s_frozen));
    }
    return lv_font_montserrat_14.get_glyph_dsc(&lv_font_montserrat_14, dsc, letter, next);
}

static void queued(lv_event_t *event)
{
    lv_draw_task_t *task = lv_event_get_draw_task(event);
    lv_draw_label_dsc_t *dsc = lv_draw_task_get_label_dsc(task);
    if(!dsc || s_queued++) return;
    const char *source = lv_label_get_text(s_label);
    if(s_static) {
        CHECK(dsc->text_static && !dsc->text_local && dsc->text == source);
        return;
    }
    /* The fork's dots are one U+2026, or three periods in a font without it. */
    CHECK(strstr(source, "\xe2\x80\xa6") != NULL || strstr(source, "...") != NULL);
    CHECK(strlen(source) < sizeof(s_frozen));
    strcpy(s_frozen, source);
    s_queued_dsc = dsc;
    s_probing = true;
    ((lv_label_t *)s_label)->invalid_size_cache = true;
    lv_point_t size = {0};
    lv_obj_send_event(s_label, LV_EVENT_GET_SELF_SIZE, &size);
    s_probing = false;
    CHECK(s_reverted > 0);
    CHECK(dsc->text_local && !dsc->text_static && dsc->text != source);
    CHECK(!strcmp(dsc->text, s_frozen));
    CHECK(!strcmp(lv_label_get_text(s_label), s_frozen));
    s_queued_dsc = NULL;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area; (void)pixels;
    lv_display_flush_ready(display);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(240, 80);
    CHECK(display != NULL);
    static uint8_t buffer[240 * 80 * 2] __attribute__((aligned(64)));
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    s_font = lv_font_montserrat_14;
    s_font.get_glyph_dsc = glyph;
    for(unsigned mode = 0; mode < 2; mode++) {
        s_static = mode != 0;
        s_queued = s_reverted = 0;
        s_label = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(s_label, &s_font, 0);
        lv_obj_set_size(s_label, 120, 18);
        lv_obj_set_pos(s_label, 10, 10);
        if(s_static) lv_label_set_text_static(s_label, "Static label");
        else {
            lv_label_set_long_mode(s_label, LV_LABEL_LONG_MODE_DOTS);
            lv_label_set_text(s_label, "lightbulb 12 - Demo Home accessory label");
        }
        lv_obj_update_layout(s_label);
        lv_obj_set_send_draw_task_events(s_label, true);
        lv_obj_add_event_cb(s_label, queued, LV_EVENT_DRAW_TASK_ADDED, NULL);
        lv_refr_now(display);
        CHECK(s_queued > 0);
        lv_obj_delete(s_label);
    }
    lv_display_delete(display);
    lv_deinit();
    printf("label task text lifetime: %u checks, deterministic ellipsis mutation and static zero-copy passed\n", s_checks);
    return 0;
}
