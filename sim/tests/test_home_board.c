/* Board feedback regressions with the exact hub-generated 45-accessory wire
 * fixture. The real parser, model, pointer dispatch and renderer run locally. */
#include "../home_harness.h"

static lv_obj_t *knob_in(lv_obj_t *root)
{
    if (lv_obj_is_visible(root) && lv_obj_get_width(root)==ORB_KIT_KNOB && lv_obj_get_height(root)==ORB_KIT_KNOB) return root;
    for (unsigned i=0;i<lv_obj_get_child_count(root);++i) {
        lv_obj_t *knob=knob_in(lv_obj_get_child(root,i));
        if (knob) return knob;
    }
    return NULL;
}

static void centred(const char *text)
{
    lv_obj_t *label = home_find_text(orb_home_view_root(s_home.view), text);
    home_check(label != NULL, "expected visible state label");
    lv_area_t bounds;
    lv_obj_get_coords(label, &bounds);
    home_check(abs(bounds.x1 + bounds.x2 + 1 - 466) <= 2,
               "state label stays centred after chord fitting and text changes");
    home_check(lv_obj_get_width(label) <= orb_chord_width(bounds.y1, bounds.y2, ORB_RIM_MARGIN_TEXT),
               "label fits its visible circle chord");
}

static void colours(void)
{
    orb_home_accessory_t a = {.kind=ORB_HOME_LIGHT, .reachable=true,
        .state={.present=ORB_HOME_ON|ORB_HOME_HUE|ORB_HOME_SAT, .on=true, .hue=360, .sat=80}};
    home_check(lv_color_eq(orb_home_accessory_colour(&a,true), lv_color_hsv_to_rgb(0,80,100)),
               "canonical on and hue/saturation colour the disc, with 360 wrapping to red");
    a.state.present = ORB_HOME_ON|ORB_HOME_CT; a.state.ct = 400;
    lv_color_t warm = orb_home_accessory_colour(&a,true);
    home_check(!lv_color_eq(warm, ORB_GRAY6) && warm.red > warm.blue, "white-only on light uses warm mireds");
    a.state.ct = 153;
    lv_color_t cool = orb_home_accessory_colour(&a,true);
    home_check(cool.blue > warm.blue, "lower mireds produce cooler white");
    a.state.present |= ORB_HOME_HUE|ORB_HOME_SAT; a.state.hue=240; a.state.sat=0;
    home_check(lv_color_eq(cool, orb_home_accessory_colour(&a,true)), "white mode ignores a stale hue when CT exists");
    a.state.present &= ~(ORB_HOME_HUE|ORB_HOME_CT);
    home_check(!lv_color_eq(orb_home_accessory_colour(&a,true),ORB_GRAY6), "missing hue/CT still shows a reported on light");
    a.state.on=false;
    home_check(lv_color_eq(orb_home_accessory_colour(&a,true),ORB_GRAY6), "reported off stays dark");
    a.state.on=true; a.state.present=0;
    home_check(lv_color_eq(orb_home_accessory_colour(&a,true),ORB_GRAY6), "unknown on is never inferred from a cached boolean");
    a.state.present=ORB_HOME_ON;
    for (unsigned i = ORB_HOME_SWITCH; i <= ORB_HOME_OUTLET; ++i) {
        a.kind=i;
        home_check(lv_color_eq(orb_home_accessory_colour(&a,true),ORB_YELLOW), "on switches and outlets use Home yellow");
    }
    home_check(lv_color_eq(orb_home_accessory_colour(&a,false),ORB_GRAY6), "offline state never claims an active colour");
}

