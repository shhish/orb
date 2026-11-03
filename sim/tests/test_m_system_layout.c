/* Design 2.0 system matrix. Real shell, throwaway models, no service callback.
 * Typography/fit assertions use prepared glyph boxes and chord presentation;
 * the emitted PNGs remain the required human visual-review evidence. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "orb_apps.h"
#include "orb_home.h"
#include "orb_kit.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "sim_display.h"
#include "src/misc/lv_text_private.h"

static uint32_t s_now;
static unsigned s_checks, s_pngs, s_failures;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static const char *s_scene, *s_state, *s_mode, *s_output;
static FILE *s_metrics;

#define CHECK(value) do { ++s_checks; if (!(value)) { \
    fprintf(stderr, "system2 %s/%s/%s at %u: %s:%d: %s\n", s_scene, s_state, s_mode, \
        s_now, __FILE__, __LINE__, #value); exit(EXIT_FAILURE); } } while (0)
#define FIT(value) do { ++s_checks; if (!(value)) { ++s_failures; \
    fprintf(stderr, "system2 layout %s/%s/%s at %u: %s:%d: %s\n", s_scene, s_state, s_mode, \
        s_now, __FILE__, __LINE__, #value); } } while (0)
static uint32_t tick(void) { return s_now; }

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 16) {
        s_now += 16;
        if (s_shell) orb_shell_poll(s_shell);
        lv_timer_handler();
    }
    lv_refr_now(s_display);
}

static lv_obj_t *find_text(lv_obj_t *root, const char *text)
{
    if (lv_obj_is_hidden(root)) return NULL;
    const char *semantic = orb_kit_button_get_text(root);
    if (semantic && !strcmp(semantic, text)) return root;
    if (lv_obj_check_type(root, &lv_label_class) && !strcmp(lv_label_get_text(root), text)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find_text(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}

static void tap_text(const char *text)
{
    lv_obj_t *label = find_text(orb_shell_root(s_shell), text);
    CHECK(label);
    lv_obj_t *target = lv_obj_is_clickable(label) ? label : lv_obj_get_parent(label);
    while (target && !lv_obj_is_clickable(target)) target = lv_obj_get_parent(target);
    CHECK(target);
    lv_obj_scroll_to_view(target, LV_ANIM_OFF);
    advance(32);
    lv_obj_send_event(target, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(target, LV_EVENT_RELEASED, NULL);
    lv_obj_send_event(target, LV_EVENT_CLICKED, NULL);
    advance(1200);
}

static unsigned primary_count(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned total = orb_kit_is_primary(root);
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) total += primary_count(lv_obj_get_child(root, i));
    return total;
}

static unsigned glass_count(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return 0;
    unsigned total = orb_glass_get_amount(root) > 0;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) total += glass_count(lv_obj_get_child(root, i));
    return total;
}

static void glyph_ink(lv_obj_t *label, lv_area_t *out)
{
    lv_obj_get_coords(label, out);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    const char *text = lv_label_get_text(label);
    int32_t top = font->line_height, bottom = 0;
    uint32_t offset = 0;
    while (text[offset]) {
        uint32_t cp = lv_text_encoded_next(text, &offset), peek = offset;
        lv_font_glyph_dsc_t glyph;
        uint32_t next = text[peek] ? lv_text_encoded_next(text, &peek) : 0;
        if (!lv_font_get_glyph_dsc(font, &glyph, cp, next) || !glyph.box_h) continue;
        int32_t y = font->line_height - font->base_line - glyph.box_h - glyph.ofs_y;
        top = LV_MIN(top, y); bottom = LV_MAX(bottom, y + glyph.box_h - 1);
    }
    lv_point_t size;
    lv_text_get_size(&size, text, font, lv_obj_get_style_text_letter_space(label, 0),
        lv_obj_get_style_text_line_space(label, 0), lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
    /* DOTS labels have an explicitly bounded line box. Wrapped prose retains
     * every line; account for all its baselines when checking the viewport. */
    int32_t ink_height = lv_label_get_long_mode(label) == LV_LABEL_LONG_MODE_DOTS ?
        LV_MIN(size.y, lv_obj_get_height(label)) : size.y;
    bottom += LV_MAX(0, ink_height - font->line_height);
    out->x2 = out->x1 + LV_MIN(lv_obj_get_width(label), size.x) - 1;
    out->y2 = out->y1 + bottom;
    out->y1 += top;
}

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static void check_tree(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return;
    orb_kit_rim_text_info_t rim;
    if (orb_kit_rim_text_info(root, &rim)) FIT(rim.angle_degrees <= ORB_KIT_TITLE_MAX_DEGREES + .01f);
    if (lv_obj_get_height(root) >= ORB_KIT_ROW_H && lv_obj_get_width(root) == 418 && lv_obj_get_child_count(root) >= 2) {
        orb_round_list_cache_stats_t cache;
        if (orb_round_list_cache_stats(lv_obj_get_parent(root), &cache)) {
            for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
                lv_obj_t *a = lv_obj_get_child(root, i);
                if (!lv_obj_check_type(a, &lv_label_class)) continue;
                int32_t lines = lv_label_get_max_lines(a);
                /* Settings and Time explicitly wrap titles at every size;
                 * AX3 also enables wrapping for ordinary kit rows. */
                FIT(lines >= 1 && lines <= 2);
                FIT(lv_obj_get_height(a) <= lines * lv_font_get_line_height(lv_obj_get_style_text_font(a, 0)) +
                    (lines - 1) * lv_obj_get_style_text_line_space(a, 0));
                lv_area_t aa; glyph_ink(a, &aa);
                for (unsigned j = i + 1; j < lv_obj_get_child_count(root); j++) {
                    lv_obj_t *b = lv_obj_get_child(root, j);
                    if (!lv_obj_check_type(b, &lv_label_class)) continue;
                    lv_area_t bb; glyph_ink(b, &bb);
                    if (overlaps(&aa, &bb)) fprintf(stderr, "text overlap: %s / %s\n", lv_label_get_text(a), lv_label_get_text(b));
                    FIT(!overlaps(&aa, &bb));
                }
            }
        }
    }
    if (lv_obj_check_type(root, &lv_label_class) && *lv_label_get_text(root)) {
        lv_area_t area; glyph_ink(root, &area);
        bool shown = true;
        for (lv_obj_t *parent = lv_obj_get_parent(root); parent; parent = lv_obj_get_parent(parent)) {
            orb_round_list_cache_stats_t cache;
            if (orb_round_list_cache_stats(lv_obj_get_parent(parent), &cache)) {
                lv_area_t mapped;
                if (!orb_round_list_child_bounds(parent, root, &mapped) || !lv_obj_area_is_visible(parent, &mapped)) shown = false;
                else area = mapped;
                break;
            }
            if (!lv_obj_is_overflow_visible(parent)) {
                lv_area_t clip; lv_obj_get_coords(parent, &clip);
                area.x1 = LV_MAX(area.x1, clip.x1); area.x2 = LV_MIN(area.x2, clip.x2);
                area.y1 = LV_MAX(area.y1, clip.y1); area.y2 = LV_MIN(area.y2, clip.y2);
            }
        }
        if (shown && area.x2 >= area.x1 && area.y2 >= area.y1 && area.y1 >= 0 && area.y2 < 466) {
            int chord = orb_chord_width(area.y1, area.y2, ORB_RIM_MARGIN_TEXT), left = (466 - chord) / 2;
            if (area.x1 < left || area.x2 >= left + chord)
                fprintf(stderr, "chord: %s (%d,%d)-(%d,%d), width%d\n", lv_label_get_text(root), area.x1, area.y1, area.x2, area.y2, chord);
            FIT(area.x1 >= left && area.x2 < left + chord);
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) check_tree(lv_obj_get_child(root, i));
}

