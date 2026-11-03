/* Public shell regressions: synthetic registry/model input, real LVGL pointer
 * dispatch and framebuffer delivery. No live hub, audio output or board. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_shell_services.h"
#include "orb_sound.h"
#include "orb_status.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include <strings.h>
#include "sim_display.h"
#include "lvgl_private.h"

static uint32_t s_now;
static unsigned s_checks;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static uint8_t s_rgb[SIM_D * SIM_D * 3];

#define CHECK(condition) do { \
    s_checks++; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    LV_UNUSED(input);
    *data = s_input;
}

static void advance(uint32_t elapsed)
{
    while (elapsed) {
        uint32_t step = LV_MIN(elapsed, 16);
        s_now += step;
        orb_shell_poll(s_shell);
        lv_timer_handler();
        lv_refr_now(s_display);
        elapsed -= step;
    }
}

static lv_obj_t *find_text(lv_obj_t *obj, const char *text)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    const char *rim = orb_kit_rim_text_get(obj);
    if (rim && !strcasecmp(rim, text)) return obj;
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), text)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(obj, i), text);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *text(const char *caption) { return find_text(orb_shell_root(s_shell), caption); }

static lv_obj_t *find_control(lv_obj_t *obj, const char *caption)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    if (lv_obj_check_type(obj, &lv_label_class) && strstr(lv_label_get_text(obj), caption)) {
        for (lv_obj_t *p = lv_obj_get_parent(obj); p; p = lv_obj_get_parent(p))
            if (lv_obj_is_clickable(p)) return p;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_control(lv_obj_get_child(obj, i), caption);
        if (found) return found;
    }
    return NULL;
}

static void tap(const char *caption)
{
    lv_obj_t *control = find_control(orb_shell_root(s_shell), caption);
    if (!control) fprintf(stderr, "missing delight control: %s\n", caption);
    CHECK(control != NULL);
    lv_area_t box; lv_obj_get_coords(control, &box);
    s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(16);
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); advance(16);
}

static orb_shell_snapshot_t connected(void)
{
    orb_shell_snapshot_t snapshot = {.net = ORB_SHELL_NET_CONNECTED,
        .link = ORB_SHELL_LINK_ONLINE, .battery_pct = 80, .rssi = -54,
        .brightness = 70, .usb = true};
    strcpy(snapshot.clock_time, "10:09");
    strcpy(snapshot.clock_date, "Tuesday, October 27");
    strcpy(snapshot.hub_name, "Mac mini");
    return snapshot;
}

static void fresh(void)
{
    if (s_shell) orb_shell_destroy(s_shell);
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    orb_ui_set_reduce_motion(false);
    orb_ui_set_reduce_transparency(false);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL);
    CHECK(s_shell != NULL);
    orb_shell_snapshot_t snapshot = connected();
    orb_shell_update(s_shell, &snapshot);
    orb_shell_environment(s_shell, 0, 0, 1, false);
    advance(1000);
}

static lv_obj_t *find_arc(lv_obj_t *obj, bool face)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    if (lv_obj_check_type(obj, &lv_arc_class) &&
        (face ? lv_obj_get_width(obj) > 400 :
                lv_obj_get_width(obj) >= 72 && lv_obj_get_width(obj) < 160)) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = find_arc(lv_obj_get_child(obj, i), face);
        if (found) return found;
    }
    return NULL;
}

static unsigned changed_frames(uint32_t duration)
{
    unsigned frames = 0;
    sim_display_take_changed();
    for (uint32_t elapsed = 0; elapsed < duration; elapsed += 16) {
        advance(16);
        frames += sim_display_take_changed();
    }
    return frames;
}

static void face_cadence_and_mute(void)
{
    fresh();
    CHECK(!find_arc(orb_shell_root(s_shell), true));
    CHECK(changed_frames(600) == 0); /* The quiet Face has no decorative rim animation. */
    orb_ui_set_reduce_motion(true);
    advance(1000);
    CHECK(changed_frames(600) == 0);
    orb_ui_set_reduce_motion(false);
    orb_shell_environment(s_shell, 0, 0, 1, true);
    advance(1000);
    CHECK(changed_frames(2000) <= 3); /* Always On never becomes a 60Hz loop. */
    orb_shell_environment(s_shell, 0, 0, -1, false);
    CHECK(!orb_sound_play(ORB_SOUND_NOTIFICATION));
    orb_shell_environment(s_shell, 0, 0, 1, false);
    CHECK(orb_sound_play(ORB_SOUND_NOTIFICATION)); /* Host backend emits no audio. */
    orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); advance(800);
    tap(ORB_ICON_BELL); advance(800);
    CHECK(!orb_sound_play(ORB_SOUND_NOTIFICATION));
    tap(ORB_ICON_BELL_SLASH); advance(800);
    CHECK(orb_sound_play(ORB_SOUND_NOTIFICATION));
    /* System setup hides the Face and must leave no working-ring repaint loop. */
    orb_shell_snapshot_t snapshot = connected(); snapshot.net = ORB_SHELL_NET_UNPROVISIONED;
    orb_shell_update(s_shell, &snapshot); advance(1000);
    /* Preparing setup code is now a named wait. It may breathe at <=10 Hz,
     * then the original no-hidden-repaint assertion applies after its budget. */
    CHECK(changed_frames(600) <= 6);
    advance(30000);
    CHECK(changed_frames(600) == 0);
}

