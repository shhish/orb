/* Alive is presentation: the real Home tree uses only a synthetic local model. */
#include "../home_harness.h"
#include "orb_home_view_private.h"
#include "orb_home_detail_private.h"
#include "orb_kit_motion.h"
#include "orb_shell.h"
#include "../../firmware/components/orb_shell/orb_home_host.h"
#include "src/core/lv_obj_raster_private.h"

static uint8_t s_before[SIM_D * SIM_D * 3], s_after[sizeof(s_before)];

static void fixture(void)
{
    orb_home_room_t rooms[6] = {0};
    orb_home_accessory_t accessories[9] = {0};
    for (unsigned i = 0; i < 6; i++) {
        snprintf(rooms[i].id, sizeof(rooms[i].id), "r%u", i);
        snprintf(rooms[i].name, sizeof(rooms[i].name), "Room %u", i + 1);
        snprintf(accessories[i].id, sizeof(accessories[i].id), "lamp%u", i);
        snprintf(accessories[i].name, sizeof(accessories[i].name), "Lamp %u", i + 1);
        snprintf(accessories[i].room, sizeof(accessories[i].room), "r%u", i);
        accessories[i].kind = ORB_HOME_LIGHT;
        accessories[i].reachable = true;
        accessories[i].writable = ORB_HOME_ON | ORB_HOME_BRIGHTNESS;
        accessories[i].state = (orb_home_state_t){.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS,
            .on = i != 1, .brightness = 72};
    }
    accessories[6] = (orb_home_accessory_t){.id = "thermostat", .name = "Thermostat", .room = "r0",
        .kind = ORB_HOME_THERMOSTAT, .reachable = true, .writable = ORB_HOME_TARGET | ORB_HOME_MODE,
        .state = {.present = ORB_HOME_TARGET | ORB_HOME_MODE | ORB_HOME_TEMP,
            .target = 21.5f, .temp = 20, .mode = ORB_HOME_MODE_HEAT}};
    for (unsigned i = 7; i < 9; i++) {
        accessories[i] = accessories[0];
        snprintf(accessories[i].id, sizeof(accessories[i].id), "extra%u", i);
        snprintf(accessories[i].name, sizeof(accessories[i].name), "Extra lamp %u", i);
    }
    orb_home_scene_t scene = {.id = "evening", .name = "Evening"};
    home_check(orb_home_snapshot_chunk(rooms, 6, accessories, 9, &scene, 1, true, false), "publish synthetic fixture");
    orb_home_set_connection(true, true);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS);
    home_advance(800);
}

static void still(void)
{
    home_advance(1600);
    home_measure_start(); home_advance(800); home_measure_stop();
    home_check(s_home.frames == 0 && s_home.damage_events == 0, "settled Home has zero frames and invalidations");
    home_check(!orb_kit_motion_timer_running(), "settled kit clock is paused");
}

static void frozen_pixels(void)
{
    lv_refr_now(s_home.display); sim_display_rgb(s_before);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_home.display); sim_display_rgb(s_after);
    home_check(!memcmp(s_before, s_after, sizeof(s_before)),
        "partial rendering matches a full redraw at the identical motion sample");
}

