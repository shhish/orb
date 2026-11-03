/* Link the unchanged production compositor through an observer. This verifies
 * the transition actually admitted by the real click/Back paths, including
 * its launching token, rather than inferring an iris from moving pixels. */
#define orb_nav_compositor_begin observed_production_begin
#include "../../firmware/components/orb_nav/orb_nav_compositor.c"
#undef orb_nav_compositor_begin

#include <stdio.h>
#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "sim_display.h"

static unsigned s_begins;
static orb_nav_transition_kind_t s_kind;
static orb_nav_token_rect_t s_token;
static bool s_reverse;
static uint32_t s_now;
static orb_shell_t *s_shell;
static lv_display_t *s_display;
static bool s_check_source;
static uint8_t s_source_rgb[SIM_D * SIM_D * 3];

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "session iris: %s:%d: %s\n", __FILE__, __LINE__, #value); exit(1); \
} } while (0)

bool orb_nav_compositor_begin(orb_nav_compositor_t *c, orb_nav_transition_kind_t kind,
    unsigned from, unsigned to, orb_nav_direction_t direction,
    const orb_nav_token_rect_t *token, bool reverse, bool reduced_motion)
{
    ++s_begins; s_kind = kind; s_token = *token; s_reverse = reverse;
    if (s_check_source) {
        for (int y = 0; y < SIM_D; ++y) for (int x = 0; x < SIM_D; ++x) {
            if ((x-233)*(x-233) + (y-233)*(y-233) > 230*230) continue;
            const uint8_t *rgb = &s_source_rgb[(y * SIM_D + x) * 3];
            uint16_t expected = (uint16_t)((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | (rgb[2] >> 3));
            CHECK(c->slots[from].pixels[y * SIM_D + x] == expected);
        }
        s_check_source = false;
    }
    return observed_production_begin(c, kind, from, to, direction, token, reverse, reduced_motion);
}

static uint32_t tick(void) { return s_now; }
static void advance(unsigned ms)
{
    uint32_t end = s_now + ms;
    while (s_now < end) {
        s_now += LV_MIN(8, end - s_now);
        orb_shell_poll(s_shell); lv_timer_handler(); lv_refr_now(s_display);
    }
}
static lv_obj_t *find_action(lv_obj_t *root, int action)
{
    if (lv_obj_is_clickable(root) && (intptr_t)lv_obj_get_user_data(root) == action) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = find_action(lv_obj_get_child(root, i), action);
        if (found) return found;
    }
    return NULL;
}
static void check_token(const orb_nav_token_rect_t *expected)
{
    CHECK(s_kind == ORB_NAV_TRANSITION_IRIS);
    CHECK(abs(s_token.x - expected->x) <= 1 && abs(s_token.y - expected->y) <= 1);
    CHECK(s_token.width == expected->width && s_token.height == expected->height);
}
static void capture(const char *directory, const char *name, unsigned position)
{
    if (!directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/session-%s-%u.png", directory, name, position);
    CHECK(sim_display_save_png(path));
}

int main(int argc, char **argv)
{
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); orb_apps_init(ORB_APP_NEEDS_IMU);
    if (argc > 1 && !strcmp(argv[1], "ax3")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    if (argc > 1 && !strcmp(argv[1], "rm")) orb_ui_set_reduce_motion(true);
    CHECK(orb_model_init() && orb_home_init()); orb_model_load_demo();
    orb_model_set_connection(true, true); orb_home_load_demo();
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .brightness = 70, .battery_pct = 80, .usb = true};
    strcpy(status.clock_time, "10:09"); orb_shell_update(s_shell, &status); advance(1000);
    bool idle = argc > 3;
    orb_shell_surface_t source = idle ? ORB_SHELL_IDLE_SESSIONS : ORB_SHELL_SESSIONS;
    orb_shell_navigate(s_shell, source); advance(1200);
    if (idle) {
        orb_idle_request_t request; CHECK(orb_model_take_idle_request(&request));
        orb_session_t session = {.agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_IDLE};
        strcpy(session.id, "archived"); strcpy(session.title, "Archived session");
        CHECK(orb_model_complete_idle(&request, &session, 1, "", NULL)); advance(1200);
    }
    for (unsigned position = 0; position < 2; ++position) {
        lv_obj_t *row = find_action(orb_shell_root(s_shell), idle ? 320 : 120); CHECK(row);
        lv_obj_t *list = lv_obj_get_parent(row);
        lv_obj_scroll_to_view(row, LV_ANIM_OFF); orb_round_list_update(list); advance(300);
        /* Select in the full-size equatorial lane; its leading disc is the object
         * identity. A whole-row iris would originate in unrelated session text. */
        lv_area_t bounds; lv_obj_get_coords(row, &bounds);
        lv_obj_scroll_by(list, 0, (233 - (int)position * 72 - (bounds.y1 + bounds.y2 + 1) / 2), LV_ANIM_OFF);
        orb_round_list_update(list); advance(300);
        lv_obj_t *disc = NULL;
        for (unsigned i = 0; i < lv_obj_get_child_count(row); ++i) {
            lv_obj_t *child = lv_obj_get_child(row, i);
            if (lv_obj_get_width(child) == ORB_KIT_DISC_ROW && lv_obj_get_height(child) == ORB_KIT_DISC_ROW) disc = child;
        }
        if (idle) disc = lv_obj_get_child(row, 0);
        CHECK(disc); CHECK(orb_round_list_child_bounds(row, disc, &bounds));
        orb_nav_token_rect_t expected = {bounds.x1, bounds.y1, bounds.x2-bounds.x1+1, bounds.y2-bounds.y1+1};
        capture(argc > 2 ? argv[2] : NULL, "row", position);
        sim_display_rgb(s_source_rgb); s_check_source = true;
        unsigned before = s_begins;
        lv_obj_send_event(row, LV_EVENT_CLICKED, NULL); advance(24);
        CHECK(s_begins == before + 1); check_token(&expected); CHECK(!s_reverse);
        CHECK(sim_display_snapshot_transition_active());
        capture(argc > 2 ? argv[2] : NULL, "iris", position);
        advance(1000); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);
        capture(argc > 2 ? argv[2] : NULL, "detail", position);
        if (idle) {
            orb_idle_page_t pager;
            CHECK(orb_model_idle_snapshot(&pager) && pager.count == 1 && !pager.pending);
            orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); advance(1200);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
            orb_shell_back(s_shell); advance(1200);
            CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);
            CHECK(orb_model_idle_snapshot(&pager) && pager.count == 1 && !pager.pending);
        }
        before = s_begins; orb_shell_back(s_shell); advance(24);
        CHECK(s_begins == before + 1); check_token(&expected); CHECK(s_reverse);
        CHECK(sim_display_snapshot_transition_active());
        capture(argc > 2 ? argv[2] : NULL, "back", position);
        advance(1000); CHECK(orb_shell_surface(s_shell) == source);
        if (idle) {
            CHECK(find_action(orb_shell_root(s_shell), 320));
            orb_idle_request_t replacement;
            CHECK(!orb_model_take_idle_request(&replacement));
        }
        orb_answer_t answer; orb_home_request_t request;
        CHECK(!orb_model_peek_answer(&answer) && !orb_home_take_request(&request));
        printf("session iris: entry and return use disc {%d,%d,%d,%d}\n", expected.x, expected.y, expected.width, expected.height);
    }
    if (idle) {
        /* Home's canonical Face history drops the pager ancestry. */
        orb_shell_navigate(s_shell, ORB_SHELL_HOME); advance(1200);
        orb_idle_page_t pager; CHECK(!orb_model_idle_snapshot(&pager));
    }
    orb_shell_destroy(s_shell); lv_deinit(); return 0;
}
