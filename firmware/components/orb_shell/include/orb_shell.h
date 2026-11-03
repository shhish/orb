#pragma once

/* Host-buildable system UI. Calls, including destroy, require the LVGL lock.
 * The caller owns service tasks and copies their state into this snapshot. */
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"
#include "orb_buttons.h"
#include "orb_power.h"
#include "orb_battery.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ORB_SHELL_NET_STARTING,
    ORB_SHELL_NET_UNPROVISIONED,
    ORB_SHELL_NET_PROVISIONING,
    ORB_SHELL_NET_CONNECTING,
    ORB_SHELL_NET_CONNECTED,
    ORB_SHELL_NET_FAILED,
} orb_shell_net_t;

typedef enum {
    ORB_SHELL_LINK_OFFLINE,
    ORB_SHELL_LINK_SEARCHING,
    ORB_SHELL_LINK_PAIRING,
    ORB_SHELL_LINK_CONNECTING,
    ORB_SHELL_LINK_ONLINE,
    ORB_SHELL_LINK_UNAUTHORIZED,
} orb_shell_link_t;

typedef enum {
    ORB_SHELL_OTA_IDLE,
    ORB_SHELL_OTA_OFFERED,
    ORB_SHELL_OTA_DOWNLOADING,
    ORB_SHELL_OTA_VERIFYING,
    ORB_SHELL_OTA_READY,
    ORB_SHELL_OTA_FAILED,
} orb_shell_ota_t;

typedef struct {
    orb_shell_net_t net;
    orb_shell_link_t link;
    orb_shell_ota_t ota;
    char service_name[32];
    char pop[9];
    char ssid[33];
    char pairing_code[7];
    char hub_name[64];
    char device_name[32];
    char device_id[32];
    char clock_time[16];     /* Empty means time unavailable; never show a fake time. */
    char clock_date[32];     /* Local date, empty until time is synchronized. */
    char firmware_version[32]; /* Running version; distinct from an OTA offer. */
    char version[32];        /* Offered/update version. */
    char notes[128];
    char error[64];          /* OTA failure. */
    char service_error[64];  /* Initialization/service failure, distinct from OTA. */
    /* Increment for each received ota.check result, including no-update/error.
     * Empty/unpublished results must never imply that a request completed. */
    uint32_t update_check_revision;
    char update_check_result[64];
    int battery_pct;         /* -1 means unknown/no battery; initialize explicitly. */
    orb_battery_detail_t battery_detail;
    bool desk_charge_limit;
    bool battery_setting_error;
    int rssi;
    int progress;            /* Download percentage, clamped to 0..100. */
    int brightness;          /* Actual display setting, 0..100. */
    bool charging;
    bool usb;
    bool identify;           /* Caller clears after the identify interval. */
    bool demo;               /* Explicit offline developer preview; label visibly. */
} orb_shell_snapshot_t;

typedef enum {
    ORB_SHELL_ACTION_UPDATE,
    ORB_SHELL_ACTION_CHECK_UPDATE,
    ORB_SHELL_ACTION_FORGET_NETWORK,
    ORB_SHELL_ACTION_UNPAIR,
    ORB_SHELL_ACTION_BRIGHTNESS,
    ORB_SHELL_ACTION_SLEEP,
    ORB_SHELL_ACTION_RESTART,
    ORB_SHELL_ACTION_DESK_LIMIT,
} orb_shell_action_t;

typedef enum {
    ORB_SHELL_FACE, ORB_SHELL_STACK, ORB_SHELL_APPS, ORB_SHELL_AGENTS,
    ORB_SHELL_SESSION, ORB_SHELL_CARD, ORB_SHELL_NOTIFICATIONS,
    ORB_SHELL_CONTROL_CENTER, ORB_SHELL_SETTINGS, ORB_SHELL_DISPLAY,
    ORB_SHELL_ACCESSIBILITY, ORB_SHELL_ABOUT, ORB_SHELL_DOT,
    ORB_SHELL_VOICE, ORB_SHELL_PLAY, ORB_SHELL_UPDATES,
    ORB_SHELL_IDLE_SESSIONS, ORB_SHELL_SESSION_SETTINGS, ORB_SHELL_HOME,
    ORB_SHELL_UNITS, ORB_SHELL_TEMPERATURE, ORB_SHELL_SOUND,
    ORB_SHELL_TIME,
    ORB_SHELL_OFFLINE, ORB_SHELL_CHARGING, ORB_SHELL_NOTIFICATION_DETAIL, ORB_SHELL_COMPASS,
    ORB_SHELL_USAGE, ORB_SHELL_SESSIONS, ORB_SHELL_BATTERY, ORB_SHELL_ONBOARDING,
    ORB_SHELL_WEATHER, /* Appended: existing places keep their stable values. */
    ORB_SHELL_SURFACE_LAST = ORB_SHELL_WEATHER,
} orb_shell_surface_t;

/* Called under the LVGL lock. Queue the action; do not block or run services here. */
typedef void (*orb_shell_action_cb_t)(orb_shell_action_t action, void *ctx);
typedef struct orb_shell orb_shell_t;

orb_shell_t *orb_shell_create(lv_obj_t *parent, orb_shell_action_cb_t cb, void *ctx);
void orb_shell_update(orb_shell_t *shell, const orb_shell_snapshot_t *snapshot);
void orb_shell_destroy(orb_shell_t *shell);
lv_obj_t *orb_shell_root(const orb_shell_t *shell);

/* UI-thread entry points. System setup/pairing/install screens take precedence.
 * poll only copies the model when its revision changes; it never calls services. */
void orb_shell_poll(orb_shell_t *shell);
/* UI-thread IMU/power delivery; no board reads or services under LVGL lock. */
void orb_shell_environment(orb_shell_t *shell, float x, float y, float z, bool always_on);
/* UI-thread power delivery, independent of IMU freshness. Ambient motion stops
 * when dimmed or off; real status/model changes still reach the surface. */
