/* Focused behavior regressions. Synthetic Home data, real LVGL input and
 * public context APIs; no Home link, board or owner's state is consulted. */
#include "../home_harness.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "../../firmware/components/orb_shell/orb_shell_power.h"

static const char *s_mode = "default";
static void capture(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "sim/build/fixes1/%s/%s.png", s_mode, name);
    home_check(sim_display_save_png(path), "capture Home design evidence");
}

static uint32_t cue_calls(void)
{
    orb_sound_stats_t stats;
    orb_sound_stats(&stats);
    /* Count requests even without an installed PCM pack or while muted. */
    return stats.queued_count + stats.dropped_count;
}

static void authoritative(void)
{
    orb_home_snapshot_t *s = calloc(1, sizeof(*s));
    home_check(s && orb_home_snapshot(s), "copy demo Home");
    home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
        s->accessory_count, s->scenes, s->scene_count, true, false), "install authoritative fixture");
    free(s);
    home_advance(100);
}

static void recreate(const orb_home_view_state_t *state)
{
    orb_home_view_destroy(s_home.view);
    s_home.view = orb_home_view_create(lv_screen_active(), NULL);
    home_check(s_home.view != NULL, "recreate Home");
    orb_home_view_restore(s_home.view, state);
    home_advance(100);
}

static void detail_context(void)
{
    orb_home_view_open(s_home.view, "climate"); home_advance(200);
    orb_home_view_detail_page(s_home.view, 1); home_advance(200);
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(list != NULL, "mode picker has a scroll owner");
    lv_obj_scroll_to_y(list, 40, LV_ANIM_OFF); home_advance(100);
    int y = lv_obj_get_scroll_y(list);
    home_check(y > 0, "mode picker is scrolled before saving");
    orb_home_view_state_t state;
    orb_home_view_save(s_home.view, &state);
    recreate(&state);
    list = orb_home_view_scroll(s_home.view);
    home_check(list && lv_obj_get_scroll_y(list) == y, "detail-owned mode scroll survives Home reconstruction");
    orb_home_view_detail_page(s_home.view, 0); home_advance(100);
    orb_home_view_detail_page(s_home.view, 1); home_advance(100);
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == y,
        "inactive detail page scroll also survives restoration");
    orb_home_view_detail_page(s_home.view, 0); home_advance(100);
    orb_home_view_save(s_home.view, &state); recreate(&state);
    orb_home_view_detail_page(s_home.view, 1); home_advance(100);
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == y,
        "saving from the primary page retains inactive secondary scroll");
}

static void scene_context(void)
{
    /* The light's scenes are one scrolled list (polish review #34): the
     * retained context is its scroll, never a pager position. */
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(200);
    orb_home_view_detail_page(s_home.view, 2); home_advance(200);
    lv_obj_t *list = orb_home_view_scroll(s_home.view);
    home_check(list != NULL && home_has_text("Good morning") && home_has_text("Evening"), "the scene list shows its scenes");
    lv_obj_update_layout(list);
    int y = LV_MIN(40, lv_obj_get_scroll_bottom(list));
    lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
    orb_home_view_state_t state;
    orb_home_view_save(s_home.view, &state);
    recreate(&state);
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == y, "detail scene scroll survives Home reconstruction");
    orb_home_view_detail_page(s_home.view, 0); home_advance(100);
    orb_home_view_state_t inactive; orb_home_view_save(s_home.view, &inactive);
    recreate(&inactive);
    orb_home_view_detail_page(s_home.view, 2); home_advance(100);
    home_check(lv_obj_get_scroll_y(orb_home_view_scroll(s_home.view)) == y, "saving from primary retains the inactive scene scroll");
    orb_home_snapshot_t *s = calloc(1, sizeof(*s));
    home_check(s && orb_home_snapshot(s), "copy scene topology");
    orb_home_scene_t first = s->scenes[0]; s->scenes[0] = s->scenes[1]; s->scenes[1] = first;
    home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
        s->accessory_count, s->scenes, s->scene_count, true, false), "reorder scene topology");
    recreate(&state);
    home_check(home_has_text("Evening") && home_has_text("Good morning"), "reordered topology still lists every safe scene");
    free(s);
}

