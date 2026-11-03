/* Public-contract regressions for the device shell. Synthetic fixtures only.
 * Drive actual LVGL pointer input: opening a menu must not invoke an action. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_home.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "orb_kit.h"
#include "src/misc/lv_text_private.h"
#include "orb_input.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "sim_display.h"

static uint32_t s_now;
static unsigned s_checks;
static const char *s_trace_case;
static bool s_trace;
static const char *s_case = "initialization";
static unsigned s_invalidations;
static unsigned long long s_invalidated_pixels;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static uint8_t s_pixels[SIM_D * SIM_D * 3];
static struct { unsigned count; orb_shell_action_t last; } s_actions;

#define CHECK(condition) do { \
    s_checks++; \
    if (s_trace && s_trace_case != s_case) { s_trace_case = s_case; fprintf(stderr, "shell test stage: %s\n", s_case); } \
    if (!(condition)) { \
        fprintf(stderr, "%s %s:%d: %s\n", s_case, __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static uint32_t tick(void) { return s_now; }
static void settle_page(void);

static void boot_back(void)
{
    orb_button_event_t event = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_SHORT, .synthetic = true};
    orb_shell_button(s_shell, &event);
    settle_page();
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    *data = s_input;
}

static void action(orb_shell_action_t value, void *ctx)
{
    CHECK(ctx == &s_actions);
    s_actions.last = value;
    s_actions.count++;
}

static void invalidated(lv_event_t *e)
{
    const lv_area_t *area = lv_event_get_param(e);
    s_invalidations++;
    s_invalidated_pixels += (unsigned long long)lv_area_get_width(area) * lv_area_get_height(area);
}

static void settle(void)
{
    s_now += 20;
    if (s_shell) orb_shell_poll(s_shell);
    lv_timer_handler();
    lv_refr_now(s_display);
}

/* LVGL's one-line DOTS presentation may temporarily expose the shortened
 * display string. Resolve that visible prefix to the requested fixture text. */
static bool display_label_matches(const char *text, const char *value)
{
    if (!strcmp(text, value)) return true;
    const char *end = strstr(text, "...");
    if (!end) end = strstr(text, "…");
    size_t prefix = end ? (size_t)(end - text) : 0;
    return prefix >= 3 && strlen(value) > prefix && !strncmp(text, value, prefix);
}

static lv_obj_t *find_label(lv_obj_t *root, const char *value)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (orb_kit_rim_text_get(root) &&
        !strcasecmp(orb_kit_rim_text_get(root), value)) return root;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *text = lv_label_get_text(root);
        size_t n = strlen(text), v = strlen(value);
        if (display_label_matches(text, value) || (n > v && text[n - v - 1] == ' ' && !strcmp(text + n - v, value))) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}

/* Offscreen content remains part of the scroll view. Tests may locate it to
 * scroll into view, but pointer dispatch still uses the visible hit target. */
static lv_obj_t *find_label_in_tree(lv_obj_t *root, const char *value)
{
    if (orb_kit_rim_text_get(root) &&
        !strcasecmp(orb_kit_rim_text_get(root), value)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && display_label_matches(lv_label_get_text(root), value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_label_in_tree(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}

static bool row_has_value(lv_obj_t *root, const char *title, const char *value)
{
    if (lv_obj_get_height(root) >= ORB_KIT_ROW_H && lv_obj_get_width(root) == 418 &&
        find_label_in_tree(root, title) && find_label_in_tree(root, value)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        if (row_has_value(lv_obj_get_child(root, i), title, value)) return true;
    return false;
}

static lv_obj_t *find_text_in_tree(lv_obj_t *root, const char *value)
{
    const char *rim = orb_kit_rim_text_get(root);
    if (rim) for (const char *at = rim; *at; ++at)
        if (!strncasecmp(at, value, strlen(value))) return root;
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), value)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_text_in_tree(lv_obj_get_child(root, i), value);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *label(const char *value)
{
    lv_obj_t *found = find_label(orb_shell_root(s_shell), value);
    return found ? found : find_label(lv_display_get_layer_top(s_display), value);
}

/* Captions can match a page title as well as an action (e.g. Choose).
 * A header is not a button; prefer a matching label with its own hit target. */
static lv_obj_t *find_control(lv_obj_t *root, const char *caption)
{
    const char *alias = !strcmp(caption, "Reply") ? orb_shell_surface(s_shell) == ORB_SHELL_SESSION ? ORB_ICON_MIC : "Reply by voice" : caption;
    if (!lv_obj_is_visible(root)) return NULL;
    /* Design 2.0 app discs carry glyphs; the focused name is curved text.
     * Find the actual icon control rather than treating its caption as a tap. */
    static const struct { const char *name, *glyph; } apps[] = {
        {"Agents", ORB_ICON_PROMPT}, {"Dot", ORB_ICON_CHAT}, {"Home", ORB_ICON_HOUSE},
        {"Time", ORB_ICON_TIMER}, {"Play", ORB_ICON_GAME_CONTROLLER},
        {"Updates", ORB_ICON_UPLOAD}, {"Settings", ORB_ICON_GEAR},
    };
    if (orb_shell_surface(s_shell) == ORB_SHELL_APPS &&
        lv_obj_get_width(root) == ORB_KIT_DISC && lv_obj_get_height(root) == ORB_KIT_DISC &&
        lv_obj_is_clickable(root)) {
        for (unsigned i = 0; i < sizeof(apps) / sizeof(*apps); ++i)
            if (!strcmp(caption, apps[i].name) && find_label(root, apps[i].glyph)) return root;
    }
    /* Design 2.0 gives Face the shared 56px mini utility disc. Its action
     * context belongs to orb_faces; identify its semantic Apps glyph. */
    if (!strcmp(caption, "Apps") && orb_shell_surface(s_shell) == ORB_SHELL_FACE &&
        lv_obj_get_width(root) == ORB_KIT_DISC_MINI && lv_obj_get_height(root) == ORB_KIT_DISC_MINI &&
        lv_obj_is_clickable(root) && find_label(root, ORB_ICON_APPS)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && (find_label(root, caption) == root || find_label(root, alias) == root ||
        (!strcmp(caption, "Connection") && find_label(root, "Mac") == root))) {
        if (lv_obj_is_clickable(root)) return root;
        for (lv_obj_t *obj = lv_obj_get_parent(root); obj; obj = lv_obj_get_parent(obj))
            if (lv_obj_check_type(obj, &lv_button_class) ||
                (lv_obj_is_clickable(obj) && !lv_obj_is_scrollable(obj) &&
                 (orb_glass_get_amount(obj) > 0 ||
                  ((intptr_t)lv_obj_get_user_data(obj) > 0 && (intptr_t)lv_obj_get_user_data(obj) < 2048)))) return obj;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_control(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}

static void dump_labels(lv_obj_t *root)
{
    if (lv_obj_is_visible(root) && lv_obj_check_type(root, &lv_label_class))
        fprintf(stderr, "label: %s\n", lv_label_get_text(root));
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i)
        dump_labels(lv_obj_get_child(root, i));
}
static void tap(const char *caption)
{
    lv_obj_t *button = find_control(orb_shell_root(s_shell), caption);
    if (!button) {
        orb_voice_state_t state; CHECK(orb_model_get_voice(&state));
        char target[49], card[65]; CHECK(orb_model_get_voice_focus(target, card));
        fprintf(stderr, "missing shell control: %s (surface=%d voice=%d target=%s card=%s)\n",
            caption, orb_shell_surface(s_shell), state.phase, state.target, state.card);
        fprintf(stderr, "test case: %s\n", s_case);
        fprintf(stderr, "voice focus: %s / %s\n", target, card);
        dump_labels(orb_shell_root(s_shell));
    }
    CHECK(button != NULL);
    lv_area_t box;
    lv_obj_get_coords(button, &box);
    lv_obj_t *row = button;
    orb_round_list_cache_stats_t cache;
    while (lv_obj_get_parent(row) && !orb_round_list_cache_stats(lv_obj_get_parent(row), &cache))
        row = lv_obj_get_parent(row);
    bool round_row = lv_obj_get_parent(row) != NULL;
    if (round_row) CHECK(orb_round_list_child_bounds(row, button, &box));
    s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    if (round_row) {
        if (!orb_round_list_hit_test(row, &s_input.point))
            fprintf(stderr, "unrevealed shell row: %s surface=%u point=%d,%d bounds=%d,%d..%d,%d\n",
                caption, (unsigned)orb_shell_surface(s_shell), s_input.point.x, s_input.point.y,
                box.x1, box.y1, box.x2, box.y2);
        CHECK(orb_round_list_hit_test(row, &s_input.point));
    }
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    settle();
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    for (unsigned i = 0; i < 40; i++) settle();
}

/* Design 2.0 starts touch capture only after LVGL's deliberate long press.
 * The label is a hint; the real tappable is the canonical microphone disc. */
static lv_obj_t *hold_to_speak(bool release)
{
    lv_obj_t *mic = find_control(orb_shell_root(s_shell), ORB_ICON_MIC); CHECK(mic);
    lv_area_t box; lv_obj_get_coords(mic, &box);
    s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer);
    for (unsigned i = 0; i < 33; ++i) settle();
    lv_indev_read(s_pointer);
    if (release) {
        s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer);
        for (unsigned i = 0; i < 40; ++i) settle();
    }
    return mic;
}

static unsigned white_pixels(int x1, int y1, int x2, int y2)
{
    sim_display_rgb(s_pixels);
    unsigned count = 0;
    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            const uint8_t *p = &s_pixels[(y * SIM_D + x) * 3];
            count += p[0] > 210 && p[1] > 210 && p[2] > 210;
        }
    }
    return count;
}

/* A valid LVGL tree can still leave stale/blank pixels after a page rebuild.
 * Check the final panel buffer, not just label existence in the object tree. */
static void header_painted(void)
{
    orb_shell_surface_t surface = orb_shell_surface(s_shell);
    if (surface == ORB_SHELL_APPS) {
        CHECK(white_pixels(100, 400, 366, 455) > 20); /* Last-touched caption. */
        return;
    }
    const char *title = surface == ORB_SHELL_APPS ? "Apps" :
                        surface == ORB_SHELL_NOTIFICATIONS ? "Notifications" :
                        surface == ORB_SHELL_SETTINGS ? "Settings" :
                        surface == ORB_SHELL_DISPLAY ? "Display" : NULL;
    if (title) {
        lv_obj_t *heading = label(title); CHECK(heading);
        lv_area_t area; lv_obj_get_coords(heading, &area);
        sim_display_rgb(s_pixels);
        unsigned ink = 0;
        for (int y = LV_MAX(0, area.y1); y <= LV_MIN(465, area.y2); ++y)
            for (int x = LV_MAX(0, area.x1); x <= LV_MIN(465, area.x2); ++x) {
                const uint8_t *p = &s_pixels[(y * SIM_D + x) * 3];
                ink += p[0] > 100 && p[1] > 100 && p[2] > 100;
            }
        CHECK(ink > 40); /* Design1.0 headings use semantic secondary ink. */
    } else if (surface != ORB_SHELL_CONTROL_CENTER || orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3) {
        /* Design 1.0 distributes headings and conversation content through the
         * circle; the old fixed toolbar crops miss legitimate new layouts. */
        unsigned ink = white_pixels(60, 40, 406, 420);
        lv_obj_t *loading = label("Loading…");
        /* A real named wait intentionally reaches 50% ink. Keep the original
         * white-pixel assertion, observing its bright phase within one 1.6s
         * cycle instead of requiring full-white ink at every breath sample. */
        if (loading && lv_obj_get_style_text_opa(loading, 0) < 210 && orb_kit_motion_timer_running())
            for (unsigned frame = 0; frame < 80 && ink <= 200; frame++) {
                settle(); ink = white_pixels(60, 40, 406, 420);
            }
        if (ink <= 200) {
            fprintf(stderr, "header surface=%u at=%u white=%u clock=%d settling=%d\n",
                (unsigned)surface, s_now, ink, orb_kit_motion_timer_running(),
                orb_kit_motion_settling(orb_shell_root(s_shell)));
            if (loading) fprintf(stderr, "visible label=%s text_opa=%u object_opa=%u\n",
                lv_label_get_text(loading), lv_obj_get_style_text_opa(loading, 0), lv_obj_get_style_opa(loading, 0));
            (void)sim_display_save_png("shell-header-failure.png");
        }
        CHECK(ink > 200);
    } else CHECK(find_text_in_tree(orb_shell_root(s_shell), ORB_ICON_SUN));
    lv_obj_t *back = find_control(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT);
    if (back) {
        lv_area_t area; lv_obj_get_coords(back, &area);
        CHECK(white_pixels(LV_MAX(0, area.x1), LV_MAX(0, area.y1), LV_MIN(465, area.x2), LV_MIN(465, area.y2)) > 20);
    }
}

static void settle_page(void);
static lv_obj_t *reveal_row(const char *caption);

static void connection(void)
{
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS);
    settle_page(); /* Content is built after the first destination frame. */
    reveal_row("Mac");
    tap("Mac");
    CHECK(row_has_value(orb_shell_root(s_shell), "Wi-Fi", "Home Wi-Fi"));
    CHECK(row_has_value(orb_shell_root(s_shell), "Mac", "On"));
    tap("Maintenance");
}

static orb_shell_snapshot_t connected(void)
{
    orb_shell_snapshot_t m = {
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70,
    };
    snprintf(m.device_name, sizeof(m.device_name), "Orb");
    snprintf(m.device_id, sizeof(m.device_id), "orb-0123456789ab");
    snprintf(m.hub_name, sizeof(m.hub_name), "Mac mini");
    snprintf(m.ssid, sizeof(m.ssid), "Home Wi-Fi");
    snprintf(m.clock_time, sizeof(m.clock_time), "10:09");
    snprintf(m.clock_date, sizeof(m.clock_date), "Tuesday, October 27");
    snprintf(m.firmware_version, sizeof(m.firmware_version), "0.4.0");
    return m;
}

static void update(orb_shell_snapshot_t *m)
{
    orb_shell_update(s_shell, m);
    settle();
}

static void fresh(orb_shell_snapshot_t *m)
{
    if (s_shell) orb_shell_destroy(s_shell);
    orb_model_load_demo();
    orb_model_set_connection(true, true);
    memset(&s_actions, 0, sizeof(s_actions));
    s_shell = orb_shell_create(lv_screen_active(), action, &s_actions);
    CHECK(s_shell != NULL);
    update(m);
}

/* Critical credentials and commands must fit without dots, wrap, or a clipped
 * line box, including after Dynamic Type + Bold Text changes on an open view. */
