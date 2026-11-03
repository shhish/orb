/* The actual kit and banded renderer, deterministic virtual 60 Hz. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_kit.h"
#include "orb_kit_controls_motion.h"
#include "orb_kit_motion.h"
#include "sim_display.h"

static uint32_t s_now, s_frames, s_damage, s_peak, s_total, s_samples;
static unsigned s_checks, s_detents;
static bool s_measure;
static uint32_t tick(void) { return s_now; }
static void check(bool valid, const char *why)
{
    s_checks++;
    if (valid) return;
    fprintf(stderr, "alive controls: %s\n", why);
    exit(EXIT_FAILURE);
}
static void panel(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_FLUSH_START:
        if (s_measure) s_damage += lv_area_get_size(lv_event_get_param(e));
        break;
    case LV_EVENT_REFR_READY:
        if (s_damage) { s_peak=LV_MAX(s_peak,s_damage); s_total+=s_damage; s_samples++; }
        s_damage=0; break;
    case LV_EVENT_RENDER_READY: s_frames++; break;
    default: break;
    }
}
static void advance(unsigned duration)
{
    for (unsigned i=0; i<duration; i+=16) { s_now+=16; lv_timer_handler(); }
}
static void detent(lv_obj_t *obj, int32_t value, void *context)
{
    (void)obj; (void)value; (void)context; s_detents++;
}
static lv_obj_t *value_label(lv_obj_t *obj)
{
    for (unsigned i=0; i<lv_obj_get_child_count(obj); i++) {
        lv_obj_t *child=lv_obj_get_child(obj,i);
        if (lv_obj_check_type(child,&lv_label_class)) return child;
    }
    return NULL;
}
static void quiet(void)
{
    advance(1500);
    check(!orb_kit_motion_timer_running(),"shared timer still running after settling");
    check(orb_kit_motion_count()==0,"settled motion jobs retained");
    unsigned frames=s_frames;
    advance(1600);
    check(s_frames==frames,"settled controls rendered idle frames");
}
static void sweep_follow(lv_obj_t *root)
{
    lv_obj_t *level=orb_kit_level_create(root,ORB_KIT_HOME,0,100,1,detent,NULL);
    check(level!=NULL,"level creation failed");
    orb_kit_level_set_value(level,25,false);
    lv_obj_t *label=value_label(level);
    check(label && !strcmp(lv_label_get_text(label),"25%"),"initial caption differs");
    orb_kit_level_set_value(level,80,true);
    check(orb_kit_rim_slider_get_value(level)==80,"animated getter reports false target");
    check(orb_kit_ring_get_presented_value(level)==25,"Follow jumped to endpoint");
    advance(80);
    float progress=orb_kit_ring_get_presented_value(level);
    check(progress>25 && progress<80,"Follow has no intermediate progress");
    check(atoi(lv_label_get_text(label))==(int)lroundf(progress),"number does not follow arc");
    const char *semantic=NULL; int32_t target;
    check(orb_kit_ring_semantic_value(level,&target) && target==80,"semantic arc target false");
    check(orb_kit_ring_semantic_text(label,&semantic) && !strcmp(semantic,"80%"),"semantic number target false");
    orb_kit_level_set_value(level,35,true);
    check(fabsf(orb_kit_ring_get_presented_value(level)-progress)<.001f,"retarget jumped");
    advance(320);
    check(orb_kit_ring_get_presented_value(level)==35 && !strcmp(lv_label_get_text(label),"35%"),"Follow exact endpoint wrong");
    check(s_detents==0,"animation fired detent");
    orb_kit_level_set_arrival(level,75);
    check(orb_kit_rim_slider_get_value(level)==75 && orb_kit_ring_get_presented_value(level)==0,"arrival snapshot start or target wrong");
    lv_refr_now(lv_display_get_default());
    s_peak=s_total=s_samples=0; s_measure=true;
    orb_kit_arrival_complete(root,true); advance(112);
    check(orb_kit_ring_get_presented_value(level)>0 && orb_kit_ring_get_presented_value(level)<75,"Sweep has no intermediate progress");
    advance(320); s_measure=false;
    check(orb_kit_ring_get_presented_value(level)==75,"Sweep exact endpoint wrong");
    printf("alive sweep: max_flushed_px=%u mean_flushed_px=%u frames=%u\n",s_peak,s_samples?s_total/s_samples:0,s_samples);
    check(s_peak<100000,"Sweep dirtied unnecessarily large square");
    check(s_detents==0,"Sweep fired detent"); quiet(); lv_obj_delete(level);
}
static void repeated_arrival_target(lv_obj_t *root)
{
    lv_obj_t *level=orb_kit_level_create(root,ORB_KIT_SOUND,0,100,1,detent,NULL);
    unsigned detents=s_detents;
    orb_kit_level_set_value(level,25,false);
    orb_kit_level_set_arrival(level,60);
    orb_kit_level_set_value(level,60,true);
    check(orb_kit_rim_slider_get_value(level)==60 && orb_kit_ring_get_presented_value(level)==0,
        "same-target animated projection consumed staged Sweep");
    orb_kit_arrival_complete(root,true); advance(80);
    float shown=orb_kit_ring_get_presented_value(level);
    check(shown>0 && shown<60,"preserved staged Sweep has no intermediate progress");
    orb_kit_level_set_value(level,60,true);
    check(orb_kit_ring_get_presented_value(level)==shown,"same-target projection jumped active Sweep");
    advance(400);
    check(orb_kit_ring_get_presented_value(level)==60,"preserved Sweep missed its endpoint");
    orb_kit_level_set_arrival(level,75);
    orb_kit_level_set_value(level,75,false);
    check(orb_kit_ring_get_presented_value(level)==75,"explicit final setter did not settle staged Sweep");
    orb_kit_arrival_complete(root,true);
    check(!orb_kit_motion_timer_running(),"explicit final setter allowed late arrival replay");
    check(s_detents==detents,"repeated presentation values fired detents");
    quiet(); lv_obj_delete(level);
}

static void interruption(lv_obj_t *root)
{
    lv_obj_t *level=orb_kit_level_create(root,ORB_KIT_HOME,0,100,1,detent,NULL);
    unsigned detents=s_detents;
    orb_kit_level_set_arrival(level,90); orb_kit_arrival_complete(root,true); advance(64);
    lv_obj_send_event(level,LV_EVENT_PRESSED,NULL);
    check(orb_kit_ring_get_presented_value(level)==90 && orb_kit_rim_slider_get_value(level)==90,"touch did not take over true value");
    check(s_detents==detents,"touch finishing sweep fired detent");
    check(!orb_kit_motion_timer_running(),"touch left sweep timer running");
    orb_kit_level_set_arrival(level,80); orb_kit_arrival_complete(root,true); advance(48);
    uint32_t key=LV_KEY_RIGHT; lv_obj_send_event(level,LV_EVENT_KEY,&key);
    check(orb_kit_rim_slider_get_value(level)==81,"rim/key took over presentation instead of true target");
    check(s_detents==detents+1,"real input did not fire one detent");
    orb_kit_level_set_arrival(level,60); orb_kit_arrival_complete(root,true); advance(32);
    lv_obj_delete(level); quiet();
    lv_obj_t *ring=orb_kit_ring_create(root,60,8,ORB_KIT_HOME);
    orb_kit_ring_set_value(ring,20,false); orb_kit_ring_set_value(ring,85,true); advance(32);
    check(orb_kit_ring_get_presented_value(ring)<85,"plain ring has no active travel");
    lv_obj_send_event(ring,LV_EVENT_PRESSED,NULL);
    check(orb_kit_ring_get_presented_value(ring)==85,"plain ring press did not finish travel");
    lv_obj_delete(ring);
    lv_obj_t *toggle=orb_kit_switch_create(root,false,ORB_KIT_HOME);
    orb_kit_switch_set_checked(toggle,true,true); advance(32);
    lv_obj_t *knob=lv_obj_get_child(toggle,0);
    check(lv_obj_get_x(knob)>4 && lv_obj_get_x(knob)<38,"switch has no active travel");
    lv_obj_send_event(toggle,LV_EVENT_PRESSED,NULL);
    check(orb_kit_switch_get_checked(toggle),"switch press changed its true target");
    lv_obj_update_layout(toggle);
    check(lv_obj_get_x(knob)==38,"switch press did not finish target geometry");
    orb_kit_press_cancel(toggle); lv_obj_delete(toggle); quiet();
}
static void arrivals(lv_obj_t *root)
{
    lv_obj_t *level=orb_kit_level_create(root,ORB_KIT_HOME,0,100,1,detent,NULL);
    orb_kit_level_set_arrival(level,0);
    check(!orb_kit_motion_timer_running(),"zero arrival started invisible motion");
    orb_kit_level_set_arrival(level,70); orb_kit_arrival_complete(root,false);
    check(orb_kit_ring_get_presented_value(level)==70,"Back replayed arrival");
    check(!orb_kit_motion_timer_running(),"Back retained arrival timer");
    orb_kit_level_set_arrival(level,65); advance(1216);
    check(orb_kit_ring_get_presented_value(level)==65,"stranded arrival never reached final");
    check(!orb_kit_motion_timer_running(),"stranded fallback timer still running");
    orb_kit_level_set_arrival(level,85); orb_kit_motion_set_active(root,false);
    check(orb_kit_ring_get_presented_value(level)==85 && !orb_kit_motion_timer_running(),"covered/dimmed retained start state");
    orb_kit_motion_set_active(root,true); orb_kit_level_set_value(level,20,true); advance(64);
    orb_ui_set_reduce_motion(true); advance(16);
    check(orb_kit_ring_get_presented_value(level)==20,"Reduce Motion did not settle Follow");
    orb_kit_level_set_arrival(level,95);
    check(orb_kit_ring_get_presented_value(level)==95 && !orb_kit_motion_timer_running(),"Reduce Motion armed Sweep");
    orb_ui_set_reduce_motion(false); lv_obj_delete(level); quiet();
}
static void indicators(lv_obj_t *root)
{
    lv_obj_t *list=lv_obj_create(root); lv_obj_remove_style_all(list);
    lv_obj_set_size(list,466,466); lv_obj_set_scroll_dir(list,LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *filler=lv_obj_create(list); lv_obj_remove_style_all(filler); lv_obj_set_size(filler,300,1000);
    lv_obj_t *arc=orb_kit_scroll_arc_attach(list); check(arc!=NULL,"scroll indicator missing");
    check(lv_obj_get_style_opa(arc,0)==0,"staged indicator flashed before arrival");
    orb_kit_arrival_complete(root,true);
    advance(64);
    check(lv_obj_get_style_arc_width(arc,LV_PART_INDICATOR)==4,"arrival flash grew without scroll");
    check(lv_obj_get_style_opa(arc,0)>0 && lv_obj_get_style_opa(arc,0)<255,"arrival flash did not fade");
    advance(64); check(lv_obj_get_style_opa(arc,0)==0,"arrival flash never faded");
    lv_obj_scroll_to_y(list,80,LV_ANIM_OFF); advance(48);
    check(lv_obj_get_style_opa(arc,0)==0,"restoring a scroll position replayed feedback");
    orb_kit_scroll_arc_input(list);
    lv_obj_scroll_to_y(list,160,LV_ANIM_OFF); advance(48);
    check(lv_obj_get_style_arc_width(arc,LV_PART_INDICATOR)>4,"indicator did not grow");
    check(lv_obj_get_style_opa(arc,0)>0,"indicator never appeared");
    advance(800);
    check(lv_obj_get_style_opa(arc,0)==0 && lv_obj_get_style_arc_width(arc,LV_PART_INDICATOR)==4,"indicator did not return to rest");
    quiet(); unsigned frames=s_frames; lv_obj_send_event(list,LV_EVENT_SCROLL,NULL); advance(32);
    check(s_frames==frames && !orb_kit_motion_timer_running(),"unchanged scroll restarted indicator");
    orb_kit_scroll_arc_arrival_prepare(list);
    orb_kit_arrival_complete(root,false);
    advance(32);
    check(lv_obj_get_style_opa(arc,0)==0 && !orb_kit_motion_timer_running(),"Back replayed scroll arrival");
    orb_kit_scroll_arc_arrival_prepare(list);
    advance(1300);
    check(!orb_kit_motion_timer_running(),"stranded scroll arrival kept its timer running");
    orb_kit_arrival_complete(root,true);
    check(lv_obj_get_style_opa(arc,0)==0 && !orb_kit_motion_timer_running(),"expired scroll arrival replayed late");
    orb_kit_scroll_arc_arrival_prepare(list);
    orb_kit_motion_set_active(root,false);
    check(!orb_kit_motion_timer_running(),"covered pending indicator retained its arrival clock");
    orb_kit_motion_set_active(root,true);
    orb_kit_arrival_complete(root,true);
    check(lv_obj_get_style_opa(arc,0)==0 && !orb_kit_motion_timer_running(),"covered arrival replayed on restoration");
    lv_obj_delete(arc); check(orb_kit_scroll_arc_attach(list)!=NULL,"indicator reattach failed"); lv_obj_delete(list);
    orb_ui_set_reduce_motion(true);
    list=lv_obj_create(root); lv_obj_remove_style_all(list); lv_obj_set_size(list,466,466);
    lv_obj_set_scroll_dir(list,LV_DIR_VER); lv_obj_set_scrollbar_mode(list,LV_SCROLLBAR_MODE_OFF);
    filler=lv_obj_create(list); lv_obj_remove_style_all(filler); lv_obj_set_size(filler,300,1000);
    arc=orb_kit_scroll_arc_attach(list);
    orb_kit_arrival_complete(root,true);
    check(lv_obj_get_style_opa(arc,0)==0 && !orb_kit_motion_timer_running(),"Reduce Motion arrival did not show quiet final indicator");
    orb_kit_scroll_arc_input(list);
    lv_obj_scroll_to_y(list,160,LV_ANIM_OFF);
    check(lv_obj_get_style_opa(arc,0)==0 && !orb_kit_motion_timer_running(),"Reduce Motion scroll started indicator motion");
    lv_obj_delete(list); orb_ui_set_reduce_motion(false);
    lv_obj_t *dots=orb_kit_page_dots_create(root,4); lv_obj_update_layout(dots);
    check(!orb_kit_motion_timer_running() && lv_obj_get_width(lv_obj_get_child(dots,0))==20,
        "rebuilt page dots replayed initial stretch");
    lv_obj_t *first=lv_obj_get_child(dots,0);
    int32_t first_center=lv_obj_get_x(first)+lv_obj_get_width(first)/2;
    orb_kit_page_dots_set_active(dots,1,ORB_KIT_HOME); advance(80); lv_obj_update_layout(dots);
    int32_t width=lv_obj_get_width(lv_obj_get_child(dots,1));
    check(width>8 && width<20,"page dot no intermediate stretch");
    check(lv_obj_get_x(first)+lv_obj_get_width(first)/2<first_center,"dot neighbour centre did not make room");
    check(lv_obj_get_x(first)+lv_obj_get_width(first)+5<=lv_obj_get_x(lv_obj_get_child(dots,1)),
        "stretching page capsules collided");
    advance(400); lv_obj_update_layout(dots);
    check(lv_obj_get_width(lv_obj_get_child(dots,1))==20,"page capsule exact endpoint wrong");
    orb_kit_page_dots_set_active(dots,2,ORB_KIT_HOME); advance(32); lv_obj_delete(dots); quiet();
}
int main(void)
{
    lv_init(); lv_tick_set_cb(tick); check(sim_display_create()!=NULL,"display creation failed");
    lv_display_add_event_cb(lv_display_get_default(),panel,LV_EVENT_ALL,NULL); orb_ui_init(NULL);
    lv_obj_t *root=lv_screen_active(); lv_obj_set_scrollable(root,false);
    lv_obj_set_style_bg_color(root,lv_color_black(),0); lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    sweep_follow(root); repeated_arrival_target(root); interruption(root); arrivals(root); indicators(root);
    printf("alive controls: %u checks passed\n",s_checks); return EXIT_SUCCESS;
}