void orb_shell_display_active(orb_shell_t *shell, bool active);
void orb_shell_navigate(orb_shell_t *shell, orb_shell_surface_t surface);
orb_shell_surface_t orb_shell_surface(const orb_shell_t *shell);
/* Diagnostic current place used for gesture ownership; no navigation side effect. */
orb_shell_surface_t orb_shell_nav_current(const orb_shell_t *shell);
/* UI-only deterministic navigation fixture. elapsed_ms == 0 prepares the
 * required source/snapshot pair from any workspace place or optional system
 * menu; later samples move cached pixels. finish restores navigation, detail,
 * page and scroll context. Mandatory setup/OTA/identify screens reject it. */
bool orb_shell_nav_bench(orb_shell_t *shell, const char *gesture, uint32_t elapsed_ms,
                         bool finish);
/* UI-only, monotonic receipt for real and developer-injected navigation.
 * Times use esp_timer on target, virtual LVGL time in the simulator. The
 * feedback rectangle identifies changed destination content or the launch
 * token's reveal region. Unrelated DMA cannot satisfy the input receipt.
 * Active includes finite arrival presentation until its final frame; a named
 * wait never delays settlement. transition_end_us remains the compositor end. */
typedef struct {
    uint32_t sequence;
    orb_shell_surface_t surface;
    bool cold, active, content_ready;
    bool retaining_snapshot;
    int64_t input_us, transition_end_us;
    uint64_t object_us, max_slice_us;
    uint32_t build_slices;
    size_t cache_bytes;
    uint32_t cache_entries;
    uint64_t snapshot_captures;
    uint32_t background_prepares, background_hits;
    lv_area_t feedback_area;
} orb_shell_open_metrics_t;
void orb_shell_open_metrics(const orb_shell_t *shell, orb_shell_open_metrics_t *out);
void orb_shell_back(orb_shell_t *shell);
void orb_shell_select_session(orb_shell_t *shell, const char *id);
void orb_shell_select_card(orb_shell_t *shell, const char *id);
void orb_shell_power(orb_shell_t *shell);
/* Debounced gestures, serialized by the UI owner. Synthetic gestures only
 * navigate/preview feedback; they never confirm an answer or power action. */
void orb_shell_button(orb_shell_t *shell, const orb_button_event_t *event);
/* Motion is navigation or a narrowly safe cancellation, never a click. The
 * speech callback must only stop local playback, not send/confirm a request. */
typedef bool (*orb_shell_speech_stop_cb_t)(void *ctx);
void orb_shell_set_speech_stop(orb_shell_t *shell, orb_shell_speech_stop_cb_t stop, void *ctx);
bool orb_shell_double_tap(orb_shell_t *shell);
bool orb_shell_twist(orb_shell_t *shell, int detents);
/* Silent drops ordinary cues and speech; timers and alarms still ring. */
void orb_shell_set_silent(orb_shell_t *shell, bool silent);
/* Caption is copied by LVGL; no pointer into the voice worker is retained. */
void orb_shell_show_volume(orb_shell_t *shell);
void orb_shell_media_volume(orb_shell_t *shell, unsigned volume);
void orb_shell_spoken_reply(orb_shell_t *shell, bool active, bool buffering, const char *caption);
/* Private developer scenes only: ages are synthetic, never inferred on replay. */
void orb_shell_fixture_notification_age(orb_shell_t *shell, const char *id, unsigned age_ms);
void orb_shell_talk(orb_shell_t *shell, bool held);
int orb_shell_brightness(const orb_shell_t *shell);

/* Serialized UI-thread Desk/Hand controller. The firmware enables it at boot;
 * deterministic fixtures can advance it without a board or wall clock.
 * Power transitions preserve navigation and all pending model work. */
void orb_shell_power_enable(orb_shell_t *shell, bool enabled);
void orb_shell_power_advance(orb_shell_t *shell, uint64_t now_ms);
void orb_shell_power_imu(orb_shell_t *shell, float x, float y, float z, uint64_t now_ms);
/* Eligible dark IMU path: update physical policy, present only on a revision
 * change. Caller still owns the UI lock; no synthetic samples belong here. */
void orb_shell_power_imu_sleep(orb_shell_t *shell, float x, float y, float z, uint64_t now_ms);
orb_power_snapshot_t orb_shell_power_snapshot(const orb_shell_t *shell);
uint32_t orb_shell_power_inhibitors(const orb_shell_t *shell);
/* Dark service slot: advance only calm/privacy, with no workspace polling. */
void orb_shell_power_poll(orb_shell_t *shell, const orb_shell_snapshot_t *status);
orb_power_preferences_t orb_shell_power_preferences(const orb_shell_t *shell);
void orb_shell_set_power_preferences(orb_shell_t *shell, const orb_power_preferences_t *preferences);
void orb_shell_set_quiet(orb_shell_t *shell, bool quiet);
bool orb_shell_quiet(const orb_shell_t *shell);
void orb_shell_power_set_contrast(orb_shell_t *shell, bool contrast);
/* Display owner reports exact rendered/DMA-drained generations. Calling this
 * opts into fail-closed input until the current safe frame can emit light. */
void orb_shell_power_display_ready(orb_shell_t *shell, uint32_t revision, bool ready);
void orb_shell_power_sleep(orb_shell_t *shell);
/* Explicit manual viewing intent; input callers consume the entire gesture
 * when consume_input is true. Automatic wakes never reveal private content. */
orb_power_input_result_t orb_shell_power_wake(orb_shell_t *shell, orb_power_wake_t reason);

#ifdef __cplusplus
}
#endif