static void fits_one_line(const char *value)
{
    lv_obj_t *obj = find_label_in_tree(orb_shell_root(s_shell), value);
    if (!obj) fprintf(stderr, "missing critical system text: %s\n", value);
    CHECK(obj != NULL);
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
    settle();
    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    lv_point_t size;
    lv_text_get_size(&size, value, font, lv_obj_get_style_text_letter_space(obj, 0),
                     lv_obj_get_style_text_line_space(obj, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    CHECK(size.x <= lv_obj_get_content_width(obj));
    CHECK(size.y <= lv_obj_get_content_height(obj));
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    CHECK(box.y1 >= 143 && box.y2 < 416);
    CHECK(lv_obj_get_width(obj) <= orb_chord_width(box.y1, box.y2, ORB_RIM_MARGIN_TEXT));
}

static void provisioning_and_pairing(void)
{
    orb_shell_snapshot_t m = connected();
    m.net = ORB_SHELL_NET_PROVISIONING;
    m.link = ORB_SHELL_LINK_OFFLINE;
    snprintf(m.service_name, sizeof(m.service_name), "Orb-89AB");
    snprintf(m.pop, sizeof(m.pop), "12345678");
    fresh(&m);
    CHECK(label("Set up Wi-Fi"));
    fits_one_line("1234 5678");
    int setup_height = lv_obj_get_style_text_font(label("1234 5678"), 0)->line_height;
    fits_one_line("orb-hub setup");
    m.net = ORB_SHELL_NET_FAILED;
    update(&m);
    CHECK(label("Try Wi-Fi again"));
    CHECK(label("Wi-Fi could not connect.\nRun setup again to retry."));
    CHECK(label("In Terminal on your Mac, run:"));
    fits_one_line("1234 5678");
    fits_one_line("orb-hub setup");
    CHECK(s_actions.count == 0);
    m.pop[0] = 'x';
    update(&m);
    CHECK(label("Preparing setup code"));
    CHECK(!label("orb-hub setup"));
    m.net = ORB_SHELL_NET_CONNECTED;
    m.link = ORB_SHELL_LINK_PAIRING;
    snprintf(m.pairing_code, sizeof(m.pairing_code), "002310");
    update(&m);
    CHECK(label("Pair with your Mac"));
    CHECK(label("Type this code in orb\u2011hub.\nUse the same Wi-Fi."));
    fits_one_line("002 310");
    int pairing_height = lv_obj_get_style_text_font(label("002 310"), 0)->line_height;
    CHECK(label("Found Mac mini"));
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    settle();
    fits_one_line("002 310");
    CHECK(lv_obj_get_style_text_font(label("002 310"), 0)->line_height >= pairing_height);
    CHECK(label("Use orb\u2011hub on your Mac.\nUse the same Wi-Fi."));
    m.net = ORB_SHELL_NET_PROVISIONING;
    snprintf(m.pop, sizeof(m.pop), "12345678");
    update(&m);
    fits_one_line("1234 5678");
    CHECK(lv_obj_get_style_text_font(label("1234 5678"), 0)->line_height >= setup_height);
    fits_one_line("orb-hub setup");
    m.net = ORB_SHELL_NET_CONNECTED;
    snprintf(m.pairing_code, sizeof(m.pairing_code), "12345");
    update(&m);
    CHECK(label("Preparing pairing code"));
    CHECK(!label("orb-hub pair 12345"));
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
}

static void honest_connectivity_and_check(void)
{
    orb_shell_snapshot_t m = connected();
    m.link = ORB_SHELL_LINK_OFFLINE;
    m.ota = ORB_SHELL_OTA_OFFERED;
    fresh(&m);
    orb_model_set_connection(false, false);
    settle_page();
    /* FaceOrb retains date/weather and shows unavailable providers as dashes. */
    CHECK(label("10:09") && label("—"));
    CHECK(!label("Connected"));
    CHECK(!label("Update"));
    m.net = ORB_SHELL_NET_CONNECTING;
    m.link = ORB_SHELL_LINK_ONLINE;
    update(&m);
    CHECK(label("10:09") && label("—"));
    CHECK(!label("Connected"));
    m = connected();
    orb_model_set_connection(true, true);
    update(&m);
    CHECK(!label("Connected"));
    CHECK(label("10:09"));
    orb_shell_navigate(s_shell, ORB_SHELL_UPDATES);
    settle_page();
    tap("Check now");
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_CHECK_UPDATE);
    CHECK(label("Checking for updates…"));
    CHECK(label("Check now"));
    CHECK(lv_obj_has_state(lv_obj_get_parent(label("Check now")), LV_STATE_DISABLED));
    s_now += 3500;
    settle(); /* Elapsed time is never a received update-check result. */
    CHECK(label("Checking for updates…"));
    m.update_check_revision = 1;
    snprintf(m.update_check_result, sizeof(m.update_check_result), "Orb is up to date.");
    update(&m);
    CHECK(label("Orb is up to date."));
    CHECK(!label("Checking for updates…"));
    CHECK(label("Check now"));
    boot_back();
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    settle();
    CHECK(!label("Connected"));
    m.clock_time[0] = '\0';
    update(&m);
    CHECK(!label("10:09"));
    /* Design 2.0 uses the valid local clock when status time is absent.
     * The old branded clock placeholder is retired. */
    CHECK(!label("Orb"));
    CHECK(s_actions.count == 1);
}

static void confirmations(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    connection();
    tap("Forget Wi-Fi");
    CHECK(label("Forget Wi-Fi?"));
    CHECK(s_actions.count == 0);
    boot_back();
    CHECK(label("Maintenance"));
    CHECK(s_actions.count == 0);
    tap("Unpair Mac");
    CHECK(label("Unpair this Mac?"));
    CHECK(s_actions.count == 0);
    boot_back();
    CHECK(s_actions.count == 0);
    tap("Forget Wi-Fi");
    CHECK(label("You'll need to set up Wi-Fi again."));
    tap("Cancel");
    CHECK(label("Maintenance"));
    CHECK(s_actions.count == 0);
    tap("Forget Wi-Fi");
    tap("Forget Wi-Fi");
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_FORGET_NETWORK);
    /* A queued action is not a completed service operation. */
    CHECK(label("Settings"));
    connection();
    tap("Unpair Mac");
    tap("Unpair");
    CHECK(s_actions.count == 2 && s_actions.last == ORB_SHELL_ACTION_UNPAIR);
    CHECK(label("Settings"));
}

static void accessible_confirmations(void)
{
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true);
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    connection();
    tap("Forget Wi-Fi");
    lv_obj_t *body = label("You'll need to set up Wi-Fi again.");
    CHECK(body && lv_label_get_long_mode(body) == LV_LABEL_LONG_MODE_WRAP);
    CHECK(label("Cancel"));
    lv_obj_t *confirm = find_control(orb_shell_root(s_shell), "Forget Wi-Fi");
    lv_obj_t *cancel = find_control(orb_shell_root(s_shell), "Cancel");
    lv_area_t a, b;
    lv_obj_get_coords(confirm, &a);
    lv_obj_get_coords(cancel, &b);
    CHECK(lv_area_get_height(&a) == ORB_KIT_SECONDARY_H && lv_area_get_height(&b) == ORB_KIT_SECONDARY_H);
    CHECK(b.y1 - a.y2 - 1 >= 20);
    CHECK(!label(ORB_ICON_CHEVRON_DOWN));
    lv_obj_t *reader = lv_obj_get_parent(body);
    int overflow = lv_obj_get_scroll_bottom(reader);
    CHECK(orb_shell_twist(s_shell, 1));
    CHECK(overflow <= 0 || lv_obj_get_scroll_y(reader) > 0);
    CHECK(s_actions.count == 0);
    tap("Cancel");
    CHECK(label("Maintenance"));
    CHECK(s_actions.count == 0);
    tap("Unpair Mac");
    body = label("Orb will ask for a new pairing code. Wi-Fi stays connected.");
    CHECK(body && lv_label_get_long_mode(body) == LV_LABEL_LONG_MODE_WRAP);
    CHECK(label("Cancel"));
    tap("Cancel");
    CHECK(s_actions.count == 0);
    orb_ui_set_reduce_transparency(false);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
}

static void priority_and_recovery(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    connection();
    tap("Forget Wi-Fi");
    snprintf(m.service_error, sizeof(m.service_error), "Connection unavailable");
    update(&m);
    CHECK(label("Needs attention"));
    CHECK(!label("Forget Wi-Fi?"));
    CHECK(s_actions.count == 0);
    tap("Connection");
    tap("Maintenance");
    CHECK(label("Forget Wi-Fi"));
    boot_back();
    boot_back();
    CHECK(label("Needs attention"));
    tap("Connection");
    m.ota = ORB_SHELL_OTA_DOWNLOADING;
    m.progress = 142;
    m.identify = true;
    update(&m);
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Updating to "));
    CHECK(label("100%"));
    CHECK(!label(ORB_ICON_GEAR));
    CHECK(!label("Forget Wi-Fi"));
    CHECK(!label("This is your Orb"));
    m.ota = ORB_SHELL_OTA_VERIFYING;
    update(&m);
    /* One stage line carrying the version (polish review #22). */
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Verifying "));
    m.ota = ORB_SHELL_OTA_READY;
    update(&m);
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Installing "));
    CHECK(!find_text_in_tree(orb_shell_root(s_shell), "Keep using"));
    m.ota = ORB_SHELL_OTA_IDLE;
    update(&m);
    CHECK(label("This is your Orb"));
    CHECK(!label(ORB_ICON_GEAR));
    m.identify = false;
    update(&m);
    CHECK(label("Needs attention"));
    m.service_error[0] = '\0';
    m.ota = ORB_SHELL_OTA_FAILED;
    snprintf(m.error, sizeof(m.error), "Connection interrupted");
    update(&m);
    CHECK(label("Update interrupted"));
    CHECK(label("Connection interrupted"));
    tap("Try again");
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_UPDATE);
    m.ota = ORB_SHELL_OTA_OFFERED;
    update(&m);
    tap("Update");
    CHECK(s_actions.count == 2 && s_actions.last == ORB_SHELL_ACTION_UPDATE);
    m.ota = ORB_SHELL_OTA_DOWNLOADING;
    m.progress = 9;
    update(&m);
    CHECK(label("9%"));
    m.progress = 100;
    update(&m);
    for (unsigned frame = 0; frame < 14; frame++) settle();
    CHECK(label("100%"));
    CHECK(!label("9%"));
    m.progress = -10;
    update(&m);
    /* Real progress now Follows; this asserts the native endpoint after its
     * bounded presentation rather than during its first 20 ms sample. */
    for (unsigned frame = 0; frame < 14; frame++) settle();
    CHECK(label("0%"));
    m.ota = ORB_SHELL_OTA_VERIFYING;
    snprintf(m.version, sizeof(m.version), "0.9.1");
    update(&m);
    CHECK(label("Verifying 0.9.1"));
    snprintf(m.version, sizeof(m.version), "0.9.2");
    update(&m);
    CHECK(label("Verifying 0.9.2"));
    CHECK(!label("Verifying 0.9.1"));
}

static lv_obj_t *find_arc(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_arc_class) &&
        lv_obj_get_style_arc_width(root, LV_PART_MAIN) == 10) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *arc = find_arc(lv_obj_get_child(root, i));
        if (arc) return arc;
    }
    return NULL;
}

static void numeric_hierarchy_and_recovery(void)
{
    orb_shell_snapshot_t m = connected();
    m.ota = ORB_SHELL_OTA_DOWNLOADING;
    m.progress = 42;
    fresh(&m);
    CHECK(label("42%"));
    int percentage_height = lv_obj_get_style_text_font(label("42%"), 0)->line_height;
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    CHECK(label("42%"));
    CHECK(lv_obj_get_style_text_font(label("42%"), 0)->line_height >= percentage_height);
    m.ota = ORB_SHELL_OTA_READY;
    update(&m);
    for (unsigned frame = 0; frame < 14; frame++) settle();
    lv_obj_t *ring = find_arc(orb_shell_root(s_shell));
    CHECK(ring != NULL);
    lv_area_t bounds;
    lv_obj_get_coords(ring, &bounds);
    CHECK(bounds.x1 == 10 && bounds.y1 == 10);
    CHECK(lv_area_get_width(&bounds) == 446 && lv_area_get_height(&bounds) == 446);
    CHECK(lv_obj_get_style_arc_width(ring, LV_PART_MAIN) == 10); /* r218 centreline. */
    CHECK(lv_arc_get_value(ring) == lv_arc_get_max_value(ring));
    m.ota = ORB_SHELL_OTA_FAILED;
    snprintf(m.error, sizeof(m.error), "Connection interrupted");
    update(&m);
    CHECK(label("Current software is safe."));
    m.ota = ORB_SHELL_OTA_IDLE;
    snprintf(m.service_error, sizeof(m.service_error), "Connection unavailable");
    update(&m);
    CHECK(label("Retry in Connection."));
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false);
}

/* These tests isolate unchanged service/light input. Purposeful working-ring
 * shimmer has separate cadence coverage in test_delight. */
static void quiet_sessions(void)
{
    orb_model_snapshot_t model;
    CHECK(orb_model_snapshot(&model));
    for (size_t i = 0; i < model.session_count; i++) {
        if (model.sessions[i].state != ORB_SESSION_BUSY) continue;
        model.sessions[i].state = ORB_SESSION_IDLE;
        CHECK(orb_model_upsert_session(&model.sessions[i]));
    }
    for (unsigned i = 0; i < 50; i++) settle();
}

static void status_noise(void)
{
    orb_shell_snapshot_t m = connected();
    m.rssi = -60;
    fresh(&m);
    quiet_sessions();
    for (unsigned i = 0; i < 50; i++) settle();
    sim_display_take_changed();
    s_invalidations = 0;
    s_invalidated_pixels = 0;
    unsigned frames = 0;
    for (unsigned i = 0; i < 100; i++) {
        m.rssi = -58 - (int)(i % 8); /* same two bars */
        m.progress = (int)i; /* irrelevant while OTA is idle */
        update(&m);
        if (sim_display_take_changed()) frames++;
    }
    printf("shell_status_noise: %u frames, %u invalidations, %llu requested pixels\n",
           frames, s_invalidations, s_invalidated_pixels);
    CHECK(frames == 0);
    CHECK(s_invalidations == 0);
    m.rssi = -81;
    update(&m);
    CHECK(!sim_display_take_changed()); /* Connected Face has no Wi-Fi chrome. */
    CHECK(!label("Connected"));
    m.battery_pct = 70;
    update(&m);
    CHECK(sim_display_take_changed());
    CHECK(label("10:09"));
    m.battery_pct = -1;
    update(&m);
    CHECK(!label("70%"));
    CHECK(!label("USB"));
    m.usb = false;
    update(&m);
    CHECK(!label("Unknown"));
    CHECK(!label("USB"));
}

static void flat_motion_noise(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    quiet_sessions();
    orb_ui_set_gravity(0, 0, 1, 0.02f);
    settle();
    sim_display_take_changed();
    s_invalidations = 0;
    unsigned frames = 0;
    for (unsigned i = 0; i < 100; i++) {
        float x = (i % 4 < 2) ? 0.004f : -0.004f;
        float y = (i % 4 == 0 || i % 4 == 3) ? 0.003f : -0.003f;
        orb_ui_set_gravity(x, y, 1, 0.02f);
        settle();
        if (sim_display_take_changed()) frames++;
    }
    printf("shell_flat_motion_noise: %u frames, %u invalidations\n", frames, s_invalidations);
    CHECK(frames == 0);
    CHECK(s_invalidations == 0);
    orb_ui_set_gravity(0.35f, 0.9f, 0, 0.2f);
    settle();
    CHECK(sim_display_take_changed()); /* A real tilt still relights the glass. */
    orb_ui_set_gravity(-0.35f, 0.9f, 0, 0.2f);
    settle();
    CHECK(sim_display_take_changed());
}

static void drag(int x1, int y1, int x2, int y2)
{
    s_input.point = (lv_point_t){x1, y1};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer);
    settle();
    for (unsigned i = 1; i <= 10; i++) {
        s_input.point = (lv_point_t){x1 + (x2 - x1) * (int)i / 10,
                                   y1 + (y2 - y1) * (int)i / 10};
        lv_indev_read(s_pointer);
        settle();
    }
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    for (unsigned i = 0; i < 40; i++) settle();
}