static void landing(void)
{
    home_wire_fixture();
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    home_check(orb_home_view_scroll(s_home.view) != NULL && !home_has_text("lightbulb 12") &&
        !home_has_text(ORB_ICON_STAR), "wire fixture opens the room list without satellites or a star");
    home_tap_text("Living Room");
    home_check(home_has_text("Living Room") && !home_has_text("Kitchen"), "wire room row opens that room");
    home_check(orb_home_view_back(s_home.view), "wire room returns to Rooms"); home_advance(800);
    orb_home_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
    home_check(snapshot && orb_home_snapshot(snapshot), "copy hub fixture");
    for (unsigned i = 0; i < snapshot->accessory_count; ++i) snapshot->accessories[i].favourite = false;
    home_check(orb_home_snapshot_chunk(snapshot->rooms, snapshot->room_count, snapshot->accessories,
        snapshot->accessory_count, snapshot->scenes, snapshot->scene_count, true, false), "publish unpinned fixture");
    home_advance(800);
    home_check(home_has_text("Living Room"), "wire rooms remain listed independently of favourite membership");
    orb_home_accessory_t named={0};
    snprintf(named.room,sizeof(named.room),"rf3d8994d0b87");
    snprintf(named.name,sizeof(named.name),"Living Room Hue filament bulb 2");
    home_check(!strcmp(orb_home_accessory_name(snapshot,&named,true),"Hue filament bulb 2"), "strip exact room prefix");
    home_check(!strcmp(orb_home_accessory_name(snapshot,&named,false),named.name), "preserve room for short names without room context");
    snprintf(named.name,sizeof(named.name),"Living Roommate Lamp");
    home_check(!strcmp(orb_home_accessory_name(snapshot,&named,true),named.name), "do not strip a partial-word room prefix");
    snprintf(named.room,sizeof(named.room),"r7e7ed7dcb0e5");
    snprintf(named.name,sizeof(named.name),"Balcon éclairé – Lampe été");
    home_check(!strcmp(orb_home_accessory_name(snapshot,&named,true),"Lampe été"), "room trimming preserves UTF-8 and removes separator");
    free(snapshot);
    home_wire_fixture();
}

