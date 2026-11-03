#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ORB_APPS_MAX 8
#define ORB_APPS_PWR_EXIT_US 1200000LL

enum {
    ORB_APP_NEEDS_IMU = 1u << 0,
    ORB_APP_NEEDS_AUDIO = 1u << 1,
    ORB_APP_NEEDS_NET = 1u << 2,
    ORB_APP_NEEDS_STORAGE = 1u << 3,
};

typedef struct {
    int64_t now_us;
    /* Monotonic capture time of this force sample. Kept separate from render
     * and fixed-step clocks so repeated/delayed samples never train a filter. */
    int64_t motion_sample_us;
    /* Screen-space specific force in g: +x right, +y up, +z out of panel.
     * Face up reads +z; this is the up vector, opposite downhill gravity. */
    float accel_x, accel_y, accel_z;
    bool motion_valid;
    /* Bias-corrected angular velocity in degrees/second, same screen axes.
     * Invalid until the board's zero-rate bias is learned; synthetic force
     * overrides leave this invalid because they have no matching gyro. */
    float gyro_x, gyro_y, gyro_z;
    bool gyro_valid;
} orb_app_input_t;

typedef struct {
    esp_err_t (*enter)(void);
    void (*exit)(void);
    void (*pause)(bool paused);
    void (*tick)(const orb_app_input_t *input);
} orb_app_entry_t;

typedef struct {
    const char *id;        /* [a-z0-9_-], <=24 bytes; also the storage namespace. */
    const char *name;      /* <=32 UTF-8 bytes. */
    const char *icon;      /* Framework7 UTF-8 glyph, drawn white by the shell. */
    uint32_t colour;       /* Flat app icon colour from hig.md section 5. */
    uint32_t needs;
    const orb_app_entry_t *entry;
} orb_app_manifest_t;

/* Main-task owned, allocation-free registry. Callbacks are compiled trusted
 * modules, must never block, and may not reenter this API. No app task owns the
 * PWR input: main routes debounced events through the shell even when an app
 * is paused (hig.md section 7 Games).
 * Shell queues requests to main; it must not call this API under LVGL lock. */
void orb_apps_init(uint32_t available);
esp_err_t orb_apps_register(const orb_app_manifest_t *manifest);
size_t orb_apps_count(void);
const orb_app_manifest_t *orb_apps_at(size_t index);
const orb_app_manifest_t *orb_apps_find(const char *id);
esp_err_t orb_apps_enter(const char *id);
void orb_apps_exit(void); /* Main-owned immediate exit. */
/* UI-thread-safe bounded requests, consumed by main's existing poll even when
 * inactive/paused. Exit takes priority over a queued launch. */
esp_err_t orb_apps_request_enter(const char *id);
void orb_apps_request_exit(void);
/* UI-owned pause sheet requests; main applies them before the next app tick.
 * A manual pause survives display wake; Resume clears only the manual reason. */
void orb_apps_request_pause(bool paused);
/* Atomic observation for shell/diagnostics; lifecycle mutations stay main-owned. */
const orb_app_manifest_t *orb_apps_active(void);
bool orb_apps_paused(void);
/* Fresh sensor input retains its capture timestamp independently of service
 * time. NULL does nothing; invalid force/rate components are zeroed before
 * reaching an app, with the corresponding validity flag cleared. */
void orb_apps_poll_input(const orb_app_input_t *input, bool pwr_held, bool screen_on);
/* Accelerometer-only compatibility entry for touch/host fixtures. */
void orb_apps_poll(int64_t now_us, bool pwr_held, bool screen_on,
                   float accel_x, float accel_y, float accel_z, bool motion_valid);

/* Proof app controller only: no LVGL/UI surface is created by this component.
 * Shell can render a white marble on true black with Close and tilt alternatives
 * after its own default/AX3/opaque design gate. Positions are unit-circle values. */
typedef struct { float x, y; bool paused; } orb_marble_snapshot_t;
bool orb_marble_snapshot(orb_marble_snapshot_t *out);
/* Visible directional controls can provide the same movement as IMU tilt. */
void orb_marble_nudge(float x, float y);