static void shell_navigation_and_answers(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(orb_model_pending_answers() == 0);
    /* Let the new kit mini complete its initial materialisation before tap. */
    settle_page();
    tap("Apps");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    tap("Agents");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
    orb_shell_select_session(s_shell, "claude-firmware"); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);
    orb_model_snapshot_t fixture;
    CHECK(orb_model_snapshot(&fixture));
    CHECK(fixture.session_count > 0);
    CHECK(label("Waiting for activity"));
    /* Approval is a separate secondary route; SessionLive stays a stream. */
    orb_shell_select_card(s_shell, "approval-build"); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
    CHECK(label("Deny"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), "Bash: idf.py build"));
    CHECK(label("Firmware build"));
    CHECK(!label("More")); /* Both decisions share the complete short context. */
    CHECK(label("Allow"));
    CHECK(label("Deny"));
    CHECK(orb_model_pending_answers() == 0); /* Looking never approves. */
    tap("Deny");
    CHECK(label("Sending…"));
    CHECK(label("Deny"));
    CHECK(!label("Delivered"));
    CHECK(orb_model_pending_answers() == 1);
    orb_answer_t answer;
    CHECK(orb_model_take_answer(&answer));
    CHECK(!strcmp(answer.card, "approval-build"));
    CHECK(!strcmp(answer.option, "deny"));
    CHECK(!answer.text[0]);
    CHECK(!orb_model_take_answer(&answer));
    CHECK(s_actions.count == 0); /* Card answers go to the model, not services. */

    fresh(&m);
    orb_shell_select_card(s_shell, "approval-build");
    for (unsigned frame = 0; frame < 40; frame++) settle();
    CHECK(label("Allow"));
    CHECK(orb_model_cancel_card("approval-build"));
    orb_shell_poll(s_shell);
    settle();
    CHECK(!label("Allow"));
    CHECK(!label("Deny"));
    CHECK(orb_model_pending_answers() == 0);

    /* OTA keeps running while intentional navigation resumes local apps. */
    m.ota = ORB_SHELL_OTA_DOWNLOADING;
    m.progress = 42;
    update(&m);
    orb_shell_navigate(s_shell, ORB_SHELL_APPS);
    settle();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    CHECK(!find_text_in_tree(orb_shell_root(s_shell), "Updating to ") || !label("42%"));
    m.ota = ORB_SHELL_OTA_IDLE;
    update(&m);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    settle();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
}

static void gestures_and_accessibility(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    drag(233, 350, 233, 150);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    drag(12, 233, 180, 233);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    drag(233, 150, 233, 350);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_NOTIFICATIONS);
    drag(12, 233, 180, 233);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    drag(451, 233, 260, 233);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CONTROL_CENTER);
    drag(12, 233, 180, 233);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);

    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_motion(true);
    orb_ui_set_reduce_transparency(true);
    for (unsigned i = 0; i < 40; i++) settle();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_AGENTS);
    CHECK(label("Firmware build"));
    orb_shell_select_card(s_shell, "approval-build");
    for (unsigned i = 0; i < 40; i++) settle();
    CHECK(!label("More"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), "Bash: idf.py build"));
    CHECK(label("Firmware build"));
    CHECK(label("Allow"));
    CHECK(label("Deny"));
    CHECK(orb_model_pending_answers() == 0);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    orb_ui_set_reduce_motion(false);
    orb_ui_set_reduce_transparency(false);
}

static void four_option_approval_and_notification_history(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_model_snapshot_t model;
    CHECK(orb_model_snapshot(&model));
    orb_card_t approval = {0};
    for (size_t i = 0; i < model.card_count; i++) {
        if (!strcmp(model.cards[i].id, "approval-build")) approval = model.cards[i];
    }
    CHECK(approval.id[0]);
    approval.options_count = 4;
    approval.options[0] = (orb_option_t){.id = "allow", .label = "Allow", .role = ORB_OPTION_DEFAULT};
    approval.options[1] = (orb_option_t){.id = "allow-turn", .label = "Allow for turn", .role = ORB_OPTION_PRIMARY};
    approval.options[2] = (orb_option_t){.id = "allow-once", .label = "Allow once", .role = ORB_OPTION_DEFAULT};
    approval.options[3] = (orb_option_t){.id = "deny", .label = "Deny", .role = ORB_OPTION_CANCEL};
    CHECK(orb_model_upsert_card(&approval));
    orb_shell_poll(s_shell);
    orb_shell_select_card(s_shell, approval.id);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    CHECK(label("Allow"));
    CHECK(label("Deny"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), "Allow for turn")); /* Scope stays reachable in reading flow. */
    lv_obj_t *scoped = find_label_in_tree(orb_shell_root(s_shell), "Allow once");
    CHECK(scoped != NULL);
    /* Extra scopes are kit rows below the command in its outer viewport. */
    reveal_row("Allow once");
    for (unsigned frame = 0; frame < 40; frame++) settle();
    CHECK(label("Deny"));
    header_painted();
    tap("Allow once");
    orb_answer_t answer;
    CHECK(orb_model_take_answer(&answer));
    CHECK(!strcmp(answer.card, approval.id));
    CHECK(!strcmp(answer.option, "allow-once"));

    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    reveal_row("Clear all"); tap("Clear all");
    CHECK(label("No notifications"));
    CHECK(orb_model_snapshot(&model));
    CHECK(model.session_count > 0);
    snprintf(model.sessions[0].summary, sizeof(model.sessions[0].summary), "A new step, without a new card.");
    CHECK(orb_model_upsert_session(&model.sessions[0]));
    orb_shell_poll(s_shell);
    settle();
    CHECK(label("No notifications"));
    CHECK(!label("Clear all"));
}

/* Long consequential arguments must remain complete and readable in scrollable
 * content while the fundamental decisions retain their stable hit targets. */
static void long_approval_and_round_list(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    /* Faces 2.0 reserves the face for complications. Approvals remain in
     * Notifications rather than replacing the Calendar date with a capsule. */
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); settle_page();
    orb_model_snapshot_t waiting_model; CHECK(orb_model_snapshot(&waiting_model));
    CHECK(waiting_model.card_count > 0);
    orb_shell_select_card(s_shell, waiting_model.cards[0].id); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
    CHECK(label("Allow"));
    CHECK(label("Deny"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), "Bash: idf.py build"));
    CHECK(orb_model_pending_answers() == 0);

    orb_model_snapshot_t model;
    CHECK(orb_model_snapshot(&model));
    orb_card_t request = model.cards[0];
    const char *command = "Bash: idf.py -B build-simulator -D SDKCONFIG=sim/sdkconfig.defaults build && python3 tools/check_assets.py --strict --report reports/round-shell-assets.json";
    snprintf(request.detail, sizeof(request.detail), "%s", command);
    CHECK(orb_model_upsert_card(&request));
    orb_shell_poll(s_shell);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    lv_obj_t *detail = find_label_in_tree(orb_shell_root(s_shell), command);
    CHECK(detail != NULL); /* Dots mode would irreversibly hide command arguments. */
    CHECK(lv_label_get_long_mode(detail) == LV_LABEL_LONG_MODE_WRAP);
    CHECK(lv_obj_get_style_text_font(detail, 0)->line_height >= 20);
    CHECK(find_control(orb_shell_root(s_shell), "Allow"));
    CHECK(find_control(orb_shell_root(s_shell), "Deny"));
    drag(233, 260, 233, 170);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
    CHECK(label("Deny"));
    CHECK(orb_model_pending_answers() == 0);
    CHECK(orb_model_cancel_card(request.id));
    orb_shell_poll(s_shell);
    settle();
    CHECK(!label("Allow"));
    CHECK(!label("Deny"));

    fresh(&m);
    orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    CHECK(!label(ORB_ICON_CHEVRON_DOWN));
    lv_obj_t *first = label("Firmware build");
    CHECK(first != NULL);
    lv_area_t before, after, row_bounds;
    lv_obj_get_coords(first, &before);
    CHECK(orb_round_list_row_bounds(lv_obj_get_parent(first), &row_bounds));
    CHECK(!find_control(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT));
    /* HIG §§3/7: the current capsule uses its round chord, rather than the
     * previous fixed340px rectangle. Both cap circles stay inside the safe
     * circle; allow two pixels for integer presentation centres/radii. */
    int radius = (lv_area_get_height(&row_bounds) + 1) / 2;
    int cy = (row_bounds.y1 + row_bounds.y2 + 1) / 2 - 233;
    int left_cap = row_bounds.x1 + radius - 233;
    int right_cap = row_bounds.x2 - radius + 1 - 233;
    int cap_limit = 233 - 24 - radius + 2;
    CHECK(left_cap * left_cap + cy * cy <= cap_limit * cap_limit);
    CHECK(right_cap * right_cap + cy * cy <= cap_limit * cap_limit);
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(first));
    int scroll_before = lv_obj_get_scroll_y(list);
    drag(233, 340, 233, 180);
    CHECK(lv_obj_get_scroll_y(list) > scroll_before + 50);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
    CHECK(lv_obj_is_valid(first)); /* Keep the actual row identity across scroll. */
    lv_obj_get_coords(first, &after);
    CHECK(after.y1 < before.y1); /* Continuous scrolling, not a rebuilt page. */
    /* The immutable native title box stays at its real content position.
     * The deepest visible row proves the fling reached later sessions; an
     * enlarged offscreen preview label is no longer a visibility proxy. */
    CHECK(label("Font assets"));
    CHECK(orb_model_pending_answers() == 0);
}

static unsigned object_count(lv_obj_t *root)
{
    unsigned count = 1;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        count += object_count(lv_obj_get_child(root, i));
    }
    return count;
}

static void lazy_screen_lifetimes(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    for (unsigned i = 0; i < 40; i++) settle();
    unsigned baseline = object_count(orb_shell_root(s_shell));
    const orb_shell_surface_t surfaces[] = {
        ORB_SHELL_STACK, ORB_SHELL_APPS, ORB_SHELL_AGENTS, ORB_SHELL_SESSION,
        ORB_SHELL_CARD, ORB_SHELL_NOTIFICATIONS, ORB_SHELL_CONTROL_CENTER,
        ORB_SHELL_SETTINGS, ORB_SHELL_DISPLAY, ORB_SHELL_ACCESSIBILITY,
        ORB_SHELL_ABOUT, ORB_SHELL_DOT, ORB_SHELL_VOICE, ORB_SHELL_PLAY,
    };
    for (unsigned lap = 0; lap < 3; lap++) {
        for (unsigned i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); i++) {
            orb_shell_navigate(s_shell, surfaces[i]);
            for (unsigned frame = 0; frame < 40; frame++) settle();
            header_painted();
        }
        orb_shell_navigate(s_shell, ORB_SHELL_FACE);
        for (unsigned frame = 0; frame < 40; frame++) settle();
        /* Leaving an app must release its LVGL tree, not merely hide it. */
        CHECK(object_count(orb_shell_root(s_shell)) == baseline);
    }
}

