/* All traces here are SYNTHETIC screen-axis g, not captured board evidence. */
#include "orb_power.h"
#include "orb_power_board.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned s_checks;
#define CHECK(c) do { s_checks++; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static unsigned s_cases;
#define CASE(f) do { f(); s_cases++; } while (0)
static int s_brightness, s_brightness_calls, s_panel_calls, s_sleep_calls;
static bool s_panel_ok = true, s_sleep_ok = true;
void orb_board_panel_brightness(int pct) { CHECK(pct >= 0 && pct <= 100); s_brightness = pct; s_brightness_calls++; }
static bool panel(void *unused, bool awake) { (void)unused; (void)awake; s_panel_calls++; return s_panel_ok; }
static bool sleep_hook(void *unused, uint32_t maximum_ms) { (void)unused; CHECK(maximum_ms == 100); s_sleep_calls++; return s_sleep_ok; }
static orb_power_snapshot_t snap(orb_power_controller_t *power) { return orb_power_get_snapshot(power); }
static void tick(orb_power_controller_t *power, uint64_t ms) { CHECK(orb_power_tick(power, ms)); }
static void samples(orb_power_controller_t *power, uint64_t start, uint64_t end, float x, float y, float z)
{
    for (uint64_t ms = start; ms <= end; ms += 50) CHECK(orb_power_imu_sample(power, x, y, z, ms));
}
static void idle_boundaries(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    CHECK(snap(&power).redact && snap(&power).brightness_pct == 70);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 0).consume_input);
    CHECK(!snap(&power).redact);
    tick(&power, 19999); CHECK(snap(&power).state == ORB_POWER_ACTIVE);
    tick(&power, 20000); CHECK(snap(&power).state == ORB_POWER_DIMMED && snap(&power).redact && snap(&power).brightness_pct == 15);
    tick(&power, 29999); CHECK(snap(&power).state == ORB_POWER_DIMMED);
    tick(&power, 30000); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON && snap(&power).brightness_pct == 9);
    CHECK(snap(&power).frame_interval_ms == 16 && snap(&power).moment_duration_ms == 250);
    tick(&power, 30250);
    CHECK(snap(&power).frame_interval_ms == 1000 && snap(&power).shift_x == -1 && snap(&power).shift_y == -1);
    tick(&power, 60000); CHECK(snap(&power).shift_x == 1 && snap(&power).shift_y == -1);
    tick(&power, 119999); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
    tick(&power, 120000); CHECK(snap(&power).state == ORB_POWER_SLEEP && snap(&power).panel_on && snap(&power).moment == ORB_POWER_MOMENT_SLEEP);
    tick(&power, 120249); CHECK(snap(&power).panel_on);
    tick(&power, 120250); CHECK(!snap(&power).panel_on && snap(&power).brightness_pct == 0);
    /* USB leaves time for every burn-in corner before its 30-minute deadline. */
    orb_power_init(&power, 0);
    CHECK(orb_power_set_battery(&power, 80, true, false, 0));
    tick(&power, 120000); CHECK(snap(&power).shift_x == 1 && snap(&power).shift_y == 1);
    tick(&power, 180000); CHECK(snap(&power).shift_x == -1 && snap(&power).shift_y == 1);
    tick(&power, 240000); CHECK(snap(&power).shift_x == -1 && snap(&power).shift_y == -1);
    orb_power_init(&power, 90000);
    tick(&power, 120000); CHECK(snap(&power).shift_x == 1 && snap(&power).shift_y == 1);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 120001).accepted);
    tick(&power, 150001); CHECK(snap(&power).shift_x == 1 && snap(&power).shift_y == 1);
}
static void delayed_and_stale(void)
{
    orb_power_controller_t power, retained;
    orb_power_init(&power, 0);
    (void)orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 0);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 500000).consume_input);
    CHECK(snap(&power).state == ORB_POWER_ACTIVE && !snap(&power).redact);
    retained = power;
    CHECK(!orb_power_tick(&power, 499999));
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_PWR, 499999).accepted);
    CHECK(!orb_power_set_battery(&power, 5, false, false, 499999));
    CHECK(!orb_power_imu_sample(&power, 0, 0, 1, 499999));
    CHECK(!memcmp(&power, &retained, sizeof(power)));
    orb_power_init(&power, 0);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 500000).accepted);
    CHECK(snap(&power).redact);
    orb_power_init(&power, 0);
    tick(&power, 500000); CHECK(snap(&power).state == ORB_POWER_SLEEP && !snap(&power).panel_on);
    orb_power_init(&power, UINT64_MAX - 10);
    CHECK(snap(&power).next_deadline_ms == UINT64_MAX);
    tick(&power, UINT64_MAX);
}
static void wakes_and_bursts(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    tick(&power, 30000);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 30000).accepted);
    CHECK(snap(&power).redact && snap(&power).moment == ORB_POWER_MOMENT_WAKE);
    for (uint64_t ms = 30100; ms < 50000; ms += 100) CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, ms).accepted);
    tick(&power, 50000); CHECK(snap(&power).state == ORB_POWER_DIMMED);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 50000).accepted);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 60000).accepted);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_PWR, 60001).consume_input);
    CHECK(!snap(&power).redact);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_PWR, 60002).consume_input);
    tick(&power, 80002);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 80003).consume_input);
    CHECK(snap(&power).moment == ORB_POWER_MOMENT_WAKE);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 80004).consume_input);
    CHECK(snap(&power).moment_elapsed_ms == 1);
}
static void preferences_and_motion(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    orb_power_preferences_t preferences = orb_power_default_preferences();
    preferences.reduce_motion = true;
    CHECK(orb_power_set_preferences(&power, &preferences, 0));
    tick(&power, 120000); CHECK(snap(&power).moment_duration_ms == 150);
    tick(&power, 120149); CHECK(snap(&power).panel_on);
    tick(&power, 120150); CHECK(!snap(&power).panel_on);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 300200).accepted);
    preferences.reduce_motion = false;
    CHECK(orb_power_set_preferences(&power, &preferences, 300201));
    CHECK(snap(&power).moment == ORB_POWER_MOMENT_WAKE && snap(&power).moment_elapsed_ms == 0);
    preferences.always_on = false; preferences.brightness_pct = 0;
    CHECK(orb_power_set_preferences(&power, &preferences, 300202));
    CHECK(snap(&power).brightness_pct == 0);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 300203).consume_input);
    CHECK(snap(&power).brightness_pct == 0);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 300204).consume_input);
    CHECK(snap(&power).brightness_pct == 0);
    tick(&power, 330204); CHECK(snap(&power).state == ORB_POWER_SLEEP);
    orb_power_init(&power, 0);
    CHECK(orb_power_set_quiet(&power, true, 0));
    CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON && snap(&power).silent && snap(&power).redact);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 1).accepted);
    CHECK(orb_power_set_quiet(&power, false, 2));
    CHECK(!snap(&power).redact && !snap(&power).silent);
}
static void quiet_deadlines(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 0).accepted);
    CHECK(orb_power_set_quiet(&power, true, 5000));
    const uint64_t boundaries[] = {19999, 20000, 20001, 29999, 30000, 60000, 119999};
    for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        tick(&power, boundaries[i]);
        CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
        CHECK(snap(&power).brightness_pct == 9 && snap(&power).redact && snap(&power).silent);
    }
    tick(&power, 120000); CHECK(snap(&power).state == ORB_POWER_SLEEP);
    tick(&power, 120250); CHECK(!snap(&power).panel_on);
    orb_power_init(&power, 0);
    CHECK(orb_power_set_battery(&power, 80, true, false, 0));
    CHECK(orb_power_set_quiet(&power, true, 1));
    tick(&power, 20000); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
    tick(&power, 1799999); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
    tick(&power, 1800000); CHECK(snap(&power).state == ORB_POWER_SLEEP);
}

