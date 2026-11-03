#pragma once

#include "lvgl.h"
#include "orb_model.h"
#include "orb_settings.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct orb_faces orb_faces_t;
typedef enum { ORB_FACES_FACE, ORB_FACES_GALLERY, ORB_FACES_COLOUR, ORB_FACES_SLOTS } orb_faces_mode_t;
typedef enum {
    ORB_FACES_OPEN_APPS, ORB_FACES_OPEN_CLAUDE, ORB_FACES_OPEN_CODEX,
    ORB_FACES_OPEN_HOME, ORB_FACES_OPEN_BATTERY, ORB_FACES_OPEN_WEATHER,
    ORB_FACES_OPEN_TIME, ORB_FACES_OPEN_DATE,
} orb_faces_target_t;

typedef struct {
    time_t now;
    /* Optional clock/date supplied by the shell; empty uses localtime(now). */
    char clock[16], weekday[24], day[8];
    int usage_percent[2]; /* -1 = unknown; Claude 5-hour, falling back to weekly. */
    bool usage_stale[2]; /* A retained quota must never look current after an outage. */
    int battery_percent; /* -1 = unavailable */
    unsigned home_on, home_total, working;
    unsigned sessions; /* All listed sessions, so "No sessions" is never a guess. */
    bool online, charging, home_stale;
    uint32_t next_seconds; /* 0 = no next timer/alarm */
    uint32_t next_total_seconds; /* timer duration;0 if no meaningful progress range */
    bool weather_override; /* deterministic preview; device reads orb_model otherwise */
    orb_weather_snapshot_t weather;
} orb_faces_snapshot_t;

typedef struct {
    void (*open)(orb_faces_target_t target, void *context);
    void (*changed)(const orb_faces_prefs_t *prefs, void *context);
    /* Optional UI-safe synchronous adapter; NULL queues the settings service. */
    bool (*persist)(const orb_faces_prefs_t *prefs, void *context);
    /* Optional typed-command port. A nonzero ticket follows the existing
     * settings result/release lifecycle; preview views never call it. */
    uint64_t (*request)(const orb_faces_prefs_t *prefs, void *context);
    void *context;
    bool always_on;
    /* Preview/test views never touch storage; the shell's normal view does. */
    bool preview;
} orb_faces_config_t;

/* LVGL/UI thread. Deleting the parent also destroys the view state. */
orb_faces_t *orb_faces_create(lv_obj_t *parent, const orb_faces_config_t *config);
void orb_faces_destroy(orb_faces_t *faces);
lv_obj_t *orb_faces_root(const orb_faces_t *faces);
void orb_faces_update(orb_faces_t *faces, const orb_faces_snapshot_t *snapshot);
void orb_faces_snapshot_model(orb_faces_snapshot_t *out, const orb_model_snapshot_t *model);
void orb_faces_current_preferences(orb_faces_prefs_t *out);
void orb_faces_set_active(orb_faces_t *faces, bool active);
/* Navigation covers restore final values without replaying a power wake. */
void orb_faces_set_covered(orb_faces_t *faces, bool covered);
void orb_faces_set_always_on(orb_faces_t *faces, bool always_on);
void orb_faces_set_mode(orb_faces_t *faces, orb_faces_mode_t mode);
orb_faces_mode_t orb_faces_mode(const orb_faces_t *faces);
void orb_faces_select(orb_faces_t *faces, orb_face_kind_t face);
/* Only gallery/colour/slots own rotary. Values are absolute item indices. */
bool orb_faces_rotary(const orb_faces_t *faces, int *value, int *maximum);
void orb_faces_turn(orb_faces_t *faces, int steps);
void orb_faces_rotate(orb_faces_t *faces, int value);
bool orb_faces_back(orb_faces_t *faces);
void orb_faces_hold(orb_faces_t *faces);
bool orb_faces_horizontal_drag(orb_faces_t *faces, int dx);
void orb_faces_select_slot(orb_faces_t *faces, unsigned slot);
void orb_faces_preferences(const orb_faces_t *faces, orb_faces_prefs_t *out);
void orb_faces_set_preferences(orb_faces_t *faces, const orb_faces_prefs_t *prefs);
/* Value-only editor context. Pending saves keep their original presenter;
 * restoring this record never submits or resumes a service command. */
typedef struct {
    orb_faces_prefs_t prefs;
    uint8_t mode, slot, face_before_gallery;
    bool browsing, dirty, save_error, failed_select;
} orb_faces_context_t;
bool orb_faces_save_context(const orb_faces_t *faces, orb_faces_context_t *out);
/* An in-flight presenter must be retained instead of overwritten. */
bool orb_faces_restore_context(orb_faces_t *faces, const orb_faces_context_t *context);
/* Explicit gallery confirmation, shared by the artwork action and callers. */
void orb_faces_confirm_selection(orb_faces_t *faces);
bool orb_faces_save(orb_faces_t *faces);
bool orb_faces_save_error(const orb_faces_t *faces);
size_t orb_faces_state_size(void);

/* The provider registry supplies all three representations from the same truth. */
typedef struct {
    const char *name, *glyph;
    char text[32];
    lv_color_t accent;
    int percent; /* -1 = absent, never an invented zero */
    /* A retained last value shown dimmed with the shared stale mark. Empty
     * values are "—" beside the provider's own glyph on every face. */
    bool stale;
    orb_faces_target_t target;
} orb_face_complication_t;
bool orb_faces_complication(const orb_faces_t *faces, orb_face_provider_t provider,
                            orb_face_complication_t *out);

/* Real retained widget geometry for host fit/overlap assertions. */
typedef struct { const char *role; lv_area_t bounds; lv_obj_t *object; } orb_faces_bounds_t;
size_t orb_faces_layout(const orb_faces_t *faces, orb_faces_bounds_t *out, size_t capacity);
unsigned orb_faces_primary_count(const orb_faces_t *faces);

#ifdef __cplusplus
}
#endif