static lv_obj_t *scroll_view(lv_obj_t *root)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_is_scrollable(root) && lv_obj_get_scroll_bottom(root) + lv_obj_get_scroll_top(root) > 0 &&
        lv_obj_get_height(root) > 100 && lv_obj_get_width(root) > 300) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *view = scroll_view(lv_obj_get_child(root, i));
        if (view) return view;
    }
    return NULL;
}

static void save(unsigned page)
{
    lv_obj_t *root = orb_shell_root(s_shell);
    FIT(primary_count(root) <= 1);
    FIT(glass_count(root) + glass_count(lv_display_get_layer_top(s_display)) <= 3);
    check_tree(root);
    check_tree(lv_display_get_layer_top(s_display));
    char path[1024];
    CHECK(snprintf(path, sizeof(path), "%s/%s-%s-%s-%u.png", s_output, s_scene, s_state, s_mode, page) < sizeof(path));
    CHECK(sim_display_save_png(path));
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    orb_glass_build_stats_t glass; orb_glass_build_stats(&glass);
    fprintf(s_metrics, "%s,%s,%s,%u,%zu,%zu\n", s_scene, s_state, s_mode, page, memory.total_size, glass.bytes);
    s_pngs++;
}

static void fixture(const char *state)
{
    if (s_shell) { orb_shell_destroy(s_shell); s_shell = NULL; }
    orb_model_reset(); orb_home_reset(); orb_time_timer_reset();
    bool offline = !strcmp(state, "offline"), empty = !strcmp(state, "empty"), long_text = !strcmp(state, "long");
    bool error = !strcmp(state, "error");
    orb_model_set_connection(!offline, !offline);
    orb_home_set_connection(!offline, !offline);
    orb_model_set_speech_voice(long_text ? "Samantha Enhanced English (United States)" : "Ava");
    if (!empty) {
        orb_session_t sessions[] = {
            {.id="system-claude", .agent=ORB_AGENT_CLAUDE, .state=ORB_SESSION_DONE, .title="Firmware build", .summary="Build finished. Ready to review."},
            {.id="system-codex", .agent=ORB_AGENT_CODEX, .state=ORB_SESSION_DONE, .title="Updates"},
        };
        if (long_text) snprintf(sessions[0].title, sizeof(sessions[0].title), "Review accessibility and synchronization across the complete firmware");
        CHECK(orb_model_sessions_chunk(sessions, 2, true, false));
        const char *titles[] = {"Build finished", "Front door unlocked", "Updated to 0.5.2", "Dot replied"};
        for (unsigned i = 0; i < 4; i++) {
            orb_card_t card = {.kind=ORB_CARD_ATTENTION};
            snprintf(card.id, sizeof(card.id), "system-card-%u", i);
            snprintf(card.session, sizeof(card.session), "%s", i == 0 ? "system-claude" : "");
            snprintf(card.title, sizeof(card.title), "%s", long_text ? "Guest Suite Bath ceiling lights need a connection to the Mac" : error ? "Build failed: retry from your Mac" : titles[i]);
            snprintf(card.body, sizeof(card.body), "%s", long_text ? "Review the interrupted synchronization of Guest Suite Bath and Living Room ceiling lights before trying the action again." : "Your latest activity is ready to review.");
            CHECK(orb_model_upsert_card(&card));
        }
    }
    orb_shell_snapshot_t status = {.net=ORB_SHELL_NET_CONNECTED, .link=offline ? ORB_SHELL_LINK_OFFLINE : ORB_SHELL_LINK_ONLINE,
        .brightness=70, .battery_pct=80, .rssi=-54, .usb=true, .desk_charge_limit=true,
        .battery_setting_error=error,
        .battery_detail={.present=!empty, .battery_pct=80, .usb=true, .charging=true,
            .health_valid=!empty, .health_pct=96, .temperature_valid=!empty, .temperature_c=25}};
    snprintf(status.hub_name, sizeof(status.hub_name), "%s", long_text ? "Alex's Mac mini in Guest Suite Bath Studio" : "Alex's Mac mini");
    snprintf(status.ssid, sizeof(status.ssid), "%s", long_text ? "Guest Suite Bath Studio Wi-Fi" : "Demo Home");
    snprintf(status.device_name, sizeof(status.device_name), "Orb");
    snprintf(status.device_id, sizeof(status.device_id), "orb-020000abcdef");
    snprintf(status.firmware_version, sizeof(status.firmware_version), "%s", long_text ? "0.5.2-accessibility-review-build" : "0.5.2");
    snprintf(status.clock_time, sizeof(status.clock_time), "10:09");
    snprintf(status.clock_date, sizeof(status.clock_date), "Wednesday, October 28");
    if (error) snprintf(status.update_check_result, sizeof(status.update_check_result), "Couldn't check. The Mac connection was interrupted. Try again.");
    if (!strncmp(s_scene, "Mac", 3)) {
        if (empty) {
            status.ssid[0] = status.hub_name[0] = 0;
            status.net = ORB_SHELL_NET_UNPROVISIONED;
            status.link = ORB_SHELL_LINK_OFFLINE;
            orb_model_set_connection(false, false);
        } else if (error) status.net = ORB_SHELL_NET_FAILED;
    }
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell);
    orb_shell_update(s_shell, &status); orb_shell_poll(s_shell);
    advance(1200);
}