static void ax3_navigation_pixels(void)
{
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    const orb_shell_surface_t surfaces[] = {
        ORB_SHELL_APPS, ORB_SHELL_AGENTS, ORB_SHELL_NOTIFICATIONS,
        ORB_SHELL_CONTROL_CENTER, ORB_SHELL_SETTINGS, ORB_SHELL_DISPLAY,
        ORB_SHELL_ACCESSIBILITY, ORB_SHELL_ABOUT, ORB_SHELL_DOT,
        ORB_SHELL_VOICE, ORB_SHELL_PLAY,
    };
    for (unsigned i = 0; i < sizeof(surfaces) / sizeof(surfaces[0]); i++) {
        orb_shell_navigate(s_shell, surfaces[i]);
        for (unsigned frame = 0; frame < 40; frame++) settle();
        header_painted();
    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
}

/* Type must fit its actual line box, including the long navigation titles
 * whose semantic style changes to preserve the whole label at Largest. */
static void measured_text_fits(const char *caption)
{
    lv_obj_t *obj = label(caption);
    CHECK(obj != NULL);
    orb_kit_rim_text_info_t rim;
    if (orb_kit_rim_text_info(obj, &rim)) {
        CHECK(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES);
        CHECK(!rim.ellipsised);
        return;
    }
    const char *value = lv_label_get_text(obj);
    lv_point_t measured;
    const lv_font_t *font = lv_obj_get_style_text_font(obj, 0);
    lv_text_get_size(&measured, value, font, lv_obj_get_style_text_letter_space(obj, 0),
                     lv_obj_get_style_text_line_space(obj, 0), LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    CHECK(measured.x <= lv_obj_get_content_width(obj));
    CHECK(measured.y <= lv_obj_get_content_height(obj));
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    CHECK(lv_obj_get_width(obj) <= orb_chord_width(box.y1, box.y2, ORB_RIM_MARGIN_TEXT));
}

static void round2_final_geometry(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_shell_select_card(s_shell, "choice-next");
    for (unsigned frame = 0; frame < 40; frame++) settle();
    const char *options[] = {"Agents", "Smart Stack", "Settings"};
    int previous_bottom = -12;
    for (unsigned i = 0; i < sizeof(options) / sizeof(options[0]); i++) {
        lv_obj_t *control = reveal_row(options[i]);
        CHECK(find_control(orb_shell_root(s_shell), options[i]) == control);
        CHECK(lv_obj_get_height(control) >= ORB_KIT_ROW_H);
        CHECK(lv_obj_get_y(control) - previous_bottom == 12);
        previous_bottom = lv_obj_get_y(control) + lv_obj_get_height(control);
        lv_obj_t *disc = lv_obj_get_child(control, 0);
        CHECK(lv_obj_get_width(disc) == ORB_KIT_DISC_ROW && lv_obj_get_height(disc) == ORB_KIT_DISC_ROW);
        lv_area_t bounds; CHECK(orb_round_list_row_bounds(control, &bounds));
        lv_point_t point = {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
        CHECK(orb_round_list_hit_test(control, &point));
    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(true);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    lv_obj_t *dismiss = find_control(orb_shell_root(s_shell), "Dismiss");
    CHECK(dismiss != NULL);
    lv_area_t before, after;
    lv_obj_get_coords(dismiss, &before);
    CHECK(lv_area_get_height(&before) == ORB_KIT_SECONDARY_H);
    drag(233, 290, 233, 205);
    dismiss = find_control(orb_shell_root(s_shell), "Dismiss");
    CHECK(dismiss != NULL);
    lv_obj_get_coords(dismiss, &after);
    CHECK(!memcmp(&before, &after, sizeof(before))); /* Cancel never scrolls away. */
    tap("Dismiss");
    orb_answer_t answer;
    CHECK(orb_model_take_answer(&answer));
    CHECK(!strcmp(answer.card, "choice-next") && !strcmp(answer.option, "later"));

    const struct {orb_shell_surface_t surface; const char *title;} headings[] = {
        {ORB_SHELL_ACCESSIBILITY, "Accessibility"},
        {ORB_SHELL_NOTIFICATIONS, "Notifications"},
    };
    for (unsigned i = 0; i < sizeof(headings) / sizeof(headings[0]); i++) {
        orb_shell_navigate(s_shell, headings[i].surface);
        for (unsigned frame = 0; frame < 40; frame++) settle();
        if (!label(headings[i].title) && headings[i].surface == ORB_SHELL_ACCESSIBILITY)
            boot_back(); /* Leave the correctly restored settings detail first. */
        measured_text_fits(headings[i].title);
    }

    orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    lv_obj_t *first = label("Firmware build");
    CHECK(first != NULL);
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(first));
    lv_area_t first_bounds, list_bounds;
    CHECK(orb_round_list_row_bounds(lv_obj_get_parent(first), &first_bounds));
    CHECK(!find_control(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT));
    int cap_radius = (lv_area_get_height(&first_bounds) + 1) / 2;
    int cap_y = (first_bounds.y1 + first_bounds.y2 + 1) / 2 - 233;
    int cap_left = first_bounds.x1 + cap_radius - 233;
    int cap_right = first_bounds.x2 - cap_radius + 1 - 233;
    int safe_cap = 233 - 24 - cap_radius + 2;
    CHECK(cap_left * cap_left + cap_y * cap_y <= safe_cap * safe_cap);
    CHECK(cap_right * cap_right + cap_y * cap_y <= safe_cap * safe_cap);
    lv_obj_get_coords(list, &list_bounds);
    for (unsigned i = 0; i < lv_obj_get_child_count(list); i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        if (lv_obj_get_style_opa(row, 0) != LV_OPA_TRANSP &&
            lv_obj_raster_presentation_visible(row)) continue;
        lv_area_t box;
        lv_obj_get_coords(row, &box);
        lv_point_t point = {(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
        if (point.y < list_bounds.y1 || point.y > list_bounds.y2) continue;
        s_input.point = point;
        s_input.state = LV_INDEV_STATE_PRESSED;
        lv_indev_read(s_pointer);
        settle();
        s_input.state = LV_INDEV_STATE_RELEASED;
        lv_indev_read(s_pointer);
        for (unsigned frame = 0; frame < 40; frame++) settle();
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
        CHECK(orb_model_pending_answers() == 0);
    }
    /* Design 1.0 draws two cached row sizes through the viewport. Unlike the
     * former perspective list, it need not produce an invisible tiny preview. */
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false);
    orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    /* The kit anchors the icon independently from its reserved live rail. */
    measured_text_fits(ORB_ICON_SUN);
    measured_text_fits("70%");
}

static lv_obj_t *reveal_row(const char *caption);

static void contrast_and_demo_truthfulness(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_shell_navigate(s_shell, ORB_SHELL_ACCESSIBILITY);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    lv_obj_t *row = reveal_row("Increase contrast");
    CHECK(orb_kit_row_switch(row));
    CHECK(!orb_kit_switch_get_checked(orb_kit_row_switch(row)));
    tap("Increase contrast");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_ACCESSIBILITY);
    row = reveal_row("Increase contrast");
    CHECK(orb_kit_switch_get_checked(orb_kit_row_switch(row)));
    reveal_row("Text size");
    const char *size_name = orb_fonts_content_size() == ORB_CONTENT_SIZE_DEFAULT ? "Default" :
        orb_fonts_content_size() == ORB_CONTENT_SIZE_LARGER ? "Larger" : "Largest";
    lv_obj_t *detail = label(size_name);
    CHECK(detail != NULL);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(detail, 0), ORB_LABEL));
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    CHECK(lv_color_eq(lv_obj_get_style_text_color(label("10:09"), 0), ORB_LABEL));
    orb_shell_navigate(s_shell, ORB_SHELL_ACCESSIBILITY);
    for (unsigned frame = 0; frame < 40; frame++) settle();
    /* Re-entry restores the inline switch's confirmed value. */
    row = reveal_row("Increase contrast");
    CHECK(orb_kit_switch_get_checked(orb_kit_row_switch(row)));
    tap("Increase contrast");
    row = reveal_row("Increase contrast");
    CHECK(!orb_kit_switch_get_checked(orb_kit_row_switch(row)));
    reveal_row("Text size");
    detail = label(size_name);
    CHECK(detail != NULL);
    CHECK(lv_color_eq(lv_obj_get_style_text_color(detail, 0), ORB_KIT_SECONDARY));
    CHECK(lv_obj_get_style_text_opa(detail, 0) == ORB_KIT_SECONDARY_OPA);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    m.demo = true;
    update(&m);
    CHECK(label("10:09"));
    CHECK(!label("Connected"));
    m.demo = false;
    update(&m);
    CHECK(!label("Connected"));
    CHECK(!label("Demo"));
}

static void settle_page(void)
{
    for (unsigned frame = 0; frame < 40; frame++) settle();
}

static orb_card_t choice_fixture(void)
{
    orb_model_snapshot_t model;
    CHECK(orb_model_snapshot(&model));
    for (size_t i = 0; i < model.card_count; i++)
        if (!strcmp(model.cards[i].id, "choice-next")) return model.cards[i];
    CHECK(false);
    return (orb_card_t){0};
}

/* Regression for the observed hardware sequence: card.cancel races the answer
 * response after a local decision. Absence of a live card is not cancellation
 * of our decision; the durable hub record can also name a different winner. */
static void answer_resolution_and_long_ids(void)
{
    orb_shell_snapshot_t m = connected();
    for (unsigned outcome = 0; outcome < 4; outcome++) {
        fresh(&m);
        orb_card_t card = choice_fixture();
        memset(card.id, 'x', ORB_MODEL_CARD_ID_MAX);
        card.id[ORB_MODEL_CARD_ID_MAX] = '\0';
        snprintf(card.options[0].id, sizeof(card.options[0].id), "link");
        snprintf(card.options[0].label, sizeof(card.options[0].label), "Device link");
        CHECK(orb_model_upsert_card(&card));
        orb_shell_poll(s_shell);
        orb_shell_select_card(s_shell, card.id);
        settle_page();
        tap("Device link");
        CHECK(label("Sending…"));
        CHECK(label("Device link"));
        orb_answer_t submitted;
        CHECK(orb_model_peek_answer(&submitted));
        CHECK(strlen(submitted.card) == ORB_MODEL_CARD_ID_MAX);
        CHECK(!strcmp(submitted.card, card.id));
        CHECK(!strcmp(submitted.option, "link"));
        CHECK(orb_model_cancel_card(card.id));
        settle_page();
        CHECK(!label("Canceled"));
        CHECK(label("Sending…"));
        CHECK(!find_control(orb_shell_root(s_shell), "Device link"));
        orb_answer_t recorded = submitted;
        if (outcome == 2) snprintf(recorded.option, sizeof(recorded.option), "stack");
        CHECK(orb_model_complete_answer(card.id, outcome == 3 ? NULL : &recorded,
            outcome != 1, outcome == 3 ? "expired" : NULL));
        settle_page();
        CHECK(orb_model_pending_answers() == 0);
        CHECK(!label("Canceled"));
        CHECK(!label("Sending…"));
        CHECK(label(outcome == 1 ? "Sent" : outcome == 3 ? "Not sent" : "Delivered"));
        CHECK(!find_control(orb_shell_root(s_shell), "Device link"));
        CHECK(!find_control(orb_shell_root(s_shell), "Smart Stack"));
        /* Text can wrap; inspect the stored full prose rather than visible
         * rows alone. A losing local option must never mask the hub winner. */
        if (outcome == 2) CHECK(find_text_in_tree(orb_shell_root(s_shell), "Smart Stack"));
        else CHECK(find_text_in_tree(orb_shell_root(s_shell), "Device link"));
        CHECK(orb_model_upsert_card(&card));
        settle_page();
        CHECK(!find_control(orb_shell_root(s_shell), "Device link"));
    }

    fresh(&m);
    orb_shell_select_card(s_shell, "choice-next");
    settle_page();
    CHECK(orb_model_cancel_card("choice-next"));
    settle_page();
    CHECK(label("Canceled"));
    CHECK(!label("Delivered"));
    CHECK(!label("Sending…"));
    CHECK(orb_model_pending_answers() == 0);
}

static void reconnect_disables_decisions(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_shell_select_card(s_shell, "choice-next");
    settle_page();
    orb_model_set_connection(false, false);
    settle_page();
    if (find_control(orb_shell_root(s_shell), "Agents")) tap("Agents");
    CHECK(orb_model_pending_answers() == 0);
    orb_model_set_connection(true, false);
    settle_page();
    if (find_control(orb_shell_root(s_shell), "Agents")) tap("Agents");
    CHECK(orb_model_pending_answers() == 0);
    orb_model_set_connection(true, true);
    settle_page();
    tap("Agents");
    CHECK(orb_model_pending_answers() == 1);
}

static void focus_is(const char *expected_target, const char *expected_card)
{
    char target[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
    CHECK(orb_model_get_voice_focus(target, card));
    CHECK(!strcmp(target, expected_target));
    CHECK(!strcmp(card, expected_card));
}

static void voice_request_is(orb_voice_action_t action, const char *target, const char *card)
{
    orb_voice_request_t request;
    CHECK(orb_model_take_voice_request(&request));
    CHECK(request.action == action);
    if (target) CHECK(!strcmp(request.target, target));
    if (card) CHECK(!strcmp(request.card, card));
}

static const char *s_transcript =
    "Please review the round shell at the largest text size. Check the Agents list, "
    "then run the simulator tests and show me the results before changing anything else. "
    "Keep the approval buttons visible while I read the complete command.";

static void voice_focus_commands_and_confirmation(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    CHECK(orb_model_cancel_card("choice-next"));
    orb_shell_select_session(s_shell, "codex-shell");
    settle_page();
    focus_is("codex-shell", "");
    tap("Reply");
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
    tap(ORB_ICON_MIC);
    orb_voice_request_t short_tap; CHECK(!orb_model_take_voice_request(&short_tap));
    hold_to_speak(true);
    voice_request_is(ORB_VOICE_START, "codex-shell", "");
    voice_request_is(ORB_VOICE_STOP, NULL, NULL);
    orb_voice_request_t request;
    CHECK(!orb_model_take_voice_request(&request));

    orb_voice_state_t voice = {.phase = ORB_VOICE_STARTING, .stream = 9};
    snprintf(voice.target, sizeof(voice.target), "codex-shell");
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Starting microphone"));
    CHECK(find_control(orb_shell_root(s_shell), ORB_ICON_STOP));
    voice.phase = ORB_VOICE_RECORDING;
    voice.level = 72;
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Listening"));
    tap(ORB_ICON_STOP);
    voice_request_is(ORB_VOICE_STOP, NULL, NULL);

    voice.phase = ORB_VOICE_TRANSCRIBING;
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(label("Transcribing…"));
    CHECK(!find_control(orb_shell_root(s_shell), "Send"));
    tap("Cancel");
    voice_request_is(ORB_VOICE_CANCEL, NULL, NULL);

    /* This manually published result has no physical recording provenance.
     * Physical-origin countdown consent is covered by agents_design1. */
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_HOLD, .synthetic=true});
    orb_shell_button(s_shell, &(orb_button_event_t){.button=ORB_BUTTON_BOOT, .gesture=ORB_BUTTON_UP, .synthetic=true});
    voice.phase = ORB_VOICE_CONFIRMING;
    snprintf(voice.text, sizeof(voice.text), "%s", s_transcript);
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    lv_obj_t *text = find_label_in_tree(orb_shell_root(s_shell), s_transcript);
    CHECK(text != NULL);
    CHECK(lv_label_get_long_mode(text) == LV_LABEL_LONG_MODE_WRAP);
    CHECK(find_control(orb_shell_root(s_shell), "Send"));
    CHECK(find_control(orb_shell_root(s_shell), "Cancel"));
    CHECK(!find_control(orb_shell_root(s_shell), ORB_ICON_CHEVRON_DOWN));
    CHECK(!orb_model_take_voice_request(&request)); /* A result is never consent. */
    orb_model_set_connection(true, false);
    settle_page();
    tap("Send");
    CHECK(!orb_model_take_voice_request(&request));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), s_transcript));
    orb_model_set_connection(true, true);
    settle_page();
    s_now += 600000;
    settle_page();
    CHECK(find_label_in_tree(orb_shell_root(s_shell), s_transcript));
    CHECK(!orb_model_take_voice_request(&request));
    tap("Send");
    voice_request_is(ORB_VOICE_CONFIRM, NULL, NULL);
    if (find_control(orb_shell_root(s_shell), "Send")) tap("Send");
    CHECK(!orb_model_take_voice_request(&request)); /* Disable immediately. */
    voice.phase = ORB_VOICE_SENDING;
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(label("Sending…"));
    CHECK(!find_control(orb_shell_root(s_shell), "Send"));

    voice.phase = ORB_VOICE_ERROR;
    snprintf(voice.error, sizeof(voice.error), "Delivery could not be confirmed");
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(find_label_in_tree(orb_shell_root(s_shell), s_transcript));
    CHECK(!find_control(orb_shell_root(s_shell), "Send"));
    tap("Done");
    voice_request_is(ORB_VOICE_CANCEL, NULL, NULL);

    voice = (orb_voice_state_t){0};
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    orb_shell_select_card(s_shell, "message-dot");
    settle_page();
    focus_is("dot-evening", "message-dot");
    tap("Reply");
    hold_to_speak(true);
    voice_request_is(ORB_VOICE_START, "dot-evening", "message-dot");
    voice_request_is(ORB_VOICE_STOP, NULL, NULL);
    orb_shell_select_session(s_shell, "codex-tests");
    settle_page();
    CHECK(lv_obj_has_state(find_control(orb_shell_root(s_shell), "Reply"), LV_STATE_DISABLED));

    /* Main owns physical BOOT capture. Shell entry points may expose its UI,
     * but must not duplicate START/STOP commands for the same sampled edge. */
    orb_shell_talk(s_shell, true);
    orb_shell_talk(s_shell, false);
    CHECK(!orb_model_take_voice_request(&request));
}

static void message_delivery_is_transport_only(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    CHECK(orb_model_cancel_card("choice-next"));
    orb_shell_select_session(s_shell, "codex-shell");
    settle_page();
    tap("Reply");
    orb_message_t message = {0};
    snprintf(message.target, sizeof(message.target), "codex-shell");
    snprintf(message.text, sizeof(message.text), "%s", s_transcript);
    orb_model_complete_message(&message, false, "timeout");
    settle_page();
    CHECK(label("Unknown"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), s_transcript));
    CHECK(!find_control(orb_shell_root(s_shell), "Send"));
    orb_model_complete_message(&message, true, NULL);
    settle_page();
    CHECK(label("Unknown"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), s_transcript));
    CHECK(!find_control(orb_shell_root(s_shell), "Send"));
}

static void coalesced_voice_delivery_keeps_transcript(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    CHECK(orb_model_cancel_card("choice-next"));
    orb_shell_select_session(s_shell, "codex-shell");
    settle_page();
    tap("Reply");
    orb_voice_state_t voice = {.phase = ORB_VOICE_SENDING, .stream = 22};
    snprintf(voice.target, sizeof(voice.target), "codex-shell");
    snprintf(voice.text, sizeof(voice.text), "%s", s_transcript);
    CHECK(orb_model_set_voice(&voice));
    voice = (orb_voice_state_t){.phase = ORB_VOICE_IDLE};
    CHECK(orb_model_set_voice(&voice));
    /* Both service transitions can happen before LVGL samples a revision. */
    settle_page();
    CHECK(label("Unknown"));
    CHECK(find_label_in_tree(orb_shell_root(s_shell), s_transcript));
    CHECK(!find_control(orb_shell_root(s_shell), "Send"));
    orb_voice_request_t request;
    CHECK(!orb_model_take_voice_request(&request));
}

static lv_area_t label_ink_bounds(lv_obj_t *text)
{
    const lv_font_t *font = lv_obj_get_style_text_font(text, 0);
    const char *value = lv_label_get_text(text);
    lv_area_t ink = {lv_obj_get_x(text), INT32_MAX,
        lv_obj_get_x(text) + lv_obj_get_width(text) - 1, INT32_MIN};
    for (uint32_t offset = 0; value[offset];) {
        uint32_t cp = lv_text_encoded_next(value, &offset), peek = offset;
        uint32_t next = value[peek] ? lv_text_encoded_next(value, &peek) : 0;
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_h) continue;
        int y = lv_obj_get_y(text) + font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        ink.y1 = LV_MIN(ink.y1, y); ink.y2 = LV_MAX(ink.y2, y + glyph.box_h - 1);
    }
    ink.y2 += (LV_MAX(1, lv_label_get_max_lines(text)) - 1) *
        (font->line_height + lv_obj_get_style_text_line_space(text, 0));
    return ink;
}