static void topology_context(void)
{
    orb_home_view_room(s_home.view, "Living Room"); home_advance(200);
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(200);
    orb_home_view_state_t state;
    orb_home_view_save(s_home.view, &state);
    orb_home_snapshot_t *s = calloc(1, sizeof(*s));
    home_check(s && orb_home_snapshot(s), "copy topology");
    for (unsigned i = 0; i < s->accessory_count; ++i) {
        if (strcmp(s->accessories[i].id, "floor-lamp")) continue;
        memmove(s->accessories + i, s->accessories + i + 1,
            (--s->accessory_count - i) * sizeof(*s->accessories)); break;
    }
    home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
        s->accessory_count, s->scenes, s->scene_count, true, false), "remove selected accessory");
    recreate(&state);
    orb_home_view_state_t restored;
    orb_home_view_save(s_home.view, &restored);
    home_check(!strcmp(restored.selected, "floor-lamp") && home_has_text("Accessory not found") &&
        !orb_home_view_dial(s_home.view, NULL, NULL), "missing accessory retains explicit unavailable detail");
    home_check(orb_home_view_back(s_home.view), "unavailable accessory keeps its local Back route"); home_advance(100);
    orb_home_view_save(s_home.view, &restored);
    home_check(!restored.selected[0] && restored.surface == ORB_HOME_ROOM && !strcmp(restored.room, "Living Room"),
        "unavailable accessory Back restores its surviving room");
    for (unsigned i = 0; i < s->room_count; ++i) {
        if (strcmp(s->rooms[i].id, "Living Room")) continue;
        memmove(s->rooms + i, s->rooms + i + 1, (--s->room_count - i) * sizeof(*s->rooms)); break;
    }
    for (unsigned i = 0; i < s->accessory_count;) {
        if (strcmp(s->accessories[i].room, "Living Room")) { i++; continue; }
        memmove(s->accessories + i, s->accessories + i + 1,
            (--s->accessory_count - i) * sizeof(*s->accessories));
    }
    home_check(orb_home_snapshot_chunk(s->rooms, s->room_count, s->accessories,
        s->accessory_count, s->scenes, s->scene_count, true, false), "remove selected room");
    recreate(&state); orb_home_view_save(s_home.view, &restored);
    home_check(restored.surface == ORB_HOME_ROOMS && !restored.selected[0] && !restored.room[0],
        "missing room returns to Rooms rather than a stale ID");
    home_check(orb_home_view_scroll(s_home.view) && restored.rooms_scroll >= 0,
        "removed room restores the surviving room list and a valid scroll position");
    free(s);
}

static void back_cue(void)
{
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(200);
    orb_home_view_detail_page(s_home.view, 1); home_advance(200);
    uint32_t before = cue_calls();
    home_check(orb_home_view_back(s_home.view), "secondary accessory Back is consumed");
    home_advance(200);
    unsigned calls = cue_calls() - before;
    printf("Home secondary Back: Close cue requests=%u\n", calls);
    home_check(calls == 1, "one secondary Back requests exactly one Close cue");
}

static void scene_cue(void)
{
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(200);
    orb_home_view_detail_page(s_home.view, 2); home_advance(200);
    home_tap_text("Good morning"); home_advance(200);
    orb_home_request_t request;
    home_check(orb_home_take_request(&request) && request.kind == ORB_HOME_REQUEST_SCENE, "scene request exists");
    uint32_t before = cue_calls();
    orb_home_complete(&request, NULL, NULL); home_advance(200);
    unsigned calls = cue_calls() - before;
    printf("Home scene completion: completion cue requests=%u\n", calls);
    home_check(calls == 1, "one scene completion requests exactly one cue");
}

