#ifndef ORB_ALIVE_PARITY
/* Real Home captures use synthetic models and a command-denying UI port.
 * Frames are exact 60 Hz virtual samples, never measurements of panel FPS. */
#include "home_harness.h"
#include "orb_kit_motion.h"
#include <errno.h>
#include <locale.h>
#include <sys/stat.h>
#include <time.h>

static uint32_t s_sample_pixels, s_sample_frames, s_sample_peak, s_refresh_pixels;
static bool s_scrolling;

time_t time(time_t *out)
{
    const time_t fixed = 1793182140; /* Same frozen wall clock as parity. */
    if (out) *out = fixed;
    return fixed;
}

static bool deny_command(void *context, const orb_home_command_t *command)
{
    (void)context; (void)command;
    home_check(false, "presentation attempted to send a Home command");
    return false;
}

static void frame_event(lv_event_t *event)
{
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START: s_refresh_pixels = 0; break;
    case LV_EVENT_FLUSH_START: {
        uint32_t pixels = lv_area_get_size(lv_event_get_param(event));
        s_sample_pixels += pixels;
        s_refresh_pixels += pixels;
        break;
    }
    case LV_EVENT_REFR_READY:
        if (s_refresh_pixels) {
            s_sample_frames++;
            s_sample_peak = LV_MAX(s_sample_peak, s_refresh_pixels);
        }
        s_refresh_pixels = 0;
        break;
    default: break;
    }
}

static void synthetic_home(void)
{
    static const char *const ids[] = {"study", "living", "kitchen", "hall", "bedroom", "spare"};
    static const char *const names[] = {"Study", "Living Room", "Kitchen", "Hall", "Bedroom", "Spare Room"};
    orb_home_snapshot_t *s = calloc(1, sizeof(*s));
    home_check(s != NULL, "allocate synthetic Home");
    s->room_count = 6;
    for (unsigned i = 0; i < 6; i++) {
        snprintf(s->rooms[i].id, sizeof(s->rooms[i].id), "%s", ids[i]);
        snprintf(s->rooms[i].name, sizeof(s->rooms[i].name), "%s", names[i]);
        orb_home_accessory_t *a = &s->accessories[i];
        snprintf(a->id, sizeof(a->id), "%s-light", ids[i]);
        snprintf(a->name, sizeof(a->name), "%s Lamp", names[i]);
        snprintf(a->room, sizeof(a->room), "%s", ids[i]);
        a->kind = ORB_HOME_LIGHT;
        a->reachable = true;
        a->writable = ORB_HOME_ON | ORB_HOME_BRIGHTNESS;
        a->state = (orb_home_state_t){.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS,
            .on = i == 0 || i == 2 || i == 5, .brightness = i == 0 ? 75 : 65};
    }
    s->accessory_count = 7;
    s->accessories[6] = (orb_home_accessory_t){.id = "climate", .name = "Thermostat", .room = "living",
        .kind = ORB_HOME_THERMOSTAT, .reachable = true, .writable = ORB_HOME_TARGET | ORB_HOME_MODE,
        .state = {.present = ORB_HOME_TEMP | ORB_HOME_TARGET | ORB_HOME_MODE,
            .temp = 20.5f, .target = 22.f, .mode = ORB_HOME_MODE_HEAT}};
    s->scene_count = 2;
    s->scenes[0] = (orb_home_scene_t){.id = "evening", .name = "Evening"};
    s->scenes[1] = (orb_home_scene_t){.id = "reading", .name = "Reading"};
    home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
        s->accessory_count, s->scenes, s->scene_count, true, false), "install synthetic Home");
    free(s);
    orb_home_set_connection(true, true);
}

static void initialize(const char *scenario, const char *mode)
{
    setenv("TZ", "UTC", 1); tzset(); setlocale(LC_ALL, "C"); srand(1330790992);
    lv_init(); lv_rand_set_seed(1330790992); s_home.now = 10000; lv_tick_set_cb(home_tick);
    s_home.display = sim_display_create();
    home_check(s_home.display != NULL, "create round display");
    orb_ui_init(NULL);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    home_check(orb_model_init() && orb_home_init(), "initialize local models");
    orb_prefs_t prefs; orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "fixture Fahrenheit units");
    if (!strcmp(mode, "ax3")) {
        home_check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK, "AX3 fonts");
        home_check(orb_fonts_set_bold_text(true) == ORB_FONTS_OK, "Bold Text fonts");
        orb_ui_set_reduce_transparency(true);
    } else if (!strcmp(mode, "rt")) orb_ui_set_reduce_transparency(true);
    else if (!strcmp(mode, "rm")) orb_ui_set_reduce_motion(true);
    orb_home_reset();
    if (!strcmp(scenario, "loading")) orb_home_set_connection(true, false);
    else synthetic_home();
    s_home.pointer = lv_indev_create();
    lv_indev_set_type(s_home.pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_home.pointer, s_home.display);
    lv_indev_set_read_cb(s_home.pointer, home_pointer_read);
    orb_home_commands_t commands = {.send = deny_command};
    s_home.view = orb_home_view_create(lv_screen_active(), &commands);
    home_check(s_home.view != NULL, "create real Home UI");
    lv_display_add_event_cb(s_home.display, home_display_event, LV_EVENT_ALL, NULL);
    lv_display_add_event_cb(s_home.display, frame_event, LV_EVENT_ALL, NULL);
}

