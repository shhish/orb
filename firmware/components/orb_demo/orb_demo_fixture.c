#include "orb_demo_fixture.h"

#include "orb_ui.h"

static lv_obj_t *label(lv_obj_t *parent, const char *text, orb_text_style_t style)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    orb_text_apply(obj, style, ORB_TEXT_REGULAR, 0);
    lv_obj_set_style_text_color(obj, ORB_LABEL, 0);
    return obj;
}

static void slide(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

void orb_demo_fixture_build(lv_obj_t *screen, orb_demo_fixture_t fixture)
{
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);
    /* HIG sections 2/3/4: diagnostic content is matte, true black and fits the
     * circular text-safe chord. This fixture has no interactive decisions. */
    lv_obj_t *clock = label(screen, "10:09", ORB_TEXT_HEADLINE);
    lv_obj_align(clock, LV_ALIGN_TOP_MID, 0, ORB_PT(36));
    lv_obj_t *strip = lv_obj_create(screen);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, fixture == ORB_DEMO_FIXTURE_TRANSITION ? 2 * ORB_SCREEN_D : ORB_SCREEN_D,
                    ORB_SCREEN_D);
    lv_obj_set_scrollable(strip, false);
    lv_obj_set_clickable(strip, false);
    unsigned pages = fixture == ORB_DEMO_FIXTURE_TRANSITION ? 2 : 1;
    for (unsigned i = 0; i < pages; i++) {
        lv_obj_t *page = lv_obj_create(strip);
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, ORB_SCREEN_D, ORB_SCREEN_D);
        lv_obj_set_pos(page, i * ORB_SCREEN_D, 0);
        lv_obj_set_scrollable(page, false);
        lv_obj_set_clickable(page, false);
        const char *heading = fixture == ORB_DEMO_FIXTURE_QUIET ? "Quiet" : i ? "Attention" : "Sessions";
        const char *body = fixture == ORB_DEMO_FIXTURE_QUIET ? "No display jobs" : i ? "Waiting for you" : "Building glass";
        lv_obj_t *title = label(page, heading, ORB_TEXT_TITLE3);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -ORB_PT(32));
        lv_obj_t *detail = label(page, body, ORB_TEXT_BODY);
        lv_obj_set_width(detail, ORB_PT(200));
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(detail, LV_ALIGN_CENTER, 0, ORB_PT(12));
        lv_obj_set_raster_matte(page, lv_color_black());
    }
    lv_obj_move_foreground(clock);
    lv_obj_raster_prepare(screen);
    if (fixture == ORB_DEMO_FIXTURE_TRANSITION) {
        /* Baseline measures the existing continuous LVGL translation path,
         * before the architecture's later two-screen snapshot implementation. */
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, strip);
        lv_anim_set_exec_cb(&anim, slide);
        lv_anim_set_values(&anim, 0, -ORB_SCREEN_D);
        lv_anim_set_duration(&anim, 500);
        lv_anim_set_reverse_duration(&anim, 500);
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
        lv_anim_start(&anim);
    }
}
