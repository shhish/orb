/**
 * @file lv_draw_blur.c
 * ORB PATCH: cull empty band tasks and track scroll-only material quality.
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_private.h"
#include "../core/lv_obj_material_private.h"
#include "../misc/lv_area_private.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void LV_ATTRIBUTE_FAST_MEM lv_draw_blur_dsc_init(lv_draw_blur_dsc_t * dsc)
{
    LV_CHECK_ARG(dsc != NULL, return);

    lv_memzero(dsc, sizeof(lv_draw_blur_dsc_t));
    dsc->base.dsc_size = sizeof(lv_draw_blur_dsc_t);
}

lv_draw_blur_dsc_t * lv_draw_task_get_blur_dsc(lv_draw_task_t * task)
{
    LV_CHECK_ARG(task != NULL, return NULL);

    return task->type == LV_DRAW_TASK_TYPE_BLUR ? (lv_draw_blur_dsc_t *)task->draw_dsc : NULL;
}

void LV_ATTRIBUTE_FAST_MEM lv_draw_blur(lv_layer_t * layer, const lv_draw_blur_dsc_t * dsc, const lv_area_t * coords)
{
    LV_CHECK_ARG(layer != NULL, return);
    LV_CHECK_ARG(dsc != NULL, return);
    LV_CHECK_ARG(coords != NULL, return);

    if(dsc->blur_radius <= 0 || lv_obj_material_skip_blur(dsc)) return;

#if defined(CONFIG_ORB_DISPLAY_GLASS_SINGLE_PASS) && CONFIG_ORB_DISPLAY_GLASS_SINGLE_PASS
    /* Orb's whole-control band guard already keeps each material sample in
     * one band. Its padded shadow intersects neighboring bands too, but that
     * is not a reason to dispatch another (empty) blur task there. */
    lv_area_t visible;
    if(!lv_area_intersect(&visible, coords, &layer->_clip_area) ||
       !lv_area_intersect(&visible, &visible, &layer->buf_area)) return;
#endif
    LV_PROFILER_DRAW_BEGIN;

    lv_draw_task_t * t = lv_draw_add_task(layer, coords, LV_DRAW_TASK_TYPE_BLUR);

    lv_memcpy(t->draw_dsc, dsc, sizeof(*dsc));

    lv_draw_finalize_task_creation(layer, t);
    LV_PROFILER_DRAW_END;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

#if (defined(CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) && CONFIG_ORB_DISPLAY_GLASS_MOTION_QUALITY) || \
    (defined(CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) && CONFIG_ORB_DISPLAY_RETAINED_SETTLED_ONLY) || \
    (defined(CONFIG_ORB_DISPLAY_TE_PACING) && CONFIG_ORB_DISPLAY_TE_PACING)
#define MOTION_DISPLAYS 4
#define MOTION_SETTLE_MS 120
static struct {
    lv_display_t * display;
    lv_obj_t * source;
    uint32_t last_motion;
    bool moving;
} s_motion[MOTION_DISPLAYS];
static lv_timer_t * s_motion_timer;

static lv_obj_tree_walk_res_t motion_settle_object(lv_obj_t * obj, void * unused)
{
    LV_UNUSED(unused);
    if(lv_obj_is_visible(obj) && lv_obj_get_style_blur_backdrop(obj, LV_PART_MAIN) &&
       lv_obj_get_style_blur_radius(obj, LV_PART_MAIN) > 0) lv_obj_invalidate(obj);
    return LV_OBJ_TREE_WALK_NEXT;
}

static void motion_timer(lv_timer_t * timer)
{
    bool any_moving = false;
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) {
        if(!s_motion[i].display || !s_motion[i].moving ||
           lv_tick_elaps(s_motion[i].last_motion) < MOTION_SETTLE_MS) continue;
        s_motion[i].moving = false;
        lv_display_t * display = s_motion[i].display;
        lv_obj_tree_walk(lv_display_get_screen_active(display), motion_settle_object, NULL);
        lv_obj_tree_walk(lv_display_get_layer_top(display), motion_settle_object, NULL);
        lv_obj_tree_walk(lv_display_get_layer_sys(display), motion_settle_object, NULL);
        lv_obj_tree_walk(lv_display_get_layer_bottom(display), motion_settle_object, NULL);
    }
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) any_moving |= s_motion[i].moving;
    if(!any_moving) {
        lv_timer_delete(timer);
        s_motion_timer = NULL;
    }
}

static void motion_display_delete(lv_event_t * event)
{
    lv_display_t * display = lv_event_get_target(event);
    bool any = false;
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) {
        if(s_motion[i].display == display) lv_memzero(&s_motion[i], sizeof(s_motion[i]));
        any |= s_motion[i].display != NULL;
    }
    if(!any && s_motion_timer) {
        lv_timer_delete(s_motion_timer);
        s_motion_timer = NULL;
    }
}

static void motion_source_delete(lv_event_t * event)
{
    lv_obj_t * obj = lv_event_get_target_obj(event);
    bool any_moving = false;
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) {
        if(s_motion[i].source == obj) {
            s_motion[i].source = NULL;
            s_motion[i].moving = false;
        }
        any_moving |= s_motion[i].moving;
    }
    /* Temporary benchmark screens must not leave a settle timer operating on
     * the restored screen after their content is destroyed. */
    if(!any_moving && s_motion_timer) {
        lv_timer_delete(s_motion_timer);
        s_motion_timer = NULL;
    }
}

void lv_draw_sw_blur_note_object_motion(lv_obj_t * obj)
{
    if(!obj) return;
    bool registered = false;
    for(uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        if(lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, i)) == motion_source_delete) {
            registered = true;
            break;
        }
    }
    if(!registered && !lv_obj_add_event_cb(obj, motion_source_delete, LV_EVENT_DELETE, NULL)) return;
    lv_display_t * display = lv_obj_get_display(obj);
    lv_draw_sw_blur_note_motion(display);
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) {
        if(s_motion[i].display == display) s_motion[i].source = obj;
    }
}

void lv_draw_sw_blur_note_motion(lv_display_t * display)
{
    if(!display) return;
    unsigned slot = MOTION_DISPLAYS;
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) {
        if(s_motion[i].display == display) { slot = i; break; }
        if(!s_motion[i].display) slot = i;
    }
    /* If resources are unavailable the ordinary material remains correct. */
    if(slot == MOTION_DISPLAYS) return;
    if(!s_motion_timer) s_motion_timer = lv_timer_create(motion_timer, 40, NULL);
    if(!s_motion_timer) return;
    if(!s_motion[slot].display) {
        if(!lv_display_add_event_cb(display, motion_display_delete, LV_EVENT_DELETE, NULL)) return;
        s_motion[slot].display = display;
    }
    s_motion[slot].source = NULL;
    s_motion[slot].last_motion = lv_tick_get();
    s_motion[slot].moving = true;
    lv_obj_material_note_motion(display);
}

bool lv_draw_sw_blur_motion_active(lv_display_t * display)
{
    for(unsigned i = 0; i < MOTION_DISPLAYS; i++) {
        if(s_motion[i].display == display) return s_motion[i].moving;
    }
    return false;
}
#endif