static bool rolling_text(lv_obj_t *root, const char *part)
{
    const char *text = NULL;
    if (orb_kit_value_roll_semantic_text(root, &text) && strstr(text, part)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (rolling_text(lv_obj_get_child(root, i), part)) return true;
    return false;
}

static void preview_start_matches_live(void)
{
    /* Existing synthetic fixture presenters use F; the endpoint test uses C. */
    orb_prefs_t prefs; orb_model_prefs_defaults(&prefs); prefs.temperature = ORB_TEMPERATURE_F;
    home_check(orb_model_set_prefs(&prefs), "set explicit snapshot fixture locale"); home_advance(800);
    static uint8_t live_pixels[SIM_D * SIM_D * 2], preview_pixels[sizeof(live_pixels)];
    orb_home_snapshot_t *model = calloc(1, sizeof(*model));
    home_check(model && orb_home_snapshot(model), "copy snapshot integration fixture");
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent); lv_obj_set_size(parent, SIM_D, SIM_D);
    orb_app_host_t *preview = orb_home_host_create(true, NULL);
    home_check(preview && orb_home_host_attach(preview, parent,
        &(orb_app_home_model_t){.hour = 19, .fixture = model}), "construct suspended Home navigation preview");
    orb_screen_instance_t *instance = orb_app_host_current(preview);
    home_check(orb_screen_visibility_get(instance) == ORB_VISIBILITY_SUSPENDED,
        "snapshot-only Home stays suspended and cannot act");
    unsigned jobs = orb_kit_motion_count();
    orb_app_home_arrival_snapshot(instance);
    home_check(orb_kit_motion_count() == jobs, "snapshot start preparation admits no animation job");
    orb_home_view_arrival_prepare(s_home.view);
    lv_draw_buf_t live, frozen;
    home_check(lv_draw_buf_init(&live, SIM_D, SIM_D, LV_COLOR_FORMAT_RGB565, SIM_D * 2,
        live_pixels, sizeof(live_pixels)) == LV_RESULT_OK &&
        lv_draw_buf_init(&frozen, SIM_D, SIM_D, LV_COLOR_FORMAT_RGB565, SIM_D * 2,
        preview_pixels, sizeof(preview_pixels)) == LV_RESULT_OK, "initialize transition snapshot buffers");
    lv_obj_raster_prepare(orb_home_view_root(s_home.view)); lv_obj_raster_prepare(parent);
    extern bool orb_display_capture_snapshot(lv_obj_t *, lv_draw_buf_t *);
    home_check(orb_display_capture_snapshot(orb_home_view_root(s_home.view), &live) &&
        orb_display_capture_snapshot(parent, &frozen), "capture through the shell's actual banded snapshot renderer");
    if (memcmp(live_pixels, preview_pixels, sizeof(live_pixels))) {
        unsigned pixels = 0; int x1 = SIM_D, y1 = SIM_D, x2 = -1, y2 = -1;
        for (int y = 0; y < SIM_D; y++) for (int x = 0; x < SIM_D; x++) {
            unsigned at = (unsigned)(y * SIM_D + x) * 2;
            if (!memcmp(live_pixels + at, preview_pixels + at, 2)) continue;
            pixels++; x1 = LV_MIN(x1, x); y1 = LV_MIN(y1, y); x2 = LV_MAX(x2, x); y2 = LV_MAX(y2, y);
        }
        fprintf(stderr, "arrival snapshot difference: %u pixels at %d,%d..%d,%d\n", pixels, x1, y1, x2, y2);
    }
    home_check(!memcmp(live_pixels, preview_pixels, sizeof(live_pixels)),
        "suspended destination snapshot and live fresh arrival have identical start pixels");
    orb_home_view_arrival_complete(s_home.view, false);
    orb_app_host_destroy(preview); lv_obj_delete(parent); free(model);
    prefs.temperature = ORB_TEMPERATURE_C;
    home_check(orb_model_set_prefs(&prefs), "restore C for thermostat endpoint test"); home_advance(800);
}

static void adapter_quiet_environment(void)
{
    orb_home_snapshot_t *model = calloc(1, sizeof(*model));
    home_check(model && orb_home_snapshot(model), "copy quiet environment fixture");
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent); lv_obj_set_size(parent, SIM_D, SIM_D);
    unsigned before = orb_kit_motion_count();
    orb_app_host_t *host = orb_home_host_create(false, NULL);
    home_check(host && orb_home_host_attach(host, parent,
        &(orb_app_home_model_t){.hour = 19, .fixture = model}), "attach real active Home adapter");
    orb_screen_instance_t *instance = orb_app_host_current(host);
    lv_obj_t *root = orb_screen_root(instance);
    home_check(orb_kit_motion_count() > before, "active adapter initially has an arrival fence");
    orb_ui_environment_t environment = {.revision = 9000, .always_on = true};
    orb_screen_update_t update = {.environment = &environment};
    home_check(orb_screen_update(instance, &update), "deliver Always On through actual adapter update");
    home_check(!orb_kit_motion_allowed(root) && orb_kit_motion_count() == before,
        "Always On ends pending Home arrival and admits no motion");
    environment.revision++; environment.always_on = false;
    home_check(orb_screen_update(instance, &update), "leave Always On through adapter");
    home_check(orb_kit_motion_allowed(root) && orb_kit_motion_count() == before,
        "leaving Always On never replays an arrival");
    orb_app_home_enter(instance);
    orb_screen_visibility(instance, ORB_VISIBILITY_COVERED);
    home_check(!orb_kit_motion_allowed(root) && orb_kit_motion_count() == before,
        "covered adapter consumes arrival at final values");
    orb_screen_visibility(instance, ORB_VISIBILITY_ACTIVE);
    home_check(orb_kit_motion_count() == before, "sheet return never replays arrival");
    orb_app_host_destroy(host); lv_obj_delete(parent); free(model); home_advance(800);
}