static void face_layout_and_count(void)
{
    fresh();
    for (unsigned size = 0; size < 2; size++) {
        CHECK(orb_fonts_set_content_size(size ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(size) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(size); advance(800);
        /* Design 2.0 replaces the status component's inline baseline with
         * one canonical 80 px kit complication and its short numeric form. */
        lv_obj_t *caption = text(ORB_ICON_BATTERY_HIGH);
        CHECK(caption && text("10:09"));
        lv_obj_t *disc = lv_obj_get_parent(caption);
        CHECK(lv_obj_get_width(disc) == ORB_KIT_DISC && lv_obj_get_height(disc) == ORB_KIT_DISC);
        lv_area_t ink, bounds;
        lv_obj_get_coords(caption, &ink); lv_obj_get_coords(disc, &bounds);
        CHECK(ink.x1 >= bounds.x1 && ink.x2 <= bounds.x2);

    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false); advance(800);
    orb_model_snapshot_t model; CHECK(orb_model_snapshot(&model));
    model.sessions[1].state = ORB_SESSION_WAITING;
    CHECK(orb_model_upsert_session(&model.sessions[1]));
    advance(176);
    /* Incoming attention no longer replaces Calendar/complication content. */
    CHECK(text("10:09") && !text("2 waiting"));
    sim_display_rgb(s_rgb);
    unsigned white = 0;
    for (int y = 130; y < 200; y++) for (int x = 90; x < 375; x++) {
        const uint8_t *p = &s_rgb[(y * SIM_D + x) * 3];
        white += p[0] > 210 && p[1] > 210 && p[2] > 210;
    }
    CHECK(white > 1000); /* An attention update never erases the clock framebuffer. */
}

static unsigned voice_bar_geometry(lv_obj_t *obj, unsigned *height)
{
    if (!lv_obj_is_visible(obj)) return 0;
    unsigned count = 0;
    /* Shell migration is a separate stream: its existing listening bars keep
     * Design 1.0 red while shared ORB_RED now follows the 2.0 palette. */
    lv_color_t colour = lv_obj_get_style_bg_color(obj, 0);
    if (lv_obj_get_width(obj) == 10 &&
        (lv_color_eq(colour, ORB_RED) || lv_color_eq(colour, lv_color_hex(0xFF4245))) &&
        lv_obj_get_style_bg_opa(obj, 0) == LV_OPA_COVER) {
        lv_area_t bounds; lv_obj_get_coords(obj, &bounds);
        CHECK(abs((bounds.y1 + bounds.y2) / 2 - 182) <= 1);
        CHECK(lv_obj_get_width(obj) == 10);
        *height = LV_MAX(*height, (unsigned)lv_obj_get_height(obj));
        count++;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++)
        count += voice_bar_geometry(lv_obj_get_child(obj, i), height);
    return count;
}

static void voice_level_geometry(void)
{
    fresh();
    orb_voice_state_t voice = {.phase = ORB_VOICE_RECORDING, .stream = 9};
    strcpy(voice.target, "codex-shell"); CHECK(orb_model_set_voice(&voice)); advance(800);
    unsigned quiet = 0, loud = 0;
    CHECK(voice_bar_geometry(orb_shell_root(s_shell), &quiet) == 9 && quiet == 6);
    voice.level = 100; CHECK(orb_model_set_voice(&voice)); advance(16);
    CHECK(voice_bar_geometry(orb_shell_root(s_shell), &loud) == 9 && loud > quiet);
    CHECK(!find_arc(orb_shell_root(s_shell), false));
    CHECK(text("Listening"));
    orb_ui_set_reduce_motion(true); advance(800);
    voice.level = 0; CHECK(orb_model_set_voice(&voice)); advance(16);
    quiet = 0; CHECK(voice_bar_geometry(orb_shell_root(s_shell), &quiet) == 9 && quiet == 6);
    voice.level = 100; CHECK(orb_model_set_voice(&voice)); advance(16);
    loud = 0; CHECK(voice_bar_geometry(orb_shell_root(s_shell), &loud) == 9 && loud > quiet);
    /* Real amplitude remains available under Reduce Motion; there is no idle
     * animation and the glyph's width and baseline never shift. */
}

static double channel_linear(unsigned channel)
{
    double value = channel / 255.0;
    return value <= .04045 ? value / 12.92 : pow((value + .055) / 1.055, 2.4);
}

static double luminance(unsigned red, unsigned green, unsigned blue)
{
    return .2126 * channel_linear(red) + .7152 * channel_linear(green) + .0722 * channel_linear(blue);
}

static void primary_contrast(void)
{
    /* Check the actual rendered tinted material, not merely the palette's
     * contrast on black. Semibold primary captions need at least 3:1 (HIG §5). */
    fresh();
    orb_voice_state_t voice = {.phase = ORB_VOICE_CONFIRMING, .stream = 17, .text = "Run the tests"};
    strcpy(voice.target, "codex-shell"); CHECK(orb_model_set_voice(&voice));
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true); advance(800);
    lv_obj_t *control = find_control(orb_shell_root(s_shell), "Send");
    lv_obj_t *caption = text("Send"); CHECK(control && caption);
    lv_area_t box; lv_obj_get_coords(control, &box);
    sim_display_rgb(s_rgb);
    /* Top-centre interior is safely inside the capsule, above the caption. */
    int x = (box.x1 + box.x2) / 2, y = box.y1 + 10;
    const uint8_t *pixel = &s_rgb[(y * SIM_D + x) * 3];
    double background = luminance(pixel[0], pixel[1], pixel[2]);
    uint32_t ink = lv_color_to_u32(lv_obj_get_style_text_color(caption, 0));
    double foreground = luminance((ink >> 16) & 255, (ink >> 8) & 255, ink & 255);
    double ratio = (fmax(background, foreground) + .05) / (fmin(background, foreground) + .05);
    if (ratio < 3) fprintf(stderr, "tinted Send contrast: %.3f:1\n", ratio);
    CHECK(ratio >= 3);
}

static void icon_framebuffer_contrast(const char *symbol)
{
    lv_obj_t *glyph = text(symbol); CHECK(glyph);
    lv_obj_t *disc = lv_obj_get_style_bg_opa(glyph, 0) == LV_OPA_COVER ? glyph : lv_obj_get_parent(glyph);
    lv_area_t box; lv_obj_get_coords(disc, &box);
    lv_obj_t *row = lv_obj_get_parent(disc);
    lv_obj_t *list = NULL;
    int original_scroll = 0;
    lv_area_t presented;
    if (orb_round_list_child_bounds(row, disc, &presented)) {
        /* Rim rows are intentionally 50% previews. Inspect each identity in
         * the focused equator position, where its actual reading contrast is
         * required, rather than sampling the faded neighbouring preview. */
        list = lv_obj_get_parent(row);
        original_scroll = lv_obj_get_scroll_y(list);
        lv_area_t row_box; lv_obj_get_coords(row, &row_box);
        lv_obj_scroll_by(list, 0, 233 - (row_box.y1 + row_box.y2) / 2, LV_ANIM_OFF);
        orb_round_list_update(list);
        lv_refr_now(s_display);
        CHECK(orb_round_list_child_bounds(row, disc, &box));
    }
    sim_display_rgb(s_rgb);
    /* Sample the circle's leading interior, clear of its glyph. AX3 rows
     * can crop the disc's top at the viewport edge, so top+5 is not a valid
     * material sample there. The centre baseline is wholly visible. */
    int x = box.x1 + 6, y = (box.y1 + box.y2) / 2;
    const uint8_t *pixel = &s_rgb[(y * SIM_D + x) * 3];
    double background = luminance(pixel[0], pixel[1], pixel[2]);
    uint32_t ink = lv_color_to_u32(lv_obj_get_style_text_color(glyph, 0)) & 0xffffff;
    lv_color_t flat = lv_obj_get_style_bg_color(disc, 0);
    CHECK(ink == (lv_color_to_u32(orb_kit_accent_ink(flat)) & 0xffffff));
    double foreground = ink ? 1 : 0;
    double ratio = (fmax(background, foreground) + .05) / (fmin(background, foreground) + .05);
    if (ratio < 3) fprintf(stderr, "launcher glyph %s at %d,%d (%dx%d): ink=%06x pixel=%u,%u,%u contrast %.3f:1\n", symbol,
        (int)box.x1, (int)box.y1, (int)lv_obj_get_width(disc), (int)lv_obj_get_height(disc),
        (unsigned)ink, pixel[0], pixel[1], pixel[2], ratio);
    CHECK(ratio >= 3);
    CHECK(abs(pixel[0]-flat.red) <= 8 && abs(pixel[1]-flat.green) <= 8 && abs(pixel[2]-flat.blue) <= 8);
    unsigned black = 0, white = 0;
    /* The central square lies wholly inside the flat circle. Count actual
     * painted ink: a white-centred black contour would fail this regression. */
    int inset = lv_obj_get_width(disc) / 5;
    for (int row = box.y1 + inset; row <= box.y2 - inset; row++)
        for (int column = box.x1 + inset; column <= box.x2 - inset; column++) {
            const uint8_t *p = &s_rgb[(row * SIM_D + column) * 3];
            black += p[0] < 12 && p[1] < 12 && p[2] < 12;
            white += p[0] > 245 && p[1] > 245 && p[2] > 245;
        }
    CHECK(ink ? white >= 10 : black >= 10);
    CHECK(ink ? black == 0 : white == 0);
    if (list) {
        lv_obj_scroll_to_y(list, original_scroll, LV_ANIM_OFF);
        lv_refr_now(s_display);
    }
}

static void launcher_glyph_contrast(void)
{
    static const char *symbols[] = {ORB_ICON_PROMPT, ORB_ICON_CHAT, ORB_ICON_HOUSE,
        ORB_ICON_TIMER, ORB_ICON_GAME_CONTROLLER, ORB_ICON_UPLOAD, ORB_ICON_GEAR};
    for (unsigned opaque = 0; opaque < 2; opaque++) {
        fresh(); orb_ui_set_reduce_transparency(opaque);
        orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(800);
        for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
            icon_framebuffer_contrast(symbols[i]);
    }
    fresh();
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(800);
    icon_framebuffer_contrast(ORB_ICON_PROMPT);
    icon_framebuffer_contrast(ORB_ICON_CHAT);
}

static void enlarged_heading_clearance(void)
{
    fresh();
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true);
    const struct { orb_shell_surface_t surface; const char *title; } cases[] = {
        {ORB_SHELL_STACK, "Smart Stack"}, {ORB_SHELL_NOTIFICATIONS, "Notifications"},
        {ORB_SHELL_AGENTS, "Firmware build"}, {ORB_SHELL_SETTINGS, "Settings"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        orb_shell_navigate(s_shell, cases[i].surface); advance(800);
        lv_obj_t *title = text(cases[i].title);
        lv_obj_t *back = find_control(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT);
        CHECK(title && !back);
        orb_kit_rim_text_info_t rim;
        if (orb_kit_rim_text_info(title, &rim)) {
            /* Curved titles are measured along their baseline, not a straight chord. */
            CHECK(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES);
            continue;
        }
        lv_point_t measured;
        lv_text_get_size(&measured, lv_label_get_text(title), lv_obj_get_style_text_font(title, 0),
                        lv_obj_get_style_text_letter_space(title, 0),
                        lv_obj_get_style_text_line_space(title, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        CHECK(measured.x <= lv_obj_get_width(title)); /* No ellipsis hiding the destination. */
        lv_area_t title_box;
        lv_obj_get_coords(title, &title_box);
        CHECK(measured.x <= orb_chord_width(title_box.y1, title_box.y2, ORB_RIM_MARGIN_TEXT));
    }
}

static lv_obj_t *rim_indicator(lv_obj_t *obj)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    int diameter = 2 * (ORB_KIT_SCROLL_RADIUS + 2);
    if (lv_obj_check_type(obj, &lv_arc_class) && lv_obj_get_width(obj) == diameter &&
        lv_obj_get_height(obj) == diameter && lv_obj_get_style_arc_width(obj, LV_PART_INDICATOR) == 4) return obj;
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *found = rim_indicator(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return NULL;
}

static void rim_list_position_and_stability(void)
{
    fresh(); orb_shell_navigate(s_shell, ORB_SHELL_IDLE_SESSIONS); advance(800);
    orb_idle_request_t request; CHECK(orb_model_take_idle_request(&request));
    orb_session_t idle[] = {
        {.id = "idle-a", .title = "Idle review A", .agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_IDLE},
        {.id = "idle-b", .title = "Idle review B", .agent = ORB_AGENT_CODEX, .state = ORB_SESSION_IDLE},
        {.id = "idle-c", .title = "Idle review C", .agent = ORB_AGENT_CLAUDE, .state = ORB_SESSION_IDLE},
    };
    /* The common list inset allows two rows without scrolling. A real third
     * row preserves this test’s overflowing-list indicator contract. */
    CHECK(orb_model_complete_idle(&request, idle, 3, "", NULL)); advance(800);
    lv_obj_t *name = text("Idle review A"); CHECK(name);
    lv_obj_t *list = lv_obj_get_parent(name);
    while (list && !lv_obj_is_scrollable(list)) list = lv_obj_get_parent(list);
    CHECK(list && lv_obj_is_scrollable(list));
    lv_obj_t *mark = rim_indicator(orb_shell_root(s_shell)); CHECK(mark);
    lv_obj_update_layout(orb_shell_root(s_shell));
    lv_area_t bounds; lv_obj_get_coords(mark, &bounds);
    if (bounds.x1 + bounds.x2 + 1 != 466 || bounds.y1 + bounds.y2 + 1 != 466) {
        lv_area_t parent; lv_obj_get_coords(lv_obj_get_parent(mark), &parent);
        fprintf(stderr, "kit scroll arc=(%d,%d)-(%d,%d) parent=(%d,%d)-(%d,%d) hidden=%d\n",
            bounds.x1, bounds.y1, bounds.x2, bounds.y2, parent.x1, parent.y1, parent.x2, parent.y2,
            lv_obj_is_hidden(mark));
    }
    CHECK(bounds.x1 + bounds.x2 + 1 == 466 && bounds.y1 + bounds.y2 + 1 == 466);
    CHECK(lv_obj_get_style_arc_width(mark, LV_PART_MAIN) == 4);
    CHECK(lv_obj_get_style_arc_opa(mark, LV_PART_INDICATOR) == 230);
    CHECK(lv_arc_get_bg_angle_start(mark) == 345 && lv_arc_get_bg_angle_end(mark) == 15);
    lv_value_precise_t before = lv_arc_get_angle_start(mark);
    int end = lv_obj_get_scroll_y(list) + lv_obj_get_scroll_bottom(list); CHECK(end > 0);
    /* Let the real list finish its deferred chord/layout preparation before
     * measuring the resting indicator; it follows any final extent change. */
    lv_obj_scroll_to_y(list, end, LV_ANIM_OFF); advance(1000);
    CHECK(mark == rim_indicator(orb_shell_root(s_shell)));
    lv_value_precise_t after = lv_arc_get_angle_start(mark); CHECK(after != before);
    advance(1000);
    CHECK(mark == rim_indicator(orb_shell_root(s_shell)) && !lv_obj_is_hidden(mark));
    CHECK(lv_arc_get_angle_start(mark) == after);
    CHECK(lv_obj_get_style_arc_opa(mark, LV_PART_INDICATOR) == 230);
}

static void play_registry(void)
{
    fresh();
    CHECK(orb_apps_count() == 0);
    orb_shell_navigate(s_shell, ORB_SHELL_PLAY); advance(800);
    CHECK(text("No games installed"));
    orb_apps_init(ORB_APP_NEEDS_IMU);
    fresh(); orb_shell_navigate(s_shell, ORB_SHELL_PLAY); advance(800);
    CHECK(text("Marble") && find_control(orb_shell_root(s_shell), "Play"));
    tap("Play");
    orb_shell_app_state_t state; orb_shell_apps_snapshot(&state);
    CHECK(state.pending && !strcmp(state.requested, "marble"));
    CHECK(!orb_apps_active());
    orb_shell_apps_service(); orb_shell_apps_publish(); advance(800);
    CHECK(orb_apps_active() == orb_apps_find("marble"));
    CHECK(text("Hold PWR to exit"));
    tap("Close");
    orb_shell_apps_service(); orb_shell_apps_publish(); advance(800);
    CHECK(!orb_apps_active());
    CHECK(text("Marble") && find_control(orb_shell_root(s_shell), "Play"));
}

static void authoritative_all_clear(void)
{
    fresh();
    orb_model_snapshot_t model; CHECK(orb_model_snapshot(&model));
    for (size_t i = 0; i < model.card_count; i++)
        if (strcmp(model.cards[i].id, "approval-build") && strcmp(model.cards[i].id, "choice-next"))
            CHECK(orb_model_cancel_card(model.cards[i].id));
    advance(800);
    /* A previous successful result must not make the next local queue look
     * delivered. Its ID is intentionally different from the final card. */
    orb_answer_t earlier = {0}; strcpy(earlier.card, "choice-next"); strcpy(earlier.option, "agents");
    CHECK(orb_model_answer(&earlier));
    orb_answer_t claimed; CHECK(orb_model_peek_answer(&claimed));
    CHECK(orb_model_cancel_card(earlier.card));
    CHECK(orb_model_complete_answer(earlier.card, &earlier, true, NULL)); advance(16);
    CHECK(!text("All caught up"));
    orb_answer_t final = {0}; strcpy(final.card, "approval-build"); strcpy(final.option, "allow");
    CHECK(orb_model_answer(&final)); advance(800);
    CHECK(!text("All caught up"));
    CHECK(orb_model_peek_answer(&claimed));
    CHECK(orb_model_cancel_card(final.card));
    CHECK(orb_model_complete_answer(final.card, &final, true, NULL)); advance(16);
    CHECK(!text("All caught up")); /* Face retains its date/attention line. */
    advance(2200); CHECK(!text("All caught up"));
    CHECK(orb_model_snapshot(&model));
    strcpy(model.sessions[0].summary, "Unrelated status revision");
    CHECK(orb_model_upsert_session(&model.sessions[0])); advance(800);
    CHECK(!text("All caught up"));
    orb_model_set_connection(false, false); advance(16);
    orb_model_set_connection(true, true); advance(800);
    CHECK(!text("All caught up"));
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init());
    s_pointer = lv_indev_create(); CHECK(s_pointer);
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    face_cadence_and_mute();
    face_layout_and_count();
    voice_level_geometry();
    primary_contrast();
    launcher_glyph_contrast();
    enlarged_heading_clearance();
    rim_list_position_and_stability();
    authoritative_all_clear();
    play_registry();
    orb_shell_destroy(s_shell); s_shell = NULL;
    advance(16); lv_indev_delete(s_pointer);
    printf("delight UI: %u checks passed\n", s_checks);
    return 0;
}