static unsigned s_transitions;
static lv_area_t s_token;
static bool s_reverse;
static void transition(void *ctx, const lv_area_t *token, bool reverse, bool before)
{
    (void)ctx;
    home_check(before == (s_transitions % 2 == 0), "local iris captures source before destination");
    if (before) { s_token = *token; s_reverse = reverse; }
    else home_check(!memcmp(&s_token, token, sizeof(*token)) && s_reverse == reverse,
        "source and destination use the identical token and direction");
    s_transitions++;
}
static void iris_tokens(void)
{
    orb_home_view_set_transition(s_home.view, transition, NULL);
    orb_home_view_room(s_home.view, "Living Room"); home_advance(100);
    home_check(s_transitions == 2 && !s_reverse && lv_area_get_width(&s_token) >= ORB_KIT_ROW_H,
        "room iris originates at its selected named row");
    lv_area_t room = s_token;
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(100);
    home_check(s_transitions == 4 && !s_reverse && lv_area_get_width(&s_token) == ORB_KIT_DISC_ROW,
        "accessory iris originates at its selected 40px row disc");
    lv_area_t accessory = s_token;
    orb_home_view_state_t state; orb_home_view_save(s_home.view, &state);
    recreate(&state); orb_home_view_set_transition(s_home.view, transition, NULL);
    home_check(orb_home_view_back(s_home.view), "accessory Back is local"); home_advance(100);
    home_check(s_transitions == 6 && s_reverse && !memcmp(&s_token, &accessory, sizeof(s_token)),
        "accessory return retains the launching token across context reconstruction");
    lv_obj_t *list = orb_home_view_scroll(s_home.view), *row = NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *candidate = lv_obj_get_child(list, i);
        const char *id = lv_obj_get_user_data(candidate);
        if (id && !strcmp(id, "climate")) { row = candidate; break; }
    }
    home_check(row != NULL, "shoulder thermostat row is present");
    lv_area_t presented, native;
    lv_obj_t *disc = lv_obj_get_child(row, 0);
    home_check(orb_round_list_child_bounds(row, disc, &presented), "resolve transformed shoulder disc");
    lv_obj_get_coords(disc, &native);
    home_check(memcmp(&native, &presented, sizeof(native)) != 0,
        "shoulder fixture distinguishes native and presented disc geometry");
    orb_home_view_open(s_home.view, "climate"); home_advance(100);
    home_check(s_transitions == 8 && !s_reverse && !memcmp(&presented, &s_token, sizeof(s_token)),
        "shoulder accessory iris originates at its painted chord-scaled disc");
    home_check(orb_home_view_back(s_home.view), "shoulder accessory Back is local"); home_advance(100);
    home_check(orb_home_view_back(s_home.view), "room Back is local"); home_advance(100);
    home_check(s_transitions == 12 && s_reverse && !memcmp(&s_token, &room, sizeof(s_token)),
        "room return retains its launching token across context reconstruction");
}

static void non_accessory_tokens(void)
{
    orb_home_view_state_t state;
    orb_home_view_save(s_home.view, &state); state.scene_page = true;
    orb_home_view_restore(s_home.view, &state); home_advance(100);
    home_check(orb_home_view_scroll(s_home.view) != NULL, "scene list has index-owned rows");
    orb_home_view_open(s_home.view, "floor-lamp"); home_advance(100);
    orb_home_view_save(s_home.view, &state);
    home_check(!strcmp(state.selected, "floor-lamp"),
        "programmatic accessory open never interprets scene indices as accessory IDs");
}