static void arrival(void)
{
    uint64_t revision = orb_home_revision();
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    lv_obj_t *row = home_find_id(orb_home_view_root(s_home.view), "room:r0");
    int y = lv_obj_get_y(row), scroll = lv_obj_get_scroll_y(list);
    orb_home_view_arrival_prepare(s_home.view);
    lv_obj_send_event(list, LV_EVENT_SCROLL, NULL);
    home_check(s_home.view->arrival_armed, "unchanged layout scroll notification keeps arrival armed");
    lv_refr_now(s_home.display); sim_display_rgb(s_before);
    home_check(s_home.view->arrival_armed, "visible reported-on rows prepare before the snapshot");
    home_check(!(s_home.view->arrival_rows & (1u << 1)), "reported-off room never lights up on arrival");
    orb_home_view_arrival_complete(s_home.view, true); home_advance(96);
    sim_display_rgb(s_after);
    home_check(memcmp(s_before, s_after, sizeof(s_before)) != 0, "room arrival has visible intermediate progress");
    home_advance(354);
    home_check(!s_home.view->arrival_armed, "room arrival completes within 450 ms");
    home_check(!orb_kit_motion_timer_running() && orb_kit_motion_count() == 0,
        "all first-page arrival motion including the scroll arc is paused by 450 ms");
    home_check(orb_home_revision() == revision, "arrival never writes the model");
    home_check(lv_obj_get_y(row) == y && lv_obj_get_scroll_y(list) == scroll, "arrival never moves hit geometry");
    still();
    orb_home_view_arrival_prepare(s_home.view); home_advance(1300);
    home_check(!s_home.view->arrival_armed, "stranded arrival falls back without a host signal");
    still();
    orb_home_view_arrival_prepare(s_home.view); home_press(233, 80);
    home_check(!s_home.view->arrival_armed, "any contact ends room arrival immediately");
    home_release(); still();
    orb_home_view_arrival_prepare(s_home.view);
    lv_obj_scroll_by(list, 0, -24, LV_ANIM_OFF);
    home_check(!s_home.view->arrival_armed, "any list scroll finishes armed arrival immediately");
    still();
    orb_home_view_room(s_home.view, "r0"); home_advance(800);
    home_check(orb_home_view_back(s_home.view), "Back returns to room list"); home_advance(16);
    home_check(!s_home.view->arrival_armed, "Back never prepares fresh room arrival");
    still();
}

static lv_obj_t *detail(const char *id)
{
    orb_home_view_open(s_home.view, id); home_advance(16);
    home_check(s_home.view->detail && s_home.view->detail->canvas, "fresh forward detail exists");
    return s_home.view->detail->canvas;
}

