#include "orb_time_view_internal.h"

#include <stdio.h>
#include <string.h>

/* Semantic action names stay available to input and accessibility while the
 * disc uses the same icon source as its matching list rows (HIG §7). */
static lv_obj_t *control(orb_time_view_t *v, lv_obj_t *parent, const char *name,
                         const char *symbol, bool tinted, int command)
{
    lv_obj_t *obj = orb_kit_glyph_button_create(parent, symbol, tinted, TV_ACCENT);
    tv_bind(v, obj, command);
    tv_semantic(obj, name);
    if (v->contrast) tv_contrast_apply(obj);
    return obj;
}

void tv_controls(orb_time_view_t *v, const char *primary, const char *secondary_name,
                  const char *symbol, int command)
{
    const char *secondary = secondary_name ? secondary_name : "";
    if (v->page == ORB_TIME_PAGE_ALARM) return;
    if (!strcmp(v->primary_name, primary) && !strcmp(v->secondary_name, secondary)) return;
    if (v->primary) lv_obj_delete(v->primary);
    if (v->secondary) lv_obj_delete(v->secondary);
    v->secondary = NULL;
    lv_obj_t *parent = v->summary ? v->summary : v->content;
    bool go = !strcmp(primary, "Start") || !strcmp(primary, "Resume");
    v->primary = control(v, parent, primary, go ? ORB_ICON_PLAY_FILL : ORB_ICON_PAUSE_FILL, true, PRIMARY);
    if (*secondary) {
        v->secondary = control(v, parent, secondary, symbol, false, command);
        if (v->page == ORB_TIME_PAGE_STOPWATCH) {
            /* HIG §§1/3/7: the primary stays at six o'clock. Lap and Reset
             * use opposite arc slots, so an extra Lap tap cannot erase. */
            lv_obj_align(v->secondary, LV_ALIGN_TOP_MID,
                v->model.stopwatch_state == ORB_TIME_RUNNING ? 105 : -105, 316);
            lv_obj_align(v->primary, LV_ALIGN_TOP_MID, 0, 316);
        } else orb_kit_place_arc_pair(v->secondary, v->primary);
    } else {
        orb_kit_place_bottom_arc(v->primary);
        /* An 80px glyph needs 24px more room above the kit dots than a
         * 64px capsule. Keep its centre on the pair arc. */
        lv_obj_align(v->primary, LV_ALIGN_TOP_MID, 0, 316);
    }
    if (v->summary) {
        /* Resolve the kit placement before converting to the scrolling
         * summary's local coordinate system. */
        lv_obj_update_layout(parent);
        lv_obj_set_y(v->primary, lv_obj_get_y(v->primary) - LAP_VIEW_TOP);
        if (v->secondary) lv_obj_set_y(v->secondary, lv_obj_get_y(v->secondary) - LAP_VIEW_TOP);
    }
    snprintf(v->primary_name, sizeof(v->primary_name), "%s", primary);
    snprintf(v->secondary_name, sizeof(v->secondary_name), "%s", secondary);
    if (v->navigation) lv_obj_move_to_index(v->navigation, -1);
}

