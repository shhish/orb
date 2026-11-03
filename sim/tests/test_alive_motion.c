#include "orb_kit.h"
#include "sim_display.h"
#include <stdio.h>
#include <stdlib.h>
static uint32_t s_now = 10000;
static unsigned s_checks, s_calls, s_arrivals, s_frames, s_damage;
static bool s_animate;
static float s_value;
static uint32_t tick(void) { return s_now; }
static void check(bool condition, const char *why)
{
    s_checks++;
    if (!condition) { fprintf(stderr,"Alive clock: %s\n",why); exit(1); }
}
static void advance(unsigned ms)
{
    for (unsigned i=0;i<ms;i+=16) { s_now+=16; lv_timer_handler(); }
}
static void rendered(lv_event_t *e)
{
    if(lv_event_get_code(e)==LV_EVENT_RENDER_START) s_frames++;
    if(lv_event_get_code(e)==LV_EVENT_INVALIDATE_AREA) s_damage++;
}
static bool frame(void *ctx,uint32_t elapsed,uint32_t dt,bool finish)
{
    (void)ctx; (void)dt; s_calls++;
    s_value=finish||elapsed>=160?1:elapsed/160.f;
    return s_value<1;
}
static void arrival(void *ctx,bool animate)
{
    (void)ctx; s_arrivals++; s_animate=animate; s_value=animate?0:1;
}
static bool delete_on_finish(void *context, uint32_t elapsed, uint32_t dt, bool finish)
{
    (void)elapsed; (void)dt;
    if (finish) lv_obj_delete(context);
    return !finish;
}
static void press_cancel_restarts_at_rest(lv_obj_t *parent)
{
    lv_obj_t *control = lv_obj_create(parent);
    orb_kit_press_apply(control);
    lv_obj_send_event(control, LV_EVENT_PRESSED, NULL);
    advance(16);
    int32_t first = lv_obj_get_style_transform_scale_x(control, 0);
    check(first < 256, "press has intermediate contraction");
    advance(48);
    orb_kit_press_cancel(control);
    check(lv_obj_get_style_transform_scale_x(control, 0) == 256,
          "cancel returns the press to rest");
    lv_obj_send_event(control, LV_EVENT_PRESSED, NULL);
    advance(16);
    check(lv_obj_get_style_transform_scale_x(control, 0) == first,
          "new press inherited cancelled contact velocity");
    orb_kit_press_cancel(control);
    lv_obj_delete(control);
}
static void material_first_paint(lv_obj_t *parent)
{
    lv_obj_t *covered = lv_obj_create(lv_screen_active());
    for (unsigned reduced = 0; reduced < 2; reduced++) {
        orb_ui_set_reduce_motion(reduced);
        lv_obj_t *control = orb_kit_disc_create(parent, ORB_KIT_DISC_KIND_MINI,
            ORB_KIT_NEUTRAL_GLASS, ORB_LABEL, ORB_ICON_APPS);
        check(orb_glass_materialise(control, true), "material entrance admitted");
        check(lv_obj_is_hidden(control) && orb_glass_get_amount(control) == 0,
              "material entrance must begin absent before the first handler");
        for (unsigned elapsed = 0; elapsed < (reduced ? 160 : 560); elapsed += 16) {
            s_now += 16;
            /* A covered workspace is settled before every shell handler.
             * Its unrelated jobs must not postpone an overdue sheet frame. */
            orb_kit_motion_finish(covered);
            lv_timer_handler();
            if (!elapsed) check(!lv_obj_is_hidden(control) && orb_glass_get_amount(control) > 0 &&
                                orb_glass_get_amount(control) < 1,
                                "covered sibling starved the first material frame");
        }
        check(orb_glass_get_amount(control) == 1, "material entrance exact endpoint");
        lv_obj_delete(control);
    }
    lv_obj_delete(covered);
    orb_ui_set_reduce_motion(false);
}
static void material_reparent_delete(lv_obj_t *parent)
{
    /* The material clock is held by the parent at start time. Moving the
     * glass elsewhere and deleting it mid-entrance must still retire it. */
    lv_obj_t *other = lv_obj_create(lv_screen_active());
    lv_obj_t *control = orb_kit_disc_create(parent, ORB_KIT_DISC_KIND_MINI,
        ORB_KIT_NEUTRAL_GLASS, ORB_LABEL, ORB_ICON_APPS);
    check(orb_glass_materialise(control, true), "reparent: material entrance admitted");
    s_now += 16; lv_timer_handler();
    check(orb_glass_get_amount(control) < 1, "reparent: entrance still in flight");
    lv_obj_set_parent(control, other);
    lv_obj_delete(control);
    for (unsigned i = 0; i < 2; i++) { s_now += 16; lv_timer_handler(); }
    check(!orb_kit_motion_timer_running(), "reparent: deleted glass left its clock running");
    lv_obj_delete(other);
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    lv_display_t *display=sim_display_create(); orb_ui_init(NULL);
    lv_display_add_event_cb(display,rendered,LV_EVENT_ALL,NULL);
    lv_obj_t *parent=lv_obj_create(lv_screen_active()),*obj=lv_obj_create(parent);
    static const char key;
    check(orb_kit_arrival_prepare(obj,arrival,NULL),"arrival arms");
    advance(128); check(!s_arrivals,"waits for transition");
    orb_kit_arrival_complete(parent,true);
    check(s_arrivals==1&&s_animate,"fresh arrival starts once");
    orb_kit_arrival_complete(parent,true); check(s_arrivals==1,"no replay");
    orb_kit_arrival_prepare(obj,arrival,NULL); orb_kit_arrival_complete(parent,false);
    check(s_arrivals==2&&!s_animate&&s_value==1,"Back final immediately");
    orb_kit_arrival_prepare(obj,arrival,NULL); advance(1232);
    check(s_arrivals==3&&!s_animate,"stranded fallback bounded");
    orb_kit_arrival_prepare(obj,arrival,NULL); orb_kit_motion_set_active(parent,false);
    check(s_arrivals==4&&!s_animate,"covered arrival completes");
    orb_kit_motion_set_active(parent,true);
    s_value=0; check(orb_kit_motion_start(obj,&key,frame,NULL),"clock starts");
    check(orb_kit_motion_settling(parent), "finite presentation belongs to visual settlement");
    advance(64); check(s_value>0&&s_value<1,"intermediate presentation");
    orb_kit_motion_set_active(parent,false);
    check(s_value==1&&!orb_kit_motion_timer_running(),"dimmed/locked final and pause");
    orb_kit_motion_set_active(parent,true);
    orb_kit_motion_start(obj,&key,frame,NULL); orb_ui_set_reduce_motion(true);
    check(s_value==1&&!orb_kit_motion_timer_running(),"Reduce Motion immediate");
    orb_ui_set_reduce_motion(false);
    orb_kit_motion_start(obj,&key,frame,NULL);
    unsigned calls=s_calls; lv_obj_delete(obj); advance(64);
    check(s_calls==calls&&!orb_kit_motion_timer_running(),"delete cancels safely");
    obj=lv_obj_create(parent); orb_kit_motion_start(obj,&key,frame,NULL); advance(192);
    check(s_value==1&&!orb_kit_motion_count()&&!orb_kit_motion_timer_running(),"exact end and paused timer");
    advance(160); s_frames=s_damage=0; advance(1200);
    check(!s_frames&&!s_damage,"zero frames and invalidations at rest");
    orb_kit_motion_start(obj,&key,frame,NULL); orb_kit_motion_interrupt(parent);
    check(s_value==1&&!orb_kit_motion_timer_running(),"surface touch ends presentation");
    s_value=0; orb_kit_motion_start_feedback(obj,&key,frame,NULL); orb_kit_motion_interrupt(parent);
    check(s_value==0&&orb_kit_motion_timer_running(),"new input feedback survives interruption");
    advance(192); lv_obj_delete(parent);
    parent=lv_obj_create(lv_screen_active()); obj=lv_obj_create(parent);
    orb_kit_motion_start(obj,&key,delete_on_finish,obj); orb_kit_motion_finish(parent);
    check(!orb_kit_motion_count()&&!orb_kit_motion_timer_running(),"finish callback may delete last motion owner");
    press_cancel_restarts_at_rest(parent);
    material_first_paint(parent);
    material_reparent_delete(parent);
    obj = lv_obj_create(parent);
    orb_kit_motion_start_wait(obj, &key, frame, NULL);
    check(!orb_kit_motion_settling(parent) && orb_kit_motion_timer_running(),
          "a named wait must not hold navigation settlement open");
    lv_obj_delete(obj);
    lv_obj_set_hidden(parent, true);
    obj = lv_obj_create(parent);
    check(!orb_kit_motion_start(obj, &key, frame, NULL) && s_value == 1,
          "hidden effect shows final values without starting its clock");
    lv_obj_set_hidden(parent, false);
    s_now = UINT32_MAX - 500;
    orb_kit_arrival_prepare(obj, arrival, NULL);
    advance(1232);
    check(!s_animate && s_value == 1 && !orb_kit_motion_timer_running(),
          "arrival watchdog settles across tick wrap");
    lv_obj_delete(parent); printf("Alive clock: %u checks passed\n",s_checks); return 0;
}