static void always_on_brightness(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    orb_power_preferences_t prefs = orb_power_default_preferences();
    CHECK(prefs.always_on_brightness_pct == 9);
    prefs.always_on_brightness_pct = 255;
    CHECK(orb_power_set_preferences(&power, &prefs, 0));
    tick(&power, 30250); CHECK(snap(&power).brightness_pct == 15);
    CHECK(power.preferences.always_on_brightness_pct == 15);
    prefs.always_on_brightness_pct = 0;
    CHECK(orb_power_set_preferences(&power, &prefs, 30250));
    CHECK(snap(&power).brightness_pct == 1);
    prefs.always_on_brightness_pct = 9; prefs.brightness_pct = 3;
    CHECK(orb_power_set_preferences(&power, &prefs, 30250));
    CHECK(snap(&power).brightness_pct == 3);
    prefs.brightness_pct = 0;
    CHECK(orb_power_set_preferences(&power, &prefs, 30250));
    CHECK(snap(&power).brightness_pct == 0);
}
static void battery_and_charge(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    CHECK(orb_power_set_battery(&power, 20, false, false, 0));
    CHECK(snap(&power).battery_mode == ORB_POWER_BATTERY_LOW && snap(&power).brightness_pct == 30);
    tick(&power, 10000); CHECK(snap(&power).state == ORB_POWER_DIMMED);
    tick(&power, 15000); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
    CHECK(orb_power_set_battery(&power, 22, false, false, 15000)); CHECK(snap(&power).low_power);
    CHECK(orb_power_set_battery(&power, 23, false, false, 15000)); CHECK(!snap(&power).low_power);
    CHECK(orb_power_set_battery(&power, 10, false, false, 15000)); CHECK(snap(&power).state == ORB_POWER_SLEEP);
    CHECK(orb_power_set_battery(&power, 12, false, false, 15000)); CHECK(snap(&power).battery_mode == ORB_POWER_BATTERY_VERY_LOW);
    CHECK(orb_power_set_battery(&power, 13, false, false, 15000)); CHECK(snap(&power).battery_mode == ORB_POWER_BATTERY_LOW);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 16000).accepted);
    CHECK(orb_power_set_battery(&power, 5, false, false, 16001));
    CHECK(snap(&power).state == ORB_POWER_SLEEP && !snap(&power).panel_on && snap(&power).moment == ORB_POWER_MOMENT_NONE);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 16002).accepted);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_PICKUP, 16002).accepted);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_PWR, 16003).consume_input);
    CHECK(snap(&power).brightness_pct == 15 && snap(&power).moment == ORB_POWER_MOMENT_NONE);
    tick(&power, 26002); CHECK(snap(&power).state == ORB_POWER_ACTIVE);
    tick(&power, 26003); CHECK(!snap(&power).panel_on);
    CHECK(orb_power_set_battery(&power, 7, false, false, 26003)); CHECK(snap(&power).battery_mode == ORB_POWER_BATTERY_CRITICAL);
    CHECK(orb_power_set_battery(&power, 8, false, false, 26003)); CHECK(snap(&power).battery_mode == ORB_POWER_BATTERY_VERY_LOW);
    CHECK(orb_power_set_battery(&power, -1, false, false, 26003)); CHECK(!snap(&power).low_power && snap(&power).battery_pct == -1);
    CHECK(orb_power_set_battery(&power, 1, true, false, 26003)); CHECK(!snap(&power).low_power && !snap(&power).charging && snap(&power).usb);
    CHECK(snap(&power).state == ORB_POWER_SLEEP);
    CHECK(!orb_power_set_battery(&power, 101, false, false, 26003));
    orb_power_init(&power, 0);
    CHECK(orb_power_set_battery(&power, 80, true, true, 0));
    tick(&power, 1799999); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON && snap(&power).charging);
    tick(&power, 1800000); CHECK(snap(&power).state == ORB_POWER_SLEEP);
}
static void imu_face_down(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    samples(&power, 0, 250, 0, 0, -1); CHECK(!snap(&power).face_down);
    samples(&power, 300, 300, 0, 0, -1); CHECK(snap(&power).face_down && snap(&power).silent && !snap(&power).panel_on);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 301).accepted);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_PWR, 302).accepted);
    CHECK(!orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 303).accepted);
    CHECK(!orb_power_imu_sample(&power, NAN, 0, 1, 350)); CHECK(snap(&power).face_down);
    samples(&power, 400, 650, 0, 0, 1); CHECK(snap(&power).face_down);
    samples(&power, 700, 700, 0, 0, 1); CHECK(!snap(&power).face_down && !snap(&power).silent && snap(&power).state == ORB_POWER_SLEEP);
    orb_power_preferences_t prefs = orb_power_default_preferences(); prefs.motion_wake = false;
    CHECK(orb_power_set_preferences(&power, &prefs, 700));
    samples(&power, 750, 2000, 0, 0, -1); CHECK(snap(&power).face_down);
    samples(&power, 2050, 3000, 0.714143f, 0, -0.7f); CHECK(snap(&power).face_down);
    samples(&power, 3050, 4200, 0.866025f, 0, -0.5f); CHECK(!snap(&power).face_down);
}
static void imu_pickup_and_rearm(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    tick(&power, 30000);
    samples(&power, 30000, 31000, 0, 0, 1);
    CHECK(power.motion.armed);
    samples(&power, 31050, 32000, 0.707107f, 0, 0.707107f);
    CHECK(snap(&power).state == ORB_POWER_ACTIVE && snap(&power).redact && power.motion.have_pickup);
    uint64_t activity = power.activity_ms;
    samples(&power, 32050, 35000, 0.707107f, 0, 0.707107f);
    CHECK(power.activity_ms == activity && power.motion.armed);
    samples(&power, 35050, 37000, 0, 0, 1); CHECK(power.motion.armed);
    samples(&power, 37050, 38000, 0.707107f, 0, 0.707107f);
    CHECK(power.activity_ms == activity && !power.motion.armed);
    /* Angled stand baseline: any stable orientation may arm, then deliberate lift. */
    samples(&power, 38050, 45000, 0.5f, 0, 0.866025f);
    CHECK(power.motion.armed);
    tick(&power, 52000);
    CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
    /* A glance returns to Always On without renewing deliberate activity. The long gap disarms. A new 800ms stable baseline is mandatory. */
    CHECK(!orb_power_imu_sample(&power, 0.5f, 0, 0.866025f, 52050));
    samples(&power, 52100, 53000, 0.5f, 0, 0.866025f);
    samples(&power, 53050, 54000, 0.939693f, 0, 0.342020f);
    CHECK(snap(&power).state == ORB_POWER_ACTIVE && power.activity_ms == activity);
}
static void imu_reject_noise(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0); tick(&power, 30000);
    for (uint64_t ms = 30000; ms <= 50000; ms += 50) {
        float noise = ((ms / 50) % 2) ? 0.02f : -0.02f;
        CHECK(orb_power_imu_sample(&power, noise, 0, 1, ms));
    }
    CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON && !power.motion.have_pickup);
    CHECK(!orb_power_imu_sample(&power, INFINITY, 0, 0, 50050)); CHECK(!power.motion.armed);
    CHECK(!orb_power_imu_sample(&power, 0, 0, 0, 50100));
    CHECK(!orb_power_imu_sample(&power, 0, 0, 2.01f, 50150));
    samples(&power, 50200, 51000, 0, 0, 1); CHECK(power.motion.armed);
    CHECK(!orb_power_imu_sample(&power, 0.707107f, 0, 0.707107f, 52000)); CHECK(!power.motion.armed);
    for (uint64_t ms = 52050; ms < 55000; ms += 50) {
        float angle = (float)(ms % 1000) * 0.0062831853f;
        CHECK(orb_power_imu_sample(&power, sinf(angle), 0, cosf(angle), ms));
    }
    CHECK(!power.motion.have_pickup);
}
static void inhibitors(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    CHECK(orb_power_set_inhibitors(&power, ORB_POWER_INHIBIT_VOICE | ORB_POWER_INHIBIT_OTA, 0));
    tick(&power, 500000); CHECK(snap(&power).state == ORB_POWER_ACTIVE);
    CHECK(orb_power_set_inhibitors(&power, 0, 500001));
    tick(&power, 520000); CHECK(snap(&power).state == ORB_POWER_ACTIVE);
    tick(&power, 520001); CHECK(snap(&power).state == ORB_POWER_DIMMED);
    CHECK(orb_power_set_inhibitors(&power, ORB_POWER_INHIBIT_DMA, 520002));
    samples(&power, 520050, 520350, 0, 0, -1);
    CHECK(!snap(&power).panel_on && snap(&power).silent && power.inhibitors == ORB_POWER_INHIBIT_DMA);
    orb_power_init(&power, 0);
    CHECK(orb_power_set_inhibitors(&power, ORB_POWER_INHIBIT_VOICE, 0));
    CHECK(orb_power_set_battery(&power, 5, false, false, 0)); CHECK(!snap(&power).panel_on);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 0).accepted);
    tick(&power, 10000); CHECK(!snap(&power).panel_on);
}
static void dma_does_not_renew_idle(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0);
    for (uint64_t ms = 16; ms < 30000; ms += 16) {
        CHECK(orb_power_set_inhibitors(&power, ORB_POWER_INHIBIT_DMA, ms));
        CHECK(orb_power_set_inhibitors(&power, 0, ms));
    }
    CHECK(power.activity_ms == 0);
    CHECK(snap(&power).state == ORB_POWER_DIMMED);
    tick(&power, 30000); CHECK(snap(&power).state == ORB_POWER_ALWAYS_ON);
}
static void frame_cadence(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 0); tick(&power, 30000);
    CHECK(snap(&power).moment == ORB_POWER_MOMENT_REST);
    uint32_t entry_revision = power.revision;
    tick(&power, 30040); CHECK(orb_power_frame_due(&power, 30000, entry_revision));
    tick(&power, 30250); CHECK(orb_power_frame_due(&power, 30240, entry_revision));
    uint32_t revision = power.revision;
    CHECK(orb_power_frame_due(&power, 30000, 0));
    tick(&power, 30999); CHECK(!orb_power_frame_due(&power, 30250, revision));
    CHECK(orb_power_set_battery(&power, 70, false, false, 30999));
    CHECK(!orb_power_frame_due(&power, 30250, revision));
    tick(&power, 31250); CHECK(orb_power_frame_due(&power, 30250, revision));
    CHECK(!orb_power_frame_due(&power, 32000, revision));
}
static void board_adapter(void)
{
    orb_power_controller_t power;
    orb_power_board_adapter_t adapter;
    orb_power_port_t port = { .panel_awake = panel, .light_sleep = sleep_hook, .wake_ready = 31 };
    orb_power_init(&power, 0); orb_power_board_adapter_init(&adapter);
    s_brightness_calls = s_panel_calls = s_sleep_calls = 0; s_panel_ok = s_sleep_ok = true;
    CHECK(orb_power_board_apply(&power, &adapter, &port, 0) == ORB_POWER_WAIT_SAFE_FRAME && s_brightness == 0);
    CHECK(s_panel_calls == 0);
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED && s_brightness == 70);
    /* A brightness drag changes only the level of the safe frame on screen:
     * every step applies directly and the panel never goes dark between. */
    orb_power_preferences_t level = orb_power_default_preferences();
    for (int pct = 60; pct >= 20; pct -= 20) {
        level.brightness_pct = (uint8_t)pct;
        CHECK(orb_power_set_preferences(&power, &level, 0));
        int calls = s_brightness_calls;
        orb_power_board_apply(&power, &adapter, &port, 0);
        CHECK(s_brightness == pct && s_brightness_calls == calls + 1);
    }
    /* main applies snapshots without a panel port (panel_on untracked). */
    {
        orb_power_controller_t other;
        orb_power_init(&other, 0);
        orb_power_board_adapter_t portless;
        orb_power_board_adapter_init(&portless);
        orb_power_snapshot_t shown = orb_power_get_snapshot(&other);
        CHECK(orb_power_board_apply_snapshot(&shown, &portless, NULL, shown.revision) == ORB_POWER_APPLIED);
        orb_power_preferences_t dim = orb_power_default_preferences();
        dim.brightness_pct = 35;
        CHECK(orb_power_set_preferences(&other, &dim, 0));
        shown = orb_power_get_snapshot(&other);
        int calls = s_brightness_calls;
        orb_power_board_apply_snapshot(&shown, &portless, NULL, 0);
        CHECK(s_brightness == 35 && s_brightness_calls == calls + 1);
        level.brightness_pct = 20; /* restore the shared stub's last write */
        orb_board_panel_brightness(20);
    }
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED && s_brightness == 20);
    level.brightness_pct = 70;
    CHECK(orb_power_set_preferences(&power, &level, 0));
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED && s_brightness == 70);
    uint32_t stale = power.revision;
    tick(&power, 300000); tick(&power, 300250);
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED && !adapter.panel_on);
    CHECK(orb_power_board_try_light_sleep(&power, &adapter, &port, 100) == ORB_POWER_APPLIED && s_sleep_calls == 1);
    port.wake_ready &= ~ORB_POWER_WAKE_READY_IMU;
    CHECK(orb_power_board_try_light_sleep(&power, &adapter, &port, 100) == ORB_POWER_SLEEP_DENIED);
    port.wake_ready = 31;
    CHECK(orb_power_set_inhibitors(&power, ORB_POWER_INHIBIT_DMA, 300250));
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED);
    CHECK(orb_power_board_try_light_sleep(&power, &adapter, &port, 100) == ORB_POWER_SLEEP_DENIED);
    CHECK(orb_power_set_inhibitors(&power, 0, 300250));
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 300401).accepted);
    CHECK(orb_power_board_apply(&power, &adapter, &port, stale) == ORB_POWER_WAIT_SAFE_FRAME && s_brightness == 0 && !adapter.panel_on);
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED && adapter.panel_on);
    /* Redaction drains before dimming; the already visible private frame can
     * remain lit for that one handoff without a black interval. */
    tick(&power, 320401);
    CHECK(orb_power_board_apply(&power, &adapter, &port, stale) == ORB_POWER_WAIT_SAFE_FRAME && s_brightness == 70);
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED && s_brightness == 15);
    tick(&power, 600401); tick(&power, 600801);
    CHECK(orb_power_board_apply(&power, &adapter, NULL, power.revision) == ORB_POWER_PANEL_UNSUPPORTED);
    CHECK(orb_power_board_try_light_sleep(&power, &adapter, NULL, 100) == ORB_POWER_SLEEP_DENIED);
    s_panel_ok = false;
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_PORT_FAILED);
    s_panel_ok = true;
    CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED);
    s_sleep_ok = false;
    CHECK(orb_power_board_try_light_sleep(&power, &adapter, &port, 100) == ORB_POWER_PORT_FAILED);
    s_sleep_ok = true;
}
static void explicit_display_sleep(void)
{
    orb_power_controller_t power;
    orb_power_init(&power, 1000);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 1000).accepted);
    CHECK(orb_power_set_inhibitors(&power, ORB_POWER_INHIBIT_VOICE, 1000));
    CHECK(orb_power_request_sleep(&power, 2000));
    CHECK(snap(&power).state == ORB_POWER_SLEEP && snap(&power).redact);
    CHECK(snap(&power).moment == ORB_POWER_MOMENT_SLEEP && !power.quiet);
    tick(&power, 2250); CHECK(!snap(&power).panel_on);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_ATTENTION, 2300).accepted);
    CHECK(snap(&power).redact && snap(&power).state == ORB_POWER_ACTIVE);
    CHECK(!orb_power_request_sleep(&power, 2299));
    CHECK(power.inhibitors == ORB_POWER_INHIBIT_VOICE);
}