static void prepare(const char *scenario)
{
    if (!strcmp(scenario, "rooms-arrival")) {
        home_advance(32); orb_home_view_arrival_prepare(s_home.view);
        lv_refr_now(s_home.display); orb_home_view_arrival_complete(s_home.view, true);
    } else if (!strcmp(scenario, "row-light") || !strcmp(scenario, "accessory-light") ||
               !strcmp(scenario, "detail-light")) {
        if (!strcmp(scenario, "accessory-light")) orb_home_view_room(s_home.view, "living");
        if (!strcmp(scenario, "detail-light")) orb_home_view_open(s_home.view, "living-light");
        home_advance(1400);
        home_check(orb_home_update("living-light", &(orb_home_state_t){.present=ORB_HOME_ON,.on=true},
            false, false, 0), "publish synthetic light-on change");
        orb_home_view_poll(s_home.view);
    } else if (!strcmp(scenario, "brightness") || !strcmp(scenario, "thermostat")) {
        home_advance(1400);
        orb_home_view_open(s_home.view, !strcmp(scenario, "brightness") ? "study-light" : "climate");
        home_advance(16); lv_refr_now(s_home.display); orb_home_view_arrival_complete(s_home.view, true);
    } else if (!strcmp(scenario, "scroll")) {
        home_advance(1400); s_scrolling = true;
    } else if (!strcmp(scenario, "page-dots-kit")) {
        home_advance(1400); lv_obj_set_hidden(orb_home_view_root(s_home.view), true);
        /* Home has no approved page dots; this dedicated kit fixture avoids
         * adding unapproved navigation to a product surface (HIG §7). */
        lv_obj_t *dots = orb_kit_page_dots_create(lv_screen_active(), 4);
        lv_refr_now(s_home.display); orb_kit_page_dots_set_active(dots, 1, ORB_KIT_HOME);
    } else if (!strcmp(scenario, "scene-requested")) {
        orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
        state.scene_page = true; orb_home_view_restore(s_home.view, &state); home_advance(1400);
        home_check(orb_home_scene("evening"), "stage local synthetic scene request");
        orb_home_view_poll(s_home.view); home_advance(200);
        orb_home_request_t request;
        home_check(orb_home_take_request(&request), "take local synthetic scene request");
        orb_home_complete(&request, NULL, NULL); orb_home_view_poll(s_home.view); home_advance(0);
    } else home_check(!strcmp(scenario, "loading"), "known capture scenario");
}

static void capture(const char *directory, const char *scenario)
{
    home_check(mkdir(directory,0755)==0 || errno==EEXIST,"create capture directory");
    char path[1024]; snprintf(path,sizeof(path),"%s/frames.csv",directory);
    FILE *metrics=fopen(path,"w"); home_check(metrics!=NULL,"open capture metrics");
    /* A sample may contain multiple LVGL refreshes. Keep both measurements so
     * per-frame damage is never confused with the 60 Hz PNG sampling rate. */
    fprintf(metrics,"frame,virtual_ms,flushed_pixels,rendered_frames,max_frame_pixels\n");
    lv_refr_now(s_home.display); home_measure_start();
    uint32_t start=s_home.now;
    unsigned count=!strcmp(scenario,"loading")?240:90;
    uint64_t total_pixels = 0;
    for (unsigned frame=0; frame<=count; frame++) {
        s_sample_pixels = s_sample_frames = s_sample_peak = 0;
        if (s_scrolling && frame<25) {
            lv_obj_t *list=orb_home_view_scroll(s_home.view);
            home_check(list!=NULL,"real Home exposes scroll owner");
            if (!orb_round_list_scroll_to(list,(int32_t)frame*9+1))
                lv_obj_scroll_to_y(list,(int32_t)frame*9+1,LV_ANIM_OFF);
        }
        uint32_t target=start+frame*1000/60;
        home_advance(target-s_home.now);
        snprintf(path,sizeof(path),"%s/%03u.png",directory,frame);
        home_check(sim_display_save_png(path),"save virtual 60 Hz sample");
        fprintf(metrics,"%u,%u,%u,%u,%u\n",frame,target-start,s_sample_pixels,
            s_sample_frames,s_sample_peak);
        total_pixels += s_sample_pixels;
    }
    home_measure_stop(); home_check(fclose(metrics)==0,"close capture metrics");
    printf("alive %-18s frames=%u mean_flushed_px=%u max_flushed_px=%u max_invalidation_px=%u\n",
        scenario,s_home.frames,s_home.frames?(unsigned)(total_pixels/s_home.frames):0,
        s_home.largest_frame,s_home.largest_damage);
}

int main(int argc,char **argv)
{
    if (argc<3 || argc>4) {
        fprintf(stderr,"usage: orb_alive_shots OUT SCENARIO [default|ax3|rt|rm]\n");
        return 2;
    }
    initialize(argv[2],argc==4?argv[3]:"default"); prepare(argv[2]); capture(argv[1],argv[2]);
    home_shutdown(); return EXIT_SUCCESS;
}

#else
#include "alive_os_shots.h"
#endif