static void round3_layout_geometry(void)
{
    const orb_content_size_t sizes[] = {
        ORB_CONTENT_SIZE_DEFAULT, ORB_CONTENT_SIZE_LARGER, ORB_CONTENT_SIZE_AX3,
    };
    for (unsigned size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
        CHECK(orb_fonts_set_content_size(sizes[size]) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(size == 2) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(size == 2);
        orb_shell_snapshot_t m = connected();
        fresh(&m);
        orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS);
        settle_page();
        lv_obj_t *name = find_label_in_tree(orb_shell_root(s_shell), "Firmware build");
        CHECK(name != NULL);
        lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(name));
        unsigned compared = 0;
        for (unsigned r = 0; r < lv_obj_get_child_count(list); r++) {
            lv_obj_t *row = lv_obj_get_child(list, r);
            if (lv_obj_get_style_opa(row, 0) == LV_OPA_TRANSP || lv_obj_get_child_count(row) < 3) continue;
            lv_obj_t *glyph = lv_obj_get_child(row, 0);
            if (lv_obj_is_hidden(glyph)) continue;
            int glyph_end = lv_obj_get_x(glyph) + lv_obj_get_width(glyph);
            lv_area_t lanes[8]; unsigned lane_count = 0;
            for (unsigned c = 1; c < lv_obj_get_child_count(row); c++) {
                lv_obj_t *text = lv_obj_get_child(row, c);
                if (lv_obj_is_hidden(text) || !lv_obj_check_type(text, &lv_label_class) ||
                    !lv_label_get_text(text)[0]) continue;
                CHECK(lv_obj_get_x(text) >= glyph_end + 8);
                CHECK(lv_label_get_max_lines(text) >= 1 &&
                      lv_label_get_max_lines(text) <= (sizes[size] == ORB_CONTENT_SIZE_AX3 ? 2 : 1));
                lv_area_t ink = label_ink_bounds(text);
                CHECK(ink.y1 >= 0 && ink.y2 < lv_obj_get_height(row));
                /* Kit row leading boxes can intersect; measured glyph ink
                 * must remain disjoint, including any trailing value rail. */
                for (unsigned previous = 0; previous < lane_count; ++previous)
                    CHECK(ink.x1 > lanes[previous].x2 || ink.x2 < lanes[previous].x1 ||
                          ink.y1 > lanes[previous].y2 || ink.y2 < lanes[previous].y1);
                CHECK(lane_count < sizeof(lanes) / sizeof(*lanes));
                lanes[lane_count++] = ink;
                compared++;
            }
        }
        CHECK(compared > 0);
        if (size < 2) {
            orb_shell_navigate(s_shell, ORB_SHELL_APPS);
            settle_page();
            const char *apps[] = {"Agents", "Dot", "Home", "Time", "Play", "Updates", "Settings"};
            lv_obj_t *caption = NULL;
            for (unsigned i = 0; i < sizeof(apps) / sizeof(*apps); ++i)
                if (!caption) caption = label(apps[i]);
            lv_obj_t *icon = find_control(orb_shell_root(s_shell), ORB_ICON_PROMPT);
            CHECK(caption != NULL && icon != NULL);
            orb_kit_rim_text_info_t rim;
            CHECK(orb_kit_rim_text_info(caption, &rim));
            CHECK(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES);
            CHECK(lv_obj_get_width(icon) == ORB_KIT_DISC && lv_obj_get_height(icon) == ORB_KIT_DISC);
            CHECK(white_pixels(100, 400, 366, 455) > 20);
        }
    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false);
}

static void cancellation_receipts_survive_clear_all(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_shell_select_card(s_shell, "choice-next");
    settle_page();
    tap("Agents");
    CHECK(orb_model_cancel_card("choice-next")); /* Link has not claimed it. */
    settle_page();
    CHECK(label("Not sent"));
    CHECK(!label("Sending…"));
    CHECK(!label("Canceled"));
    CHECK(orb_model_pending_answers() == 0);

    fresh(&m);
    orb_shell_select_card(s_shell, "choice-next");
    settle_page();
    tap("Agents");
    orb_answer_t answer;
    CHECK(orb_model_peek_answer(&answer));
    CHECK(orb_model_cancel_card(answer.card));
    CHECK(orb_model_complete_answer(answer.card, &answer, true, NULL));
    settle_page();
    CHECK(label("Delivered"));
    orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS);
    settle_page();
    reveal_row("Clear all"); tap("Clear all");
    CHECK(label("No notifications"));
    orb_shell_select_card(s_shell, answer.card);
    settle_page();
    CHECK(label("Delivered"));
    CHECK(!label("Canceled"));
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Agents"));

    fresh(&m);
    orb_shell_select_card(s_shell, "choice-next");
    settle_page();
    orb_model_snapshot_t snapshot;
    CHECK(orb_model_snapshot(&snapshot));
    CHECK(orb_model_sessions_chunk(snapshot.sessions, snapshot.session_count, true, false));
    settle_page();
    CHECK(label("Loading…")); /* Card replay gap is not a cancel. */
    CHECK(!label("Canceled"));
}

static void mic_lifetime_and_service_precedence(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    CHECK(orb_model_cancel_card("choice-next"));
    orb_shell_select_session(s_shell, "codex-shell");
    settle_page();
    tap("Reply");
    /* A completed hold/release can finish before the next voice snapshot. It
     * must not leave the mic disabled waiting for a phase we missed. */
    hold_to_speak(true);
    voice_request_is(ORB_VOICE_START, "codex-shell", "");
    voice_request_is(ORB_VOICE_STOP, NULL, NULL);
    hold_to_speak(true);
    voice_request_is(ORB_VOICE_START, "codex-shell", "");
    voice_request_is(ORB_VOICE_STOP, NULL, NULL);
    lv_obj_t *hold = hold_to_speak(false);
    voice_request_is(ORB_VOICE_START, "codex-shell", "");
    orb_voice_state_t voice = {.phase = ORB_VOICE_STARTING, .stream = 33};
    snprintf(voice.target, sizeof(voice.target), "codex-shell");
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(lv_obj_is_valid(hold));
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Listening"));
    voice.phase = ORB_VOICE_RECORDING;
    voice.level = 82;
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    CHECK(lv_obj_is_valid(hold));
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Listening"));
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer);
    settle_page();
    voice_request_is(ORB_VOICE_STOP, NULL, NULL);
    orb_voice_request_t request;
    CHECK(!orb_model_take_voice_request(&request));

    m.ota = ORB_SHELL_OTA_DOWNLOADING;
    m.progress = 42;
    update(&m);
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Updating to "));
    CHECK(label(ORB_ICON_MIC " Listening")); /* Mandatory service screens keep capture visible. */
    CHECK(lv_obj_is_visible(label(ORB_ICON_MIC " Listening")));

    fresh(&(orb_shell_snapshot_t){
        .net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_ONLINE,
        .battery_pct = 80, .rssi = -54, .brightness = 70,
    });
    orb_shell_select_session(s_shell, "codex-tests");
    settle_page();
    CHECK(lv_obj_has_state(find_control(orb_shell_root(s_shell), "Reply"), LV_STATE_DISABLED));
    voice = (orb_voice_state_t){.phase = ORB_VOICE_CONFIRMING, .stream = 34};
    snprintf(voice.target, sizeof(voice.target), "codex-tests");
    snprintf(voice.text, sizeof(voice.text), "Review the test results.");
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    lv_obj_t *send = find_control(orb_shell_root(s_shell), "Send");
    CHECK(send != NULL);
    CHECK(lv_obj_has_state(send, LV_STATE_DISABLED));
    tap("Send");
    CHECK(!orb_model_take_voice_request(&request));

    /* A bounded card reply is a different grant from starting a new turn in
     * an observed session. Its explicit reply:true survives can_message:false,
     * but never the disappearance of the matching live card. */
    m.ota = ORB_SHELL_OTA_IDLE;
    fresh(&m);
    orb_card_t reply = {.kind = ORB_CARD_MESSAGE, .reply = true};
    snprintf(reply.id, sizeof(reply.id), "reply-observed");
    snprintf(reply.session, sizeof(reply.session), "codex-tests");
    snprintf(reply.title, sizeof(reply.title), "Question from test runner");
    CHECK(orb_model_upsert_card(&reply));
    settle_page();
    orb_shell_select_card(s_shell, reply.id);
    settle_page();
    tap("Reply");
    CHECK(find_control(orb_shell_root(s_shell), ORB_ICON_MIC));
    voice = (orb_voice_state_t){.phase = ORB_VOICE_CONFIRMING, .stream = 35};
    snprintf(voice.target, sizeof(voice.target), "%s", reply.session);
    snprintf(voice.card, sizeof(voice.card), "%s", reply.id);
    snprintf(voice.text, sizeof(voice.text), "Report the results.");
    CHECK(orb_model_set_voice(&voice));
    settle_page();
    send = find_control(orb_shell_root(s_shell), "Send");
    CHECK(send && !lv_obj_has_state(send, LV_STATE_DISABLED));
    CHECK(orb_model_cancel_card(reply.id));
    settle_page();
    send = find_control(orb_shell_root(s_shell), "Send");
    CHECK(send && lv_obj_has_state(send, LV_STATE_DISABLED));
    tap("Send");
    CHECK(!orb_model_take_voice_request(&request));
}

static void voice_queue_failure_is_visible(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    /* Voice fallback requires the actual per-device Dot subscription grant. */
    orb_session_t dot = {.id = "dot", .agent = ORB_AGENT_DOT, .title = "Dot",
        .state = ORB_SESSION_IDLE, .reason = "Ready for your message", .can_message = true};
    CHECK(orb_model_upsert_session(&dot));
    orb_shell_navigate(s_shell, ORB_SHELL_VOICE);
    settle_page();
    for (unsigned i = 0; i < ORB_MODEL_MAX_VOICE_REQUESTS; i++)
        CHECK(orb_model_voice_request(ORB_VOICE_STOP, "dot", NULL));
    hold_to_speak(true);
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Voice queue full. Try again."));
    orb_voice_request_t request;
    for (unsigned i = 0; i < ORB_MODEL_MAX_VOICE_REQUESTS; i++) {
        CHECK(orb_model_take_voice_request(&request));
        CHECK(request.action == ORB_VOICE_STOP);
    }
    CHECK(!orb_model_take_voice_request(&request));
}

static lv_obj_t *reveal_row(const char *caption)
{
    lv_obj_t *text = find_label_in_tree(orb_shell_root(s_shell), caption);
    /* Re-entry restores the prior scroll, so start the search at its beginning
     * when the compositor has hidden the requested row near the far rim. */
    if (!text) { orb_shell_twist(s_shell, -64); settle_page(); }
    text = find_label_in_tree(orb_shell_root(s_shell), caption);
    for (unsigned attempt = 0; !text && attempt < 16; attempt++) {
        if (!orb_shell_twist(s_shell, 1)) break;
        settle_page();
        text = find_label_in_tree(orb_shell_root(s_shell), caption);
    }
    if (!text) { fprintf(stderr, "missing row: %s\n", caption); dump_labels(orb_shell_root(s_shell)); }
    CHECK(text);
    lv_obj_t *row = lv_obj_get_parent(text), *list = lv_obj_get_parent(row);
    while (list && !lv_obj_is_scrollable(list)) list = lv_obj_get_parent(list);
    CHECK(list);
    lv_area_t row_box, viewport;
    lv_obj_get_coords(row, &row_box); lv_obj_get_coords(list, &viewport);
    int scroll = lv_obj_get_scroll_y(list) +
        (row_box.y1 + row_box.y2 - viewport.y1 - viewport.y2) / 2;
    lv_obj_scroll_to_y(list, LV_MAX(0, scroll), LV_ANIM_OFF);
    settle_page();
    return row;
}

static void temperature_preferences(void)
{
    orb_shell_snapshot_t m = connected();
    for (unsigned size = 0; size < 3; size++) {
        CHECK(orb_fonts_set_content_size(size ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(size == 2) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(size == 2);
        fresh(&m);
        orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); settle_page();
        reveal_row("Units"); tap("Units");
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_UNITS);
        tap("Temperature");
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TEMPERATURE);
        orb_model_snapshot_t model;
        const char *names[] = {"°F", "°C", "Automatic"};
        orb_temperature_t units[] = {ORB_TEMPERATURE_F, ORB_TEMPERATURE_C, ORB_TEMPERATURE_UNKNOWN};
        for (unsigned i = 0; i < 3; ++i) {
            reveal_row(names[i]); tap(names[i]);
            CHECK(orb_model_snapshot(&model) && model.prefs_pending);
            CHECK(label("Saving…"));
            orb_prefs_patch_t patch;
            CHECK(orb_model_take_prefs_request(&patch));
            CHECK(patch.has_temperature && patch.temperature == units[i] && !patch.sources_mask && !patch.has_idle_minutes);
            model.prefs.temperature = i == 1 ? ORB_TEMPERATURE_C : ORB_TEMPERATURE_F;
            model.prefs.temperature_owner = i != 2;
            orb_model_complete_prefs(&model.prefs, NULL); settle_page();
            CHECK(orb_model_temperature() == model.prefs.temperature);
            CHECK(!label("Saving…"));
        }
        reveal_row("°C"); tap("°C");
        orb_model_complete_prefs(NULL, "timeout"); settle_page();
        reveal_row("°C");
        CHECK(label("Not saved"));
        CHECK(orb_model_temperature() == ORB_TEMPERATURE_F);
        orb_model_set_connection(false, false); settle_page();
        CHECK(lv_obj_has_state(find_control(orb_shell_root(s_shell), "°C"), LV_STATE_DISABLED));
    }
}

static void silent_restores_with_workspace(void)
{
    orb_sound_preferences_t original, changed;
    orb_sound_preferences(&original);
    orb_shell_snapshot_t m = connected();
    for (unsigned state = 0; state < 2; state++) {
        /* The storage test verifies NVS round trips; this verifies the shell
         * consumes that restored choice rather than replacing it at startup. */
        orb_sound_set_silent(state);
        fresh(&m);
        orb_shell_environment(s_shell, 0, 0, 1, false);
        orb_sound_stats_t stats;
        orb_sound_stats(&stats);
        CHECK(stats.silent == (bool)state);
        orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); settle_page();
        CHECK(find_label_in_tree(orb_shell_root(s_shell), state ? ORB_ICON_BELL_SLASH : ORB_ICON_BELL));
        orb_shell_navigate(s_shell, ORB_SHELL_SOUND); settle_page();
        reveal_row("Silent"); tap("Silent");
        orb_sound_preferences(&changed);
        CHECK(changed.silent == !state);
        /* A new workspace is the reboot boundary for UI-owned state. */
        fresh(&m);
        orb_shell_environment(s_shell, 0, 0, 1, false);
        orb_sound_stats(&stats);
        CHECK(stats.silent == !state);
    }
    CHECK(orb_sound_set_preferences(&original));
    orb_shell_set_silent(s_shell, original.silent);
}

