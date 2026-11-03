/* Faces/transitions polish regressions that need the production workspace
 * (docs/agent-briefs/wave2/polish-faces-results.md). The workspace source is
 * included so its navigation history can be inspected, as test_fixes2 does. */
#include "../../firmware/components/orb_shell/orb_workspace.c"
#include "sim_display.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static orb_workspace_t *s_workspace;
static const char *s_case;
#define CHECK(v) do { ++s_checks; if (!(v)) { \
    fprintf(stderr, "polish faces workspace %s:%d: %s\n", s_case, __LINE__, #v); exit(1); \
} } while (0)
static uint32_t tick(void) { return s_now; }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 16) {
        s_now += 16;
        orb_workspace_poll(s_workspace);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}

static lv_obj_t *find_glyph(lv_obj_t *object, const char *glyph)
{
    if (lv_obj_is_hidden(object)) return NULL;
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), glyph)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_glyph(lv_obj_get_child(object, i), glyph);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_arc(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_arc_class)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_arc(lv_obj_get_child(object, i));
        if (found) return found;
    }
    return NULL;
}

/* Polish #34: Apps -> Agents is reset under the Face, so its Back closes the
 * iris onto the Face's Apps control (233,408), not the launcher disc. */
static void iris_close_token(void)
{
    orb_workspace_navigate(s_workspace, ORB_SHELL_APPS); advance(800);
    CHECK(s_workspace->surface == ORB_SHELL_APPS);
    lv_obj_t *disc = find_glyph(s_workspace->page, ORB_ICON_PROMPT);
    while (disc && !lv_obj_is_clickable(disc)) disc = lv_obj_get_parent(disc);
    CHECK(disc);
    lv_area_t area; lv_obj_get_coords(disc, &area);
    CHECK((area.y1 + area.y2) / 2 < 200); /* The launcher's Agents disc is near the top. */
    lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(disc, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
    advance(1200);
    CHECK(s_workspace->surface == ORB_SHELL_AGENTS);
    orb_nav_back_entry_t entry;
    CHECK(orb_nav_back_peek(&s_workspace->nav, &entry));
    CHECK(entry.id == ORB_SHELL_FACE && entry.iris);
    CHECK(entry.token.x + entry.token.width / 2 == 233 && entry.token.y + entry.token.height / 2 == 408);
    CHECK(entry.token.width == ORB_KIT_DISC_MINI && entry.token.height == ORB_KIT_DISC_MINI);
}

/* Polish #34: freeze the selected launch identity, rather than a focus ring
 * still travelling from Home after the physical press changed the caption. */
static void iris_frame_zero(void)
{
    orb_workspace_navigate(s_workspace, ORB_SHELL_APPS); advance(800);
    lv_obj_t *disc = find_glyph(s_workspace->page, ORB_ICON_PROMPT);
    while (disc && !lv_obj_is_clickable(disc)) disc = lv_obj_get_parent(disc);
    CHECK(disc);
    lv_obj_send_event(disc, LV_EVENT_PRESSED, NULL);
    CHECK(orb_apps_view_focus(s_workspace->apps_view) == ORB_SHELL_AGENTS);
    lv_obj_send_event(disc, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(disc, LV_EVENT_CLICKED, NULL);
    CHECK(s_workspace->nav_pending);
    CHECK(nav_capture(s_workspace, ORB_SHELL_APPS, 0));
    const uint16_t *pixels = orb_nav_compositor_pixels(s_workspace->compositor, 0);
    unsigned selected = 0, previous = 0;
    for (int y = -49; y <= 49; y++) for (int x = -49; x <= 49; x++) {
        int square = x * x + y * y;
        if (square < 43 * 43 || square > 49 * 49) continue;
        uint16_t a = pixels[(133 + y) * 466 + 233 + x];
        uint16_t b = pixels[(183 + y) * 466 + 146 + x];
        selected += (a >> 11) > 27 && ((a >> 5) & 63) > 55 && (a & 31) > 27;
        previous += (b >> 11) > 27 && ((b >> 5) & 63) > 55 && (b & 31) > 27;
    }
    CHECK(selected > 100);
    CHECK(previous == 0);
    /* A route changes before its async tree retirement. A late release from
     * the old Apps page must not roll its remembered launch focus back. */
    orb_workspace_navigate(s_workspace, ORB_SHELL_STACK);
    CHECK(s_workspace->surface == ORB_SHELL_STACK);
    CHECK(orb_apps_view_current(s_workspace->apps_view));
    bool suppressed = s_workspace->suppress_click;
    lv_obj_send_event(disc, LV_EVENT_PRESS_LOST, NULL);
    CHECK(orb_apps_view_focus(s_workspace->apps_view) == ORB_SHELL_AGENTS);
    CHECK(s_workspace->suppress_click == suppressed);
    advance(1200);
    CHECK(orb_apps_view_focus(s_workspace->apps_view) == ORB_SHELL_AGENTS);
}

/* Polish #9: Lock reads the committed preference. A face chosen in the
 * production gallery is what the locked surface presents. */
static void lock_reads_chosen_face(void)
{
    CHECK(faces_screen(s_workspace));
    orb_app_faces_hold(faces_screen(s_workspace));
    orb_app_faces_rotate(faces_screen(s_workspace), ORB_FACE_ACTIVITY);
    orb_app_faces_confirm_selection(faces_screen(s_workspace));
    for (unsigned i = 0; i < 50 && orb_app_faces_mode(faces_screen(s_workspace)) != ORB_FACES_FACE; i++) {
        advance(32);
        orb_faces_snapshot_t snapshot;
        if (orb_workspace_face_snapshot(s_workspace, &snapshot)) orb_faces_host_update(s_workspace->faces_host, &snapshot);
    }
    CHECK(orb_app_faces_mode(faces_screen(s_workspace)) == ORB_FACES_FACE);
    orb_faces_prefs_t committed;
    orb_faces_current_preferences(&committed);
    CHECK(committed.face == ORB_FACE_ACTIVITY);
}

/* The three ordinary shell layers may replace a page while Faces owns an
 * editor. Its draft and rollback target survive as the same framework instance. */
static void editor_layers(void)
{
    const orb_shell_surface_t layers[] = {ORB_SHELL_STACK, ORB_SHELL_NOTIFICATIONS, ORB_SHELL_CONTROL_CENTER};
    for (unsigned mode = ORB_FACES_GALLERY; mode <= ORB_FACES_SLOTS; mode++) {
        for (size_t layer = 0; layer < sizeof(layers) / sizeof(layers[0]); layer++) {
            orb_screen_instance_t *screen = faces_screen(s_workspace);
            CHECK(screen);
            unsigned char bytes[sizeof(orb_screen_context_t) + sizeof(orb_app_faces_context_t)] = {0};
            orb_screen_context_t *context = (orb_screen_context_t *)bytes;
            CHECK(orb_screen_save(screen, context, sizeof(bytes)));
            orb_app_faces_context_t value;
            memcpy(&value, context->payload, sizeof(value));
            value.prefs[1] = ORB_FACE_MERIDIAN;
            value.prefs[2] = 3;
            value.mode = mode;
            value.slot = 3;
            value.face_before_gallery = ORB_FACE_ORB;
            value.browsing = value.dirty = true;
            memcpy(context->payload, &value, sizeof(value));
            CHECK(orb_screen_restore(screen, context, sizeof(bytes)));
            lv_obj_t *view = orb_app_faces_root(screen);
            orb_workspace_navigate(s_workspace, layers[layer]); advance(1200);
            CHECK(s_workspace->surface == layers[layer] && !faces_screen(s_workspace));
            CHECK(orb_app_faces_retained(orb_app_host_current(s_workspace->faces_host)));
            CHECK(orb_app_host_current(s_workspace->faces_host) == screen);
            orb_workspace_back(s_workspace); advance(1200);
            CHECK(s_workspace->surface == ORB_SHELL_FACE && faces_screen(s_workspace) == screen);
            CHECK(orb_app_faces_root(screen) == view);
            CHECK(orb_screen_save(screen, context, sizeof(bytes)));
            orb_app_faces_context_t restored;
            memcpy(&restored, context->payload, sizeof(restored));
            CHECK(!memcmp(&restored, &value, sizeof(value)));
        }
    }
}

/* Power can change while Battery is open. Copy, symbol and ring must change
 * together instead of retaining the state that built the screen. */
static void charging_live_updates(void)
{
    orb_workspace_navigate(s_workspace, ORB_SHELL_CHARGING); advance(1200);
    CHECK(s_workspace->surface == ORB_SHELL_CHARGING);
    orb_shell_snapshot_t status = s_workspace->status;
    status.battery_pct = 9; status.usb = false; status.charging = false;
    orb_workspace_update(s_workspace, &status);
    orb_screen_instance_t *screen = orb_app_host_current(s_workspace->charging_host);
    CHECK(screen);
    lv_obj_t *root = orb_screen_root(screen);
    lv_obj_t *caption = find_glyph(root, "Low battery");
    lv_obj_t *icon = find_glyph(root, ORB_ICON_BATTERY_LOW);
    lv_obj_t *ring = find_arc(root);
    CHECK(caption && icon && ring);
    CHECK(!strcmp(lv_label_get_text(caption), "Low battery"));
    CHECK(!strcmp(lv_label_get_text(icon), ORB_ICON_BATTERY_LOW));
    CHECK(!lv_obj_is_hidden(icon));
    lv_color_t red = design_accent(s_workspace, ORB_DELIGHT_RED);
    CHECK(lv_color_eq(lv_obj_get_style_arc_color(ring, LV_PART_INDICATOR), red));
    status.battery_pct = 100; status.usb = true;
    orb_workspace_update(s_workspace, &status);
    CHECK(!strcmp(lv_label_get_text(caption), "Fully charged"));
    CHECK(!strcmp(lv_label_get_text(icon), ORB_ICON_PLUG));
    status.battery_pct = 80;
    orb_workspace_update(s_workspace, &status);
    CHECK(!strcmp(lv_label_get_text(caption), "Not charging"));
    status.usb = false;
    orb_workspace_update(s_workspace, &status);
    CHECK(!strcmp(lv_label_get_text(caption), "On battery"));
    CHECK(lv_obj_is_hidden(icon));
    status.battery_pct = -1;
    orb_workspace_update(s_workspace, &status);
    CHECK(!strcmp(lv_label_get_text(caption), "Battery unavailable"));
    CHECK(!strcmp(lv_label_get_text(icon), ORB_ICON_BATTERY_WARNING));
    CHECK(!lv_obj_is_hidden(icon));
}

/* A short arrival replaces the title lane without leaving clipped arc text,
 * then restores the stable place title when the transient expires. */
static void arrival_title_lane(void)
{
    orb_workspace_navigate(s_workspace, ORB_SHELL_STACK); advance(1200);
    CHECK(s_workspace->heading && !lv_obj_is_hidden(s_workspace->heading));
    orb_card_t card = {.id = "title-lane-arrival", .kind = ORB_CARD_MESSAGE,
        .title = "Build complete", .body = "Ready to review"};
    CHECK(orb_model_upsert_card(&card)); advance(64);
    CHECK(s_workspace->notification);
    CHECK(s_workspace->heading && lv_obj_is_hidden(s_workspace->heading));
    advance(2100);
    CHECK(!s_workspace->notification);
    CHECK(s_workspace->heading && !lv_obj_is_hidden(s_workspace->heading));
    CHECK(!strcmp(orb_kit_rim_text_get(s_workspace->heading), "SMART STACK"));
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2); s_case = argv[1];
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init());
    orb_model_set_connection(true, true);
    s_workspace = orb_workspace_create(lv_screen_active(), NULL, NULL, NULL); CHECK(s_workspace);
    orb_workspace_show(s_workspace, true);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .brightness = 70, .battery_pct = 80};
    strcpy(status.clock_time, "10:09"); orb_workspace_update(s_workspace, &status); advance(1200);
    if (!strcmp(s_case, "iris-close-token")) iris_close_token();
    else if (!strcmp(s_case, "iris-frame-zero")) iris_frame_zero();
    else if (!strcmp(s_case, "lock-chosen-face")) lock_reads_chosen_face();
    else if (!strcmp(s_case, "editor-layers")) editor_layers();
    else if (!strcmp(s_case, "charging-live-updates")) charging_live_updates();
    else if (!strcmp(s_case, "arrival-title-lane")) arrival_title_lane();
    else CHECK(false);
    orb_workspace_destroy(s_workspace);
    printf("polish faces workspace %s: %u assertions passed\n", s_case, s_checks);
    return 0;
}