static void copied_snapshot_adapter(void)
{
    orb_power_controller_t power;
    orb_power_board_adapter_t adapter;
    orb_power_init(&power, 0);
    orb_power_board_adapter_init(&adapter);
    orb_power_snapshot_t initial = orb_power_get_snapshot(&power);
    CHECK(orb_power_board_apply_snapshot(NULL, &adapter, NULL, initial.revision) == ORB_POWER_PORT_FAILED);
    CHECK(orb_power_board_apply_snapshot(&initial, NULL, NULL, initial.revision) == ORB_POWER_PORT_FAILED);
    CHECK(orb_power_board_apply_snapshot(&initial, &adapter, NULL, initial.revision) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 70);
    tick(&power, 300000); tick(&power, 300250);
    orb_power_snapshot_t asleep = orb_power_get_snapshot(&power);
    CHECK(orb_power_board_apply_snapshot(&asleep, &adapter, NULL, asleep.revision) == ORB_POWER_PANEL_UNSUPPORTED);
    CHECK(s_brightness == 0);
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_PICKUP, 300500).accepted);
    orb_power_snapshot_t automatic = orb_power_get_snapshot(&power);
    CHECK(automatic.redact);
    CHECK(orb_power_board_apply_snapshot(&automatic, &adapter, NULL, initial.revision) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 0);
    CHECK(orb_power_board_apply_snapshot(&automatic, &adapter, NULL, automatic.revision) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 70 && adapter.acknowledged_revision == automatic.revision);
}