static void sweeps(void)
{
    lv_obj_t *level = detail("lamp0");
    home_check(orb_kit_rim_slider_get_value(level) == 72 && orb_kit_ring_get_presented_value(level) == 0,
        "brightness snapshot starts at minimum while getter reports true72 target");
    orb_home_view_arrival_complete(s_home.view, true); home_advance(96);
    float intermediate = orb_kit_ring_get_presented_value(level);
    home_check(intermediate > 0 && intermediate < 72, "brightness Sweep makes intermediate progress");
    char number[16]; snprintf(number, sizeof(number), "%.0f%%", (double)intermediate);
    home_check(home_has_text(number), "brightness number follows presented arc");
    frozen_pixels();
    home_advance(340);
    home_check(orb_kit_ring_get_presented_value(level) == 72 && home_has_text("72%"), "brightness Sweep has exact endpoint");
    home_check(s_home.view->detail->disc && lv_obj_is_visible(s_home.view->detail->disc),
        "brightness identity disc remains visible after Sweep");
    frozen_pixels();
    home_check(orb_home_update("lamp0", &(orb_home_state_t){.present = ORB_HOME_BRIGHTNESS, .brightness = 91}, false, true, 0),
        "publish real brightness change");
    home_advance(64);
    home_check(orb_kit_rim_slider_get_value(level) == 91 && orb_kit_ring_get_presented_value(level) < 91,
        "Follow reports new target while presentation moves");
    int detent = s_home.view->detail->tick_index;
    home_check(!orb_rim_detent(.9101f, 100, &detent, false),
        "Follow hands over true value without a new sub-detent tick");
    uint64_t revision = orb_home_revision(); home_press(233, 260);
    home_check(orb_kit_ring_get_presented_value(level) == 91, "touch finishes Follow at true value");
    home_release();
    home_check(orb_home_revision() == revision, "decorative cancellation never sends a command"); still();
    level = detail("thermostat"); int32_t target = orb_kit_rim_slider_get_value(level);
    home_check(target > 0 && orb_kit_ring_get_presented_value(level) == 0, "reported thermostat target prepares Sweep");
    orb_home_view_arrival_complete(s_home.view, true); home_advance(96);
    home_check(orb_kit_ring_get_presented_value(level) > 0 && orb_kit_ring_get_presented_value(level) < target,
        "thermostat Sweep has intermediate progress");
    home_advance(340);
    home_check(orb_kit_ring_get_presented_value(level) == target && home_has_text("21.5°"),
        "thermostat keeps authoritative half-degree endpoint spelling"); still();
}

static void real_changes(void)
{
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(1400);
    lv_obj_t *root = orb_home_view_root(s_home.view);
    lv_obj_t *row = home_find_id(root, "room:r1");
    home_check(row != NULL, "off room has a retained named row");
    lv_area_t before, after; lv_obj_get_coords(row, &before);
    home_check(orb_home_update("lamp1", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, true, 0),
        "publish a light turning on while the room list is visible");
    uint64_t revision = orb_home_revision(); home_advance(96);
    home_check(home_find_id(root, "room:r1") == row && orb_kit_motion_timer_running(),
        "room Light up retains the row and has intermediate motion");
    frozen_pixels(); home_advance(400);
    lv_obj_get_coords(row, &after);
    home_check(!memcmp(&before, &after, sizeof(before)) && orb_home_revision() == revision,
        "room Light up changes neither hit geometry nor authoritative state");
    home_check(orb_home_update("extra7", &(orb_home_state_t){.present = ORB_HOME_ON, .on = false}, false, true, 0),
        "publish a digit-only room count change");
    home_advance(80);
    home_check(rolling_text(root, "2 lights on"), "room subtitle rolls a changed count with unchanged wording");
    frozen_pixels(); still();

    orb_home_view_room(s_home.view, "r1"); home_advance(1400);
    row = home_find_id(root, "lamp1"); home_check(row != NULL, "accessory row is visible in its room");
    home_check(orb_home_update("lamp1", &(orb_home_state_t){.present = ORB_HOME_ON, .on = false}, false, true, 0),
        "publish a light going dark in its accessory row");
    home_advance(96);
    home_check(home_find_id(root, "lamp1") == row && orb_kit_motion_timer_running(),
        "accessory Light up retains the live row during its reverse change");
    frozen_pixels(); still();

    lv_obj_t *level = detail("lamp1"); orb_home_view_arrival_complete(s_home.view, true); home_advance(800);
    lv_obj_t *disc = s_home.view->detail->disc;
    home_check(disc && lv_obj_is_visible(disc), "accessory detail keeps its identity disc while off");
    home_check(orb_home_update("lamp1", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, true, 0),
        "publish a detail light turning on");
    revision = orb_home_revision(); home_advance(96);
    home_check(s_home.view->detail->disc == disc && orb_kit_motion_timer_running(),
        "detail Light up retains its disc and input listeners");
    frozen_pixels();
    home_check(orb_home_update("lamp1", &(orb_home_state_t){.present = ORB_HOME_ON, .on = false}, false, true, 0),
        "publish Off before the in-flight detail Follow settles");
    home_advance(32);
    home_check(orb_kit_rim_slider_get_value(level) == 0 && home_has_text("Off"),
        "Off during Follow retargets zero and preserves the existing Off wording");
    home_check(orb_home_update("lamp1", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, true, 0),
        "publish On before the reverse Follow settles");
    revision = orb_home_revision(); home_advance(500);
    home_check(orb_home_revision() == revision && orb_kit_rim_slider_get_value(level) == 72 &&
        lv_obj_is_visible(disc), "detail animation keeps authority and the complete endpoint");
    frozen_pixels(); still();
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "all live animations dispatch zero Home requests");
}

