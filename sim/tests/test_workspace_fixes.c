/* Behavior regressions use the production workspace and isolated model data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static const char *s_case;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
static lv_area_t s_notifications_title, s_stack_title;
#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "workspace fixes %s:%d at %u: %s\n", s_case, __LINE__, s_now, #value); \
    exit(EXIT_FAILURE); } } while (0)
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *pointer, lv_indev_data_t *data) { (void)pointer; *data = s_input; }
static void advance(unsigned ms)
{
    for (unsigned elapsed = 0; elapsed < ms; elapsed += 20) {
        s_now += 20;
        if (s_shell) orb_shell_poll(s_shell);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}
static lv_obj_t *find(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return NULL;
    const char *rim = orb_kit_rim_text_get(root), *button = orb_kit_button_get_text(root);
    if ((rim && !strcasecmp(rim, text)) || (button && !strcmp(button, text)) ||
        (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *row_for(lv_obj_t *root, const char *text)
{
    if (orb_kit_row_switch(root) && find(root, text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = row_for(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static void fresh(bool online)
{
    if (s_shell) orb_shell_destroy(s_shell);
    orb_model_reset(); orb_home_reset();
    orb_model_set_connection(online, online);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=ORB_SHELL_LINK_ONLINE,
        .battery_pct=80, .brightness=70};
    strcpy(status.clock_time, "10:09");
    orb_shell_update(s_shell, &status); advance(1200);
}
static void tap(lv_obj_t *target)
{
    CHECK(target);
    lv_area_t bounds; lv_obj_get_coords(target, &bounds);
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(40);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(100);
}
static void tap_text(const char *text)
{
    lv_obj_t *target = find(orb_shell_root(s_shell), text); CHECK(target);
    while (target && !lv_obj_is_clickable(target)) target = lv_obj_get_parent(target);
    tap(target);
}
static void boot(void)
{
    orb_button_event_t event = {.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_SHORT, .synthetic=true};
    orb_shell_button(s_shell, &event); advance(1200);
}
static void receipts(void)
{
    for (unsigned close = 0; close < 2; close++) {
        fresh(true);
        orb_card_t card = {.id="approval-fixes1", .kind=ORB_CARD_APPROVAL,
            .title="Run checks", .body="Run the local checks?", .options_count=2};
        card.options[0] = (orb_option_t){.id="allow", .label="Allow", .role=ORB_OPTION_DEFAULT};
        card.options[1] = (orb_option_t){.id="deny", .label="Deny", .role=ORB_OPTION_CANCEL};
        CHECK(orb_model_upsert_card(&card)); advance(200);
        orb_shell_select_card(s_shell, card.id); advance(1200);
        tap_text(close ? "Allow" : "Deny");
        orb_answer_t answer; CHECK(orb_model_peek_answer(&answer));
        CHECK(orb_model_complete_answer(card.id, &answer, true, NULL)); advance(200);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
        CHECK(find(orb_shell_root(s_shell), "Delivered"));
        advance(2000); /* More than the former 600 ms reward departure. */
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
        CHECK(find(orb_shell_root(s_shell), "Delivered"));
        if (close) { tap_text("Done"); advance(1200); }
        else boot();
        CHECK(orb_shell_surface(s_shell) != ORB_SHELL_CARD);
    }
    for (unsigned local_time = 0; local_time < 2; local_time++) {
        fresh(true);
        orb_session_t session = {.id="sender-fixes1", .agent=ORB_AGENT_CODEX,
            .title="Local tests", .can_message=true, .state=ORB_SESSION_IDLE};
        CHECK(orb_model_upsert_session(&session));
        orb_voice_state_t voice = {.phase=ORB_VOICE_CONFIRMING, .stream=71, .text="Remember the checks"};
        strcpy(voice.target, local_time ? "time" : session.id);
        CHECK(orb_model_set_voice(&voice)); advance(1200);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
        tap_text(local_time ? "Save" : "Send");
        orb_voice_request_t request;
        CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CONFIRM);
        voice.phase = ORB_VOICE_SENDING; CHECK(orb_model_set_voice(&voice)); advance(200);
        orb_message_t message = {.stream=71};
        strcpy(message.target, voice.target); strcpy(message.text, voice.text);
        orb_model_complete_message_receipt(&message, ORB_SEND_DELIVERED, "Received", NULL);
        voice.phase = ORB_VOICE_IDLE; CHECK(orb_model_set_voice(&voice)); advance(2000);
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
        CHECK(find(orb_shell_root(s_shell), local_time ? "Saved" : "Delivered"));
        tap_text("Done"); advance(1200);
        CHECK(orb_shell_surface(s_shell) != ORB_SHELL_VOICE);
    }
}
static void disabled_sources(void)
{
    for (unsigned pending = 0; pending < 2; pending++) {
        fresh(pending != 0);
        if (pending) {
            orb_prefs_patch_t patch = {.has_idle_minutes=true, .idle_minutes=5};
            CHECK(orb_model_prefs_request(&patch));
        }
        orb_shell_navigate(s_shell, ORB_SHELL_SESSION_SETTINGS); advance(1200);
        const char *names[] = {"Claude", "Codex", "Dot"};
        /* Offline before the hub confirmed anything, sources are Unknown with
         * no switch to press at all (polish review #12). */
        if (!pending) { CHECK(find(orb_shell_root(s_shell), "Unknown")); CHECK(!row_for(orb_shell_root(s_shell), names[0])); continue; }
        for (unsigned i = 0; i < 3; i++) {
            lv_obj_t *row = row_for(orb_shell_root(s_shell), names[i]); CHECK(row);
            lv_obj_t *toggle = orb_kit_row_switch(row); CHECK(toggle);
            CHECK(lv_obj_has_state(row, LV_STATE_DISABLED));
            CHECK(lv_obj_has_state(toggle, LV_STATE_DISABLED));
            bool checked = orb_kit_switch_get_checked(toggle);
            if (lv_obj_is_visible(toggle)) tap(toggle);
            CHECK(orb_kit_switch_get_checked(toggle) == checked);
        }
    }
}
static void reference_geometry(const char *text, lv_area_t *bounds)
{
    lv_obj_t *reference = orb_kit_rim_title_create(lv_screen_active(), text, ORB_KIT_SECONDARY);
    CHECK(reference); lv_obj_update_layout(reference); lv_obj_get_coords(reference, bounds);
    lv_obj_delete(reference);
}
static void rim_geometry(const char *text, const lv_area_t *reference)
{
    lv_obj_t *actual = find(orb_shell_root(s_shell), text); CHECK(actual && orb_kit_rim_text_get(actual));
    lv_area_t bounds; lv_obj_get_coords(actual, &bounds);
    CHECK(bounds.x1 == reference->x1); CHECK(bounds.y1 == reference->y1);
    CHECK(bounds.x2 == reference->x2); CHECK(bounds.y2 == reference->y2);
}
static void visible_ink(const char *text)
{
    lv_obj_t *object = find(orb_shell_root(s_shell), text); CHECK(object);
    lv_area_t box; lv_obj_get_coords(object, &box);
    sim_display_rgb(s_pixels);
    unsigned ink = 0;
    for (int y = LV_MAX(0, box.y1); y <= LV_MIN(SIM_D - 1, box.y2); y++)
        for (int x = LV_MAX(0, box.x1); x <= LV_MIN(SIM_D - 1, box.x2); x++) {
            const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
            ink += pixel[0] > 90 && pixel[1] > 90 && pixel[2] > 90;
        }
    CHECK(ink > 20);
}
static void capture(const char *directory, const char *name)
{
    if (!directory) return;
    char path[1024]; CHECK(snprintf(path, sizeof(path), "%s/%s.png", directory, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
}
static void empties(const char *directory)
{
    /* Measure canonical wrappers before constructing the production tree, so
     * these test-only objects never perturb the captured surface's damage. */
    reference_geometry("Notifications", &s_notifications_title);
    reference_geometry("Smart Stack", &s_stack_title);
    fresh(true);
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "No notifications"));
    rim_geometry("Notifications", &s_notifications_title); visible_ink("Notifications"); visible_ink("No notifications");
    capture(directory, "notifications-empty");
    orb_card_t notice = {.id="empty-notice", .kind=ORB_CARD_ATTENTION, .title="Build finished"};
    CHECK(orb_model_upsert_card(&notice)); advance(1200);
    tap_text("Clear all"); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "No notifications"));
    rim_geometry("Notifications", &s_notifications_title); visible_ink("Notifications"); visible_ink("No notifications");
    capture(directory, "notifications-cleared");
    fresh(true);
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(1200);
    CHECK(find(orb_shell_root(s_shell), "All caught up"));
    rim_geometry("Smart Stack", &s_stack_title); visible_ink("Smart Stack"); visible_ink("All caught up");
    capture(directory, "stack-empty");
}
int main(int argc, char **argv)
{
    s_case = argc > 1 ? argv[1] : "receipts";
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init());
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    if (argc > 2 && !strcmp(argv[2], "ax3-bold-rt")) {
        CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    if (!strcmp(s_case, "receipts")) receipts();
    else if (!strcmp(s_case, "disabled")) disabled_sources();
    else empties(argc > 3 ? argv[3] : NULL);
    orb_shell_destroy(s_shell);
    printf("workspace fixes %s: %u checks passed\n", s_case, s_checks);
    return EXIT_SUCCESS;
}
