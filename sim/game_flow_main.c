#include "orb_app_play.h"
/* F16 integration evidence: real shell pointer input, registry/service queue,
 * game controller and LVGL view. Storage is throwaway memory; no hub or board. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_fs.h"
#include "orb_games.h"
#include "orb_game_logic.h"
#include "orb_home.h"
#include "orb_input.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_shell_services.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static const char *s_directory;
static lv_indev_data_t s_synthetic_input;

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Resolve the same bounded storage API as firmware, without mounting anything.
 * Four games keep one 12-byte high-score record each. Nothing reaches disk. */
static struct { char id[25]; unsigned char data[12]; size_t size; } s_scores[8];
static unsigned char s_garden[ORB_KEEPSAKE_RECORD_SIZE];
static size_t s_garden_size;
bool orb_fs_valid_app_id(const char *id)
{
    if (!id || !*id || strlen(id) > ORB_FS_APP_ID_MAX) return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    return true;
}
bool orb_fs_ready(void) { return true; }
esp_err_t orb_fs_app_prepare(const char *id)
{
    if (!orb_fs_valid_app_id(id)) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 8; i++) {
        if (!strcmp(s_scores[i].id, id)) return ESP_OK;
        if (!s_scores[i].id[0]) {
            snprintf(s_scores[i].id, sizeof(s_scores[i].id), "%s", id);
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
esp_err_t orb_fs_read(const char *id, const char *key, void *data, size_t capacity, size_t *size)
{
    if (!strcmp(id, "marble") && !strcmp(key, "keepsakes")) {
        if (!s_garden_size) return ESP_ERR_NOT_FOUND;
        if (capacity < s_garden_size) return ESP_ERR_INVALID_SIZE;
        memcpy(data, s_garden, s_garden_size);
        *size = s_garden_size;
        return ESP_OK;
    }
    if (strcmp(key, !strcmp(id, "ring-snake") ? "high-score-v2" : "high-score")) return ESP_ERR_NOT_FOUND;
    for (unsigned i = 0; i < 8; i++) {
        if (strcmp(s_scores[i].id, id) || !s_scores[i].size) continue;
        if (capacity < s_scores[i].size) return ESP_ERR_INVALID_SIZE;
        memcpy(data, s_scores[i].data, s_scores[i].size);
        *size = s_scores[i].size;
        return ESP_OK;
    }
    *size = 0;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t orb_fs_write(const char *id, const char *key, const void *data, size_t size)
{
    if (!strcmp(id, "marble") && !strcmp(key, "keepsakes")) {
        if (size > sizeof(s_garden)) return ESP_ERR_INVALID_SIZE;
        memcpy(s_garden, data, size);
        s_garden_size = size;
        return ESP_OK;
    }
    if (strcmp(key, !strcmp(id, "ring-snake") ? "high-score-v2" : "high-score") || size != 12) return ESP_ERR_INVALID_ARG;
    for (unsigned i = 0; i < 8; i++) {
        if (strcmp(s_scores[i].id, id)) continue;
        memcpy(s_scores[i].data, data, size);
        s_scores[i].size = size;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    *data = s_input;
}

static void read_synthetic_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    *data = s_synthetic_input;
}

static void advance(uint32_t ms, bool pwr, bool screen_on)
{
    while (ms) {
        uint32_t step = LV_MIN(ms, 16);
        s_now += step;
        /* This is main.c's real lifecycle order. PWR still polls when asleep. */
        orb_shell_apps_service();
        orb_apps_poll((int64_t)s_now * 1000, pwr, screen_on, .12f, .05f, .9915f, true);
        orb_shell_apps_publish();
        orb_shell_poll(s_shell);
        lv_timer_handler();
        lv_refr_now(s_display);
        ms -= step;
    }
}

static bool caption_is(lv_obj_t *obj, const char *caption)
{
    const char *rim = orb_kit_rim_text_get(obj);
    if (rim && !strcasecmp(rim, caption)) return true;
    if (!lv_obj_check_type(obj, &lv_label_class)) return false;
    const char *text = lv_label_get_text(obj);
    size_t n = strlen(text), v = strlen(caption);
    return !strcmp(text, caption) ||
        (n > v && text[n - v - 1] == ' ' && !strcmp(text + n - v, caption));
}

static lv_obj_t *control_matching(lv_obj_t *root, const char *caption, bool visible)
{
    if (visible && !lv_obj_is_visible(root)) return NULL;
    /* The shared Face launcher migrated from a60px button to the kit56 mini. */
    if (!strcmp(caption, "Apps") && lv_obj_is_clickable(root) &&
        lv_obj_get_width(root) == 56 && lv_obj_get_height(root) == 56) return root;
    if (caption_is(root, caption)) {
        if (lv_obj_is_clickable(root)) return root;
        for (lv_obj_t *obj = lv_obj_get_parent(root); obj; obj = lv_obj_get_parent(obj))
            if (lv_obj_is_clickable(obj)) return obj;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = control_matching(lv_obj_get_child(root, i), caption, visible);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *control(lv_obj_t *root, const char *caption)
{
    return control_matching(root, caption, true);
}

static void pointer(int x, int y, bool pressed)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
}

static void tap_xy(int x, int y)
{
    pointer(x, y, true);
    advance(32, false, true);
    pointer(x, y, false);
    advance(800, false, true);
}

static bool tap(const char *caption)
{
    lv_obj_t *obj = control(lv_screen_active(), caption);
    if (!obj) return false;
    lv_area_t box; lv_obj_get_coords(obj, &box);
    int x = (box.x1 + box.x2) / 2, y = (box.y1 + box.y2) / 2;
    if (x < 20 || x > 446 || y < 145 || y > 410) {
        /* Toolbar controls and the quiet bottom Done action sit outside a
         * list's reading lane, but still inside their round touch chords. */
        if (strcmp(caption, ORB_ICON_X) && strcmp(caption, ORB_ICON_STAR) && strcmp(caption, "Apps") &&
            strcmp(caption, "Done") && strcmp(caption, "Exit") && strcmp(caption, "Garden")) return false;
    }
    tap_xy(x, y);
    return true;
}


static void shot(const char *name)
{
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.png", s_directory, name) < sizeof(path));
    lv_refr_now(s_display);
    CHECK(sim_display_save_png(path));
}

/* Design 1.0's seven app catalogue retains the touch-safe honeycomb at Default
 * and reflows the same applications into accessible cached rows at AX3. */
static void apps_catalogue(void)
{
    CHECK(!control(lv_screen_active(), ORB_ICON_CHEVRON_LEFT));
    const char *symbols[] = {ORB_ICON_PROMPT, ORB_ICON_CHAT, ORB_ICON_HOUSE,
        ORB_ICON_TIMER, ORB_ICON_GAME_CONTROLLER, ORB_ICON_UPLOAD, ORB_ICON_GEAR};
    lv_area_t boxes[7];
    for (unsigned i = 0; i < 7; i++) {
        lv_obj_t *item = control_matching(orb_shell_root(s_shell), symbols[i], true);
        CHECK(item);
        lv_obj_get_coords(item, &boxes[i]);
        CHECK(lv_area_get_width(&boxes[i]) == 80 && lv_area_get_height(&boxes[i]) == 80);
        float x = (boxes[i].x1 + boxes[i].x2 + 1) * .5f;
        float y = (boxes[i].y1 + boxes[i].y2 + 1) * .5f;
        CHECK(hypotf(x - 233, y - 233) + 40 <= 223);
        for (unsigned j = 0; j < i; j++) {
            float previous_x = (boxes[j].x1 + boxes[j].x2 + 1) * .5f;
            float previous_y = (boxes[j].y1 + boxes[j].y2 + 1) * .5f;
            CHECK(hypotf(x - previous_x, y - previous_y) >= 100);
        }
    }

}

static lv_obj_t *visible_caption(lv_obj_t *root, const char *caption);

static void picker_swipe(int direction)
{
    pointer(233, 240, true);
    advance(32, false, true);
    for (unsigned i = 1; i <= 12; i++) {
        pointer(233 + direction * (int)i * 12, 240, true);
        advance(16, false, true);
    }
    pointer(233 + direction * 144, 240, false);
    advance(800, false, true);
}

static bool picker_select(const char *name)
{
    for (unsigned i = 0; i < ORB_GAME_COUNT; i++) {
        if (visible_caption(lv_screen_active(), name)) return true;
        picker_swipe(-1);
    }
    return false;
}

static void play_catalogue(void)
{
    CHECK(!control(lv_screen_active(), ORB_ICON_CHEVRON_UP));
    CHECK(!control(lv_screen_active(), ORB_ICON_CHEVRON_DOWN));
    CHECK(visible_caption(lv_screen_active(), "Marble"));
    CHECK(visible_caption(lv_screen_active(), "1/3 stages · 1 keepsake"));
    CHECK(!visible_caption(lv_screen_active(), "Play offline"));
    picker_swipe(-1);
    CHECK(visible_caption(lv_screen_active(), "Beads"));
    CHECK(!orb_apps_active()); /* Dragging on the miniature never launches it. */
    CHECK(orb_shell_twist(s_shell, 1)); advance(800, false, true);
    CHECK(visible_caption(lv_screen_active(), "Orbit"));
    CHECK(visible_caption(lv_screen_active(), "Best 31"));
    CHECK(orb_shell_twist(s_shell, -1)); advance(800, false, true);
    CHECK(visible_caption(lv_screen_active(), "Beads"));
    picker_swipe(1);
    CHECK(visible_caption(lv_screen_active(), "Marble"));
    /* A tangential contact on the outer rim belongs to the picker crown. */
    pointer(233, 23, true); advance(32, false, true);
    for (unsigned i = 1; i <= 24; i++) {
        float angle = -(float)M_PI / 2 + (float)i * (float)M_PI / 72;
        pointer(233 + (int)lroundf(210 * cosf(angle)),
                233 + (int)lroundf(210 * sinf(angle)), true);
        advance(32, false, true);
    }
    advance(240, false, true);
    pointer(s_input.point.x, s_input.point.y, false); advance(800, false, true);
    CHECK(visible_caption(lv_screen_active(), "Beads"));
    CHECK(!orb_apps_active() && orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    CHECK(picker_select("Marble"));
    /* The launcher manifest retains its model identity contract; scoped game
     * chrome uses Main2.0 tokens and picker dots retain Play green. */
    for (unsigned i = 0; i < ORB_GAME_COUNT; i++) {
        const orb_app_manifest_t *manifest = orb_apps_find(orb_game_id((orb_game_kind_t)i));
        CHECK(manifest && manifest->colour == orb_game_accent((orb_game_kind_t)i));
        CHECK(visible_caption(lv_screen_active(), manifest->name));
        tap_xy(431, 210);
    }
    CHECK(visible_caption(lv_screen_active(), "Marble"));
}

static void active_marble(lv_obj_t *shell_screen)
{
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "marble"));
    CHECK(lv_screen_active() != shell_screen);
    CHECK(!control(lv_screen_active(), ORB_ICON_X));
    CHECK(!control(lv_screen_active(), "Play"));
    CHECK(!control(lv_screen_active(), "Tap to Play"));
    CHECK(!control(lv_screen_active(), "Resume"));
    CHECK(!control(lv_screen_active(), "Garden"));
    orb_marble_snapshot_t proof;
    CHECK(!orb_marble_snapshot(&proof)); /* The old shell-only proof is not running. */
}

static void launch_marble(lv_obj_t *shell_screen)
{
    CHECK(picker_select("Marble"));
    CHECK(tap("Play"));
    active_marble(shell_screen);
}

static bool returned_item(lv_obj_t *shell_screen, const char *name)
{
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    lv_obj_t *item = visible_caption(shell_screen, name);
    CHECK(item);
    lv_area_t bounds; lv_obj_get_coords(item, &bounds);
    /* Existence in an offscreen LVGL tree is not a return-to-item proof. */
    int center_y = (bounds.y1 + bounds.y2) / 2;
    CHECK(center_y >= 145 && center_y <= 410);
    return true;
}

static lv_obj_t *visible_caption(lv_obj_t *root, const char *caption)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (caption_is(root, caption)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = visible_caption(lv_obj_get_child(root, (int32_t)i), caption);
        if (found) return found;
    }
    return NULL;
}

/* A balanced transcript replaces spaces with line breaks. Compare every
 * non-whitespace byte and every word boundary, so truncation or lost words
 * still fail the production overlay's full-transcript assertion. */
static bool reflowed_caption_is(lv_obj_t *object, const char *expected)
{
    if (!lv_obj_check_type(object, &lv_label_class)) return false;
    const char *shown = lv_label_get_text(object);
    while (*shown || *expected) {
        if (isspace((unsigned char)*shown) && isspace((unsigned char)*expected)) {
            while (isspace((unsigned char)*shown)) shown++;
            while (isspace((unsigned char)*expected)) expected++;
        } else {
            if (*shown != *expected) return false;
            shown++; expected++;
        }
    }
    return true;
}

static lv_obj_t *visible_reflowed_caption(lv_obj_t *root, const char *expected)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (reflowed_caption_is(root, expected)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = visible_reflowed_caption(lv_obj_get_child(root, (int32_t)i), expected);
        if (found) return found;
    }
    return NULL;
}

static void physical_button(orb_button_t button, orb_button_gesture_t gesture, bool synthetic)
{
    orb_button_event_t event = {
        .button = button, .gesture = gesture, .at_ms = s_now, .synthetic = synthetic,
    };
    if (!synthetic && gesture != ORB_BUTTON_DOWN && gesture != ORB_BUTTON_UP) {
        orb_button_event_t edge = event;
        edge.gesture = ORB_BUTTON_DOWN;
        orb_shell_button(s_shell, &edge);
        if (gesture == ORB_BUTTON_SHORT || gesture == ORB_BUTTON_DOUBLE) {
            edge.gesture = ORB_BUTTON_UP;
            orb_shell_button(s_shell, &edge);
        }
    }
    orb_shell_button(s_shell, &event);
    advance(800, false, true);
}

static void lock_and_wake_game(void)
{
    const orb_app_manifest_t *game = orb_apps_active();
    CHECK(game);
    physical_button(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_apps_active() == game);
    physical_button(ORB_BUTTON_PWR, ORB_BUTTON_SHORT, false);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_apps_active() == game);
}