static void visible_preferences_and_sound(void)
{
    orb_shell_snapshot_t m = connected();
    for (unsigned profile = 0; profile < 2; profile++) {
        CHECK(orb_fonts_set_content_size(profile ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(profile) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(profile);
        fresh(&m);
        orb_shell_navigate(s_shell, ORB_SHELL_ACCESSIBILITY); settle_page();
        const char *items[] = {"Text size", "Bold text", "Reduce motion", "Reduce transparency", "Increase contrast"};
        for (unsigned i = 0; i < 5; i++) {
            lv_obj_t *r = reveal_row(items[i]);
            lv_obj_t *name = lv_obj_get_child(r, 1);
            CHECK(lv_label_get_long_mode(name) == LV_LABEL_LONG_MODE_DOTS ||
                lv_label_get_long_mode(name) == LV_LABEL_LONG_MODE_WRAP);
            if (i) CHECK(orb_kit_row_switch(r));
            CHECK(lv_obj_get_width(lv_obj_get_child(r, 0)) == ORB_KIT_DISC_ROW);
            CHECK(find_control(orb_shell_root(s_shell), items[i]));
        }
        reveal_row("Text size"); tap("Text size");
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Default"));
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Larger"));
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Largest"));
        reveal_row("Largest"); tap("Largest");
        CHECK(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3);
        boot_back();
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Text size"));
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Largest"));
        CHECK(orb_fonts_bold_text() == (bool)profile && orb_ui_reduce_transparency() == (bool)profile);
        orb_shell_navigate(s_shell, ORB_SHELL_SESSION_SETTINGS); settle_page();
        const char *sources[] = {"Claude", "Codex", "Dot"};
        for (unsigned i = 0; i < 3; i++) {
            lv_obj_t *r = reveal_row(sources[i]);
            lv_obj_t *name = lv_obj_get_child(r, 1), *track = orb_kit_row_switch(r);
            if (track) {
                lv_area_t name_bounds, switch_bounds;
                CHECK(orb_round_list_child_bounds(r, name, &name_bounds));
                lv_obj_get_coords(track, &switch_bounds);
                CHECK(name_bounds.x2 < switch_bounds.x1);
            } else {
                track = lv_obj_get_child(r, 3);
                CHECK(lv_obj_get_style_max_width(name, 0) > 0);
                CHECK(lv_obj_get_x(name) + lv_obj_get_width(name) + 20 <= lv_obj_get_x(track));
            }
            /* The live switch follows its row's prepared rim projection. */
            CHECK(lv_obj_get_width(track) >= 64 && lv_obj_get_width(track) <= 80);
        }
        reveal_row("Recent activity"); CHECK(label("10 min"));
        orb_shell_navigate(s_shell, ORB_SHELL_ABOUT); settle_page();
        const char *about[] = {"Version", "Mac", "Device ID", "Licenses"};
        for (unsigned i = 0; i < 4; i++) CHECK(find_label_in_tree(orb_shell_root(s_shell), about[i]));
        reveal_row("Licenses"); tap("Licenses");
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Framework7 icons"));
        reveal_row("Phosphor icons"); CHECK(label("MIT"));
        boot_back(); CHECK(find_label_in_tree(orb_shell_root(s_shell), "Version"));
        orb_shell_navigate(s_shell, ORB_SHELL_SOUND); settle_page();
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Silent"));
        CHECK(find_label_in_tree(orb_shell_root(s_shell), "Quiet hours"));
        reveal_row("Volume"); tap("Volume");
        orb_sound_preferences_t prefs, changed;
        orb_sound_preferences(&prefs);
        /* Sound now opens the shared bottom-arc level editor. Its retained
         * percentage replaces the old page-specific "Turn the rim" hint. */
        char volume[16]; snprintf(volume, sizeof(volume), "%u%%", prefs.volume);
        CHECK(label(volume));
        CHECK(orb_shell_twist(s_shell, -2)); orb_sound_preferences(&changed);
        CHECK(changed.volume == (prefs.volume > 2 ? prefs.volume - 2 : 0));
        for (unsigned i = 0; i < 80; ++i) settle();
        snprintf(volume, sizeof(volume), "%u%%", changed.volume);
        CHECK(label(volume)); /* The visible Volume page keeps its owner. */
        boot_back();
        reveal_row("Quiet hours"); tap("Quiet hours");
        reveal_row("Quiet hours"); tap("Quiet hours");
        orb_sound_preferences(&changed); CHECK(changed.quiet_enabled != prefs.quiet_enabled);
        reveal_row("Starts"); tap("Starts"); tap(ORB_ICON_PLUS);
        orb_sound_preferences(&changed); CHECK(changed.quiet_start == (prefs.quiet_start + 1) % 24);
        boot_back(); CHECK(find_label_in_tree(orb_shell_root(s_shell), "Ends"));
        CHECK(orb_sound_set_preferences(&prefs));
    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false);
}

static void focus_preferences_and_idle(void)
{
    orb_shell_snapshot_t m = connected();
    for (unsigned size = 0; size < 3; size++) {
        CHECK(orb_fonts_set_content_size(size ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(size == 2) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(size == 2);
        fresh(&m);
        orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS);
        settle_page();
        reveal_row("Sessions"); tap("Sessions");
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION_SETTINGS);
        lv_obj_t *claude = reveal_row("Claude");
        lv_obj_t *track = orb_kit_row_switch(claude); CHECK(track);
        lv_obj_t *knob = lv_obj_get_child(track, 0); CHECK(knob);
        for (unsigned i = 0; i < lv_obj_get_child_count(knob); ++i)
            CHECK(orb_glass_get_amount(lv_obj_get_child(knob, i)) == 0);
        tap("Claude");
        orb_model_snapshot_t model;
        CHECK(orb_model_snapshot(&model));
        CHECK(model.prefs_pending && model.prefs.sources[ORB_AGENT_CLAUDE]);
        CHECK(label("Saving…"));
        CHECK(lv_obj_has_state(find_control(orb_shell_root(s_shell), "Claude"), LV_STATE_DISABLED));
        orb_prefs_patch_t patch;
        CHECK(orb_model_take_prefs_request(&patch));
        CHECK(patch.sources_mask == 1 && !patch.sources[0] && !patch.has_idle_minutes);
        model.prefs.sources[0] = false;
        orb_model_complete_prefs(&model.prefs, NULL);
        settle_page();
        CHECK(!lv_obj_has_state(find_control(orb_shell_root(s_shell), "Claude"), LV_STATE_DISABLED));
        tap("Claude");
        orb_model_complete_prefs(NULL, "timeout"); settle_page();
        CHECK(label("Not saved"));
        CHECK(orb_model_snapshot(&model));
        CHECK(!model.prefs.sources[0]);
        reveal_row("Recent activity");
        const unsigned windows[] = {30, 0, 5, 10};
        for (unsigned i = 0; i < 4; i++) {
            tap("Recent activity");
            CHECK(orb_model_take_prefs_request(&patch));
            CHECK(patch.sources_mask == 0 && patch.has_idle_minutes && patch.idle_minutes == windows[i]);
            model.prefs.idle_minutes = windows[i];
            orb_model_complete_prefs(&model.prefs, NULL); settle_page();
        }
        orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle_page();
        CHECK(!label("0 waiting"));
        CHECK(orb_model_set_summary(12, (uint32_t[3]){1, 0, 0}));
        orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS); settle_page();
        CHECK(!find_label_in_tree(orb_shell_root(s_shell), "Firmware build"));
        /* Sessions owns the idle pager inline in Design 1.0. Opening it
         * requests the first page without a separate summary-row detour. */
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
        orb_idle_request_t request;
        CHECK(orb_model_take_idle_request(&request));
        CHECK(!request.cursor[0]);
        orb_session_t idle[] = {
            {.id="idle-one", .agent=ORB_AGENT_CODEX, .title="Earlier review", .project="Orb", .state=ORB_SESSION_IDLE, .can_message=true},
            {.id="idle-two", .agent=ORB_AGENT_CODEX, .title="Earlier build", .project="Orb", .state=ORB_SESSION_DONE},
        };
        CHECK(orb_model_complete_idle(&request, idle, 2, "next", NULL));
        settle_page();
        reveal_row("Earlier review"); tap("Earlier review");
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);
        CHECK(label("Earlier review"));
        orb_idle_page_t page;
        /* The nested session keeps its launching row available for reverse
         * iris. Closing this generation here would replace Back with a reload. */
        CHECK(orb_model_idle_snapshot(&page) && page.count == 2 && !page.pending);
        char target[49], card[65];
        CHECK(orb_model_get_voice_focus(target, card));
        CHECK(!strcmp(target, "idle-one"));
        tap("Reply");
        orb_voice_state_t voice = {.phase = ORB_VOICE_CONFIRMING, .stream = 19};
        strcpy(voice.target, "idle-one"); strcpy(voice.text, "Please continue the review.");
        CHECK(orb_model_set_voice(&voice)); settle_page();
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "Earlier review"));
        CHECK(!find_text_in_tree(orb_shell_root(s_shell), "To Dot"));
        voice.phase = ORB_VOICE_IDLE; voice.text[0] = '\0';
        CHECK(orb_model_set_voice(&voice)); settle_page();
        boot_back();
        boot_back();
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSIONS);
        CHECK(!orb_model_take_idle_request(&request));
        lv_obj_t *more = reveal_row("Idle sessions");
        lv_obj_t *list = lv_obj_get_parent(more);
        lv_obj_t *previous = lv_obj_get_child(list, 1);
        CHECK(lv_obj_get_y(more) >= lv_obj_get_y(previous) + lv_obj_get_height(previous));
        tap("Idle sessions");
        CHECK(orb_model_take_idle_request(&request) && !strcmp(request.cursor, "next"));
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "Loading…"));
        CHECK(orb_model_complete_idle(&request, NULL, 0, NULL, "timeout")); settle_page();
        reveal_row("Idle sessions");
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "Couldn’t load"));
        tap("Idle sessions");
        CHECK(orb_model_take_idle_request(&request) && !strcmp(request.cursor, "next"));
        CHECK(orb_model_complete_idle(&request, NULL, 0, NULL, NULL)); settle_page();
        CHECK(!find_label_in_tree(orb_shell_root(s_shell), "Earlier review"));
        CHECK(!find_label_in_tree(orb_shell_root(s_shell), "Idle sessions"));
        orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle_page();
        CHECK(!orb_model_idle_snapshot(&page));
    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false);
}

static void touched_toggle_survives_service_interrupt(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_shell_navigate(s_shell, ORB_SHELL_SESSION_SETTINGS); settle_page();
    lv_obj_t *control = reveal_row("Claude");
    lv_obj_t *track = orb_kit_row_switch(control); CHECK(track);
    lv_obj_t *knob = lv_obj_get_child(track, 0); CHECK(knob);
    unsigned prepared = lv_obj_get_child_count(knob); CHECK(prepared >= 1);
    lv_area_t box; lv_obj_get_coords(track, &box);
    s_input.point = (lv_point_t){(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); settle();
    CHECK(lv_obj_get_child_count(knob) == prepared); /* Prepared, never allocated on press. */
    lv_obj_t *pressed_glass = NULL;
    for (unsigned i = 0; i < prepared; ++i) {
        lv_obj_t *child = lv_obj_get_child(knob, i);
        if (orb_glass_get_amount(child) > 0) pressed_glass = child;
    }
    CHECK(pressed_glass);
    settle(); /* Keep the service interruption inside materialisation. */
    CHECK(lv_obj_get_style_blur_backdrop(pressed_glass, 0) ||
          (lv_obj_get_style_blur_radius(pressed_glass, 0) == 0 &&
           lv_obj_get_style_bg_opa(pressed_glass, 0) > 0));
    m.identify = true; update(&m);
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); settle_page();
    m.identify = false; update(&m); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION_SETTINGS);
    control = reveal_row("Claude");
    track = orb_kit_row_switch(control); CHECK(track);
    knob = lv_obj_get_child(track, 0); CHECK(knob);
    CHECK(lv_obj_get_child_count(knob) == prepared);
    for (unsigned i = 0; i < prepared; ++i)
        CHECK(orb_glass_get_amount(lv_obj_get_child(knob, i)) == 0);
    tap("Claude");
    orb_prefs_patch_t patch;
    CHECK(orb_model_take_prefs_request(&patch));
}

static void leaving_focus_preserves_conversation(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_shell_select_session(s_shell, "claude-firmware"); settle_page();
    CHECK(orb_model_remove_session("claude-firmware")); settle_page();
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Outside focus"));
    CHECK(!find_text_in_tree(orb_shell_root(s_shell), "Session ended"));
    CHECK(label("Waiting for activity"));
    char target[49], card[65];
    CHECK(orb_model_get_voice_focus(target, card));
    CHECK(!strcmp(target, "claude-firmware"));
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle_page();
    CHECK(!label("0 waiting"));
    orb_prefs_t prefs;
    orb_model_prefs_defaults(&prefs); prefs.sources[ORB_AGENT_DOT] = false;
    CHECK(orb_model_set_prefs(&prefs));
    orb_shell_navigate(s_shell, ORB_SHELL_VOICE); settle_page();
    CHECK(!find_control(orb_shell_root(s_shell), ORB_ICON_MIC));
}

static void no_speech_retries_in_place(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_session_t dot = {.id = "dot", .agent = ORB_AGENT_DOT, .title = "Dot",
        .state = ORB_SESSION_IDLE, .reason = "Ready for your message", .can_message = true};
    CHECK(orb_model_upsert_session(&dot));
    orb_voice_state_t voice = {.phase = ORB_VOICE_ERROR};
    strcpy(voice.target, "dot"); strcpy(voice.error_code, "no_speech"); strcpy(voice.error, "Hold BOOT to talk");
    CHECK(orb_model_set_voice(&voice)); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Didn’t catch that"));
    CHECK(label("Hold BOOT to talk"));
    CHECK(!find_control(orb_shell_root(s_shell), "Cancel"));
    hold_to_speak(true);
    orb_voice_request_t request;
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_START);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_STOP);
    CHECK(!orb_model_take_voice_request(&request));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_VOICE);
}

static unsigned receipt_ink_pixels(const char *caption)
{
    lv_obj_t *text = label(caption);
    if (!text) { fprintf(stderr, "missing receipt %s surface=%u\n", caption, (unsigned)orb_shell_surface(s_shell)); dump_labels(orb_shell_root(s_shell)); }
    CHECK(text);
    lv_area_t box; lv_obj_get_coords(text, &box);
    for (lv_obj_t *parent = lv_obj_get_parent(text); parent; parent = lv_obj_get_parent(parent)) {
        lv_area_t clip; lv_obj_get_coords(parent, &clip);
        box.x1 = LV_MAX(box.x1, clip.x1); box.x2 = LV_MIN(box.x2, clip.x2);
        box.y1 = LV_MAX(box.y1, clip.y1); box.y2 = LV_MIN(box.y2, clip.y2);
    }
    sim_display_rgb(s_pixels);
    unsigned count = 0;
    for (int y = LV_MAX(0, box.y1); y <= LV_MIN(SIM_D - 1, box.y2); y++)
        for (int x = LV_MAX(0, box.x1); x <= LV_MIN(SIM_D - 1, box.x2); x++) {
            const uint8_t *pixel = &s_pixels[(y * SIM_D + x) * 3];
            /* Named receipt waits breathe to 50% white (delight §6);
             * visible body ink must remain present throughout that range. */
            count += pixel[0] > 110 && pixel[1] > 110 && pixel[2] > 110;
        }
    return count;
}

