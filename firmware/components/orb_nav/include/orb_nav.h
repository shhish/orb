#pragma once

/* Portable interaction state. The UI thread owns one instance; no service or
 * action callback is invoked by a gesture, including rotary and Compass. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORB_NAV_DIAMETER 466
#define ORB_NAV_EDGE_BAND 40
#define ORB_NAV_INVALID_ID UINT16_MAX
#define ORB_NAV_MAX_PLACES 32
#define ORB_NAV_BACK_CAPACITY 16
#define ORB_NAV_LAYER_CAPACITY 4
#define ORB_NAV_VELOCITY_SAMPLES 16

typedef uint16_t orb_nav_id_t;
typedef enum {
    ORB_NAV_ROLE_SCREEN, ORB_NAV_ROLE_FACE, ORB_NAV_ROLE_AGENTS,
    ORB_NAV_ROLE_HOME, ORB_NAV_ROLE_STACK, ORB_NAV_ROLE_NOTIFICATIONS,
    ORB_NAV_ROLE_CONTROL,
} orb_nav_role_t;
typedef struct {
    orb_nav_id_t id;
    orb_nav_role_t role;
    bool scrollable;
} orb_nav_place_t;
typedef struct { int16_t x, y, width, height; } orb_nav_token_t;
typedef struct {
    orb_nav_id_t id;
    orb_nav_token_t token;
    bool iris;
} orb_nav_back_entry_t;

typedef enum {
    ORB_NAV_GESTURE_NONE, ORB_NAV_GESTURE_LEFT, ORB_NAV_GESTURE_RIGHT,
    ORB_NAV_GESTURE_UP, ORB_NAV_GESTURE_DOWN, ORB_NAV_GESTURE_BACK,
    ORB_NAV_GESTURE_CONTROL, ORB_NAV_GESTURE_ROTARY,
    ORB_NAV_GESTURE_CONTENT, ORB_NAV_GESTURE_COMPASS,
} orb_nav_gesture_t;
typedef enum {
    ORB_NAV_TOUCH_DOWN, ORB_NAV_TOUCH_MOVE, ORB_NAV_TOUCH_UP,
    ORB_NAV_TOUCH_CANCEL, ORB_NAV_TOUCH_TICK,
} orb_nav_touch_phase_t;
typedef enum {
    ORB_NAV_EVENT_NONE, ORB_NAV_EVENT_BEGIN, ORB_NAV_EVENT_UPDATE,
    ORB_NAV_EVENT_END, ORB_NAV_EVENT_CANCEL, ORB_NAV_EVENT_COMPASS,
} orb_nav_event_type_t;
typedef enum {
    ORB_NAV_COMPASS_CANCEL, ORB_NAV_COMPASS_AGENTS, ORB_NAV_COMPASS_HOME,
    ORB_NAV_COMPASS_PLAY, ORB_NAV_COMPASS_SETTINGS,
} orb_nav_compass_selection_t;
typedef struct {
    orb_nav_event_type_t type;
    orb_nav_gesture_t gesture;
    float dx, dy, vx, vy; /* Pixels and pixels/second. */
    float progress;      /* Signed distance / display diameter. */
    bool commit;
    orb_nav_compass_selection_t selection;
} orb_nav_event_t;

typedef void (*orb_nav_rotary_change_cb_t)(void *ctx, float value, float delta);
typedef void (*orb_nav_rotary_tick_cb_t)(void *ctx);
typedef struct {
    orb_nav_id_t owner;
    void *ctx;
    orb_nav_rotary_change_cb_t change;
    orb_nav_rotary_tick_cb_t tick;
    float minimum, maximum, value, detent, units_per_turn;
} orb_nav_rotary_owner_t;
typedef struct {
    orb_nav_rotary_owner_t owner;
    float velocity, last_angle, detent_position;
    uint32_t last_ms;
    bool bound, flinging;
} orb_nav_rotary_t;
typedef struct {
    float x, y;
    uint32_t ms;
} orb_nav_touch_sample_t;
typedef struct {
    bool down, rim, blank;
    bool resumed, resume_commit, moved;
    float start_x, start_y, last_x, last_y, vx, vy, radial_x, radial_y;
    float progress_origin;
    uint32_t start_ms, last_ms;
    orb_nav_touch_sample_t samples[ORB_NAV_VELOCITY_SAMPLES];
    uint8_t sample_count;
    orb_nav_gesture_t owner;
} orb_nav_recognizer_t;
typedef struct {
    orb_nav_place_t places[ORB_NAV_MAX_PLACES];
    size_t place_count;
    orb_nav_id_t current, face;
    orb_nav_back_entry_t back[ORB_NAV_BACK_CAPACITY];
    size_t back_count;
    orb_nav_id_t layers[ORB_NAV_LAYER_CAPACITY];
    size_t layer_count;
    orb_nav_recognizer_t touch;
    orb_nav_rotary_t rotary;
} orb_nav_t;

void orb_nav_init(orb_nav_t *nav, orb_nav_id_t face);
bool orb_nav_register(orb_nav_t *nav, orb_nav_place_t place);
void orb_nav_set_current(orb_nav_t *nav, orb_nav_id_t id);
orb_nav_id_t orb_nav_neighbor(const orb_nav_t *nav, orb_nav_gesture_t gesture);
/* push records the current screen and launch token. token == NULL is a slide.
 * Layers are the contiguous sheets above the current plain place; opening
 * an existing sheet unwinds to it rather than stacking its identity twice.
 * Leaving that stack releases it, and Back restores it from bounded history. */
bool orb_nav_push(orb_nav_t *nav, orb_nav_id_t id, const orb_nav_token_t *token);
bool orb_nav_back(orb_nav_t *nav, orb_nav_back_entry_t *entry);
bool orb_nav_back_peek(const orb_nav_t *nav, orb_nav_back_entry_t *entry);
void orb_nav_reset(orb_nav_t *nav, orb_nav_id_t id);
/* blank_start is sampled on DOWN. scroll_at_top is retained for adapter
 * compatibility; content scroll bounds never admit system navigation.
 * TICK drives the 400ms long-press without depending on pointer movement. */
orb_nav_event_t orb_nav_touch(orb_nav_t *nav, orb_nav_touch_phase_t phase,
                             float x, float y, uint32_t now_ms,
                             bool blank_start, bool scroll_at_top);
/* After a matching edge DOWN (or a Face centre DOWN for its four swipes), grab
 * an in-flight navigation at its presentation position. Lifting without
 * movement keeps its existing target. A centre content contact cannot regrab
 * a system layer or Back. */
bool orb_nav_touch_resume(orb_nav_t *nav, orb_nav_gesture_t gesture,
                          float progress, bool commit);
bool orb_nav_touch_owned(const orb_nav_t *nav);
void orb_nav_rotary_bind(orb_nav_t *nav, const orb_nav_rotary_owner_t *owner);
void orb_nav_rotary_unbind(orb_nav_t *nav, orb_nav_id_t owner);
void orb_nav_rotary_set(orb_nav_t *nav, float value);
void orb_nav_rotary_stop(orb_nav_t *nav);
/* Desk twist enters the identical bounded owner/change/tick path as the rim.
 * UI owner only. A touch/navigation transition must retain its own input. */
bool orb_nav_rotary_step(orb_nav_t *nav, int detents);
/* Bounded inertial scrolling only; never synthesizes a click or action. */
bool orb_nav_tick(orb_nav_t *nav, uint32_t now_ms);
float orb_nav_rubber_band(float distance, float extent);

#ifdef __cplusplus
}
#endif