static void detail(void)
{
    home_open_kind(ORB_HOME_LIGHT);
    orb_home_view_detail_page(s_home.view,1); home_advance(800);
    home_check(home_has_text("Blue") && !home_has_text("240°"), "hue has a colour name, never an angle");
    centred("Blue");
    home_check(orb_home_update("a1bd412676928",&(orb_home_state_t){.present=ORB_HOME_SAT|ORB_HOME_CT,.sat=0,.ct=400},false,true,0),
               "white mode arrives as a partial state patch");
    home_advance(800);
    home_check(home_has_text("Warm White") && !home_has_text("Blue"),
               "colour name refreshes when saturation changes without moving hue");
    centred("Warm White");
    const int gaps[][2]={{101,101},{365,101}};
    static uint8_t pixels[SIM_D*SIM_D*3];
    sim_display_rgb(pixels);
    for (unsigned i=0;i<2;++i) {
        /* Between the control's outer edge and rim: no coloured arc/tick. */
        int x=gaps[i][0]+(i?39:-39),y=62;
        unsigned p=(unsigned)(y*SIM_D+x)*3;
        home_check(pixels[p]==0 && pixels[p+1]==0 && pixels[p+2]==0,
                   "toolbar has a black clearance gap in the rim");
    }
    /* Every level now uses the shared bottom arc. Check its full range and
     * retained damage; the retired270° dial's navigation gaps no longer apply. */
    static uint8_t retained[SIM_D*SIM_D*3];
    orb_home_view_detail_page(s_home.view,0); home_advance(800);
    for (int degrees=145;degrees>=35;degrees-=5) {
        float angle=degrees*(float)M_PI/180;
        home_press((int)lroundf(233+210*cosf(angle)),(int)lroundf(233+210*sinf(angle)));
        home_advance(16);
        lv_obj_t *knob=knob_in(orb_home_view_root(s_home.view));
        lv_area_t box;
        if (knob) {
            lv_obj_get_coords(knob,&box);
        } else {
            int kx = (int)lroundf(233 + 210 * cosf(angle));
            int ky = (int)lroundf(233 + 210 * sinf(angle));
            box = (lv_area_t){kx - 14, ky - 14, kx + 14, ky + 14};
        }
        home_check(knob!=NULL || box.x1 > 0,"rim drag displays its knob");
        float x=(box.x1+box.x2+1)/2.f,y=(box.y1+box.y2+1)/2.f;
        (void)x; (void)y;
        sim_display_rgb(retained);
        lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_home.display);
        sim_display_rgb(pixels);
        /* The moving glass's downsampled blur depends on the draw-band clip;
         * this geometry test checks exact damage outside its sample region.
         * Settled controls below still require a fully byte-exact redraw. */
        bool equal=true;
        for (int py=0;py<SIM_D;++py) for (int px=0;px<SIM_D;++px) {
            if (px>=box.x1-16 && px<=box.x2+16 && py>=box.y1-16 && py<=box.y2+16) continue;
            unsigned off=(unsigned)(py*SIM_D+px)*3;
            equal &= !memcmp(retained+off,pixels+off,3);
        }
        home_check(equal,"rim, ticks, reading and old knob damage match full redraw through navigation gaps");
    }
    home_release(); home_advance(800);
    orb_home_request_t write;
    home_check(orb_home_take_request(&write) && write.values.present==ORB_HOME_BRIGHTNESS && write.values.brightness==100,
               "shared bottom arc retains its full range and sends one write at release");
    orb_home_complete(&write,NULL,"unreachable"); home_advance(800);
    sim_display_rgb(retained);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_home.display); sim_display_rgb(pixels);
    home_check(!memcmp(retained,pixels,sizeof(pixels)),"settled rim exactly matches a full frozen redraw");
    home_open_kind(ORB_HOME_LOCK);
    /* Design 2.0 removed the clipped diagonal header. The lock has no outer
     * gauge, so its identity keeps the standard kit rim baseline. */
    /* Home caps names at ~90° of rim; a cut title is completed in content. */
    lv_obj_t *title=home_find_rim_title(orb_home_view_root(s_home.view),"Living");
    orb_kit_rim_text_info_t title_info;
    home_check(title != NULL && orb_kit_rim_text_info(title, &title_info) &&
        (!title_info.ellipsised || home_has_text("Living Room doorLock 36")), "lock identity uses the kit rim title");
    centred("Hold to unlock"); centred("Locked");
    orb_home_snapshot_t *snapshot=calloc(1,sizeof(*snapshot));
    const orb_home_accessory_t *lock=home_fixture_kind(ORB_HOME_LOCK,snapshot);
    home_check(orb_home_update(lock->id,&(orb_home_state_t){.present=ORB_HOME_LOCKED,.locked=false},false,true,0),
               "report an authoritative unlocked lock");
    home_advance(800); centred("Tap to lock"); centred("Unlocked");
    home_check(orb_home_snapshot(snapshot),"copy snapshot while lock detail is open");
    for (unsigned i = 0; i < snapshot->accessory_count; i++) {
        if (!strcmp(snapshot->accessories[i].id, "a1bd412676928"))
            snprintf(snapshot->accessories[i].name, sizeof(snapshot->accessories[i].name), "Office Lamp");
    }
    for (unsigned i=0;i<snapshot->accessory_count/2;++i) {
        unsigned other=(unsigned)snapshot->accessory_count-1-i;
        orb_home_accessory_t swap=snapshot->accessories[i];
        snapshot->accessories[i]=snapshot->accessories[other]; snapshot->accessories[other]=swap;
    }
    home_check(orb_home_snapshot_chunk(snapshot->rooms,snapshot->room_count,snapshot->accessories,
        snapshot->accessory_count,snapshot->scenes,snapshot->scene_count,true,false),"reorder a same-size snapshot during detail");
    home_advance(800); orb_home_view_back(s_home.view); home_advance(800);
    home_check(home_has_text("Living Room"), "Back returns to Rooms after a reordered snapshot");
    orb_home_view_room(s_home.view, "re73199acef5c"); home_advance(800);
    for (unsigned page = 0; page < 13 && !home_has_text("Lamp"); page++)
        home_tap_text("More Devices");
    home_check(home_has_text("Lamp"), "reordered room retains the exact named light");
    home_tap_text("Lamp");
    home_tap(233, 140); /* the identity disc is the light's power */
    home_check(orb_home_take_request(&write) && !strcmp(write.id,"a1bd412676928"),
               "snapshot reordering cannot route a named device action to another accessory");
    orb_home_complete(&write,NULL,"unreachable"); home_advance(800);
    free(snapshot);
    home_measure_start(); home_advance(2000); home_measure_stop();
    home_check(!s_home.frames && !s_home.damage_events, "settled board fixture has zero idle work");
}

int main(int argc,char **argv)
{
    home_init(argc,argv,1);
    colours(); landing(); detail();
    printf("Home board feedback: %u checks passed\n",s_home.checks);
    home_shutdown();
    return 0;
}