static void shellux_contract(void)
{
    orb_shell_snapshot_t m = connected();
    for (unsigned profile = 0; profile < 2; profile++) {
        CHECK(orb_fonts_set_content_size(profile ? ORB_CONTENT_SIZE_AX3 : ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
        CHECK(orb_fonts_set_bold_text(profile) == ORB_FONTS_OK);
        orb_ui_set_reduce_transparency(profile);
        for (unsigned result = 0; result < 3; result++) {
            fresh(&m);
            orb_shell_select_card(s_shell, "approval-build"); settle_page(); tap("Allow");
            CHECK(label("Sending…")); CHECK(receipt_ink_pixels("Sending…") > 20);
            CHECK(!label("Delivered") && !label("Sent") && !label("Not sent"));
            orb_answer_t answer; CHECK(orb_model_peek_answer(&answer));
            CHECK(orb_model_cancel_card(answer.card));
            CHECK(orb_model_complete_answer(answer.card, result == 0 ? NULL : &answer, result == 2, result == 0 ? "expired" : NULL));
            for (unsigned frame = 0; frame < 16; frame++) settle();
            /* Receipt truth is explicit text in Design 2.0, rather than a
             * decorative green check. Prove the authoritative state and ink. */
            const char *expected = result == 0 ? "Not sent" : result == 1 ? "Sent" : "Delivered";
            CHECK(orb_model_pending_answers() == 0 && !label("Sending…"));
            CHECK(receipt_ink_pixels(expected) > 20);
            CHECK(result == 2 ? !label("Sent") && !label("Not sent") : !label("Delivered"));
        }
        /* A retry returns to review; only a fresh explicit decision queues an answer. */
        fresh(&m);
        orb_shell_select_card(s_shell, "approval-build"); settle_page(); tap("Allow");
        orb_answer_t failed_answer; CHECK(orb_model_peek_answer(&failed_answer));
        CHECK(orb_model_complete_answer(failed_answer.card, NULL, false, "unavailable"));
        settle_page();
        CHECK(label("Not sent") && find_control(orb_shell_root(s_shell), "Try again"));
        CHECK(orb_model_pending_answers() == 0);
        tap("Try again");
        CHECK(orb_shell_surface(s_shell) == ORB_SHELL_SESSION);
        CHECK(orb_model_pending_answers() == 0);
        fresh(&m);
        CHECK(orb_model_cancel_card("choice-next"));
        orb_shell_select_session(s_shell, "codex-shell"); settle_page(); tap("Reply");
        CHECK(label("Round shell"));
        orb_voice_state_t v = {.stream=71}; strcpy(v.target, "codex-shell");
        for (unsigned phase = ORB_VOICE_STARTING; phase <= ORB_VOICE_SENDING; phase++) {
            v.phase = phase;
            if (phase >= ORB_VOICE_CONFIRMING) strcpy(v.text, "Keep the recipient visible.");
            CHECK(orb_model_set_voice(&v)); settle_page(); CHECK(label("Round shell"));
        }
        fresh(&m);
        orb_shell_navigate(s_shell, ORB_SHELL_SESSIONS); settle_page();
        lv_obj_t *first = find_label_in_tree(orb_shell_root(s_shell), "Firmware build");
        lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(first));
        CHECK(orb_shell_twist(s_shell, 1)); settle_page();
        lv_obj_t *next = label("Round shell"); CHECK(next);
        lv_area_t bounds; lv_obj_get_coords(next, &bounds); CHECK(bounds.y1 >= 60 && bounds.y2 < 420);
        for (unsigned i = 0; i < 10; i++) { orb_shell_twist(s_shell, 1); settle_page(); }
        int end = lv_obj_get_scroll_y(list); orb_shell_twist(s_shell, 1); settle_page();
        CHECK(lv_obj_get_scroll_y(list) == end); CHECK(end > 0);
        fresh(&m);
        orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); settle_page();
        lv_obj_t *title = label("Notifications"); CHECK(title);
        orb_kit_rim_text_info_t title_info;
        CHECK(orb_kit_rim_text_info(title, &title_info));
        CHECK(orb_kit_rim_text_measure("NOTIFICATIONS", ORB_TEXT_FOOTNOTE1, ORB_KIT_TITLE_RADIUS) <= ORB_KIT_TITLE_MAX_DEGREES);
        reveal_row("Clear all"); tap("Clear all"); CHECK(label("No notifications"));
        CHECK(find_control(orb_shell_root(s_shell), "Undo clear")); tap("Undo clear");
        CHECK(!label("No notifications"));
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "Build"));
        reveal_row("Clear all"); tap("Clear all");
        orb_card_t event = {.id="fresh-timed-event", .session="codex-shell", .kind=ORB_CARD_ATTENTION,
            .title="Tests finished", .body="All checks passed."};
        CHECK(orb_model_upsert_card(&event)); settle_page();
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "now"));
        uint64_t revision = orb_model_revision();
        s_now += 61000; settle_page();
        CHECK(orb_model_revision() == revision);
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "1 min ago"));
        reveal_row("Undo clear"); tap("Undo clear");
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "Build"));
        orb_shell_navigate(s_shell, ORB_SHELL_NOTIFICATIONS); settle_page();
        CHECK(find_text_in_tree(orb_shell_root(s_shell), "Tests"));
    }
    CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_DEFAULT) == ORB_FONTS_OK);
    CHECK(orb_fonts_set_bold_text(false) == ORB_FONTS_OK);
    orb_ui_set_reduce_transparency(false);
}

static void offline_shot(const char *directory, const char *name)
{
    if (!directory) return;
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s.png", directory, name) < sizeof(path));
    CHECK(sim_display_save_png(path));
}

static void offline_navigation(const char *directory)
{
    orb_shell_snapshot_t m = connected();
    m.link = ORB_SHELL_LINK_OFFLINE;
    fresh(&m);
    orb_home_load_demo();
    orb_home_set_connection(false, false);
    orb_model_set_connection(false, false);
    settle_page();
    CHECK(label("10:09") && label("—"));
    offline_shot(directory, "face");
    /* Connection diagnostics and maintenance must be escapable without a hub. */
    /* The removed Face status capsule has a documented Settings > Mac route. */
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); settle_page();
    reveal_row("Mac"); tap("Mac");
    /* Wi-Fi is up and the Mac link is down: a state, not a generic Off (polish review #5). */
    CHECK(row_has_value(orb_shell_root(s_shell), "Mac", "Offline"));
    offline_shot(directory, "connection");
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    boot_back();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    offline_shot(directory, "apps");
    orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); settle_page();
    CHECK(label("Settings"));
    offline_shot(directory, "settings");
    reveal_row("Mac"); tap("Mac"); tap("Maintenance");
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle_page();
    CHECK(label("10:09") && label("—"));
    orb_shell_navigate(s_shell, ORB_SHELL_CONTROL_CENTER); settle_page();
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Control Center"));
    offline_shot(directory, "controls");
    int brightness = orb_shell_brightness(s_shell);
    for (unsigned i = 0; i < 10; i++) CHECK(orb_shell_twist(s_shell, 1));
    CHECK(orb_shell_brightness(s_shell) == brightness + 10);
    CHECK(s_actions.count == 0);
    lv_refr_now(s_display);
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_BRIGHTNESS);
    tap(ORB_ICON_BELL); CHECK(label(ORB_ICON_BELL_SLASH));
    offline_shot(directory, "controls-silent");
    /* Motion has a named setting in Accessibility; the new system grid's
     * moon is Quiet, with independent privacy/power semantics. */
    orb_shell_navigate(s_shell, ORB_SHELL_ACCESSIBILITY); settle_page();
    bool reduced = orb_ui_reduce_motion();
    reveal_row("Reduce motion"); tap("Reduce motion");
    CHECK(orb_ui_reduce_motion() != reduced);
    offline_shot(directory, "controls-motion");
    tap("Reduce motion");
    orb_shell_navigate(s_shell, ORB_SHELL_VOICE); settle_page();
    CHECK(label("Voice needs your Mac"));
    CHECK(!find_control(orb_shell_root(s_shell), ORB_ICON_MIC));
    orb_voice_request_t request;
    CHECK(!orb_model_take_voice_request(&request));
    offline_shot(directory, "voice");
    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS); settle_page();
    /* Stale precedes truncatable activity so a long reason cannot hide it. */
    lv_obj_t *stale = find_text_in_tree(orb_shell_root(s_shell), ORB_KIT_OFFLINE_CAPTION); /* polish #12 */
    CHECK(stale && lv_obj_is_visible(stale));
    offline_shot(directory, "agents-last-known");
    orb_shell_navigate(s_shell, ORB_SHELL_DOT); settle_page();
    CHECK(find_text_in_tree(orb_shell_root(s_shell), "Mac offline"));
    offline_shot(directory, "dot-last-known");
    orb_shell_select_session(s_shell, "codex-shell"); settle_page();
    CHECK(find_text_in_tree(orb_shell_root(s_shell), ORB_KIT_OFFLINE_CAPTION));
    offline_shot(directory, "session-last-known");
    orb_shell_select_card(s_shell, "choice-next"); settle_page();
    /* Disabled choices still need a real visible contact: AX3 prose places
     * this row below the outer viewport until the reader reveals it. */
    reveal_row("Agents");
    lv_obj_t *answer = find_control(orb_shell_root(s_shell), "Agents");
    CHECK(answer && lv_obj_has_state(answer, LV_STATE_DISABLED));
    tap("Agents"); CHECK(orb_model_pending_answers() == 0);
    offline_shot(directory, "card-disabled");
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); settle_page();
    CHECK(label("Offline"));
    CHECK(label("Connect your Mac to load Home."));
    CHECK(!label("Living Room") && !label("Floor Lamp"));
    CHECK(!find_control(orb_shell_root(s_shell), "Good Night"));
    CHECK(!find_control(orb_shell_root(s_shell), "Scenes"));
    offline_shot(directory, "home-last-known");
    orb_home_request_t home_request;
    CHECK(!orb_home_take_request(&home_request));
    orb_shell_navigate(s_shell, ORB_SHELL_UPDATES); settle_page();
    CHECK(label("Connect your Mac to check for updates."));
    CHECK(lv_obj_has_state(find_control(orb_shell_root(s_shell), "Check now"), LV_STATE_DISABLED));
    tap("Check now"); CHECK(s_actions.count == 1);
    offline_shot(directory, "updates-disabled");
    /* No cached sessions is different from a current empty inventory. */
    orb_model_reset(); orb_model_set_connection(false, false); settle_page();
    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS); settle_page();
    CHECK(label("Mac offline") && label("Start a session on your Mac"));
    CHECK(!label("No sessions yet"));
    offline_shot(directory, "agents-empty");
    orb_shell_navigate(s_shell, ORB_SHELL_DOT); settle_page();
    CHECK(label("Mac offline") && label("Connect your Mac to reach Dot"));
    offline_shot(directory, "dot-empty");
    orb_home_reset();
    orb_shell_navigate(s_shell, ORB_SHELL_HOME); settle_page();
    CHECK(label("Offline") && label("Connect your Mac to load Home."));
    CHECK(!label("Choose favourites on your Mac"));
    offline_shot(directory, "home-empty");
    /* Cold boots and all ordinary reconnect/failure states retain local routes.
     * A stale OTA offer/error cannot lock the device into a hub-only screen. */
    const orb_shell_net_t nets[] = {ORB_SHELL_NET_UNPROVISIONED,
        ORB_SHELL_NET_CONNECTING, ORB_SHELL_NET_FAILED, ORB_SHELL_NET_CONNECTED};
    const orb_shell_link_t links[] = {ORB_SHELL_LINK_OFFLINE, ORB_SHELL_LINK_SEARCHING,
        ORB_SHELL_LINK_CONNECTING, ORB_SHELL_LINK_UNAUTHORIZED, ORB_SHELL_LINK_ONLINE};
    for (unsigned n = 0; n < sizeof(nets) / sizeof(*nets); n++) {
        for (unsigned l = 0; l < sizeof(links) / sizeof(*links); l++) {
            if (nets[n] == ORB_SHELL_NET_CONNECTED && links[l] == ORB_SHELL_LINK_ONLINE) continue;
            m.net = nets[n]; m.link = links[l]; m.ota = ORB_SHELL_OTA_OFFERED;
            fresh(&m); orb_model_set_connection(false, false); settle_page();
            CHECK(find_control(orb_shell_root(s_shell), "Apps"));
            boot_back(); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
            orb_shell_navigate(s_shell, ORB_SHELL_PLAY); settle_page(); CHECK(label("Play"));
            m.ota = ORB_SHELL_OTA_FAILED; update(&m); settle_page(); CHECK(label("Play"));
            orb_shell_navigate(s_shell, ORB_SHELL_SETTINGS); settle_page(); CHECK(label("Settings"));
            reveal_row("Mac"); tap("Mac"); boot_back(); CHECK(label("Settings"));
            boot_back(); CHECK(label("Play"));
        }
    }
}

static void synthetic_decisions(const char *shot)
{
    lv_indev_t *physical = s_pointer;
    lv_indev_set_mode(physical, LV_INDEV_MODE_EVENT);
    lv_indev_t *synthetic = lv_indev_create();
    CHECK(synthetic != NULL);
    lv_indev_set_type(synthetic, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(synthetic, s_display);
    lv_indev_set_read_cb(synthetic, read_pointer);
    lv_indev_set_mode(synthetic, LV_INDEV_MODE_EVENT);
    orb_input_mark_synthetic(synthetic);
    CHECK(orb_input_is_synthetic(synthetic));
    CHECK(!orb_input_is_synthetic(physical));
    orb_shell_snapshot_t m = connected();
    const char *options[] = {"Allow", "Deny"};
    for (unsigned stack = 0; stack < 2; stack++) {
        for (unsigned kind = ORB_CARD_APPROVAL; kind <= ORB_CARD_CHOICE; kind++) {
            for (unsigned option = 0; option < 2; option++) {
                fresh(&m);
                /* Stack resolves its first card through page_index rather than
                 * the explicit card route; both reach the same action boundary. */
                orb_model_reset(); orb_model_set_connection(true, true);
                orb_card_t card = {.id="synthetic-decision", .session="codex-shell", .kind=kind,
                    .title="Run the build?", .body="Bash: idf.py build", .options_count=2,
                    .options={{.id="allow", .label="Allow"}, {.id="deny", .label="Deny", .role=ORB_OPTION_CANCEL}}};
                CHECK(orb_model_upsert_card(&card));
                if (stack) orb_shell_navigate(s_shell, ORB_SHELL_STACK);
                else orb_shell_select_card(s_shell, card.id);
                settle_page();
                if (stack) {
                    /* Stack presents a safe preview. Even a synthetic Review
                     * can navigate; the full card still requires a real tap. */
                    const char *preview = kind == ORB_CARD_APPROVAL ? "Review" : "Open";
                    CHECK(label(preview) && !label("Allow") && !label("Deny"));
                    s_pointer = synthetic; tap(preview);
                    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_CARD);
                    s_pointer = physical;
                }
                for (unsigned tap_index = 0; tap_index < 4; tap_index++) orb_shell_double_tap(s_shell);
                orb_answer_t motion_answer;
                CHECK(!orb_model_take_answer(&motion_answer));
                s_pointer = synthetic;
                tap("Allow"); tap(kind == ORB_CARD_APPROVAL ? "Deny" : "Dismiss");
                orb_answer_t answer;
                CHECK(!orb_model_take_answer(&answer));
                CHECK(orb_model_pending_answers() == 0);
                CHECK(label("Allow") && label(kind == ORB_CARD_APPROVAL ? "Deny" : "Dismiss"));
                CHECK(find_label(lv_layer_top(), "Answer on Orb"));
                if (shot && !stack && kind == ORB_CARD_APPROVAL && option == 0) CHECK(sim_display_save_png(shot));
                s_pointer = physical;
                tap(option && kind != ORB_CARD_APPROVAL ? "Dismiss" : options[option]);
                CHECK(orb_model_take_answer(&answer));
                CHECK(!strcmp(answer.card, card.id));
                CHECK(!strcmp(answer.option, option ? "deny" : "allow"));
                CHECK(!orb_model_take_answer(&answer));
            }
        }
    }

    fresh(&m);
    orb_card_t doorbell = {.id="home-doorbell-synthetic", .kind=ORB_CARD_ATTENTION,
        .title="Front Door", .body="Doorbell rang at 10:09", .options_count=1,
        .options={{.id="dismiss", .label="Dismiss", .role=ORB_OPTION_CANCEL}}};
    CHECK(orb_model_upsert_card(&doorbell));
    orb_shell_select_card(s_shell, doorbell.id); settle_page();
    s_pointer = synthetic; tap("Dismiss");
    orb_answer_t answer;
    CHECK(!orb_model_take_answer(&answer));
    s_pointer = physical; tap("Dismiss");
    CHECK(orb_model_take_answer(&answer) && !strcmp(answer.option, "dismiss"));

    fresh(&m);
    orb_shell_select_session(s_shell, "codex-shell"); settle_page();
    orb_voice_state_t voice = {.phase=ORB_VOICE_CONFIRMING, .stream=19, .target="codex-shell", .text="Run the build"};
    CHECK(orb_model_set_voice(&voice));
    orb_shell_navigate(s_shell, ORB_SHELL_VOICE); settle_page();
    for (unsigned i = 0; i < 4; i++) orb_shell_double_tap(s_shell);
    orb_voice_request_t motion_voice;
    CHECK(!orb_model_take_voice_request(&motion_voice));
    s_pointer = synthetic; tap("Send");
    orb_voice_request_t request;
    CHECK(!orb_model_take_voice_request(&request));
    CHECK(label("Run the build"));
    s_pointer = physical; tap("Send");
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CONFIRM);
    CHECK(!orb_model_take_voice_request(&request));

    fresh(&m); s_pointer = synthetic;
    connection(); /* Navigation stays available through the devtools pointer. */
    tap("Forget Wi-Fi"); tap("Forget Wi-Fi");
    CHECK(s_actions.count == 0 && label("Forget Wi-Fi?"));
    s_pointer = physical; tap("Forget Wi-Fi");
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_FORGET_NETWORK);
    fresh(&m); s_pointer = synthetic;
    connection(); tap("Unpair Mac"); tap("Unpair");
    CHECK(s_actions.count == 0 && label("Unpair this Mac?"));
    s_pointer = physical; tap("Unpair");
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_UNPAIR);

    fresh(&m);
    m.ota = ORB_SHELL_OTA_OFFERED;
    strcpy(m.version, "0.5.0"); update(&m); settle_page();
    s_pointer = synthetic; tap("Update");
    CHECK(s_actions.count == 0);
    s_pointer = physical; tap("Update");
    CHECK(s_actions.count == 1 && s_actions.last == ORB_SHELL_ACTION_UPDATE);
    /* Hint expires independently; refusal never changes the card or service. */
    s_now += 1500; settle();
    CHECK(!find_label(lv_layer_top(), "Answer on Orb"));
    lv_indev_delete(synthetic);
}