static void pending_and_outcomes(void)
{
    orb_home_view_room(s_home.view, "r1"); home_advance(800);
    home_check(orb_home_set("lamp1", &(orb_home_state_t){.present = ORB_HOME_BRIGHTNESS, .brightness = 61}, false),
        "stage one synthetic pending brightness request");
    home_advance(800);
    lv_obj_t *word = home_find_text(orb_home_view_root(s_home.view), "Updating…");
    home_check(word && lv_obj_get_style_text_opa(word, 0) < LV_OPA_COVER, "accessory Updating breathes");
    uint32_t started = orb_home_accessory_wait_start(s_home.view, "lamp1");
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(21000);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(16);
    home_check(orb_home_accessory_wait_start(s_home.view, "lamp1") == started,
        "the same pending request keeps its original wait budget across a row rebuild");
    home_advance(9000); still();
    orb_home_request_t request;
    home_check(orb_home_take_request(&request), "take the explicitly staged synthetic request");
    orb_home_state_t accepted = {.present = ORB_HOME_ON | ORB_HOME_BRIGHTNESS, .on = true, .brightness = 61};
    orb_home_complete(&request, &accepted, NULL); home_advance(800);
    home_check(!home_has_text("Updating…"), "authoritative completion removes Updating");

    orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
    state.scene_page = true; orb_home_view_restore(s_home.view, &state); home_advance(800);
    home_check(orb_home_scene("evening"), "stage one synthetic scene request"); home_advance(800);
    word = home_find_text(orb_home_view_root(s_home.view), "Running…");
    home_check(word && lv_obj_get_style_text_opa(word, 0) < LV_OPA_COVER, "scene Running breathes");
    home_check(orb_home_take_request(&request), "take the explicitly staged scene request");
    orb_home_complete(&request, NULL, NULL); home_advance(16);
    home_check(home_has_text("Requested") && s_home.view->current.outcome && orb_kit_motion_timer_running(),
        "a newly Requested scene starts its draw-on check");
    home_advance(96); frozen_pixels(); home_advance(200); still();
    orb_home_view_save(s_home.view, &state); orb_home_view_restore(s_home.view, &state); home_advance(16);
    home_check(s_home.view->current.outcome && !orb_kit_motion_timer_running(),
        "restoring the scene receipt shows the final check without replay");

    orb_home_view_room(s_home.view, "r1"); home_advance(800);
    orb_home_view_save(s_home.view, &state); state.sections = true; state.scene_page = false;
    orb_home_view_restore(s_home.view, &state); home_advance(800);
    home_check(orb_home_room_off("r1"), "stage a synthetic All off request"); home_advance(800);
    word = home_find_text(orb_home_view_root(s_home.view), "Turning off…");
    home_check(word && lv_obj_get_style_text_opa(word, 0) < LV_OPA_COVER, "All off Turning off breathes");
    home_check(orb_home_take_request(&request), "take the explicitly staged All off request");
    orb_home_complete(&request, &request.values, NULL); home_advance(16);
    home_check(home_has_text("All off requested") && s_home.view->current.outcome && orb_kit_motion_timer_running(),
        "a newly completed All off starts its draw-on check");
    home_advance(96); frozen_pixels(); home_advance(200); still();
    home_check(!orb_home_take_request(&request), "feedback and completion checks enqueue no extra commands");
}

