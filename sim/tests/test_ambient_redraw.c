/* Real shell + banded framebuffer: measure flushed pixels, not widget bounds
 * or host FPS. Delivery matches main's 20ms IMU/poll and 1Hz status cadence. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_fonts.h"
#include "orb_model.h"
#include "orb_shell.h"
#include "orb_ui.h"
#include "sim_display.h"

static uint32_t s_now, s_pixels, s_frames, s_max_pixels;
static bool s_measure, s_face_down;
static lv_display_t *s_display;
static orb_shell_t *s_shell;
static orb_shell_snapshot_t s_status;
static uint8_t s_partial[SIM_D * SIM_D * 3], s_full[sizeof(s_partial)];
static uint8_t s_band[SIM_D * 84 * 2] __attribute__((aligned(64)));

static uint32_t tick(void) { return s_now; }
static void panel(lv_event_t *event)
{
    if (!s_measure) return;
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START: s_pixels = 0; break;
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = lv_event_get_param(event);
        s_pixels += lv_area_get_size(area);
        break;
    }
    case LV_EVENT_REFR_READY:
        if (s_pixels) { s_frames++; s_max_pixels = LV_MAX(s_max_pixels, s_pixels); }
        s_pixels = 0;
        break;
    default: break;
    }
}

static void advance(unsigned duration, bool always_on)
{
    for (unsigned elapsed = 0; elapsed < duration; elapsed += 5) {
        s_now += 5;
        if (s_now % 20 == 0) {
            orb_ui_set_gravity(0, 0, 1, .020f);
            orb_shell_environment(s_shell, 0, 0, s_face_down ? -1 : 1, always_on);
            orb_shell_poll(s_shell);
        }
        if (s_now % 1000 == 0) orb_shell_update(s_shell, &s_status);
        lv_timer_handler();
    }
}

static void measure(const char *name, bool always_on)
{
    /* Settle navigation, mask preparation and the initial power transition.
     * Stay in one virtual minute so burn-in movement is a separate event. */
    advance(1500, always_on);
    s_frames = s_pixels = s_max_pixels = 0;
    s_measure = true;
    advance(10000, always_on);
    s_measure = false;
    printf("ambient %-20s frames=%u/10s max_flushed_px=%u (%.3f%%)\n",
           name, s_frames, s_max_pixels, 100.f * s_max_pixels / (SIM_D * SIM_D));
    assert(s_frames == 0);
    assert(s_max_pixels <= SIM_D * SIM_D / 20);
    /* Partial refresh must preserve exactly the pixels of a fresh rendering,
     * including the Face content and the navigation controls. */
    sim_display_rgb(s_partial);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(s_display);
    sim_display_rgb(s_full);
    assert(!memcmp(s_partial, s_full, sizeof(s_full)));
    /* Keep each assertion inside a minute, even across long multi-case runs. */
    s_now = ((s_now / 60000) + 1) * 60000;
}

int main(void)
{
    lv_init(); lv_tick_set_cb(tick);
    s_display = sim_display_create(); assert(s_display);
    lv_display_set_buffers(s_display, s_band, NULL, sizeof(s_band), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(s_display, panel, LV_EVENT_ALL, NULL);
    orb_ui_init(NULL); assert(orb_model_init()); orb_model_reset();
    for (unsigned i = 0; i < 5; i++) {
        orb_session_t session = {.agent = ORB_AGENT_CODEX, .state = ORB_SESSION_BUSY};
        snprintf(session.id, sizeof(session.id), "working-%u", i);
        snprintf(session.title, sizeof(session.title), "Codex session %u", i);
        strcpy(session.reason, "Checking simulator layouts");
        assert(orb_model_upsert_session(&session));
    }
    orb_model_set_connection(true, true);
    s_shell = orb_shell_create(lv_screen_active(), NULL, NULL); assert(s_shell);
    s_status = (orb_shell_snapshot_t){.net = ORB_SHELL_NET_CONNECTED,
        .link = ORB_SHELL_LINK_ONLINE, .battery_pct = 80, .rssi = -54,
        .brightness = 70, .usb = true};
    strcpy(s_status.clock_time, "10:09"); strcpy(s_status.clock_date, "Tuesday, October 27");
    orb_shell_update(s_shell, &s_status);
    measure("Face / five working", false);
    lv_obj_t *saved = lv_screen_active(), *other = lv_obj_create(NULL);
    lv_screen_load(other);
    measure("Other active screen", false);
    lv_screen_load(saved); lv_obj_delete(other);
    measure("Screen restored", false);
    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS);
    measure("Agents / five working", false);
    orb_shell_select_session(s_shell, "working-0");
    measure("Detail / five working", false);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    orb_ui_set_reduce_motion(true);
    measure("Reduce Motion", false);
    orb_ui_set_reduce_motion(false);
    measure("RM off resumes", false);
    measure("Always On", true);
    orb_shell_display_active(s_shell, false);
    measure("Dimmed / off", false);
    orb_shell_display_active(s_shell, true);
    measure("Wake resumes", false);
    s_face_down = true;
    measure("Face down / asleep", false);
    s_face_down = false;
    s_status.link = ORB_SHELL_LINK_OFFLINE;
    orb_model_set_connection(false, false);
    orb_shell_update(s_shell, &s_status);
    measure("Offline cached work", false);
    s_status.link = ORB_SHELL_LINK_ONLINE;
    orb_model_set_connection(true, true);
    orb_shell_update(s_shell, &s_status);
    measure("Reconnected resumes", false);
    s_status.net = ORB_SHELL_NET_PROVISIONING;
    orb_shell_update(s_shell, &s_status);
    /* Preparing the setup code is a real named wait in Alive. Its finite
     * breath is allowed first; the original zero-frame assertion still
     * applies once its absolute 30 s budget has expired. */
    advance(31000, false);
    measure("Hidden / five working", false);
    s_status.net = ORB_SHELL_NET_CONNECTED;
    orb_shell_update(s_shell, &s_status);
    measure("Shown resumes", false);
    for (unsigned i = 0; i < 5; i++) {
        char id[32]; snprintf(id, sizeof(id), "working-%u", i);
        assert(orb_model_remove_session(id));
    }
    measure("Face / no working", false);
    orb_shell_navigate(s_shell, ORB_SHELL_AGENTS);
    measure("Agents / no working", false);
    orb_shell_navigate(s_shell, ORB_SHELL_FACE);
    s_status.net = ORB_SHELL_NET_PROVISIONING;
    orb_shell_update(s_shell, &s_status);
    advance(31000, false);
    measure("Hidden by setup", false);
    /* HIG §7's discrete minute burn-in shift remains available while lit in
     * Always On. It is one real state change, outside the idle redraw budget. */
    s_status.net = ORB_SHELL_NET_CONNECTED;
    orb_shell_update(s_shell, &s_status);
    /* The calm controller owns Always On, not an active Face IMU hint. */
    orb_shell_power_enable(s_shell, true);
    orb_shell_set_quiet(s_shell, true);
    advance(1500, true);
    s_now = ((s_now / 60000) + 1) * 60000 - 500;
    advance(20, true);
    s_frames = s_pixels = s_max_pixels = 0;
    s_measure = true;
    advance(1000, true);
    s_measure = false;
    assert(s_frames == 1);
    puts("ambient Always On minute boundary: exactly one burn-in shift frame");
    orb_shell_destroy(s_shell); orb_glass_cache_clear();
    puts("ambient redraw: cadence, dirty-area, power, off-screen and frozen-pixel checks passed");
    return 0;
}