static bool fake_speech_stop(void *ctx)
{
    unsigned *stops = ctx;
    ++*stops;
    return true;
}

static void motion_safe_actions(void)
{
    orb_shell_snapshot_t m = connected();
    fresh(&m);
    orb_time_init();
    orb_home_load_demo();
    orb_home_set_connection(true, true);
    /* No screen, including Home/control/confirm, supplies its generic primary
     * button to motion. Exercise both cancellation/no-cancellation paths. */
    for (unsigned surface = ORB_SHELL_FACE; surface <= ORB_SHELL_TIME; surface++) {
        orb_shell_navigate(s_shell, (orb_shell_surface_t)surface);
        settle_page();
        (void)orb_shell_double_tap(s_shell);
        unsigned stops = 0;
        orb_shell_set_speech_stop(s_shell, fake_speech_stop, &stops);
        CHECK(orb_shell_double_tap(s_shell) && stops == 1);
        orb_shell_set_speech_stop(s_shell, NULL, NULL);
        orb_answer_t answer;
        orb_home_request_t home;
        orb_voice_request_t voice;
        CHECK(!orb_model_take_answer(&answer));
        CHECK(!orb_home_take_request(&home));
        CHECK(!orb_model_take_voice_request(&voice));
        CHECK(s_actions.count == 0);
    }
    CHECK(orb_time_timer_set(1));
    CHECK(orb_time_timer_start(0));
    orb_time_poll(1000000, 0);
    CHECK(orb_shell_double_tap(s_shell));
    orb_time_snapshot_t time;
    orb_time_snapshot(1000000, &time);
    CHECK(time.timer_state == ORB_TIME_IDLE);
    orb_time_init();
}

/* A successful text lookup does not prove readable output: catch full line
 * boxes crossing the circular bezel or wrapping outside their reserved band. */
static void system_text_fits(const char *value)
{
    lv_obj_t *obj = label(value); CHECK(obj);
    /* Setup and pairing retain complete prose in a bounded reader at AX3.
     * Scroll it into view, then inspect emitted bounds rather than treating
     * intentionally clipped offscreen source lines as bezel overflow. */
    lv_obj_scroll_to_view(obj, LV_ANIM_OFF);
    lv_obj_update_layout(obj);
    lv_point_t size;
    lv_text_get_size(&size, value, lv_obj_get_style_text_font(obj, 0),
                     lv_obj_get_style_text_letter_space(obj, 0),
                     lv_obj_get_style_text_line_space(obj, 0), lv_obj_get_width(obj), LV_TEXT_FLAG_NONE);
    CHECK(size.y <= lv_obj_get_height(obj));
    lv_area_t box; lv_obj_get_coords(obj, &box);
    for (lv_obj_t *parent = lv_obj_get_parent(obj); parent; parent = lv_obj_get_parent(parent)) {
        lv_area_t clip; lv_obj_get_coords(parent, &clip);
        box.x1 = LV_MAX(box.x1, clip.x1); box.x2 = LV_MIN(box.x2, clip.x2);
        box.y1 = LV_MAX(box.y1, clip.y1); box.y2 = LV_MIN(box.y2, clip.y2);
    }
    CHECK(box.x1 <= box.x2 && box.y1 <= box.y2);
    CHECK(box.y1 >= 16 && box.y2 < 450);
    CHECK(lv_area_get_width(&box) <= orb_chord_width(box.y1, box.y2, ORB_RIM_MARGIN_TEXT));
}

/* System states use the retained firmware labels/rim, including after status
 * arrives during local browsing. Screenshots are synthetic and never pair. */
static void system_polish(const char *directory)
{
    orb_shell_snapshot_t m = connected();
    m.net = ORB_SHELL_NET_PROVISIONING;
    m.link = ORB_SHELL_LINK_OFFLINE;
    strcpy(m.pop, "12345678"); strcpy(m.service_name, "Orb-Test");
    fresh(&m); settle_page();
    CHECK(label("Set up Wi-Fi") && label(ORB_ICON_DESKTOP));
    fits_one_line("1234 5678");
    system_text_fits("orb-hub setup");
    system_text_fits(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Enter this code." :
                     "Enter the setup code above.");
    offline_shot(directory, "wifi-setup");
    m.net = ORB_SHELL_NET_FAILED; update(&m); settle_page();
    CHECK(label("Try Wi-Fi again"));
    system_text_fits(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Run setup again." :
                     "Wi-Fi could not connect.\nRun setup again to retry.");
    offline_shot(directory, "wifi-retry");
    m.net = ORB_SHELL_NET_CONNECTING; update(&m); settle_page();
    CHECK(label("Connecting to Wi-Fi")); CHECK(!label("1234 5678"));
    offline_shot(directory, "wifi-connecting");
    m.net = ORB_SHELL_NET_CONNECTED; m.link = ORB_SHELL_LINK_PAIRING;
    strcpy(m.pairing_code, "482915"); strcpy(m.hub_name, "Alex’s Mac mini");
    update(&m); settle_page();
    CHECK(label("Pair with your Mac") && label(ORB_ICON_DESKTOP));
    fits_one_line("482 915");
    offline_shot(directory, "pairing");
    m.pairing_code[0] = '\0'; update(&m); settle_page();
    CHECK(label("Preparing pairing code")); CHECK(!label("482 915"));
    offline_shot(directory, "pairing-preparing");
    m = connected(); m.ota = ORB_SHELL_OTA_DOWNLOADING; m.progress = 64;
    strcpy(m.version, "0.4.1"); fresh(&m); settle_page();
    /* Polish review #22: one stage line that always carries the version. */
    CHECK(label("64%") && label("Downloading 0.4.1") && label(ORB_ICON_DOWNLOAD));
    lv_obj_t *ring = find_arc(orb_shell_root(s_shell)); CHECK(ring);
    CHECK(lv_obj_get_style_arc_width(ring, LV_PART_INDICATOR) == 10);
    CHECK(lv_arc_get_value(ring) * 100 / lv_arc_get_max_value(ring) == 64);
    system_text_fits("Downloading 0.4.1");
    /* Keep the complete restart explanation inside the two-line AX3 budget. */
    system_text_fits("Keep using Orb.\nRestarts when ready.");
    offline_shot(directory, "downloading");
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!label("64%"));
    m.progress = 72; update(&m); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    CHECK(!label("72%"));
    orb_shell_navigate(s_shell, ORB_SHELL_UPDATES); settle_page();
    CHECK(label("72%") && label("Downloading 0.4.1"));
    m.ota = ORB_SHELL_OTA_VERIFYING; update(&m); settle_page();
    CHECK(label("100%") && label("Verifying 0.4.1") && label(ORB_ICON_SHIELD_CHECK));
    CHECK(!label("Installing 0.4.1"));
    system_text_fits("Verifying 0.4.1");
    offline_shot(directory, "verifying");
    boot_back();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    m.ota = ORB_SHELL_OTA_READY; update(&m); settle_page();
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_shell_navigate(s_shell, ORB_SHELL_UPDATES); settle_page();
    /* ORB_OTA_READY restarts 1.5 s later: no "keep using", no second ready line. */
    /* Draw on replaces the native glyph's ink while retaining its semantic
     * target; the shared drawn check is verified by the Alive effect tests. */
    CHECK(label("Installing 0.4.1") && find_label_in_tree(orb_shell_root(s_shell), ORB_ICON_CHECK));
    CHECK(!label("Keep using Orb.\nRestarts when ready."));
    system_text_fits("Installing 0.4.1");
    system_text_fits("Orb restarts now to finish.");
    offline_shot(directory, "ready");
    m.ota = ORB_SHELL_OTA_FAILED; strcpy(m.error, "Signature could not be verified.");
    update(&m); settle_page();
    CHECK(label("Update interrupted") && label(m.error));
    CHECK(label("Current software is safe.") && label("Try again"));
    CHECK(label(ORB_ICON_WARNING));
    offline_shot(directory, "failed");
    boot_back(); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    update(&m); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_FACE);
    orb_shell_navigate(s_shell, ORB_SHELL_UPDATES); settle_page();
    CHECK(label("Update interrupted"));
    CHECK(s_actions.count == 0);
}

int main(int argc, char **argv)
{
    s_trace = getenv("ORB_TEST_TRACE") != NULL;
    s_case = "lv_init"; lv_init();
    lv_tick_set_cb(tick);
    s_display = sim_display_create();
    CHECK(s_display != NULL);
    orb_ui_init(NULL);
    CHECK(orb_model_init());
    lv_display_add_event_cb(s_display, invalidated, LV_EVENT_INVALIDATE_AREA, NULL);
    s_pointer = lv_indev_create();
    lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display);
    lv_indev_set_read_cb(s_pointer, read_pointer);
    if (argc >= 2 && !strcmp(argv[1], "--synthetic")) {
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "ax3")) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
            else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
            else return 2;
        }
        synthetic_decisions(argc >= 3 ? argv[2] : NULL);
        motion_safe_actions();
        orb_shell_destroy(s_shell); lv_indev_delete(s_pointer);
        printf("orb_shell synthetic input: %u checks passed\n", s_checks);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--offline")) {
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "ax3")) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
            else if (!strcmp(argv[i], "bold")) CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
            else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
            else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
            else return 2;
        }
        offline_navigation(argc >= 3 ? argv[2] : NULL);
        orb_shell_destroy(s_shell); lv_indev_delete(s_pointer);
        printf("orb_shell offline: %u checks passed\n", s_checks);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--shellux")) {
        shellux_contract();
        orb_shell_destroy(s_shell); lv_indev_delete(s_pointer);
        printf("orb_shell ux: %u checks passed\n", s_checks);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--system-polish")) {
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "ax3")) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
            else if (!strcmp(argv[i], "bold")) CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
            else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
            else return 2;
        }
        system_polish(argv[2]);
        orb_shell_destroy(s_shell); lv_indev_delete(s_pointer);
        printf("orb_shell system polish: %u checks passed\n", s_checks);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--system")) {
        provisioning_and_pairing(); honest_connectivity_and_check();
        confirmations(); accessible_confirmations(); priority_and_recovery();
        numeric_hierarchy_and_recovery(); system_polish(NULL);
        orb_shell_destroy(s_shell); lv_indev_delete(s_pointer);
        printf("orb_shell system: %u checks passed\n", s_checks);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--preferences")) {
        silent_restores_with_workspace(); visible_preferences_and_sound(); temperature_preferences();
        orb_shell_destroy(s_shell); lv_indev_delete(s_pointer);
        printf("orb_shell preferences: %u checks passed\n", s_checks);
        return 0;
    }
    s_case = "shellux_contract"; shellux_contract();
    s_case = "provisioning_and_pairing"; provisioning_and_pairing();
    s_case = "honest_connectivity_and_check"; honest_connectivity_and_check();
    s_case = "confirmations"; confirmations();
    s_case = "accessible_confirmations"; accessible_confirmations();
    s_case = "priority_and_recovery"; priority_and_recovery();
    s_case = "numeric_hierarchy_and_recovery"; numeric_hierarchy_and_recovery();
    s_case = "system_polish"; system_polish(NULL);
    s_case = "status_noise"; status_noise();
    s_case = "flat_motion_noise"; flat_motion_noise();
    s_case = "shell_navigation_and_answers"; shell_navigation_and_answers();
    s_case = "four_option_approval_and_notification_history"; four_option_approval_and_notification_history();
    s_case = "answer_resolution_and_long_ids"; answer_resolution_and_long_ids();
    s_case = "reconnect_disables_decisions"; reconnect_disables_decisions();
    s_case = "voice_focus_commands_and_confirmation"; voice_focus_commands_and_confirmation();
    s_case = "message_delivery_is_transport_only"; message_delivery_is_transport_only();
    s_case = "coalesced_voice_delivery_keeps_transcript"; coalesced_voice_delivery_keeps_transcript();
    s_case = "cancellation_receipts_survive_clear_all"; cancellation_receipts_survive_clear_all();
    s_case = "mic_lifetime_and_service_precedence"; mic_lifetime_and_service_precedence();
    s_case = "voice_queue_failure_is_visible"; voice_queue_failure_is_visible();
    s_case = "gestures_and_accessibility"; gestures_and_accessibility();
    s_case = "long_approval_and_round_list"; long_approval_and_round_list();
    s_case = "lazy_screen_lifetimes"; lazy_screen_lifetimes();
    s_case = "ax3_navigation_pixels"; ax3_navigation_pixels();
    s_case = "contrast_and_demo_truthfulness"; contrast_and_demo_truthfulness();
    s_case = "round2_final_geometry"; round2_final_geometry();
    s_case = "round3_layout_geometry"; round3_layout_geometry();
    s_case = "silent_restores_with_workspace"; silent_restores_with_workspace();
    s_case = "visible_preferences_and_sound"; visible_preferences_and_sound();
    s_case = "focus_preferences_and_idle"; focus_preferences_and_idle();
    s_case = "temperature_preferences"; temperature_preferences();
    s_case = "no_speech_retries_in_place"; no_speech_retries_in_place();
    s_case = "touched_toggle_survives_service_interrupt"; touched_toggle_survives_service_interrupt();
    s_case = "leaving_focus_preserves_conversation"; leaving_focus_preserves_conversation();
    orb_shell_destroy(s_shell);
    lv_indev_delete(s_pointer);
    printf("orb_shell: %u public-contract checks passed\n", s_checks);
    return 0;
}