static void lit_frame_handoff(void)
{
    orb_power_port_t port = { .panel_awake = panel };
    for (unsigned mode = 0; mode < 2; mode++) {
        const orb_power_port_t *transport = mode ? &port : NULL;
        orb_power_controller_t power;
        orb_power_board_adapter_t adapter;
        orb_power_init(&power, 0); orb_power_board_adapter_init(&adapter);
        CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 0).accepted);
        CHECK(orb_power_board_apply(&power, &adapter, transport, power.revision) == ORB_POWER_APPLIED);
        CHECK(s_brightness == 70 && !adapter.shown_redact);
        uint32_t stale = power.revision;
        int calls = s_brightness_calls;
        tick(&power, 20000);
        CHECK(orb_power_board_apply(&power, &adapter, transport, stale) == ORB_POWER_WAIT_SAFE_FRAME);
        CHECK(s_brightness == 70 && s_brightness_calls == calls && adapter.shown_safe);
        CHECK(orb_power_board_apply(&power, &adapter, transport, 0) == ORB_POWER_WAIT_SAFE_FRAME);
        CHECK(s_brightness == 70 && s_brightness_calls == calls);
        CHECK(orb_power_board_apply(&power, &adapter, transport, power.revision) == ORB_POWER_APPLIED);
        CHECK(s_brightness == 15 && adapter.shown_redact);
        stale = power.revision; calls = s_brightness_calls;
        tick(&power, 30000);
        CHECK(orb_power_board_apply(&power, &adapter, transport, stale) == ORB_POWER_WAIT_SAFE_FRAME);
        CHECK(s_brightness == 15 && s_brightness_calls == calls);
        CHECK(orb_power_board_apply(&power, &adapter, transport, power.revision) == ORB_POWER_APPLIED);
        CHECK(s_brightness == 9);
        stale = power.revision; calls = s_brightness_calls;
        CHECK(orb_power_wake(&power, ORB_POWER_WAKE_TOUCH, 30001).consume_input);
        CHECK(!snap(&power).redact);
        CHECK(orb_power_board_apply(&power, &adapter, transport, stale) == ORB_POWER_WAIT_SAFE_FRAME);
        CHECK(s_brightness == 9 && s_brightness_calls == calls);
        CHECK(orb_power_board_apply(&power, &adapter, transport, power.revision) == ORB_POWER_APPLIED);
        CHECK(s_brightness == 70 && !adapter.shown_redact);
    }
}