static void unknown(void)
{
    orb_home_snapshot_t *model = calloc(1, sizeof(*model));
    home_check(model && orb_home_snapshot(model), "copy unknown-value fixture");
    lv_obj_t *level = detail("lamp0");
    orb_home_view_arrival_complete(s_home.view, true); home_advance(96);
    home_check(orb_kit_ring_get_presented_value(level) > 0 &&
        orb_kit_ring_get_presented_value(level) < orb_kit_rim_slider_get_value(level),
        "known detail is mid-Sweep before its reported field disappears");
    model->accessories[0].state.present &= ~ORB_HOME_BRIGHTNESS;
    home_check(orb_home_snapshot_chunk(model->rooms, model->room_count, model->accessories,
        model->accessory_count, model->scenes, model->scene_count, true, false), "remove reported brightness field");
    home_advance(16);
    level = s_home.view->detail->canvas;
    home_check(home_has_text("—") && orb_kit_ring_get_presented_value(level) == orb_kit_rim_slider_get_value(level),
        "losing the reported field cancels the in-flight Sweep immediately");
    level = detail("lamp0");
    orb_home_view_arrival_complete(s_home.view, true);
    home_check(home_has_text("—") && orb_kit_ring_get_presented_value(level) == orb_kit_rim_slider_get_value(level),
        "unknown brightness stays blank and never sweeps");
    model->accessories[0].state.present |= ORB_HOME_BRIGHTNESS;
    home_check(orb_home_snapshot_chunk(model->rooms, model->room_count, model->accessories,
        model->accessory_count, model->scenes, model->scene_count, true, false), "restore reported brightness field");
    free(model); home_advance(800);
}

static void quiet_policy(void)
{
    lv_obj_t *level = detail("lamp0");
    orb_kit_motion_set_active(orb_home_view_root(s_home.view), false);
    home_check(orb_kit_ring_get_presented_value(level) == orb_kit_rim_slider_get_value(level),
        "covered or dimmed surface finishes pending Sweep immediately");
    orb_home_view_arrival_complete(s_home.view, true); still();
    orb_kit_motion_set_active(orb_home_view_root(s_home.view), true); orb_ui_set_reduce_motion(true);
    level = detail("thermostat");
    home_check(orb_kit_ring_get_presented_value(level) == orb_kit_rim_slider_get_value(level),
        "Reduce Motion shows final thermostat values immediately"); still();
    orb_ui_set_reduce_motion(false);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(800);
    orb_home_view_arrival_prepare(s_home.view); orb_home_view_arrival_complete(s_home.view, true); home_advance(48);
    orb_home_view_destroy(s_home.view); s_home.view = NULL; home_advance(16);
    home_check(!orb_kit_motion_timer_running() && orb_kit_motion_count() == 0,
        "deleting Home during motion cancels every owned kit job safely");
}

static void render_modes(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        orb_ui_set_reduce_transparency(true);
        home_check(orb_fonts_set_content_size(mode ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK,
            "set reduced-transparency motion rendering profile");
        fixture();
        detail("lamp1"); orb_home_view_arrival_complete(s_home.view, true); home_advance(800);
        home_check(orb_home_update("lamp1", &(orb_home_state_t){.present = ORB_HOME_ON, .on = true}, false, true, 0),
            "start concurrent Light up and Follow under reduced transparency");
        for (unsigned frame = 0; frame < 28; frame++) { home_advance(16); frozen_pixels(); }
        still();
    }
    orb_ui_set_reduce_transparency(false);
    home_check(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK, "restore default text size");
    home_advance(800);
}