static orb_shell_t *s_shell;
static void shell_advance(unsigned ms)
{
    uint32_t end = s_home.now + ms;
    while (s_home.now < end) {
        s_home.now += LV_MIN(8, end - s_home.now);
        orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_home.display);
    }
}
static lv_obj_t *semantic(lv_obj_t *root, const char *id)
{
    const char *data = lv_obj_get_user_data(root);
    if ((uintptr_t)data > 4096 && !strcmp(data, id)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = semantic(lv_obj_get_child(root, i), id);
        if (found) return found;
    }
    return NULL;
}
static void shell_tap_for(lv_obj_t *object, unsigned settle)
{
    home_check(object && lv_obj_is_visible(object), "shell target is visible");
    lv_area_t area; lv_obj_get_coords(object, &area);
    s_home.input.point = (lv_point_t){(area.x1 + area.x2)/2, (area.y1 + area.y2)/2};
    s_home.input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_home.pointer); shell_advance(32);
    s_home.input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_home.pointer); shell_advance(settle);
}
static void shell_tap(lv_obj_t *object) { shell_tap_for(object, 1000); }
static void shell_button(orb_button_t button, orb_button_gesture_t gesture)
{
    orb_button_event_t event = {.button=button, .gesture=gesture, .at_ms=s_home.now};
    orb_shell_button(s_shell, &event); shell_advance(1000);
}
static void shell_start(void)
{
    orb_home_view_destroy(s_home.view); s_home.view = NULL;
    orb_model_load_demo(); orb_model_set_connection(true, true);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    home_check(s_shell != NULL, "create shell");
    orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80, .rssi=-54, .brightness=70, .usb=true};
    strcpy(status.clock_time, "10:09"); strcpy(status.clock_date, "Tuesday, October 27");
    orb_shell_update(s_shell, &status); shell_advance(1000);
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); shell_advance(1000);
}
static void shell_context(void)
{
    shell_start();
    shell_tap(semantic(orb_shell_root(s_shell), "room:Living Room"));
    lv_obj_t *row = semantic(orb_shell_root(s_shell), "floor-lamp");
    home_check(row != NULL, "Living Room is open before departure");
    lv_obj_t *list = lv_obj_get_parent(row);
    lv_obj_scroll_to_y(list, 100, LV_ANIM_OFF); shell_advance(100);
    shell_button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE);
    home_check(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER, "PWR double opens Control Center");
    shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
    home_check(orb_shell_surface(s_shell) == ORB_SHELL_HOME &&
        semantic(orb_shell_root(s_shell), "floor-lamp") != NULL, "ordinary Back restores the live Living Room tree");
    row = semantic(orb_shell_root(s_shell), "floor-lamp"); list = lv_obj_get_parent(row);
    home_check(lv_obj_get_scroll_y(list) == 100, "ordinary Back restores the room's scroll before its first live frame");
    lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF); shell_advance(100);
    shell_tap(semantic(orb_shell_root(s_shell), "climate"));
    /* The thermostat's identity disc opens its mode list. */
    lv_obj_t *mode = lv_indev_search_obj(orb_shell_root(s_shell), &(lv_point_t){233, 140});
    home_check(mode != NULL, "thermostat exposes its mode entry on the identity disc");
    while (mode && !lv_obj_is_clickable(mode)) mode = lv_obj_get_parent(mode);
    shell_tap(mode);
    mode = home_find_text(orb_shell_root(s_shell), "Heat");
    home_check(mode != NULL, "mode picker is selected before departure");
    list = lv_obj_get_parent(mode);
    while (list && !lv_obj_is_scrollable(list)) list = lv_obj_get_parent(list);
    home_check(list != NULL, "mode picker owns a native list");
    lv_obj_scroll_to_y(list, 40, LV_ANIM_OFF); shell_advance(100);
    int scroll = lv_obj_get_scroll_y(list);
    shell_button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE);
    orb_session_t background = {.id="home-context-background", .title="Background revision",
        .agent=ORB_AGENT_CODEX, .state=ORB_SESSION_BUSY};
    home_check(orb_model_upsert_session(&background), "invalidate return cache while Home is behind Control Center");
    orb_button_event_t back = {.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_SHORT, .at_ms=s_home.now};
    orb_shell_button(s_shell, &back); shell_advance(200);
    home_check(sim_display_snapshot_transition_active(), "Home return preview is captured before the live endpoint");
    capture("home-return-preview"); shell_advance(800);
    mode = home_find_text(orb_shell_root(s_shell), "Heat"); list = mode ? lv_obj_get_parent(mode) : NULL;
    while (list && !lv_obj_is_scrollable(list)) list = lv_obj_get_parent(list);
    if (!list || lv_obj_get_scroll_y(list) != scroll || !scroll)
        fprintf(stderr, "Home secondary context expected=%d actual=%d mode=%p surface=%d\n",
            scroll, list ? (int)lv_obj_get_scroll_y(list) : -1, (void *)mode, orb_shell_surface(s_shell));
    home_check(list && lv_obj_get_scroll_y(list) == scroll && scroll > 0,
        "ordinary Back restores accessory secondary page and its detail-owned scroll");
    capture("home-restored-mode");
    orb_shell_destroy(s_shell); s_shell = NULL;
}