static void reveal_at_lower_brightness(void)
{
    orb_power_controller_t power;
    orb_power_board_adapter_t adapter;
    orb_power_init(&power, 0); orb_power_board_adapter_init(&adapter);
    CHECK(snap(&power).redact);
    CHECK(orb_power_board_apply(&power, &adapter, NULL, power.revision) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 70);
    uint32_t stale = power.revision;
    orb_power_preferences_t prefs = orb_power_default_preferences();
    prefs.brightness_pct = 15;
    CHECK(orb_power_set_preferences(&power, &prefs, 1));
    CHECK(orb_power_wake(&power, ORB_POWER_WAKE_PWR, 1).consume_input);
    CHECK(!snap(&power).redact);
    CHECK(orb_power_board_apply(&power, &adapter, NULL, stale) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 15 && adapter.acknowledged_revision == stale);
    CHECK(orb_power_board_apply(&power, &adapter, NULL, power.revision) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 15 && !adapter.shown_redact);
    prefs.brightness_pct = 0;
    CHECK(orb_power_set_preferences(&power, &prefs, 2));
    CHECK(orb_power_board_apply(&power, &adapter, NULL, 0) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 0);
    prefs.brightness_pct = 15;
    CHECK(orb_power_set_preferences(&power, &prefs, 3));
    CHECK(orb_power_board_apply(&power, &adapter, NULL, 0) == ORB_POWER_WAIT_SAFE_FRAME);
    CHECK(s_brightness == 0); /* An owner-darkened frame must pass the wake fence again. */
    CHECK(orb_power_board_apply(&power, &adapter, NULL, power.revision) == ORB_POWER_APPLIED);
    CHECK(s_brightness == 15);
}