/* Drive the real fixed-step model through Pop's full countdown. Rendering only
 * the resulting reading surface keeps this lifecycle check bounded and quick. */
static void finish_pop_round(void)
{
    for (unsigned steps = 0; steps <= ORB_GAME_POP_ROUND_STEPS; steps += 6) {
        s_now += 100;
        orb_apps_poll((int64_t)s_now * 1000, false, true, .12f, .05f, .9915f, true);
    }
    advance(800, false, true);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "pop"));
    CHECK(control(lv_screen_active(), "Play again"));
    CHECK(control(lv_screen_active(), "Done"));
    CHECK(!control(lv_screen_active(), "Play"));
    CHECK(!control(lv_screen_active(), "Tap to Play"));
}

static void pop_result_lifecycle(lv_obj_t *shell_screen)
{
    CHECK(picker_select("Pop"));
    CHECK(tap("Play"));
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "pop"));
    CHECK(!control(lv_screen_active(), "Play"));
    CHECK(!control(lv_screen_active(), "Tap to Play"));
    CHECK(!control(lv_screen_active(), "Play again"));
    finish_pop_round();
    shot("24-pop-round-over");
    CHECK(tap("Play again"));
    CHECK(orb_apps_active() && lv_screen_active() != shell_screen);
    CHECK(!control(lv_screen_active(), "Play"));
    CHECK(!control(lv_screen_active(), "Tap to Play"));
    CHECK(!control(lv_screen_active(), "Play again"));
    CHECK(!control(lv_screen_active(), "Done"));
    shot("25-pop-play-again-playing");
    finish_pop_round();
    CHECK(tap("Done"));
    CHECK(returned_item(shell_screen, "Pop"));
    shot("26-pop-done-return-picker");
}

