/* Cross-owner regressions exercise the production snapshot adapter directly.
 * Inspecting its admitted plane proves the cached destination during Back,
 * before the settled live tree can conceal a wrong preview. */
#ifndef ORB_FIXES2_WORKSPACE_SOURCE
#define ORB_FIXES2_WORKSPACE_SOURCE "../../firmware/components/orb_shell/orb_workspace.c"
#endif
#include ORB_FIXES2_WORKSPACE_SOURCE
#include "sim_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static orb_workspace_t *s_workspace;
static const char *s_case, *s_out;
#define CHECK(v) do { ++s_checks; if (!(v)) { \
    fprintf(stderr, "fixes2 workspace %s:%d: %s\n", s_case, __LINE__, #v); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        s_now += 20;
        if (s_workspace) orb_workspace_poll(s_workspace);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *object, const char *text)
{
    if (lv_obj_is_hidden(object)) return NULL;
    const char *rim = orb_kit_rim_text_get(object), *button_text = orb_kit_button_get_text(object);
    if ((rim && !strcasecmp(rim, text)) || (button_text && !strcmp(button_text, text)) ||
        (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text))) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}
static void click(const char *text)
{
    lv_obj_t *object = find(s_workspace->page, text); CHECK(object);
    while (object && !lv_obj_is_clickable(object)) object = lv_obj_get_parent(object);
    CHECK(object);
    if (lv_obj_get_parent(object) == s_workspace->list) {
        lv_obj_scroll_to_y(s_workspace->list, lv_obj_get_y(object), LV_ANIM_OFF); advance(100);
    }
    lv_obj_send_event(object, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(object, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL);
    advance(1200);
}
static void capture(const char *name)
{
    if (!s_out) return;
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.png", s_out, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
}
static void route(orb_shell_surface_t surface)
{
    orb_workspace_navigate(s_workspace, surface); advance(1200);
    CHECK(s_workspace->surface == surface);
}
static void idle(void)
{
    route(ORB_SHELL_IDLE_SESSIONS);
    orb_idle_request_t request; CHECK(orb_model_take_idle_request(&request));
    orb_session_t sessions[3] = {
        {.id="idle-readonly", .agent=ORB_AGENT_CODEX, .state=ORB_SESSION_IDLE,
         .title="Read-only session", .project="orb", .can_message=false},
        {.id="idle-owned", .agent=ORB_AGENT_CLAUDE, .state=ORB_SESSION_IDLE,
         .title="Owned session", .project="firmware", .can_message=true},
        {.id="idle-dot", .agent=ORB_AGENT_DOT, .state=ORB_SESSION_IDLE,
         .title="Dot session", .project="Notes", .can_message=true},
    };
    CHECK(orb_model_complete_idle(&request, sessions, 3, "page-2", NULL)); advance(1200);
    CHECK(orb_kit_rim_text_get(s_workspace->heading));
    CHECK(!strcasecmp(orb_kit_rim_text_get(s_workspace->heading), "Idle sessions"));
    CHECK(s_workspace->list && lv_obj_get_y(s_workspace->list) == 88);
    CHECK(lv_obj_get_child_count(s_workspace->list) == 4);
    /* 72 px minimum rows on the kit's 12 px gap; AX3 rows may grow. */
    for (int i = 0, next = 0; i < 4; i++) {
        lv_obj_t *item = lv_obj_get_child(s_workspace->list, i);
        CHECK(lv_obj_get_height(item) == ORB_KIT_ROW_H ||
              (orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 && lv_obj_get_height(item) > ORB_KIT_ROW_H));
        CHECK(lv_obj_get_y(item) == next);
        next = lv_obj_get_y(item) + lv_obj_get_height(item) + 12;
    }
    capture("idle-page");
    click("More"); CHECK(orb_model_take_idle_request(&request)); CHECK(!strcmp(request.cursor, "page-2"));
    CHECK(orb_model_complete_idle(&request, NULL, 0, NULL, "Load failed")); advance(1200);
    lv_obj_t *retry = find(s_workspace->page, "Try again"); CHECK(retry);
    while (retry && lv_obj_get_parent(retry) != s_workspace->list) retry = lv_obj_get_parent(retry);
    CHECK(retry); lv_obj_scroll_to_y(s_workspace->list, lv_obj_get_y(retry), LV_ANIM_OFF); advance(100);
    capture("idle-error");
    click("Try again"); CHECK(orb_model_take_idle_request(&request));
    CHECK(orb_model_complete_idle(&request, sessions, 3, "", NULL)); advance(1200);
    CHECK(!find(s_workspace->page, "More"));
    click("Read-only session");
    CHECK(s_workspace->surface == ORB_SHELL_SESSION && s_workspace->has_selection);
    CHECK(!s_workspace->selection.can_message);
    CHECK(!orb_app_voice_supported(&(orb_app_voice_model_t){.model = s_workspace->model,
        .selection = s_workspace->has_selection ? &s_workspace->selection : NULL,
        .selection_external = s_workspace->selection_external}, s_workspace->session, NULL));
    orb_workspace_back(s_workspace); advance(1200);
    CHECK(s_workspace->surface == ORB_SHELL_IDLE_SESSIONS);
    CHECK(orb_kit_rim_text_get(s_workspace->heading));
    click("Owned session");
    CHECK(s_workspace->surface == ORB_SHELL_SESSION && s_workspace->selection.can_message);
    CHECK(orb_app_voice_supported(&(orb_app_voice_model_t){.model = s_workspace->model,
        .selection = s_workspace->has_selection ? &s_workspace->selection : NULL,
        .selection_external = s_workspace->selection_external}, s_workspace->session, NULL));
    orb_workspace_back(s_workspace); advance(1200);
    CHECK(s_workspace->surface == ORB_SHELL_IDLE_SESSIONS);
    lv_obj_scroll_to_y(s_workspace->list, 84, LV_ANIM_OFF); advance(100);
    int scroll = lv_obj_get_scroll_y(s_workspace->list); CHECK(scroll > 0);
    route(ORB_SHELL_CONTROL_CENTER); orb_workspace_back(s_workspace); advance(1200);
    CHECK(s_workspace->surface == ORB_SHELL_IDLE_SESSIONS);
    CHECK(lv_obj_get_scroll_y(s_workspace->list) == scroll); capture("idle-back");
    orb_workspace_payload_free(s_workspace->idle); s_workspace->idle = NULL;
    render_now(s_workspace); advance(1200);
    CHECK(find(s_workspace->page, "Couldn’t load sessions"));
    CHECK(find(s_workspace->page, "Not enough memory"));
    CHECK(!find(s_workspace->page, "Try again"));
    capture("idle-allocation-error");
}
static uint16_t s_live[466 * 466];
static void preview_equal(orb_shell_surface_t destination)
{
    route(destination == ORB_SHELL_DISPLAY ? ORB_SHELL_CONTROL_CENTER : ORB_SHELL_STACK);
    /* A neighbour may have displaced the earlier live plane. Force the same
     * cold fixture path instead of allowing a warm live snapshot to mask an
     * incomplete context copy. No compositor or renderer is stubbed. */
    for (unsigned slot = 0; slot < ORB_NAV_SNAPSHOT_SLOTS; slot++)
        if (s_workspace->nav_cached[slot] == destination)
            orb_nav_compositor_invalidate(s_workspace->compositor, slot);
    orb_workspace_back(s_workspace);
    for (unsigned i = 0; i < 20 && !s_workspace->nav_presenting; i++) advance(20);
    CHECK(s_workspace->nav_presenting && s_workspace->nav_back);
    CHECK(s_workspace->nav_target == destination);
    const uint16_t *preview = orb_nav_compositor_pixels(s_workspace->compositor, s_workspace->nav_to);
    CHECK(preview);
    memcpy(s_live, preview, sizeof(s_live));
    /* Hold one deterministic intermediate Back frame, then complete the real
     * destination build at the same tick so age labels cannot change. */
    orb_nav_compositor_drag(s_workspace->compositor, .62f);
    lv_obj_invalidate(s_workspace->nav_overlay); lv_refr_now(s_display); capture(s_case);
    nav_build_live(s_workspace); nav_accept(s_workspace);
    CHECK(s_workspace->surface == destination);
    lv_draw_buf_t draw; static uint16_t live[466 * 466];
    CHECK(lv_draw_buf_init(&draw, 466, 466, LV_COLOR_FORMAT_RGB565, 932, live, sizeof(live)) == LV_RESULT_OK);
    lv_obj_update_layout(s_workspace->page); lv_obj_raster_prepare(s_workspace->page);
    CHECK(orb_display_capture_snapshot(s_workspace->page, &draw));
    CHECK(!memcmp(s_live, live, sizeof(live)));
}
static void preview_display(void)
{
    route(ORB_SHELL_DISPLAY); click("Brightness");
    CHECK(orb_settings_host_page_for(s_workspace->settings, ORB_SHELL_DISPLAY) == ORB_SETTINGS_BRIGHTNESS_PAGE); preview_equal(ORB_SHELL_DISPLAY);
}
static void preview_notification(void)
{
    orb_card_t a = {.id="preview-a", .kind=ORB_CARD_ATTENTION, .title="Notice A", .body="Body A"};
    orb_card_t b = {.id="preview-b", .kind=ORB_CARD_ATTENTION, .title="Notice B", .body="Body B"};
    CHECK(orb_model_upsert_card(&a)); CHECK(orb_model_upsert_card(&b)); advance(2400);
    for (unsigned i = 0; i < orb_notification_history_count(s_workspace->notifications); i++)
        orb_notification_history_fixture_age(s_workspace->notifications,
            orb_notification_history_at(s_workspace->notifications, i)->id, false, 0, s_now);
    route(ORB_SHELL_NOTIFICATIONS); click("Notice B");
    CHECK(s_workspace->surface == ORB_SHELL_NOTIFICATION_DETAIL);
    CHECK(!strcmp(s_workspace->card, b.id)); preview_equal(ORB_SHELL_NOTIFICATION_DETAIL);
}
static void alarm_sheet(void)
{
    orb_time_init(); orb_time_poll((int64_t)s_now * 1000, 1793182140);
    orb_time_view_t *view = orb_time_view_create(lv_screen_active(), NULL); CHECK(view);
    orb_time_view_select(view, ORB_TIME_PAGE_ALARM); advance(1200);
    lv_obj_t *options = find(orb_time_view_root(view), "Options"); CHECK(options);
    while (options && !lv_obj_is_clickable(options)) options = lv_obj_get_parent(options);
    CHECK(options); lv_obj_send_event(options, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(options, LV_EVENT_RELEASED, NULL); lv_obj_send_event(options, LV_EVENT_CLICKED, NULL);
    advance(1200);
    CHECK(find(orb_time_view_root(view), "Alarm options"));
    CHECK(find(orb_time_view_root(view), ORB_ICON_X));
    CHECK(!find(orb_time_view_root(view), "Done")); capture("alarm-options");
    CHECK(orb_time_view_back(view)); advance(1200);
    CHECK(!find(orb_time_view_root(view), "Alarm options")); orb_time_view_destroy(view);
}
int main(int argc, char **argv)
{
    CHECK(argc >= 3); s_case = argv[1]; s_out = argc > 3 ? argv[3] : NULL;
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_set_connection(true, true);
    if (!strcmp(argv[2], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    if (!strcmp(s_case, "alarm-sheet")) alarm_sheet();
    else {
        s_workspace = orb_workspace_create(lv_screen_active(), NULL, NULL, NULL); CHECK(s_workspace);
        orb_workspace_show(s_workspace, true);
        orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
            .brightness=70, .battery_pct=80};
        strcpy(status.clock_time, "10:09"); orb_workspace_update(s_workspace, &status); advance(1200);
        if (!strcmp(s_case, "idle")) idle();
        else if (!strcmp(s_case, "preview-display")) preview_display();
        else if (!strcmp(s_case, "preview-notification")) preview_notification();
        else CHECK(false);
        orb_workspace_destroy(s_workspace);
    }
    printf("fixes2 workspace %s: %u assertions passed\n", s_case, s_checks);
    return 0;
}