static void immediate_dark_handoff(void)
{
    orb_power_port_t port = { .panel_awake = panel };
    for (unsigned critical = 0; critical < 2; critical++) {
        orb_power_controller_t power;
        orb_power_board_adapter_t adapter;
        orb_power_init(&power, 0); orb_power_board_adapter_init(&adapter);
        CHECK(orb_power_board_apply(&power, &adapter, &port, power.revision) == ORB_POWER_APPLIED);
        CHECK(s_brightness == 70);
        uint32_t stale = power.revision;
        if (critical) CHECK(orb_power_set_battery(&power, 5, false, false, 1));
        else samples(&power, 0, 300, 0, 0, -1);
        CHECK(!snap(&power).panel_on);
        CHECK(orb_power_board_apply(&power, &adapter, &port, stale) == ORB_POWER_APPLIED);
        CHECK(s_brightness == 0 && !adapter.shown_safe && !adapter.panel_on);
    }
}

int main(void)
{
    CASE(idle_boundaries); CASE(delayed_and_stale); CASE(wakes_and_bursts);
    CASE(preferences_and_motion); CASE(quiet_deadlines); CASE(always_on_brightness); CASE(battery_and_charge); CASE(imu_face_down);
    CASE(imu_pickup_and_rearm); CASE(imu_reject_noise); CASE(inhibitors);
    CASE(dma_does_not_renew_idle); CASE(frame_cadence); CASE(board_adapter); CASE(explicit_display_sleep); CASE(copied_snapshot_adapter);
    CASE(lit_frame_handoff); CASE(reveal_at_lower_brightness); CASE(immediate_dark_handoff);
    printf("orb_power: %u cases, %u checks passed (synthetic traces; ASan/UBSan)\n", s_cases, s_checks);
    return 0;
}