static void route(const char *scene)
{
    static const struct { const char *name; orb_shell_surface_t surface; } routes[] = {
        {"Apps", ORB_SHELL_APPS}, {"Control", ORB_SHELL_CONTROL_CENTER}, {"ControlVolume", ORB_SHELL_CONTROL_CENTER},
        {"Notifications", ORB_SHELL_NOTIFICATIONS}, {"Stack", ORB_SHELL_STACK}, {"Settings", ORB_SHELL_SETTINGS},
        {"Display", ORB_SHELL_DISPLAY}, {"Accessibility", ORB_SHELL_ACCESSIBILITY}, {"About", ORB_SHELL_ABOUT},
        {"Units", ORB_SHELL_UNITS}, {"Temperature", ORB_SHELL_TEMPERATURE}, {"Sessions", ORB_SHELL_SESSION_SETTINGS},
        {"Sound", ORB_SHELL_SOUND}, {"Volume", ORB_SHELL_SOUND}, {"QuietHours", ORB_SHELL_SOUND},
        {"Starts", ORB_SHELL_SOUND}, {"Ends", ORB_SHELL_SOUND}, {"Palette", ORB_SHELL_SOUND},
        {"Battery", ORB_SHELL_BATTERY}, {"Onboarding", ORB_SHELL_ONBOARDING}, {"Updates", ORB_SHELL_UPDATES},
        {"Mac", ORB_SHELL_SETTINGS}, {"Glass", ORB_SHELL_SETTINGS}, {"Licences", ORB_SHELL_ABOUT},
        {"MacMaintenance", ORB_SHELL_SETTINGS}, {"MacForget", ORB_SHELL_SETTINGS}, {"MacUnpair", ORB_SHELL_SETTINGS},
        {"PowerSheet", ORB_SHELL_SETTINGS}, {"VolumeHUD", ORB_SHELL_SETTINGS},
    };
    bool found = false;
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) if (!strcmp(scene, routes[i].name)) {
        orb_shell_navigate(s_shell, routes[i].surface); found = true; break;
    }
    CHECK(found); advance(1200);
    if (!strcmp(scene, "ControlVolume")) tap_text(ORB_ICON_SPEAKER_LOW " 60%");
    else if (!strncmp(scene, "Mac", 3)) {
        tap_text("Mac");
        if (strcmp(scene, "Mac")) tap_text("Maintenance");
        if (!strcmp(scene, "MacForget")) tap_text("Forget Wi-Fi");
        if (!strcmp(scene, "MacUnpair")) tap_text("Unpair Mac");
    }
    else if (!strcmp(scene, "Glass")) tap_text("Glass");
    else if (!strcmp(scene, "Licences")) tap_text("Licenses");
    else if (!strcmp(scene, "Volume")) tap_text("Volume");
    else if (!strcmp(scene, "Palette")) tap_text("System sounds");
    else if (!strcmp(scene, "QuietHours") || !strcmp(scene, "Starts") || !strcmp(scene, "Ends")) {
        tap_text("Quiet hours");
        if (strcmp(scene, "QuietHours")) tap_text(scene);
    } else if (!strcmp(scene, "PowerSheet")) {
        const orb_button_event_t hold = {.button=ORB_BUTTON_PWR, .gesture=ORB_BUTTON_HOLD};
        orb_shell_button(s_shell, &hold); advance(1200);
    } else if (!strcmp(scene, "VolumeHUD")) {
        orb_shell_show_volume(s_shell); advance(32);
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 3); s_output = argv[1]; s_mode = argv[2]; s_scene = "init"; s_state = "normal";
    CHECK(mkdir(s_output, 0755) == 0 || errno == EEXIST);
    char metrics[1024]; snprintf(metrics, sizeof(metrics), "%s/metrics-%s.csv", s_output, s_mode);
    s_metrics = fopen(metrics, "w"); CHECK(s_metrics);
    fprintf(s_metrics, "scene,state,mode,page,retained_lvgl_payload,shared_glass_payload\n");
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display);
    orb_ui_init(NULL); CHECK(orb_model_init()); CHECK(orb_home_init()); orb_time_init();
    orb_apps_init(ORB_APP_NEEDS_IMU | ORB_APP_NEEDS_AUDIO | ORB_APP_NEEDS_STORAGE);
    orb_sound_set_volume(60);
    if (strncmp(s_mode, "ax3", 3) == 0) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
    if (!strcmp(s_mode, "ax3-bold-rt")) {
        CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK); orb_ui_set_reduce_transparency(true);
    }
    orb_glass_cache_prewarm();
    const char *scenes[] = {"Apps", "Control", "ControlVolume", "Notifications", "Stack", "Settings", "Display",
        "Accessibility", "About", "Units", "Temperature", "Sessions", "Sound", "Volume", "QuietHours", "Starts", "Ends",
        "Palette", "Battery", "Onboarding", "Updates", "Mac", "MacMaintenance", "MacForget", "MacUnpair",
        "Glass", "Licences", "PowerSheet", "VolumeHUD"};
    const char *states[] = {"normal", "long", "empty", "offline", "error"};
    for (unsigned i = 0; i < sizeof(scenes) / sizeof(scenes[0]); i++) {
        s_scene = scenes[i];
        for (unsigned j = 0; j < sizeof(states) / sizeof(states[0]); j++) {
            s_state = states[j]; fixture(s_state); route(s_scene); save(0);
            if (!strcmp(s_scene, "VolumeHUD") || !strcmp(s_scene, "PowerSheet")) continue;
            lv_obj_t *view = scroll_view(orb_shell_root(s_shell));
            if (view) {
                int total = lv_obj_get_scroll_top(view) + lv_obj_get_scroll_bottom(view);
                int step = LV_MAX(72, lv_obj_get_height(view) / 2), page = 1;
                for (int offset = step; offset < total + step; offset += step) {
                    lv_obj_scroll_to_y(view, LV_MIN(offset, total), LV_ANIM_OFF); advance(48); save(page++);
                }
            }
        }
    }
    orb_shell_destroy(s_shell); s_shell = NULL; fclose(s_metrics);
    printf("system2 %s: %u checks, %u inspected-layout PNGs emitted\n", s_mode, s_checks, s_pngs);
    return s_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
