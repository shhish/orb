/* Stream F: actual shell pointer input + registry lifecycle, with no hub,
 * board, wall-clock dependency, config writes or persisted utility state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_apps.h"
#include "orb_fonts.h"
#include "orb_home.h"
#include "orb_model.h"
#include "orb_nav_admission.h"
#include "orb_shell.h"
#include "orb_shell_services.h"
#include "orb_sound.h"
#include "orb_time.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now;
static time_t s_wall;
static unsigned s_checks;
static const char *s_directory;
static lv_display_t *s_display;
static lv_indev_t *s_pointer;
static lv_indev_data_t s_input;
static orb_shell_t *s_shell;
static int s_tap_scroll;
static void shot(const char *name);
static orb_time_snapshot_t snapshot(void);

#define CHECK(condition) do { \
    ++s_checks; \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s at %u ms\n", __FILE__, __LINE__, #condition, s_now); \
        exit(1); \
    } \
} while (0)

/* The production background hook reads time(NULL). Start with an unset RTC,
 * then inject a synthetic hub/SNTP sample; never read or mutate live time. */
time_t time(time_t *result)
{
    time_t now = s_wall ? s_wall + (time_t)(s_now / 1000) : 0;
    if (result) *result = now;
    return now;
}
static uint32_t tick(void) { return s_now; }
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev; *data = s_input;
}
static void frame(unsigned ms)
{
    s_now += ms;
    orb_shell_apps_service();
    orb_apps_poll((int64_t)s_now * 1000, false, true, 0, 0, 1, false);
    orb_shell_apps_publish();
    orb_shell_poll(s_shell);
    lv_timer_handler();
    lv_refr_now(s_display);
}
static void advance(unsigned ms)
{
    while (ms) { unsigned step = LV_MIN(ms, 16); frame(step); ms -= step; }
}
static void power_press(void)
{
    orb_button_event_t event = {.button = ORB_BUTTON_PWR, .gesture = ORB_BUTTON_DOWN, .at_ms = s_now};
    orb_shell_button(s_shell, &event); advance(40);
    event.gesture = ORB_BUTTON_UP; event.at_ms = s_now;
    orb_shell_button(s_shell, &event); advance(320);
    event.gesture = ORB_BUTTON_SHORT; event.at_ms = s_now;
    orb_shell_button(s_shell, &event);
}
static lv_obj_t *find(lv_obj_t *root, const char *caption)
{
    /* Design 2.0 glyph controls keep nonpainting semantic names for discovery.
     * The parent still has to be visible, so inactive places cannot match. */
    bool semantic = lv_obj_check_type(root, &lv_label_class) && lv_obj_is_hidden(root) &&
        lv_obj_get_parent(root) && lv_obj_is_visible(lv_obj_get_parent(root));
    if (lv_obj_is_hidden(root) && !semantic) return NULL;
    /* The Design 2.0 Face launcher is an icon-only 56px mini. Resolve its
     * physical target while keeping this flow's readable semantic name. */
    if (!strcmp(caption, "Apps") && lv_obj_is_clickable(root) &&
        lv_obj_get_width(root) == 56 && lv_obj_get_height(root) == 56)
        return lv_obj_get_child(root, 0);
    if (!strcmp(caption, "Time") && lv_obj_get_width(root) == 80 && lv_obj_get_height(root) == 80 && lv_obj_is_clickable(root)) {
        lv_obj_t *glyph = lv_obj_get_child(root, 0);
        if (glyph && lv_obj_check_type(glyph, &lv_label_class) && !strcmp(lv_label_get_text(glyph), ORB_ICON_TIMER)) return glyph;
    }
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *shown = lv_label_get_text(root);
        if (!strcmp(shown, caption)) return root;
        const char *dots = strstr(shown, "...");
        if (!dots) dots = strstr(shown, "\xe2\x80\xa6");
        if (dots && dots > shown && !strncmp(shown, caption, dots - shown)) return root;
        /* Wrapping Time rows end a third line's worth of title in U+2026. */
        const char *ellipsis = strstr(shown, "\xE2\x80\xA6");
        if (ellipsis && ellipsis > shown && !strncmp(shown, caption, ellipsis - shown)) return root;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = find(lv_obj_get_child(root, i), caption);
        if (found) return found;
    }
    return NULL;
}
static void tap(const char *caption, unsigned settle)
{
    s_tap_scroll = -1;
    lv_obj_t *text = find(lv_display_get_layer_top(s_display), caption);
    if (!text) text = find(orb_shell_root(s_shell), caption);
    CHECK(text != NULL);
    lv_obj_t *control = lv_obj_get_parent(text);
    while (control && !lv_obj_is_clickable(control)) control = lv_obj_get_parent(control);
    CHECK(control != NULL);
    for (lv_obj_t *parent = lv_obj_get_parent(control); parent; parent = lv_obj_get_parent(parent)) {
        if (lv_obj_is_scrollable(parent)) {
            lv_obj_scroll_to_view(control, LV_ANIM_OFF);
            advance(700);
            s_tap_scroll = lv_obj_get_scroll_y(parent);
            break;
        }
    }
    lv_area_t bounds; lv_obj_get_coords(control, &bounds);
    s_input.point = (lv_point_t){(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    s_input.state = LV_INDEV_STATE_PRESSED; lv_indev_read(s_pointer); advance(16);
    if (orb_shell_surface(s_shell) == ORB_SHELL_APPS &&
        (!strcmp(caption, "Time") || !strcmp(caption, ORB_ICON_TIMER))) shot("01b-apps-time-focused");
    s_input.state = LV_INDEV_STATE_RELEASED; lv_indev_read(s_pointer); advance(16);
    advance(settle);
}
static void shot(const char *name)
{
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.png", s_directory, name);
    CHECK(sim_display_save_png(path));
}
static void point(int x, int y, bool down)
{
    s_input.point = (lv_point_t){x, y};
    s_input.state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); advance(16);
}
static void crown_quarter(void)
{
    point(449, 233, true);
    point(449, 250, true); /* First tangential sample admits the rotary owner. */
    point(386, 386, true);
}
static void crown_release(void)
{
    /* A stationary sample removes fling velocity, keeping the value check
     * about owner arbitration rather than the separate inertial policy. */
    advance(160); point(386, 386, true); point(386, 386, false); advance(200);
}
static lv_obj_t *time_scroll(lv_obj_t *root, int width, int height)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_is_scrollable(root) && lv_obj_get_width(root) == width &&
        lv_obj_get_height(root) == height) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *found = time_scroll(lv_obj_get_child(root, i), width, height);
        if (found) return found;
    }
    return NULL;
}
static int scroll_position(int width, int height)
{
    lv_obj_t *list = time_scroll(orb_shell_root(s_shell), width, height);
    CHECK(list != NULL);
    return lv_obj_get_scroll_y(list);
}
static void time_page(unsigned page)
{
    CHECK(page < 4);
    /* This is the production segmented target around the kit page dots. */
    int x = 129 + (int)page * 52 + 26;
    point(x, 419, true); point(x, 419, false); advance(400);
}
static void rebuild_time(void)
{
    uint32_t generation = snapshot().generation;
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    /* Utility routes can show Time without app admission. Re-enter from the
     * owner's launcher so the existing registry/Back contracts stay active. */
    orb_shell_navigate(s_shell, ORB_SHELL_APPS); advance(800);
    tap(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Time" : ORB_ICON_TIMER, 800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "time"));
    CHECK(snapshot().generation == generation);
}
static lv_obj_t *timer_complication(lv_obj_t *root)
{
    if (lv_obj_is_hidden(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class) &&
        !strncmp(lv_label_get_text(root), ORB_ICON_TIMER " ", strlen(ORB_ICON_TIMER " "))) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = timer_complication(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static void left_rim_back(void)
{
    s_input.point = (lv_point_t){18, 233};
    s_input.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_pointer); advance(16);
    for (int x = 34; x <= 146; x += 16) {
        s_input.point = (lv_point_t){x, 233};
        lv_indev_read(s_pointer); advance(16);
    }
    s_input.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_pointer); advance(800);
}
static void equivalent_full_refresh(void)
{
    static uint8_t before[SIM_D * SIM_D * 3], after[SIM_D * SIM_D * 3];
    sim_display_rgb(before);
    lv_obj_invalidate(orb_shell_root(s_shell)); lv_refr_now(s_display);
    sim_display_rgb(after);
    unsigned different = 0;
    for (unsigned i = 0; i < sizeof(before); ++i) different += before[i] != after[i];
    if (different) fprintf(stderr, "Time partial/full RGB difference: %u bytes at %u ms\n", different, s_now);
    CHECK(!different);
}
static orb_time_snapshot_t snapshot(void)
{
    orb_time_snapshot_t state; orb_time_snapshot(orb_time_now_us(), &state); return state;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    setenv("TZ", "UTC0", 1); tzset();
    s_directory = argv[1];
    lv_init(); lv_tick_set_cb(tick); s_display = sim_display_create(); CHECK(s_display != NULL);
    orb_ui_init(NULL); CHECK(orb_model_init() && orb_home_init());
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "ax3")) CHECK(orb_fonts_set_content_size(ORB_CONTENT_SIZE_AX3) == ORB_FONTS_OK);
        else if (!strcmp(argv[i], "bold")) CHECK(orb_fonts_set_bold_text(true) == ORB_FONTS_OK);
        else if (!strcmp(argv[i], "rt")) orb_ui_set_reduce_transparency(true);
        else if (!strcmp(argv[i], "rm")) orb_ui_set_reduce_motion(true);
        else return 2;
    }
    orb_time_init(); orb_apps_init(0); CHECK(orb_apps_find("time") != NULL);
    s_pointer = lv_indev_create(); lv_indev_set_type(s_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_pointer, s_display); lv_indev_set_read_cb(s_pointer, read_pointer);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); CHECK(s_shell != NULL);
    orb_shell_snapshot_t status = {.net = ORB_SHELL_NET_CONNECTED, .link = ORB_SHELL_LINK_OFFLINE,
        .battery_pct = 80, .rssi = -54, .usb = true, .brightness = 70,
        .clock_time = "10:09", .clock_date = "Tuesday, October 27", .hub_name = "Mac mini"};
    orb_model_set_connection(false, false); orb_shell_update(s_shell, &status); advance(800);
    tap("Apps", 800); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS); shot("01-apps");
    orb_nav_admission_receipt_t admission = orb_nav_admission_last();
    tap(orb_fonts_content_size() == ORB_CONTENT_SIZE_AX3 ? "Time" : ORB_ICON_TIMER, 0);
    orb_shell_open_metrics_t opened; orb_shell_open_metrics(s_shell, &opened);
    CHECK(opened.surface == ORB_SHELL_TIME && opened.active);
    for (unsigned i = 0; i < 12; i++) {
        advance(50); char name[64]; snprintf(name, sizeof(name), "motion-launch-%02u", i); shot(name);
    }
    advance(300); CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "time"));
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    orb_shell_open_metrics(s_shell, &opened);
    CHECK(opened.content_ready && !opened.active);
    CHECK(opened.cache_entries == 3 && opened.cache_bytes == 3u * 466u * 466u * 2u);
    CHECK(orb_nav_admission_last().sequence == admission.sequence);
    CHECK(find(orb_shell_root(s_shell), "Start") != NULL);
    CHECK(find(orb_shell_root(s_shell), ORB_ICON_CHEVRON_LEFT) == NULL);
    CHECK(find(orb_shell_root(s_shell), ORB_ICON_CHEVRON_RIGHT) == NULL);
    equivalent_full_refresh(); shot("02-time-open");
    sim_display_take_changed(); advance(1000); CHECK(!sim_display_take_changed());
    /* Time's crown passes through orb_nav once. The first arbitration sample
     * anchors the contact; the remaining ~40 degrees changes seven units. */
    int64_t before = snapshot().timer_duration_us;
    crown_quarter();
    int64_t delta = snapshot().timer_duration_us - before;
    CHECK(delta >= 6 * 60000000LL && delta <= 8 * 60000000LL);
    crown_release();
    CHECK(orb_time_timer_set(120)); advance(260);
    tap("00", 16); /* Select the visible seconds field without changing it. */
    before = snapshot().timer_duration_us;
    crown_quarter(); delta = snapshot().timer_duration_us - before;
    CHECK(delta >= 6000000 && delta <= 8000000);
    crown_release(); equivalent_full_refresh(); shot("02b-time-crown-seconds");
    /* S3 must resolve the strong Time API, rather than the shell's weak
     * page-only bridge. Seconds focus and an open editor survive recreation. */
    rebuild_time();
    before = snapshot().timer_duration_us;
    CHECK(orb_shell_twist(s_shell, 1)); advance(300);
    CHECK(snapshot().timer_duration_us - before == 1000000);
    tap("Quick timers", 300);
    CHECK(orb_shell_twist(s_shell, 3)); advance(300);
    int presets_scroll = scroll_position(466, 252);
    CHECK(presets_scroll > 0);
    rebuild_time();
    CHECK(find(orb_shell_root(s_shell), "Close") != NULL);
    CHECK(scroll_position(466, 252) == presets_scroll);
    orb_shell_back(s_shell); advance(400);
    CHECK(find(orb_shell_root(s_shell), "Start") != NULL);
    before = snapshot().timer_duration_us;
    CHECK(orb_shell_twist(s_shell, 1)); advance(300);
    CHECK(snapshot().timer_duration_us - before == 1000000);
    CHECK(orb_time_timer_set(120)); advance(260); tap("Start", 400);
    CHECK(snapshot().timer_state == ORB_TIME_RUNNING); advance(800); equivalent_full_refresh(); shot("03-time-running");
    sim_display_take_changed();
    unsigned redraws = 0;
    for (unsigned i = 0; i < 100; ++i) { frame(10); redraws += sim_display_take_changed(); }
    CHECK(redraws <= 4);
    /* A background agent revision cannot discard Time's retained UI. */
    lv_obj_t *pause = find(orb_shell_root(s_shell), "Pause"); CHECK(pause != NULL);
    orb_session_t session = {.agent = ORB_AGENT_CODEX, .state = ORB_SESSION_IDLE, .id = "time-fake", .title = "Background"};
    CHECK(orb_model_upsert_session(&session)); advance(400);
    CHECK(find(orb_shell_root(s_shell), "Pause") == pause);
    /* Destination hit trees may exist before the reverse iris commits. A
     * cancelled system Back must keep Time admitted and restore its page. */
    point(18, 233, true); point(34, 233, true); point(50, 233, true);
    advance(160); point(50, 233, false); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    CHECK(orb_apps_active() && !strcmp(orb_apps_active()->id, "time"));
    CHECK(find(orb_shell_root(s_shell), "Pause") != NULL);
    CHECK(snapshot().timer_state == ORB_TIME_RUNNING);
    left_rim_back(); CHECK(orb_shell_surface(s_shell) == ORB_SHELL_APPS);
    CHECK(orb_apps_active() == NULL && snapshot().timer_state == ORB_TIME_RUNNING);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(800); shot("04-face-running");
    CHECK(timer_complication(orb_shell_root(s_shell)) == NULL);
    advance(1100); CHECK(timer_complication(orb_shell_root(s_shell)) == NULL);
    equivalent_full_refresh(); shot("04b-face-updated");
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(800);
    CHECK(find(orb_shell_root(s_shell), "Timer running") != NULL); shot("05-stack-running");
    /* The neighboring date card is a real navigation target. Preview taps
     * change Stack pages instead of attempting to scroll an absent list. */
    CHECK(orb_shell_twist(s_shell, 1)); advance(800);
    CHECK(find(orb_shell_root(s_shell), "October 27") != NULL);
    CHECK(find(orb_shell_root(s_shell), "Timer running") == NULL);
    CHECK(orb_shell_twist(s_shell, -1)); advance(800);
    CHECK(find(orb_shell_root(s_shell), "Timer running") != NULL);
    /* A local activity remains a real Stack page alongside hub cards, even
     * offline. Its crown maximum follows the page count. */
    orb_card_t background = {.id = "time-stack-card", .kind = ORB_CARD_ATTENTION,
        .title = "Background card", .body = "A separate Stack page"};
    CHECK(orb_model_upsert_card(&background)); advance(400);
    CHECK(find(orb_shell_root(s_shell), "Background card") != NULL);
    CHECK(find(orb_shell_root(s_shell), ORB_ICON_CHEVRON_RIGHT) == NULL);
    crown_quarter(); crown_release();
    CHECK(find(orb_shell_root(s_shell), "Timer running") != NULL);
    CHECK(orb_model_cancel_card(background.id)); advance(400);
    CHECK(find(orb_shell_root(s_shell), "Timer running") != NULL);
    CHECK(find(orb_shell_root(s_shell), ORB_ICON_CHEVRON_RIGHT) == NULL);
    equivalent_full_refresh(); shot("05b-stack-card-removed");
    frame(120000); advance(800); CHECK(snapshot().timer_state == ORB_TIME_FINISHED);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    CHECK(find(lv_display_get_layer_top(s_display), "Timer finished") != NULL); shot("06-timer-interrupt");
    sim_display_take_changed(); advance(1200); CHECK(!sim_display_take_changed());
    /* The completion interrupts the owner immediately, even over another
     * screen. Dismiss restores the exact Stack underneath it. */
    CHECK(orb_shell_double_tap(s_shell)); advance(300);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE); advance(800); shot("07-face-dismissed");
    CHECK(timer_complication(orb_shell_root(s_shell)) == NULL);
    s_wall = (time_t)1704092340 - (time_t)(s_now / 1000); /* 06:59 UTC. */
    advance(16);
    CHECK(snapshot().clock_valid);
    CHECK(orb_time_alarm_set(7, 0)); CHECK(orb_time_alarm_enable(true, snapshot().clock_now));
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(800);
    orb_shell_power_enable(s_shell, true);
    orb_shell_power_sleep(s_shell); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP);
    frame(61000); advance(800);
    CHECK(snapshot().alarm_ringing);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    /* An automatic alert wakes its own opaque presentation, never grants
     * manual viewing intent to the private task retained beneath it. */
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    CHECK(find(lv_display_get_layer_top(s_display), "Stop") != NULL);
    CHECK(find(lv_display_get_layer_top(s_display), "Snooze") != NULL);
    shot("09-alarm-stack");
    CHECK(find(lv_display_get_layer_top(s_display), "Alarm") != NULL);
    CHECK(find(lv_display_get_layer_top(s_display), ORB_ICON_CHEVRON_LEFT) == NULL);
    equivalent_full_refresh(); shot("10-alarm-open");
    advance(35000);
    CHECK(snapshot().alarm_ringing && orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    /* Design 2.0 Back dismisses presentation only. Polling, twisting and
     * locking cannot silently Stop or Snooze the service-owned alarm. */
    unsigned volume = orb_sound_volume();
    CHECK(!orb_shell_twist(s_shell, 4)); CHECK(orb_sound_volume() == volume);
    power_press(); advance(800);
    CHECK(snapshot().alarm_ringing);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    CHECK(!orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    CHECK(snapshot().alarm_ringing);
    CHECK(find(lv_display_get_layer_top(s_display), "Snooze") != NULL);
    orb_button_event_t alarm_boot = {.button = ORB_BUTTON_BOOT,
        .gesture = ORB_BUTTON_SHORT, .at_ms = s_now};
    orb_shell_button(s_shell, &alarm_boot); advance(1200);
    CHECK(snapshot().alarm_ringing);
    CHECK(find(lv_display_get_layer_top(s_display), "Snooze") == NULL);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    advance(1200);
    CHECK(snapshot().alarm_ringing && orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    /* After Back removes the opaque alert, a whole first PWR contact reveals
     * the retained Stack. Only another physical press may perform its lock. */
    power_press(); advance(800);
    CHECK(snapshot().alarm_ringing);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    power_press(); advance(800);
    CHECK(snapshot().alarm_ringing);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_snapshot(s_shell).redact);
    power_press(); advance(800);
    CHECK(!orb_shell_power_snapshot(s_shell).redact);
    CHECK(snapshot().alarm_ringing);
    CHECK(!orb_shell_double_tap(s_shell)); CHECK(snapshot().alarm_ringing);
    orb_shell_navigate(s_shell, ORB_SHELL_TIME); advance(800);
    tap("Stop", 400);
    CHECK(!snapshot().alarm_ringing);
    orb_shell_navigate(s_shell, ORB_SHELL_STACK); advance(800);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    CHECK(find(lv_display_get_layer_top(s_display), "Snooze") == NULL);
    shot("11-alarm-dismissed");
    orb_shell_power_enable(s_shell, false);
    sim_display_take_changed(); advance(1000); CHECK(!sim_display_take_changed());
    /* Reminder due is a system interruption with an explicit Done action. */
    uint32_t reminder_id = 0;
    orb_shell_power_enable(s_shell, true);
    orb_shell_power_sleep(s_shell); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_SLEEP);
    CHECK(orb_time_reminder_set("Take the second Orb to the office", time(NULL) + 1, &reminder_id));
    advance(1200);
    CHECK(snapshot().reminder_due);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    CHECK(find(lv_display_get_layer_top(s_display), "In 1 hour") != NULL);
    CHECK(find(lv_display_get_layer_top(s_display), "Done") != NULL);
    shot("12-reminder-interrupt");
    tap("Done", 400);
    CHECK(!snapshot().reminder_due);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_STACK);
    /* A completed timer wakes from the automatic Always On state too. */
    CHECK(orb_time_timer_set(40)); CHECK(orb_time_timer_start(orb_time_now_us()));
    advance(31000);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    advance(10000);
    CHECK(snapshot().timer_state == ORB_TIME_FINISHED);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    CHECK(find(lv_display_get_layer_top(s_display), "Timer finished") != NULL);
    shot("13-timer-aod-wake");
    CHECK(orb_shell_double_tap(s_shell)); advance(300);
    /* Orientation and case taps are not explicit Stop/Snooze controls. An
     * alarm keeps its service latch through both face-down entry and a clock
     * boundary that arrives after that orientation sample. */
    struct tm local;
    time_t wall = time(NULL);
    CHECK(localtime_r(&wall, &local) != NULL);
    unsigned next = ((unsigned)local.tm_hour * 60 + (unsigned)local.tm_min + 1) % 1440;
    CHECK(orb_time_alarm_set(next / 60, next % 60));
    CHECK(orb_time_alarm_enable(true, wall));
    advance(61000); CHECK(snapshot().alarm_ringing);
    orb_shell_environment(s_shell, 0, 0, -1, false); advance(16);
    CHECK(snapshot().alarm_ringing);
    tap("Stop", 300); CHECK(!snapshot().alarm_ringing);
    wall = time(NULL); CHECK(localtime_r(&wall, &local) != NULL);
    next = ((unsigned)local.tm_hour * 60 + (unsigned)local.tm_min + 1) % 1440;
    CHECK(orb_time_alarm_set(next / 60, next % 60));
    CHECK(orb_time_alarm_enable(true, wall));
    advance(61000); CHECK(snapshot().alarm_ringing);
    tap("Stop", 300); CHECK(!snapshot().alarm_ringing);
    orb_shell_environment(s_shell, 0, 0, 1, false);
    orb_shell_power_enable(s_shell, false);
    orb_shell_display_active(s_shell, true);
    /* Exercise S3's page, list, detail and power paths through the shell's
     * actual hit tree. Every fixture is a future local reminder. */
    static const char *const reminder_titles[] = {
        "Bring the signed documents to tomorrow's appointment",
        "Pick up the new prescription at the neighborhood pharmacy",
        "Water the basil and rosemary on the kitchen windowsill",
        "Call the electrician about the reading room ceiling light",
        "Pack the second Orb and its cable for the office tomorrow",
        "Review the family travel itinerary before the reservation",
        "Take the library books back after the afternoon meeting",
        "Medication review: bring yesterday's appointment notes",
    };
    for (unsigned i = 0; i < ORB_TIME_REMINDERS_MAX; ++i)
        CHECK(orb_time_reminder_set(reminder_titles[i], time(NULL) + (i + 1) * 3600, NULL));
    orb_shell_navigate(s_shell, ORB_SHELL_TIME); advance(800);
    time_page(3);
    CHECK(orb_shell_twist(s_shell, 4)); advance(300);
    int reminders_scroll = scroll_position(466, 252);
    CHECK(reminders_scroll > 0);
    rebuild_time();
    CHECK(scroll_position(466, 252) == reminders_scroll);
    tap(reminder_titles[ORB_TIME_REMINDERS_MAX - 1], 400);
    /* Revealing the row may adjust the offset before the actual tap. Back
     * restores that final owner-visible position, not the earlier preview. */
    int detail_return_scroll = s_tap_scroll;
    CHECK(detail_return_scroll >= 0);
    CHECK(find(orb_shell_root(s_shell), "In 1 hour") != NULL);
    /* The reminder body (polish review 4) scrolls only when its caption and
     * title outgrow it; at default size three title lines fit at rest. */
    lv_obj_t *body = time_scroll(orb_shell_root(s_shell), 466, 168);
    CHECK(body != NULL);
    bool overflow = lv_obj_get_scroll_bottom(body) > 0;
    if (overflow) CHECK(orb_shell_twist(s_shell, 1));
    advance(300);
    int detail_scroll = scroll_position(466, 168);
    CHECK(detail_scroll > 0 || !overflow);
    rebuild_time();
    CHECK(find(orb_shell_root(s_shell), "In 1 hour") != NULL);
    CHECK(scroll_position(466, 168) == detail_scroll);
    orb_shell_power_enable(s_shell, true);
    orb_shell_power(s_shell); advance(800);
    CHECK(orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    CHECK(find(orb_shell_root(s_shell), "In 1 hour") != NULL);
    CHECK(scroll_position(466, 168) == detail_scroll);
    orb_shell_back(s_shell); advance(400);
    CHECK(orb_shell_surface(s_shell) == ORB_SHELL_TIME);
    CHECK(find(orb_shell_root(s_shell), "In 1 hour") == NULL);
    CHECK(scroll_position(466, 252) == detail_return_scroll);
    time_page(0);
    CHECK(find(orb_shell_root(s_shell), "Start") != NULL);
    orb_shell_power_enable(s_shell, false);
    /* Holding BOOT while Time is focused records for Time. Its transcript
     * always waits for physical Save, even when the agent auto-send timer
     * would normally have elapsed. */
    orb_model_set_connection(true, true); status.link = ORB_SHELL_LINK_ONLINE;
    orb_shell_update(s_shell, &status);
    orb_shell_navigate(s_shell, ORB_SHELL_TIME); advance(800);
    char focus[ORB_MODEL_ID_MAX + 1], card[ORB_MODEL_CARD_ID_MAX + 1];
    orb_model_get_voice_focus(focus, card); CHECK(!strcmp(focus, "time") && !card[0]);
    orb_button_event_t boot = {.button = ORB_BUTTON_BOOT, .gesture = ORB_BUTTON_HOLD, .at_ms = s_now};
    orb_shell_button(s_shell, &boot); advance(400);
    boot.gesture = ORB_BUTTON_UP; orb_shell_button(s_shell, &boot); advance(400);
    orb_voice_request_t request;
    while (orb_model_take_voice_request(&request)) CHECK(request.action != ORB_VOICE_CONFIRM);
    orb_voice_state_t voice = {.phase = ORB_VOICE_CONFIRMING, .stream = 99, .target = "time",
        .text = "Remind me in five minutes to take a break"};
    CHECK(orb_model_set_voice(&voice)); advance(3000);
    shot("14-time-voice-review");
    CHECK(find(orb_shell_root(s_shell), "Save") != NULL);
    CHECK(!orb_model_take_voice_request(&request));
    tap("Save", 400);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CONFIRM);
    voice.phase = ORB_VOICE_IDLE; voice.stream = 0; voice.text[0] = 0;
    CHECK(orb_model_set_voice(&voice));
    orb_message_t message = {.stream = 99, .target = "time",
        .text = "Remind me in five minutes to take a break"};
    orb_model_complete_message_receipt(&message, ORB_SEND_DELIVERED,
        "Added to Time on this Orb.", "");
    advance(300); shot("15-time-voice-saved");
    voice.phase = ORB_VOICE_STARTING; voice.stream = 100;
    CHECK(orb_model_set_voice(&voice)); advance(100);
    voice.phase = ORB_VOICE_CONFIRMING;
    strcpy(voice.text, "Remind me at 6:30 to call home");
    CHECK(orb_model_set_voice(&voice)); advance(300);
    tap("Save", 100);
    CHECK(orb_model_take_voice_request(&request) && request.action == ORB_VOICE_CONFIRM);
    voice.phase = ORB_VOICE_ERROR;
    voice.stream = 0;
    strcpy(voice.error_code, "bad_request");
    strcpy(voice.error, "Include AM or PM in the time.");
    message.stream = 100;
    strcpy(message.text, voice.text);
    orb_model_complete_message_receipt(&message, ORB_SEND_FAILED, voice.error, "bad_request");
    CHECK(orb_model_set_voice(&voice)); advance(500);
    equivalent_full_refresh();
    shot("16-time-voice-error");
    orb_shell_navigate(s_shell, ORB_SHELL_TIME); advance(800);
    orb_shell_surface_t retained = orb_shell_surface(s_shell);
    bool retained_app = orb_apps_active() != NULL;
    orb_shell_power(s_shell); advance(800);
    CHECK(retained == ORB_SHELL_TIME && orb_shell_surface(s_shell) == retained &&
        (orb_apps_active() != NULL) == retained_app &&
        orb_shell_power_snapshot(s_shell).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_shell_power_wake(s_shell, ORB_POWER_WAKE_PWR).accepted); advance(800);
    CHECK(orb_shell_surface(s_shell) == retained && (orb_apps_active() != NULL) == retained_app &&
        orb_shell_power_snapshot(s_shell).state == ORB_POWER_ACTIVE);
    orb_shell_back(s_shell); advance(800);
    CHECK(!snapshot().alarm_ringing);
    orb_shell_destroy(s_shell); orb_apps_exit();
    printf("Time shell: %u checks passed\n", s_checks);
    return 0;
}