static void loading(void)
{
    orb_home_reset(); orb_home_set_connection(true, false);
    s_home.view = orb_home_view_create(lv_screen_active(), NULL);
    home_check(s_home.view != NULL, "create Loading Home without any reported data");
    home_advance(16);
    lv_obj_t *word = home_find_text(orb_home_view_root(s_home.view), "Loading");
    home_check(word != NULL, "Loading names its real wait");
    home_advance(784);
    home_check(lv_obj_get_style_text_opa(word, 0) < LV_OPA_COVER, "Loading breath makes visible progress");
    orb_home_snapshot_t *model = calloc(1, sizeof(*model));
    home_check(model && orb_home_snapshot(model), "copy Loading adapter projection");
    lv_obj_t *parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent); lv_obj_set_size(parent, SIM_D, SIM_D);
    orb_app_host_t *host = orb_home_host_create(false, NULL);
    home_check(host && orb_home_host_attach(host, parent, &(orb_app_home_model_t){.fixture = model}),
        "commit actual Loading adapter through its covered staging phase");
    lv_obj_t *host_word = home_find_text(orb_screen_root(orb_app_host_current(host)), "Loading");
    home_check(host_word != NULL, "committed adapter retains its named wait");
    home_advance(800);
    home_check(lv_obj_get_style_text_opa(host_word, 0) < LV_OPA_COVER,
        "Loading resumes breathing after staging commits Active");
    orb_app_host_destroy(host); lv_obj_delete(parent); free(model);

    home_advance(20000);
    orb_home_view_select(s_home.view, ORB_HOME_ROOMS); home_advance(16);
    home_check(home_find_text(orb_home_view_root(s_home.view), "Loading") != NULL, "model rebuild retains the same named wait");
    home_advance(10000); still();
}

static void shell_advance(orb_shell_t *shell, unsigned milliseconds)
{
    unsigned end = s_home.now + milliseconds;
    while (s_home.now < end) {
        s_home.now += LV_MIN(8, end - s_home.now);
        orb_shell_poll(shell); lv_timer_handler(); lv_refr_now(s_home.display);
    }
}

static void covered_sheet(void)
{
    orb_home_view_destroy(s_home.view); s_home.view = NULL;
    orb_home_reset(); orb_home_set_connection(true, false);
    orb_model_load_demo(); orb_model_set_connection(true, true);
    orb_shell_t *shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    home_check(shell != NULL, "create the real shell for modal coverage");
    orb_shell_snapshot_t snapshot = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70, .usb = true,
        .clock_time = "10:09", .clock_date = "Tuesday, October 27", .hub_name = "Test Mac"};
    orb_shell_update(shell, &snapshot); orb_shell_environment(shell, 0, 0, 1, false);
    shell_advance(shell, 1000); orb_shell_navigate(shell, ORB_SHELL_HOME); shell_advance(shell, 1000);
    lv_obj_t *word = home_find_text(orb_shell_root(shell), "Loading");
    home_check(word && orb_kit_motion_allowed(word) && orb_kit_motion_timer_running(), "visible Home Loading breathes");
    orb_button_event_t event = {.button = ORB_BUTTON_PWR, .gesture = ORB_BUTTON_HOLD, .at_ms = s_home.now};
    orb_shell_button(shell, &event);
    home_check(!orb_kit_motion_allowed(word), "Power sheet immediately finishes the covered Home presentation");
    orb_shell_environment(shell, 0, 0, 1, false); orb_shell_display_active(shell, true);
    shell_advance(shell, 800);
    home_check(!orb_kit_motion_allowed(word) && !orb_kit_motion_timer_running(),
        "environment and power updates cannot restart a wait beneath the settled sheet");
    orb_shell_back(shell); shell_advance(shell, 160);
    home_check(orb_shell_surface(shell) == ORB_SHELL_HOME && lv_obj_is_valid(word) &&
        orb_kit_motion_allowed(word) && lv_obj_get_style_text_opa(word, 0) < LV_OPA_COVER,
        "Back preserves Home and resumes only the existing wait within its budget");
    event.at_ms = s_home.now; orb_shell_button(shell, &event); shell_advance(shell, 31000);
    orb_shell_back(shell); shell_advance(shell, 800);
    home_check(!orb_kit_motion_timer_running(), "sheet return after thirty seconds never restarts an expired wait");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "modal motion gates enqueue no Home requests");
    orb_shell_destroy(shell);
}

int main(int argc, char **argv)
{
    home_init(argc, argv, 1); fixture(); preview_start_matches_live(); adapter_quiet_environment(); arrival(); sweeps();
    real_changes(); pending_and_outcomes(); unknown(); render_modes(); quiet_policy(); loading(); covered_sheet(); home_shutdown();
    printf("Home Alive: %u checks passed\n", s_home.checks); return 0;
}