static void shell_iris(void)
{
    shell_start();
    shell_tap_for(semantic(orb_shell_root(s_shell), "room:Living Room"), 24);
    home_check(sim_display_snapshot_transition_active(), "opening a Home room presents a token iris");
    shell_advance(100); capture("home-room-iris");
    shell_advance(1000);
    capture("home-room");
    lv_obj_t *row = semantic(orb_shell_root(s_shell), "floor-lamp");
    shell_tap_for(row, 24);
    home_check(sim_display_snapshot_transition_active(), "opening an accessory presents its row-disc iris");
    shell_advance(100); capture("home-accessory-iris");
    shell_advance(1000);
    capture("home-accessory");
    orb_shell_back(s_shell); shell_advance(24);
    home_check(sim_display_snapshot_transition_active(), "accessory Back presents reciprocal iris");
    shell_advance(100); capture("home-accessory-back");
    shell_advance(1000);
    orb_shell_back(s_shell); shell_advance(24);
    home_check(sim_display_snapshot_transition_active(), "room Back presents reciprocal iris");
    shell_advance(100); capture("home-room-back");
    shell_advance(1000);
    orb_shell_destroy(s_shell); s_shell = NULL;
}

static void interrupted_iris(void)
{
    shell_start();
    orb_shell_power_enable(s_shell, true);
    home_check(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted, "wake Home for interruption fixture");
    shell_advance(500);
    /* Release dispatch captures the source synchronously. Do not run the
     * deferred Home rebuild until after the power owner suspends its tree. */
    shell_tap_for(semantic(orb_shell_root(s_shell), "room:Living Room"), 0);
    home_check(!sim_display_snapshot_transition_active(), "local destination is still deferred before lock");
    orb_shell_power(s_shell); shell_advance(1000);
    home_check(orb_shell_power_snapshot(s_shell).redact && !sim_display_snapshot_transition_active(),
        "deferred Home completion cannot present an iris over the lock");
    home_check(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted, "explicit wake restores underlying Home");
    shell_advance(1000);
    home_check(!sim_display_snapshot_transition_active() &&
        semantic(orb_shell_root(s_shell), "floor-lamp") != NULL,
        "wake restores the committed local room without reviving its retired iris");
    orb_shell_back(s_shell); shell_advance(1000);

    orb_time_init();
    home_check(orb_time_timer_set(1) && orb_time_timer_start(1000), "arm synthetic local timer");
    shell_tap_for(semantic(orb_shell_root(s_shell), "room:Living Room"), 0);
    orb_time_poll(1001000, 1700000000);
    /* Deliver alert priority before lv_timer_handler can complete Home. */
    orb_shell_poll(s_shell); shell_advance(1000);
    home_check(orb_shell_time_alert_active(s_shell) && !sim_display_snapshot_transition_active(),
        "deferred Home completion cannot present an iris over a ringing alert");
    shell_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT);
    home_check(!orb_shell_time_alert_active(s_shell) && !sim_display_snapshot_transition_active() &&
        semantic(orb_shell_root(s_shell), "floor-lamp") != NULL,
        "alert dismissal restores committed Home context without a delayed iris");
    home_check(!orb_home_take_request(&(orb_home_request_t){0}), "interrupted Home navigation never issues a Home write");
    orb_time_init(); shell_advance(20);
    orb_shell_destroy(s_shell); s_shell = NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    s_mode = argc > 2 ? argv[2] : "default";
    if (!strcmp(s_mode, "ax3-bold-rt")) {
        char *flags[] = {argv[0], argv[1], "ax3", "bold", "rt"};
        home_init(5, flags, 2);
    } else home_init(argc, argv, 2);
    authoritative();
    if (!strcmp(argv[1], "detail-context")) detail_context();
    else if (!strcmp(argv[1], "scene-context")) scene_context();
    else if (!strcmp(argv[1], "topology")) topology_context();
    else if (!strcmp(argv[1], "back-cue")) back_cue();
    else if (!strcmp(argv[1], "scene-cue")) scene_cue();
    else if (!strcmp(argv[1], "shell-context")) shell_context();
    else if (!strcmp(argv[1], "shell-iris")) shell_iris();
    else if (!strcmp(argv[1], "interrupted-iris")) interrupted_iris();
    else if (!strcmp(argv[1], "iris-tokens")) iris_tokens();
    else if (!strcmp(argv[1], "non-accessory-tokens")) non_accessory_tokens();
    else return 2;
    printf("Home fixes1 %s: %u checks passed\n", argv[1], s_home.checks);
    home_shutdown();
    return 0;
}