/* Production games load a separate LVGL screen. A Voice tree under the old
 * shell screen can contain every right label while remaining invisible. This
 * regression checks the visible parent, real input provenance and app owner. */
static void game_system_overlays(lv_obj_t *shell_screen)
{
    /* The fake backend explicitly advertises a device-matching subscription;
     * games cannot manufacture Dot availability from the source preference. */
    orb_session_t dot = {.id = "dot", .agent = ORB_AGENT_DOT, .title = "Dot",
        .state = ORB_SESSION_IDLE, .reason = "Ready for your message", .can_message = true};
    CHECK(orb_model_upsert_session(&dot));
    launch_marble(shell_screen);
    const orb_app_manifest_t *app = orb_apps_active();
    lv_obj_t *game_screen = lv_screen_active();
    lv_obj_t *root = orb_shell_root(s_shell);
    lv_obj_t *parent = lv_obj_get_parent(root);
    lv_obj_t *top = lv_display_get_layer_top(s_display);
    CHECK(parent == shell_screen && game_screen != shell_screen);
    orb_voice_request_t request;
    CHECK(!orb_model_take_voice_request(&request));

    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, true);
    CHECK(lv_obj_get_parent(root) == parent && lv_screen_active() == game_screen);
    CHECK(!orb_apps_paused() && orb_apps_active() == app);
    CHECK(visible_caption(top, "Listening"));
    CHECK(!orb_model_take_voice_request(&request));
    shot("18-synthetic-game-listening-preview");
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_UP, true);

    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_START);
    CHECK(lv_obj_get_parent(root) == top);
    CHECK(lv_screen_active() == game_screen && orb_apps_active() == app && orb_apps_paused());
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
    orb_voice_state_t voice = {.phase = ORB_VOICE_RECORDING, .level = 35};
    strcpy(voice.target, "dot");
    CHECK(orb_model_set_voice(&voice));
    advance(800, false, true);
    CHECK(visible_caption(top, "Listening…"));
    shot("19-real-game-listening-overlay");
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_UP, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_STOP);
    voice.phase = ORB_VOICE_TRANSCRIBING;
    voice.level = 0;
    CHECK(orb_model_set_voice(&voice));
    advance(800, false, true);
    CHECK(visible_caption(top, "Transcribing…"));
    voice.phase = ORB_VOICE_CONFIRMING;
    strcpy(voice.text, "Tell me how the current session is progressing.");
    CHECK(orb_model_set_voice(&voice));
    advance(800, false, true);
    /* Design 2.0 keeps the recipient in the kit rim throughout review. */
    CHECK(visible_caption(top, "Dot") && orb_kit_rim_text_get(visible_caption(top, "Dot")));
    CHECK(visible_reflowed_caption(top, voice.text));
    lv_obj_t *send = control(top, "Send");
    CHECK(send && orb_apps_paused() && orb_apps_active() == app);
    shot("20-real-game-transcript-review");

    /* A separate pointer keeps the physical test pointer released throughout
     * the synthetic Send. Provenance must survive genuine LVGL dispatch. */
    lv_indev_t *synthetic = lv_indev_create();
    CHECK(synthetic);
    lv_indev_set_type(synthetic, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(synthetic, s_display);
    lv_indev_set_read_cb(synthetic, read_synthetic_pointer);
    orb_input_mark_synthetic(synthetic);
    lv_area_t bounds; lv_obj_get_coords(send, &bounds);
    s_synthetic_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    s_synthetic_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(synthetic); advance(32, false, true);
    s_synthetic_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(synthetic); advance(800, false, true);
    CHECK(!orb_model_take_voice_request(&request));
    CHECK(visible_caption(top, "Dot") && orb_kit_rim_text_get(visible_caption(top, "Dot")) &&
          control(top, "Send"));
    lv_indev_delete(synthetic);

    /* Simulate the authoritative worker result separately from input: a
     * completed send's receipt must remain visible until explicit Back. */
    voice.phase = ORB_VOICE_SENDING;
    CHECK(orb_model_set_voice(&voice));
    advance(800, false, true);
    orb_message_t delivered = {0};
    strcpy(delivered.target, voice.target);
    strcpy(delivered.text, voice.text);
    delivered.stream = voice.stream;
    orb_model_complete_message_receipt(&delivered, ORB_SEND_DELIVERED, "Received by the session.", NULL);
    voice.phase = ORB_VOICE_IDLE;
    CHECK(orb_model_set_voice(&voice));
    advance(800, false, true);
    CHECK(visible_caption(top, "Delivered"));
    CHECK(lv_obj_get_parent(root) == top && orb_apps_paused() && orb_apps_active() == app);
    shot("20b-real-game-delivery-receipt");

    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CANCEL);
    CHECK(lv_obj_get_parent(root) == parent && lv_screen_active() == game_screen);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    CHECK(orb_apps_active() == app && !orb_apps_paused());
    memset(&voice, 0, sizeof(voice));
    CHECK(orb_model_set_voice(&voice));
    advance(800, false, true);
    shot("21-game-restored-after-voice-back");

    physical_button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE, false);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    CHECK(lv_obj_get_parent(root) == top && orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    CHECK(visible_caption(top, "Controls") || visible_caption(top, ORB_ICON_GEAR));
    CHECK(lv_screen_active() == game_screen && orb_apps_active() == app && orb_apps_paused());
    CHECK(!orb_model_take_voice_request(&request));
    shot("23-real-game-control-center");
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(lv_obj_get_parent(root) == parent && orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    CHECK(lv_screen_active() == game_screen && orb_apps_active() == app && !orb_apps_paused());
    CHECK(!orb_model_take_voice_request(&request));

    /* Synthetic double presses may preview system navigation, while a real
     * PTT nested above it must retain the original app parent/pause owner. */
    physical_button(ORB_BUTTON_PWR, ORB_BUTTON_DOUBLE, true);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    CHECK(lv_obj_get_parent(root) == top && orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    CHECK(visible_caption(top, "Controls") || visible_caption(top, ORB_ICON_GEAR));
    CHECK(orb_apps_active() == app && orb_apps_paused());
    CHECK(!orb_model_take_voice_request(&request));
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_START);
    CHECK(lv_obj_get_parent(root) == top && orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_UP, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_STOP);
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CANCEL);
    CHECK(lv_obj_get_parent(root) == parent && orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    CHECK(lv_screen_active() == game_screen && orb_apps_active() == app && !orb_apps_paused());

    physical_button(ORB_BUTTON_PWR, ORB_BUTTON_HOLD, false);
    CHECK(visible_caption(top, "Sleep display") && visible_caption(top, "Restart"));
    CHECK(orb_apps_active() == app && orb_apps_paused() && lv_screen_active() == game_screen);
    shot("22-real-game-power-sheet");
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(!visible_caption(top, "Restart"));
    CHECK(orb_apps_active() == app && !orb_apps_paused() && lv_screen_active() == game_screen);

    /* PTT must also preserve a preexisting app pause rather than treating its
     * dismissal as a command to resume a game the owner already paused. */
    orb_apps_request_pause(true);
    advance(32, false, true);
    CHECK(orb_apps_paused());
    physical_button(ORB_BUTTON_PWR, ORB_BUTTON_HOLD, false);
    CHECK(visible_caption(top, "Restart") && orb_apps_paused());
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(!visible_caption(top, "Restart") && orb_apps_active() == app && orb_apps_paused());
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_HOLD, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_START);
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_UP, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_STOP);
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CANCEL);
    CHECK(lv_obj_get_parent(root) == parent && orb_apps_active() == app && orb_apps_paused());
    orb_apps_request_pause(false);
    advance(32, false, true);
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(returned_item(shell_screen, "Marble"));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: orb_game_flow_sim EXISTING_DIRECTORY [ax3] [bold] [rt] [rm]\n");
        return 2;
    }
    s_directory = argv[1];
    lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display && orb_model_init());
    orb_ui_init(NULL);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "ax3")) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        else if (!strcmp(argv[i], "bold")) CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
        else return 2;
    }
    orb_keepsake_record_t garden = {.magic = {'O', 'G', 'K', '1'}, .version = 1,
        .count = 1, .stages_cleared = 1, .items = {{.kind = 0, .stage = 1, .x = 0, .y = 0}}};
    s_garden_size = orb_keepsake_encode(&garden, s_garden, sizeof(s_garden));
    CHECK(s_garden_size == ORB_KEEPSAKE_RECORD_SIZE);
    strcpy(s_scores[0].id, "orbit");
    memcpy(s_scores[0].data, "OGS1", 4);
    uint32_t best = 31, checksum = best ^ 0x4f524231u;
    for (unsigned i = 0; i < 4; i++) {
        s_scores[0].data[4 + i] = (uint8_t)(best >> (8 * i));
        s_scores[0].data[8 + i] = (uint8_t)(checksum >> (8 * i));
    }
    s_scores[0].size = 12;
    /* No network capability is supplied, proving the game has no hub dependency. */
    (void)orb_app_play_init();
    orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_AUDIO | ORB_APP_NEEDS_STORAGE);
    char progress[48];
    orb_games_progress(ORB_GAME_BEADS, progress, sizeof(progress)); CHECK(!progress[0]);
    orb_games_progress(ORB_GAME_POP, progress, sizeof(progress)); CHECK(!progress[0]);
    CHECK(orb_apps_count() == 6); /* Five games plus the Time utility. */
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    CHECK(s_shell);
    lv_obj_t *shell_screen = lv_screen_active();
    orb_shell_snapshot_t snapshot = {
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_OFFLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70,
        .device_name = "Orb", .hub_name = "Mac mini", .clock_time = "10:09",
        .clock_date = "Tuesday, October 27",
    };
    orb_model_set_connection(false, false);
    orb_shell_update(s_shell, &snapshot);
    advance(800, false, true);
    CHECK(tap("Apps"));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    apps_catalogue();
    /* A resting navigation frame must already contain the complete catalogue.
     * Refreshing it explicitly must not restore missing title/icon pixels. */
    static uint8_t before[SIM_D * SIM_D * 3], after[SIM_D * SIM_D * 3];
    sim_display_rgb(before);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(s_display);
    sim_display_rgb(after);
    CHECK(!memcmp(before, after, sizeof(before)));
    shot("01-apps");
    CHECK(tap(ORB_ICON_GAME_CONTROLLER));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    play_catalogue();
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    CHECK(tap(ORB_ICON_GAME_CONTROLLER));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_PLAY);
    lv_obj_t *progress_label = visible_caption(lv_screen_active(), "1/3 stages · 1 keepsake");
    CHECK(progress_label);
    lv_area_t progress_bounds; lv_obj_get_coords(progress_label, &progress_bounds);
    CHECK(lv_area_get_height(&progress_bounds) >= 20);
    CHECK(lv_area_get_width(&progress_bounds) >= 200);
    lv_refr_now(s_display);
    sim_display_rgb(after);
    unsigned ink = 0;
    for (int y = progress_bounds.y1; y <= progress_bounds.y2; y++)
        for (int x = progress_bounds.x1; x <= progress_bounds.x2; x++)
            if (after[(y * SIM_D + x) * 3] > 50) ink++;
    CHECK(ink > 100); /* A valid label tree is insufficient if the saved pixels are blank. */
    shot("02-play-marble");
    CHECK(picker_select("Snake"));
    shot("02b-play-ring-snake");
    CHECK(picker_select("Marble"));
    /* Miniature and capsule must both queue the selected real game. */
    tap_xy(233, 162);
    active_marble(shell_screen);
    orb_apps_request_exit(); advance(800, false, true);
    CHECK(returned_item(shell_screen, "Marble"));
    launch_marble(shell_screen);
    shot("04-real-marble-playing");
    CHECK(!control(lv_screen_active(), ORB_ICON_X));
    CHECK(!control(lv_screen_active(), ORB_ICON_PAUSE));
    /* Actual screen and rim holds must release back into play, without the
     * synthetic short-click event accidentally opening the pause sheet. */
    pointer(420, 300, true);
    advance(800, false, true);
    CHECK(!control(lv_screen_active(), "Resume"));
    shot("18-screen-held-rewind");
    pointer(420, 300, false);
    advance(800, false, true);
    CHECK(!control(lv_screen_active(), "Resume"));
    pointer(233, 20, true);
    advance(800, false, true);
    shot("19-rim-held-rewind");
    pointer(233, 20, false);
    advance(800, false, true);
    CHECK(!control(lv_screen_active(), "Resume"));
    /* A short screen tap opens the explicit Resume surface. */
    tap_xy(365, 101);
    CHECK(control(lv_screen_active(), "Resume"));
    CHECK(control(lv_screen_active(), "Exit"));
    CHECK(!control(lv_screen_active(), ORB_ICON_X));
    shot("05-real-marble-paused-done");
    CHECK(tap("End round"));
    CHECK(tap(ORB_ICON_STAR));
    CHECK(control(lv_screen_active(), "Done"));
    shot("20-ended-round-garden");
    CHECK(tap("Done"));
    CHECK(control(lv_screen_active(), "Play again"));
    CHECK(tap("Play again"));
    CHECK(!control(lv_screen_active(), "Resume"));
    lock_and_wake_game();
    CHECK(control(lv_screen_active(), "Resume"));
    lock_and_wake_game();
    CHECK(control(lv_screen_active(), "Resume")); /* Wake preserves local Pause. */
    shot("21-pwr-paused");
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
    CHECK(returned_item(shell_screen, "Marble"));
    shot("06-close-return-same-play-item");
    launch_marble(shell_screen);
    lock_and_wake_game();
    CHECK(control(lv_screen_active(), "Resume"));
    orb_shell_back(s_shell);
    advance(800, false, true);
    CHECK(returned_item(shell_screen, "Marble"));
    shot("22-back-from-pause-return");
    launch_marble(shell_screen);
    tap_xy(420, 300);
    CHECK(control(lv_screen_active(), "Exit"));
    CHECK(control(lv_screen_active(), "End round"));
    CHECK(!control(lv_screen_active(), ORB_ICON_X));
    CHECK(tap("Exit"));
    CHECK(returned_item(shell_screen, "Marble"));
    shot("23-done-return");
    launch_marble(shell_screen);
    advance(64, false, false);
    CHECK(orb_apps_paused());
    shot("07-screen-off-paused");
    advance(64, false, true);
    CHECK(!orb_apps_paused());
    shot("08-wake-resumed");
    advance(16, true, true);
    shot("09-held-pwr-0000");
    advance(600, true, true);
    CHECK(orb_apps_active());
    shot("10-held-pwr-0600");
    advance(600, true, true);
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
    CHECK(returned_item(shell_screen, "Marble"));
    shot("11-held-pwr-1200-return-same-play-item");
    launch_marble(shell_screen);
    /* Wi-Fi loss as well as a missing Mac must preserve the game and return. */
    snapshot.net = ORB_SHELL_NET_FAILED;
    snapshot.link = ORB_SHELL_LINK_OFFLINE;
    orb_model_set_connection(false, false);
    orb_shell_update(s_shell, &snapshot);
    advance(1000, false, true);
    CHECK(orb_apps_active() && lv_screen_active() != shell_screen);
    shot("12-wifi-offline-game-continues");
    advance(1216, true, true);
    CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
    CHECK(returned_item(shell_screen, "Marble"));
    shot("13-wifi-offline-return-same-play-item");
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    advance(800, false, true);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    CHECK(tap(ORB_ICON_GAME_CONTROLLER));
    launch_marble(shell_screen);
    lock_and_wake_game();
    CHECK(tap("Exit"));
    CHECK(returned_item(shell_screen, "Marble"));
    shot("14-wifi-offline-apps-play-close-return");
    /* Every registered game immediately plays and returns through both routes offline.
     * Record another selected item so a reset-to-first-row bug cannot pass. */
    for (unsigned i = 0; i < orb_apps_count(); i++) {
        const orb_app_manifest_t *app = orb_apps_at(i);
        if (!strcmp(app->id, "time")) continue; /* Utility is covered by time_shell_main. */
        CHECK(!(app->needs & ORB_APP_NEEDS_NET));
        for (unsigned route = 0; route < 2; route++) {
            CHECK(picker_select(app->name));
            CHECK(tap("Play"));
            CHECK(orb_apps_active() == app && lv_screen_active() != shell_screen);
            CHECK(!control(lv_screen_active(), "Play"));
            CHECK(!control(lv_screen_active(), "Tap to Play"));
            CHECK(!control(lv_screen_active(), "Resume"));
            CHECK(!control(lv_screen_active(), "Play again"));
            char frame[100];
            snprintf(frame, sizeof(frame), "15-%s-%s-playing", app->id, route ? "pwr" : "boot");
            shot(frame);
            if (route) advance(1216, true, true);
            else {
                lock_and_wake_game();
                CHECK(control(lv_screen_active(), "Resume"));
                physical_button(ORB_BUTTON_BOOT, ORB_BUTTON_SHORT, false);
            }
            CHECK(!orb_apps_active() && lv_screen_active() == shell_screen);
            CHECK(returned_item(shell_screen, app->name));
            snprintf(frame, sizeof(frame), "16-%s-%s-return", app->id, route ? "pwr" : "boot");
            shot(frame);
        }
    }
    pop_result_lifecycle(shell_screen);
    snapshot.net = ORB_SHELL_NET_CONNECTED;
    snapshot.link = ORB_SHELL_LINK_ONLINE;
    orb_model_set_connection(true, true);
    orb_shell_update(s_shell, &snapshot);
    advance(800, false, true);
    CHECK(returned_item(shell_screen, "Pop"));
    shot("17-reconnected-same-play-item");
    game_system_overlays(shell_screen);
    CHECK(orb_games_last_save_error() == ESP_OK);
    orb_shell_destroy(s_shell);
    lv_indev_delete(s_pointer);
    printf("game flow: %u checks; real Apps→Play→Marble, Pause/BOOT, screen-off/wake, held PWR, immediate offline play/BOOT/held PWR for all five games, same Play item, Pop Play again/Done, game Voice review and Power overlays\n", s_checks);
    return 0;
}
