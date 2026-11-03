#pragma once

/* Deterministic Desk/Hand policy. One caller serializes all calls; timestamps
 * are monotonic milliseconds (e.g. esp_timer_get_time()/1000), not wall time.
 * No task, allocation, LVGL or board access. See docs/power-and-calm.md. */
#include <stdbool.h>
#include <stdint.h>

typedef enum { ORB_POWER_ACTIVE, ORB_POWER_DIMMED, ORB_POWER_ALWAYS_ON, ORB_POWER_SLEEP } orb_power_state_t;
typedef enum { ORB_POWER_MOMENT_NONE, ORB_POWER_MOMENT_SLEEP, ORB_POWER_MOMENT_WAKE, ORB_POWER_MOMENT_REST } orb_power_moment_t;
typedef enum { ORB_POWER_WAKE_TOUCH, ORB_POWER_WAKE_PWR, ORB_POWER_WAKE_PICKUP, ORB_POWER_WAKE_ATTENTION } orb_power_wake_t;
typedef enum { ORB_POWER_BATTERY_NORMAL, ORB_POWER_BATTERY_LOW, ORB_POWER_BATTERY_VERY_LOW, ORB_POWER_BATTERY_CRITICAL } orb_power_battery_mode_t;

enum {
    ORB_POWER_INHIBIT_VOICE = 1u << 0,
    ORB_POWER_INHIBIT_PLAYBACK = 1u << 1,
    ORB_POWER_INHIBIT_OTA = 1u << 2,
    ORB_POWER_INHIBIT_APP = 1u << 3,
    ORB_POWER_INHIBIT_DMA = 1u << 4,
};

typedef struct {
    uint8_t brightness_pct; /* 0..100, clamped; zero is respected. */
    uint8_t always_on_brightness_pct; /* Named AOD setting: 1..15%, default 9; owner cap still applies. */
    bool always_on;
    bool motion_wake;
    bool reduce_motion;
} orb_power_preferences_t;

typedef struct {
    bool accepted;
    /* Consume the ENTIRE first contact/press when waking or revealing. The
     * shell must not replay its release into an approval or Apps action. */
    bool consume_input;
} orb_power_input_result_t;

typedef struct {
    orb_power_state_t state;
    orb_power_moment_t moment;
    orb_power_battery_mode_t battery_mode;
    uint32_t revision; /* Safe-frame acknowledgement token; zero is invalid. */
    uint64_t now_ms;
    uint64_t next_deadline_ms; /* UINT64_MAX when no policy deadline. */
    uint32_t moment_elapsed_ms;
    uint32_t moment_duration_ms;
    uint32_t frame_interval_ms; /* 1000 in settled AOD; 16 during transitions; 0=event driven. */
    uint8_t brightness_pct;
    int8_t shift_x, shift_y;
    int battery_pct; /* -1 unknown; never interpreted as empty. */
    bool redact;
    bool silent; /* Forced silence; caller also respects its normal Silent. */
    bool panel_on; /* Request only: does NOT prove physical panel power. */
    bool face_down;
    bool wake_from_sleep; /* Wake starts from a dark panel, rather than a retained lit face. */
    bool usb;
    bool charging;
    bool low_power;
    bool shutdown_requested; /* Advisory PMU soft-off request; consumer must fence active writes. */
} orb_power_snapshot_t;

/* Internal detector storage is public only to permit allocation-free contexts. */
typedef struct {
    float filtered[3], previous[3], baseline[3];
    float baseline_norm;
    uint64_t sample_ms, rest_ms, candidate_ms, down_ms, up_ms, pickup_ms;
    uint64_t acceleration_ms;
    bool have_sample, resting, armed, candidate, down_pending, up_pending;
    bool acceleration_candidate;
    int8_t acceleration_direction;
    bool face_down, have_pickup;
} orb_power_motion_t;

typedef struct {
    orb_power_preferences_t preferences;
    orb_power_motion_t motion;
    orb_power_state_t state;
    orb_power_moment_t moment;
    orb_power_battery_mode_t battery_mode;
    uint64_t now_ms, activity_ms, moment_ms, attention_ms, pickup_wake_ms;
    uint64_t automatic_ms, pickup_suppressed_until_ms, cutoff_ms;
    orb_power_state_t automatic_return;
    uint32_t revision, inhibitors;
    int battery_pct;
    bool redact, quiet, usb, charging, have_attention, have_pickup_wake, wake_from_sleep;
    bool display_locked; /* Explicit shell lock ignores display leases, retaining system inhibitors. */
    uint8_t ignored_pickups; /* Saturates once the 60-minute suppression cap is reached. */
    bool automatic, pickup_glance, pickup_budgeted, cutoff_pending, shutdown_requested;
} orb_power_controller_t;

orb_power_preferences_t orb_power_default_preferences(void);
void orb_power_init(orb_power_controller_t *power, uint64_t now_ms);
/* False means stale time; no state changes are made. */
bool orb_power_tick(orb_power_controller_t *power, uint64_t now_ms);
/* Explicit Sleep display, preserving work/navigation and ordinary automatic
 * wake admission. Quiet remains a separate owner preference. */
bool orb_power_request_sleep(orb_power_controller_t *power, uint64_t now_ms);
orb_power_input_result_t orb_power_wake(orb_power_controller_t *power, orb_power_wake_t source, uint64_t now_ms);
/* Physical interaction with an opaque alert renews activity/budget without
 * waking or revealing the retained task. Never call for synthetic input. */
bool orb_power_note_activity(orb_power_controller_t *power, uint64_t now_ms);
bool orb_power_set_preferences(orb_power_controller_t *power, const orb_power_preferences_t *preferences, uint64_t now_ms);
bool orb_power_set_battery(orb_power_controller_t *power, int pct, bool usb, bool charging, uint64_t now_ms);
bool orb_power_set_inhibitors(orb_power_controller_t *power, uint32_t inhibitors, uint64_t now_ms);
/* Quiet selects the desk presentation now. Active is an explicit reveal. */
bool orb_power_set_quiet(orb_power_controller_t *power, bool quiet, uint64_t now_ms);
/* False on non-finite/out-of-range/gapped/backward data. Invalid samples reset
 * candidate/arming continuity; an already latched face-down is preserved. */
bool orb_power_imu_sample(orb_power_controller_t *power, float x, float y, float z, uint64_t now_ms);
orb_power_snapshot_t orb_power_get_snapshot(const orb_power_controller_t *power);
/* AOD grants at most one update opportunity/second; unchanged content should
 * still produce zero redraws. Call only when considering a frame. */
bool orb_power_frame_due(const orb_power_controller_t *power, uint64_t last_frame_ms, uint32_t last_revision);
