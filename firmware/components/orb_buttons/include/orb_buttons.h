#pragma once

/* Allocation-free button recognizer. One owner serializes poll/next/inject and
 * history calls; timestamps are monotonic milliseconds, never wall time. The
 * recognizer has no board, task, LVGL or action/approval dependencies. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ORB_BUTTON_DEBOUNCE_MS = 30,
    ORB_BUTTON_DOUBLE_MS = 300,
    ORB_BUTTON_PWR_HOLD_MS = 2000,
    ORB_BUTTON_BOOT_HOLD_MS = 350,
    ORB_BUTTON_HISTORY = 16,
    ORB_BUTTON_EVENTS = 32,
};

typedef enum { ORB_BUTTON_PWR, ORB_BUTTON_BOOT, ORB_BUTTON_COUNT } orb_button_t;
typedef enum {
    ORB_BUTTON_DOWN, ORB_BUTTON_SHORT, ORB_BUTTON_DOUBLE,
    ORB_BUTTON_HOLD, ORB_BUTTON_UP,
} orb_button_gesture_t;

typedef struct {
    orb_button_t button;
    orb_button_gesture_t gesture;
    uint64_t at_ms;
    bool synthetic;
} orb_button_event_t;

typedef struct {
    orb_button_t button;
    uint64_t at_ms;
    bool held;
    bool debounced; /* false records the raw edge, useful for diagnosing bounce. */
    bool synthetic;
} orb_button_edge_t;

typedef struct {
    uint64_t raw_at_ms, down_at_ms, release_at_ms;
    bool raw, held, hold_sent, pending_short, second_press, consumed;
} orb_button_state_t;

typedef struct {
    orb_button_state_t buttons[ORB_BUTTON_COUNT];
    orb_button_event_t events[ORB_BUTTON_EVENTS];
    orb_button_edge_t edges[ORB_BUTTON_HISTORY];
    uint64_t synthetic_release_ms;
    orb_button_t synthetic_button;
    unsigned event_read, event_count, edge_write, edge_count;
    unsigned dropped_events;
    bool synthetic_held;
} orb_buttons_t;

void orb_buttons_init(orb_buttons_t *buttons);
/* pressed_mask uses bit 0=PWR, bit 1=BOOT, matching ORB_BTN_* on the board.
 * Replay captured GPIO edges in order, then service debounce/gesture deadlines;
 * no periodic poll is needed without a contact or pending gesture. */
void orb_buttons_poll(orb_buttons_t *buttons, uint64_t now_ms, unsigned pressed_mask);
bool orb_buttons_next(orb_buttons_t *buttons, orb_button_event_t *event);
/* After DOWN wakes a dim/off panel, consume that complete physical press.
 * Its UP is still delivered, but no SHORT/DOUBLE/HOLD may navigate afterward. */
void orb_buttons_consume_press(orb_buttons_t *buttons, orb_button_t button);
/* Last 16 raw/debounced/synthetic edges, oldest first. */
size_t orb_buttons_history(const orb_buttons_t *buttons, orb_button_edge_t out[ORB_BUTTON_HISTORY]);
/* Developer input only. It never becomes a trusted physical event. Synthetic
 * HOLD remains visible for 250 ms, then poll emits its synthetic UP. */
bool orb_buttons_inject(orb_buttons_t *buttons, orb_button_t button,
                        orb_button_gesture_t gesture, uint64_t now_ms);
const char *orb_button_name(orb_button_t button);
const char *orb_button_gesture_name(orb_button_gesture_t gesture);
