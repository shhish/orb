#include "orb_buttons.h"

#include <string.h>

static void event(orb_buttons_t *buttons, orb_button_t button,
                  orb_button_gesture_t gesture, uint64_t at_ms, bool synthetic)
{
    if (buttons->event_count == ORB_BUTTON_EVENTS) {
        buttons->dropped_events++;
        return;
    }
    unsigned write = (buttons->event_read + buttons->event_count) % ORB_BUTTON_EVENTS;
    buttons->events[write] = (orb_button_event_t){button, gesture, at_ms, synthetic};
    buttons->event_count++;
}

static void edge(orb_buttons_t *buttons, orb_button_t button, uint64_t at_ms,
                 bool held, bool debounced, bool synthetic)
{
    buttons->edges[buttons->edge_write] = (orb_button_edge_t){
        .button = button, .at_ms = at_ms, .held = held,
        .debounced = debounced, .synthetic = synthetic,
    };
    buttons->edge_write = (buttons->edge_write + 1) % ORB_BUTTON_HISTORY;
    if (buttons->edge_count < ORB_BUTTON_HISTORY) buttons->edge_count++;
}

void orb_buttons_init(orb_buttons_t *buttons)
{
    if (buttons) memset(buttons, 0, sizeof(*buttons));
}

void orb_buttons_poll(orb_buttons_t *buttons, uint64_t now_ms, unsigned pressed_mask)
{
    if (!buttons) return;
    if (buttons->synthetic_held && now_ms >= buttons->synthetic_release_ms) {
        edge(buttons, buttons->synthetic_button, buttons->synthetic_release_ms, false, true, true);
        event(buttons, buttons->synthetic_button, ORB_BUTTON_UP, buttons->synthetic_release_ms, true);
        buttons->synthetic_held = false;
    }
    for (unsigned i = 0; i < ORB_BUTTON_COUNT; i++) {
        orb_button_state_t *state = &buttons->buttons[i];
        orb_button_t button = (orb_button_t)i;
        unsigned hold_ms = button == ORB_BUTTON_PWR ? ORB_BUTTON_PWR_HOLD_MS : ORB_BUTTON_BOOT_HOLD_MS;
        bool raw = (pressed_mask & (1u << i)) != 0;
        if (raw != state->raw) {
            state->raw = raw;
            state->raw_at_ms = now_ms;
            edge(buttons, button, now_ms, raw, false, false);
        }
        if (state->raw != state->held && now_ms - state->raw_at_ms >= ORB_BUTTON_DEBOUNCE_MS) {
            uint64_t at_ms = state->raw_at_ms + ORB_BUTTON_DEBOUNCE_MS;
            state->held = state->raw;
            edge(buttons, button, at_ms, state->held, true, false);
            if (state->held) {
                state->down_at_ms = at_ms;
                state->hold_sent = false;
                state->consumed = false;
                state->second_press = state->pending_short &&
                    at_ms - state->release_at_ms < ORB_BUTTON_DOUBLE_MS;
                event(buttons, button, ORB_BUTTON_DOWN, at_ms, false);
            } else {
                /* A late poll can observe the release after crossing HOLD's
                 * threshold. Preserve HOLD then UP even without a mid-hold
                 * sample; release must never turn that hold into a click. */
                if (!state->consumed && !state->hold_sent && at_ms - state->down_at_ms >= hold_ms) {
                    state->hold_sent = true;
                    state->pending_short = false;
                    event(buttons, button, ORB_BUTTON_HOLD, state->down_at_ms + hold_ms, false);
                }
                event(buttons, button, ORB_BUTTON_UP, at_ms, false);
                if (!state->consumed && !state->hold_sent) {
                    if (state->second_press) {
                        state->pending_short = false;
                        event(buttons, button, ORB_BUTTON_DOUBLE, at_ms, false);
                    } else {
                        state->pending_short = true;
                        state->release_at_ms = at_ms;
                    }
                }
                state->second_press = false;
                state->consumed = false;
            }
        }
        if (state->held && !state->consumed && !state->hold_sent && now_ms - state->down_at_ms >= hold_ms) {
            state->hold_sent = true;
            state->pending_short = false;
            event(buttons, button, ORB_BUTTON_HOLD, state->down_at_ms + hold_ms, false);
        }
        /* A second down within the window reserves the click until release;
         * a long second press becomes HOLD, never a delayed short plus HOLD. */
        if (state->pending_short && !state->second_press &&
            now_ms - state->release_at_ms >= ORB_BUTTON_DOUBLE_MS) {
            state->pending_short = false;
            event(buttons, button, ORB_BUTTON_SHORT,
                  state->release_at_ms + ORB_BUTTON_DOUBLE_MS, false);
        }
    }
}

bool orb_buttons_next(orb_buttons_t *buttons, orb_button_event_t *out)
{
    if (!buttons || !out || !buttons->event_count) return false;
    *out = buttons->events[buttons->event_read];
    buttons->event_read = (buttons->event_read + 1) % ORB_BUTTON_EVENTS;
    buttons->event_count--;
    return true;
}

void orb_buttons_consume_press(orb_buttons_t *buttons, orb_button_t button)
{
    if (!buttons || button < ORB_BUTTON_PWR || button >= ORB_BUTTON_COUNT) return;
    orb_button_state_t *state = &buttons->buttons[button];
    state->consumed = true;
    state->pending_short = false;
    state->second_press = false;
}

size_t orb_buttons_history(const orb_buttons_t *buttons, orb_button_edge_t out[ORB_BUTTON_HISTORY])
{
    if (!buttons || !out) return 0;
    unsigned oldest = (buttons->edge_write + ORB_BUTTON_HISTORY - buttons->edge_count) % ORB_BUTTON_HISTORY;
    for (unsigned i = 0; i < buttons->edge_count; i++)
        out[i] = buttons->edges[(oldest + i) % ORB_BUTTON_HISTORY];
    return buttons->edge_count;
}

bool orb_buttons_inject(orb_buttons_t *buttons, orb_button_t button,
                        orb_button_gesture_t gesture, uint64_t now_ms)
{
    if (!buttons || button >= ORB_BUTTON_COUNT || button < ORB_BUTTON_PWR ||
        (gesture != ORB_BUTTON_SHORT && gesture != ORB_BUTTON_DOUBLE && gesture != ORB_BUTTON_HOLD) ||
        buttons->synthetic_held || buttons->buttons[button].raw || buttons->buttons[button].held ||
        buttons->event_count > ORB_BUTTON_EVENTS - 3) return false;
    edge(buttons, button, now_ms, true, true, true);
    event(buttons, button, ORB_BUTTON_DOWN, now_ms, true);
    event(buttons, button, gesture, now_ms, true);
    if (gesture == ORB_BUTTON_HOLD) {
        buttons->synthetic_button = button;
        buttons->synthetic_release_ms = now_ms + 250;
        buttons->synthetic_held = true;
    } else {
        edge(buttons, button, now_ms, false, true, true);
        event(buttons, button, ORB_BUTTON_UP, now_ms, true);
    }
    return true;
}

const char *orb_button_name(orb_button_t button)
{
    return button == ORB_BUTTON_PWR ? "pwr" : button == ORB_BUTTON_BOOT ? "boot" : "unknown";
}

const char *orb_button_gesture_name(orb_button_gesture_t gesture)
{
    static const char *const names[] = {"down", "short", "double", "hold", "up"};
    return gesture >= ORB_BUTTON_DOWN && gesture <= ORB_BUTTON_UP ? names[gesture] : "unknown";
}
