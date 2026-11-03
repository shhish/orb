#pragma once

#include "orb_senses.h"

/* Main is the sole motion reader; diagnostics do not steal hardware tap
 * status or feed a synthetic gesture into the shell. */
void orb_devtools_publish_senses(const orb_motion_sample_t *sample,
                                 const orb_senses_result_t *result, uint64_t now_ms);
/* Main-task only: bounded synthetic specific force for app input. Real senses,
 * power gestures, and diagnostics always retain the physical sensor sample. */
bool orb_devtools_motion_override(int64_t now_us, float *ax, float *ay, float *az);
#include <stdbool.h>
#include "orb_buttons.h"

/* UI-task query: temporarily suspend app rendering/motion delivery while a
 * developer benchmark owns the screen. Network, OTA and input keep running. */
bool orb_devtools_benchmark_active(void);

struct orb_shell;
/* UI-thread registration; bench_open operates on the real shell, never a demo
 * model or a second LVGL object tree. */
void orb_devtools_set_shell(struct orb_shell *shell);

/* Callbacks copy history or enqueue injection into the main task's mailbox.
 * The main task remains the sole recognizer owner; callbacks must serialize
 * their own shared mailbox access and must never call LVGL or run actions. */
typedef size_t (*orb_devtools_buttons_snapshot_fn)(orb_button_edge_t out[ORB_BUTTON_HISTORY], void *ctx);
typedef bool (*orb_devtools_buttons_inject_fn)(orb_button_t button, orb_button_gesture_t gesture, void *ctx);
void orb_devtools_set_buttons(orb_devtools_buttons_snapshot_fn snapshot,
                             orb_devtools_buttons_inject_fn inject, void *ctx);
